<#
i47 P4 Haiku layer: async worker spawned by the Stop hook (i47-attention.ps1).

Takes the transcript delta since `_haiku_cursor`, the current item list and the
currently open questions, asks a small Claude model for
  - a one line summary of what this terminal is doing,
  - a worked/mentioned relevance verdict per tracked item,
  - the questions the assistant asked Fernando and is still waiting on,
applies the verdict to ~/.claude/attention/<session>.json and re-POSTs only the
`worked` items plus the question rows to the speak.exe daemon (POST /panel).

Never writes to stdout, always exits 0.
Log: ~/.claude/attention/haiku.log (one JSON line per run).

Usage:
  pwsh -NoProfile -File i47-haiku.ps1 -Session <id> [-Transcript <path>]
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Session,
    [string]$Transcript
)

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

# The nested `claude -p` call must never recurse through the Stop hook.
if ($env:I47_NESTED) { exit 0 }

$Dir      = Join-Path $env:USERPROFILE '.claude\attention'
$LogFile  = Join-Path $Dir 'haiku.log'
$Endpoint = 'http://127.0.0.1:8124/panel'
$File     = Join-Path $Dir ("{0}.json" -f $Session)
$Lock     = Join-Path $Dir ("{0}.haiku.lock" -f $Session)

$MaxDeltaChars    = 12000
$MaxRawBytes      = 600KB     # first run: do not walk a 3 MB transcript
$LastAssistBudget = 4000
$LastUserBudget   = 2000
$FillerPerMsg     = 600
$ModelTimeoutMs   = 40000
if ($env:I47_HAIKU_TIMEOUT_MS) { try { $ModelTimeoutMs = [int]$env:I47_HAIKU_TIMEOUT_MS } catch {} }
$LockStaleMin     = 5
$ApiModel         = 'claude-haiku-4-5-20251001'

$sw     = [System.Diagnostics.Stopwatch]::StartNew()
$errors = New-Object System.Collections.ArrayList
function Add-Err([string]$where, $e) {
    try { [void]$errors.Add(("{0}: {1}" -f $where, (($e | Out-String).Trim() -replace '\s+', ' '))) } catch {}
}

$log = [ordered]@{
    ts = (Get-Date).ToString('o'); session = $Session
    delta_chars = 0; model = $null; ms = $null; cost_usd = $null
    in_tokens = $null; out_tokens = $null; attempts = 0
    summary = $null; worked = 0; mentioned = 0
    q_open = 0; q_resolved = 0; errors = @()
}
$lockHeld = $false
function Write-Log {
    try {
        $log.ms     = [int]$sw.ElapsedMilliseconds
        $log.errors = @($errors)
        if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Force -Path $Dir | Out-Null }
        $line = ($log | ConvertTo-Json -Depth 6 -Compress)
        $s = New-Object System.IO.StreamWriter($LogFile, $true, (New-Object System.Text.UTF8Encoding($false)))
        try { $s.WriteLine($line) } finally { $s.Dispose() }
    } catch {}
}
function Release-Lock {
    if ($script:lockHeld) {
        try { Remove-Item -LiteralPath $Lock -Force -ErrorAction SilentlyContinue } catch {}
        $script:lockHeld = $false
    }
}
function Done([string]$why) {
    if ($why) { $log.model = $why }
    Release-Lock; Write-Log; exit 0
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Read-JsonFile([string]$path) {
    $raw = [System.IO.File]::ReadAllText($path)
    if (-not $raw.Trim()) { return $null }
    # -DateKind String: keep last_seen/updated_at exactly as the producer wrote them
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { return ($raw | ConvertFrom-Json -DateKind String) }
    return ($raw | ConvertFrom-Json)
}
function Write-JsonFileAtomic([string]$path, $obj) {
    $json = $obj | ConvertTo-Json -Depth 12
    $tmp  = "$path.haiku.tmp"
    [System.IO.File]::WriteAllText($tmp, $json, $Utf8NoBom)
    Move-Item -LiteralPath $tmp -Destination $path -Force
}
function Norm([string]$s) {
    if (-not $s) { return '' }
    return (($s.ToLower() -replace '[^a-z0-9]+', ' ').Trim())
}
function Stamp { (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
function Head([string]$s, [int]$n) { if (-not $s) { return '' }; if ($s.Length -le $n) { return $s }; return $s.Substring(0, $n) + [char]0x2026 }
function Tail([string]$s, [int]$n) { if (-not $s) { return '' }; if ($s.Length -le $n) { return $s }; return [string][char]0x2026 + $s.Substring($s.Length - $n) }

try {

if (-not (Test-Path -LiteralPath $File)) { Done 'skip:no-session-file' }

# --- 1. per session lock ----------------------------------------------------
try {
    if (Test-Path -LiteralPath $Lock) {
        $age = ((Get-Date) - (Get-Item -LiteralPath $Lock).LastWriteTime).TotalMinutes
        if ($age -lt $LockStaleMin) { $log.model = 'skip:locked'; Write-Log; exit 0 }
        Remove-Item -LiteralPath $Lock -Force -ErrorAction SilentlyContinue
    }
    $fs = [System.IO.File]::Open($Lock, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try { $b = [System.Text.Encoding]::UTF8.GetBytes([string]$PID); $fs.Write($b, 0, $b.Length) } finally { $fs.Dispose() }
    $lockHeld = $true
} catch { $log.model = 'skip:lock-race'; Write-Log; exit 0 }

# --- 2. state ---------------------------------------------------------------
$state = $null
try { $state = Read-JsonFile $File } catch { Add-Err 'load-state' $_ }
if (-not $state) { Done 'skip:unreadable' }

if (-not $Transcript) {
    # the hook always passes it; this is the manual-run fallback
    $guess = Get-ChildItem -Path (Join-Path $env:USERPROFILE '.claude\projects') -Filter ("{0}.jsonl" -f $Session) -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($guess) { $Transcript = $guess.FullName }
}
if ((-not $Transcript) -or (-not (Test-Path -LiteralPath $Transcript))) { Done 'skip:no-transcript' }

$hcursor = 0
try { if ($state.PSObject.Properties['_haiku_cursor']) { $hcursor = [int64]$state._haiku_cursor } } catch {}

# --- 3. transcript delta ----------------------------------------------------
$msgs       = New-Object System.Collections.ArrayList   # @{ role; text }
$newHCursor = $hcursor
try {
    $fs = [System.IO.File]::Open($Transcript, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        if ($hcursor -gt $fs.Length) { $hcursor = 0 }
        $start = $hcursor
        if (($fs.Length - $start) -gt $MaxRawBytes) { $start = $fs.Length - $MaxRawBytes }
        [void]$fs.Seek($start, [System.IO.SeekOrigin]::Begin)
        $mem = New-Object System.IO.MemoryStream
        $fs.CopyTo($mem)
        $bytes = $mem.ToArray(); $mem.Dispose()
        $lastNl = -1
        for ($i = $bytes.Length - 1; $i -ge 0; $i--) { if ($bytes[$i] -eq 10) { $lastNl = $i; break } }
        if ($lastNl -lt 0) { Done 'skip:no-delta' }
        $newHCursor = $start + $lastNl + 1
        $text  = [System.Text.Encoding]::UTF8.GetString($bytes, 0, $lastNl + 1)
        $lines = $text -split "`n"
        # skipped ahead: the first line is a fragment
        if ($start -gt $hcursor -and $lines.Count -gt 1) { $lines = $lines[1..($lines.Count - 1)] }
        foreach ($line in $lines) {
            if (-not $line -or $line.Length -lt 2) { continue }
            $j = $null
            try { $j = $line | ConvertFrom-Json } catch { continue }
            if (-not $j) { continue }
            $t = [string]$j.type
            if ($t -ne 'user' -and $t -ne 'assistant') { continue }
            if ($j.PSObject.Properties['isMeta'] -and $j.isMeta) { continue }
            if ($j.PSObject.Properties['isSidechain'] -and $j.isSidechain -eq $true) { continue }
            $m = $j.message
            if (-not $m) { continue }
            $c = $m.content
            $parts  = New-Object System.Collections.ArrayList
            $isTool = $false
            if ($c -is [string]) { if ($c.Trim()) { [void]$parts.Add($c) } }
            elseif ($c) {
                foreach ($b in @($c)) {
                    if ($null -eq $b) { continue }
                    if ($b -is [string]) { [void]$parts.Add($b); continue }
                    switch ([string]$b.type) {
                        'text'     { if ($b.text) { [void]$parts.Add([string]$b.text) } }
                        'thinking' { }
                        'tool_use' {
                            # tool calls are the evidence for `worked`; keep them short
                            $n = [string]$b.name
                            $arg = ''
                            foreach ($p in @('command', 'file_path', 'pattern', 'path', 'description', 'prompt', 'url')) {
                                try { if ($b.input -and $b.input.PSObject.Properties[$p] -and $b.input.$p) { $arg = [string]$b.input.$p; break } } catch {}
                            }
                            [void]$parts.Add(('<tool {0}> {1}' -f $n, ((Head $arg 220) -replace '\s+', ' ')))
                        }
                        'tool_result' { $isTool = $true }   # results are noise for this call
                        default { }
                    }
                }
            }
            if ($isTool -and $t -eq 'user' -and $parts.Count -eq 0) { continue }
            $body = (($parts -join "`n").Trim())
            if (-not $body) { continue }
            if ($t -eq 'user') {
                # not Fernando talking: shell echoes, task notifications, image stubs
                if ($body -match '^\s*<(bash-input|bash-stdout|bash-stderr|task-notification|local-command|command-name|command-message|system-reminder)') { continue }
                if ($body -match '^\s*\[Image: source:') { continue }
            }
            [void]$msgs.Add(@{ role = $t; text = $body })
        }
    } finally { $fs.Dispose() }
} catch { Add-Err 'transcript' $_ }

if ($msgs.Count -eq 0) {
    # nothing new to reason about; still advance the cursor so we do not rescan
    try {
        if ($state.PSObject.Properties['_haiku_cursor']) { $state._haiku_cursor = $newHCursor }
        else { $state | Add-Member -NotePropertyName _haiku_cursor -NotePropertyValue $newHCursor }
        Write-JsonFileAtomic $File $state
    } catch { Add-Err 'cursor-only' $_ }
    Done 'skip:empty-delta'
}

# --- 4. assemble the capped delta ------------------------------------------
$iAssist = -1
for ($i = $msgs.Count - 1; $i -ge 0; $i--) { if ($msgs[$i].role -eq 'assistant') { $iAssist = $i; break } }
$iUser = -1
for ($i = $msgs.Count - 1; $i -ge 0; $i--) { if ($msgs[$i].role -eq 'user') { $iUser = $i; break } }
$hasNewUser = ($iUser -ge 0)

$finalAssist = if ($iAssist -ge 0) { Tail $msgs[$iAssist].text $LastAssistBudget } else { '' }
$lastUser    = if ($iUser   -ge 0) { Tail $msgs[$iUser].text   $LastUserBudget   } else { '' }

$budget = $MaxDeltaChars - $finalAssist.Length - $lastUser.Length - 200
$filler = New-Object System.Collections.ArrayList
for ($i = $msgs.Count - 1; $i -ge 0; $i--) {
    if ($i -eq $iAssist -or $i -eq $iUser) { continue }
    if ($budget -le 0) { break }
    $chunk = Head $msgs[$i].text ([Math]::Min($FillerPerMsg, $budget))
    $budget -= ($chunk.Length + 12)
    [void]$filler.Insert(0, ('{0}: {1}' -f $msgs[$i].role, $chunk))
}

$deltaSb = New-Object System.Text.StringBuilder
if ($filler.Count -gt 0) {
    [void]$deltaSb.AppendLine('--- earlier in this delta (truncated) ---')
    [void]$deltaSb.AppendLine(($filler -join "`n"))
}
if ($lastUser) {
    [void]$deltaSb.AppendLine('--- last message from the user (Fernando) ---')
    [void]$deltaSb.AppendLine($lastUser)
}
if ($finalAssist) {
    [void]$deltaSb.AppendLine('--- final assistant message ---')
    [void]$deltaSb.AppendLine($finalAssist)
}
$delta = $deltaSb.ToString()
$log.delta_chars = $delta.Length

# --- 5. current items + open questions -------------------------------------
$itemLines = New-Object System.Collections.ArrayList
$openQ     = New-Object System.Collections.ArrayList
foreach ($it in @($state.items)) {
    if (-not $it) { continue }
    $kind = [string]$it.kind
    if ($kind -eq 'question') {
        if (([string]$it.status) -ne 'resolved') { [void]$openQ.Add([string]$it.title) }
        continue
    }
    $key = if ($kind -eq 'path') { [string]$it.url } else { ('{0}#{1}' -f (([string]$it.repo) -replace '^[^/]+/', ''), [int]$it.number) }
    $rel = if ($it.PSObject.Properties['relevance'] -and $it.relevance) { [string]$it.relevance } else { 'worked' }
    [void]$itemLines.Add(('- key: {0} | kind: {1} | current: {2} | title: {3}' -f $key, $kind, $rel, (Head ([string]$it.title) 80)))
}

$itemsBlock = if ($itemLines.Count) { $itemLines -join "`n" } else { '(none)' }
$openQBlock = if ($openQ.Count) { (@($openQ | ForEach-Object { '- ' + $_ }) -join "`n") } else { '(none)' }

$prompt = @"
You classify one terminal running Claude Code. Answer with JSON only.

TRACKED ITEMS (a regex producer scraped these from the transcript; some are noise):
$itemsBlock

CURRENTLY OPEN QUESTIONS (asked earlier by the assistant, still recorded as unanswered):
$openQBlock

TRANSCRIPT DELTA (newest activity of this session):
$delta
--- end of delta ---

Produce:
1. summary: at most 70 characters, what THIS terminal is doing right now. No trailing period, do not start with "the session".
2. items: one entry for EVERY key listed in TRACKED ITEMS, with relevance:
   - "worked": this session created / edited / reviewed / merged / actively tracks it. Evidence: a gh command run here for it, a branch or work directory for it here, files edited for it, or the user and the assistant explicitly working on it.
   - "mentioned": referenced only as context. A row in a table about other people's or other sessions' work, a name inside a brief handed to a subagent, a memory recall, an example, or something merely talked about.
   The "current" value is an input, not the truth. Only change "mentioned" to "worked" when THIS delta shows work on that item. Keep an item "worked" unless the delta shows it was only ever context.
3. questions_open: questions the ASSISTANT asked the USER (Fernando) that are still waiting for his answer. Short, at most 90 characters each, phrased as the question. Exclude rhetorical questions, questions the user already answered in the delta, questions asked to subagents, and offers of further work.
4. questions_resolved: entries from CURRENTLY OPEN QUESTIONS that the delta shows answered or made obsolete. Copy their text exactly.
"@

$schema = '{"type":"object","properties":{"summary":{"type":"string"},"items":{"type":"array","items":{"type":"object","properties":{"key":{"type":"string"},"relevance":{"type":"string","enum":["worked","mentioned"]}},"required":["key","relevance"],"additionalProperties":false}},"questions_open":{"type":"array","items":{"type":"string"}},"questions_resolved":{"type":"array","items":{"type":"string"}}},"required":["summary","items","questions_open","questions_resolved"],"additionalProperties":false}'

$SysPrompt = 'You are a terse classifier. You answer with a single JSON object and nothing else.'

# --- 6. model invocation ----------------------------------------------------
# `claude -p` uses Fernando's subscription, no API key. --safe-mode disables
# hooks/CLAUDE.md/skills/MCP while leaving auth intact; I47_NESTED is the belt
# and braces guard so the nested run can never re-enter this worker.
function Invoke-Cli([string]$text) {
    $exe = $null
    try { $exe = (Get-Command claude -ErrorAction Stop).Source } catch { Add-Err 'cli' 'claude not on PATH'; return $null }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    if ($exe -match '(?i)\.(cmd|bat)$') {
        # a .cmd shim cannot be launched with UseShellExecute=false
        $psi.FileName = $env:ComSpec
        [void]$psi.ArgumentList.Add('/c')
        [void]$psi.ArgumentList.Add($exe)
    } else {
        $psi.FileName = $exe
    }
    foreach ($a in @(
        '-p',
        '--model', 'haiku',
        '--safe-mode',
        '--no-session-persistence',
        '--output-format', 'json',
        '--json-schema', $schema,
        '--tools', '',
        '--permission-prompts', 'none',
        '--system-prompt', $SysPrompt
    )) { [void]$psi.ArgumentList.Add($a) }
    $psi.RedirectStandardInput  = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute  = $false
    $psi.CreateNoWindow   = $true
    $psi.WorkingDirectory = $env:TEMP
    $psi.EnvironmentVariables['I47_NESTED'] = '1'
    $p  = [System.Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()
    try { $p.StandardInput.Write($text); $p.StandardInput.Close() } catch {}
    if (-not $p.WaitForExit($ModelTimeoutMs)) {
        try { $p.Kill($true) } catch {}
        Add-Err 'cli' 'timeout'
        return $null
    }
    $out = ''; $err = ''
    if ($so.Wait(3000)) { $out = $so.Result }
    if ($se.Wait(3000)) { $err = $se.Result }
    $code = $p.ExitCode
    $p.Dispose()
    if ($code -ne 0) { Add-Err 'cli' ('exit {0} {1}' -f $code, (($err -replace '\s+', ' ').Trim())); return $null }
    $envl = $null
    try { $envl = $out | ConvertFrom-Json } catch { Add-Err 'cli' 'envelope is not json'; return $null }
    if ($envl.is_error) { Add-Err 'cli' ([string]$envl.result); return $null }
    try { $log.model = [string](@($envl.modelUsage.PSObject.Properties.Name)[0]) } catch {}
    if (-not $log.model) { $log.model = 'haiku (cli)' }
    $log.cost_usd = $envl.total_cost_usd
    try { $log.in_tokens = [int]$envl.usage.input_tokens; $log.out_tokens = [int]$envl.usage.output_tokens } catch {}
    return [string]$envl.result
}

function Invoke-Api([string]$text) {
    if (-not $env:ANTHROPIC_API_KEY) { return $null }
    $body = @{
        model      = $ApiModel
        max_tokens = 1024
        system     = $SysPrompt
        messages   = @(@{ role = 'user'; content = $text })
    } | ConvertTo-Json -Depth 6
    try {
        $client = New-Object System.Net.Http.HttpClient
        $client.Timeout = [TimeSpan]::FromMilliseconds($ModelTimeoutMs)
        $req = New-Object System.Net.Http.HttpRequestMessage('POST', 'https://api.anthropic.com/v1/messages')
        [void]$req.Headers.TryAddWithoutValidation('x-api-key', $env:ANTHROPIC_API_KEY)
        [void]$req.Headers.TryAddWithoutValidation('anthropic-version', '2023-06-01')
        $req.Content = New-Object System.Net.Http.StringContent($body, [System.Text.Encoding]::UTF8, 'application/json')
        $t = $client.SendAsync($req)
        if (-not $t.Wait($ModelTimeoutMs)) { Add-Err 'api' 'timeout'; return $null }
        $raw = $t.Result.Content.ReadAsStringAsync().Result
        $client.Dispose()
        $j = $raw | ConvertFrom-Json
        if (-not $j.content) { Add-Err 'api' (Head (($raw -replace '\s+', ' ')) 200); return $null }
        $log.model = $ApiModel
        try { $log.in_tokens = [int]$j.usage.input_tokens; $log.out_tokens = [int]$j.usage.output_tokens } catch {}
        return [string](@($j.content | Where-Object { $_.type -eq 'text' } | ForEach-Object { $_.text }) -join '')
    } catch { Add-Err 'api' $_; return $null }
}

function Parse-Verdict([string]$raw) {
    if (-not $raw) { return $null }
    $s = $raw.Trim()
    if ($s -match '(?s)```(?:json)?\s*(.+?)\s*```') { $s = $Matches[1].Trim() }
    $a = $s.IndexOf('{'); $b = $s.LastIndexOf('}')
    if ($a -lt 0 -or $b -le $a) { return $null }
    $s = $s.Substring($a, $b - $a + 1)
    try { return ($s | ConvertFrom-Json) } catch { return $null }
}

$verdict = $null
$useApi  = $false
foreach ($attempt in 1, 2) {
    $log.attempts = $attempt
    $text = if ($attempt -eq 1) { $prompt } else { $prompt + "`n`nIMPORTANT: return only the JSON object. No prose, no code fence." }
    $raw = $null
    if (-not $useApi) {
        $raw = Invoke-Cli $text
        if ($null -eq $raw) { $useApi = $true }
    }
    if ($null -eq $raw) { $raw = Invoke-Api $text }
    $verdict = Parse-Verdict $raw
    if ($verdict) { break }
    if ($attempt -eq 1) { Add-Err 'parse' 'no usable json, retrying with a nudge' }
}
if (-not $verdict) { Done 'skip:no-verdict' }

# --- 7. apply ---------------------------------------------------------------
# re-read: the producer may have written while the model was thinking
try { $state = Read-JsonFile $File } catch { Add-Err 'reload' $_ }
if (-not $state) { Done 'skip:reload-failed' }

function Set-Prop($o, [string]$name, $value) {
    if ($o.PSObject.Properties[$name]) { $o.$name = $value } else { $o | Add-Member -NotePropertyName $name -NotePropertyValue $value -Force }
}

$verdictRel = @{}
foreach ($e in @($verdict.items)) {
    if (-not $e -or -not $e.key) { continue }
    $r = ([string]$e.relevance).ToLower()
    if ($r -ne 'worked' -and $r -ne 'mentioned') { continue }
    $k = [string]$e.key
    $verdictRel[(Norm $k)] = $r
    # tolerate owner/repo#N when the key we handed out was repo#N
    $verdictRel[(Norm ($k -replace '^[A-Za-z0-9._-]+/([A-Za-z0-9._-]+#)', '$1'))] = $r
}

$keep = New-Object System.Collections.ArrayList
$now  = Stamp
foreach ($it in @($state.items)) {
    if (-not $it) { continue }
    if (([string]$it.kind) -eq 'question') { continue }   # rebuilt below
    $kind = [string]$it.kind
    $key = if ($kind -eq 'path') { [string]$it.url } else { ('{0}#{1}' -f (([string]$it.repo) -replace '^[^/]+/', ''), [int]$it.number) }
    $new = if ($it.PSObject.Properties['relevance'] -and $it.relevance) { [string]$it.relevance } else { 'worked' }
    $nk = Norm $key
    if ($verdictRel.ContainsKey($nk)) { $new = $verdictRel[$nk] }
    Set-Prop $it 'relevance' $new
    if ($new -eq 'worked') { $log.worked++ } else { $log.mentioned++ }
    [void]$keep.Add($it)
}

# questions: keep the ones the model still lists, drop the resolved ones, and
# clear the board when Fernando has spoken since (unless re-listed as open)
$resolved = @{}
foreach ($q in @($verdict.questions_resolved)) { if ($q) { $resolved[(Norm ([string]$q))] = $true } }
$stillOpen = @{}
$openOrder = New-Object System.Collections.ArrayList
foreach ($q in @($verdict.questions_open)) {
    $t = ([string]$q).Trim()
    if (-not $t) { continue }
    $n = Norm $t
    if ($resolved.ContainsKey($n) -or $stillOpen.ContainsKey($n)) { continue }
    $stillOpen[$n] = $t
    [void]$openOrder.Add($n)
}
$prevQ = @{}
foreach ($it in @($state.items)) {
    if (-not $it -or ([string]$it.kind) -ne 'question') { continue }
    $prevQ[(Norm ([string]$it.title))] = $it
}
foreach ($n in $openOrder) {
    $it = $prevQ[$n]
    if ($it) {
        Set-Prop $it 'title'     $stillOpen[$n]
        Set-Prop $it 'status'    'open'
        Set-Prop $it 'last_seen' $now
        [void]$keep.Add($it)
    } else {
        [void]$keep.Add([pscustomobject][ordered]@{
            kind = 'question'; url = ''; title = $stillOpen[$n]
            status = 'open'; last_seen = $now
        })
    }
}
# a previously open question survives only while Fernando has not spoken
if (-not $hasNewUser) {
    foreach ($n in @($prevQ.Keys)) {
        if ($stillOpen.ContainsKey($n) -or $resolved.ContainsKey($n)) { continue }
        [void]$keep.Add($prevQ[$n])
    }
}
foreach ($n in @($prevQ.Keys)) { if ($resolved.ContainsKey($n)) { $log.q_resolved++ } }
$log.q_open = $openOrder.Count

$state.items = @($keep.ToArray())

$sum = ([string]$verdict.summary).Trim()
if ($sum.Length -gt 90) { $sum = $sum.Substring(0, 90) }
if ($sum) { Set-Prop $state 'summary' $sum; $log.summary = $sum }
Set-Prop $state '_haiku_cursor' $newHCursor
Set-Prop $state 'updated_at' $now

try { Write-JsonFileAtomic $File $state } catch { Add-Err 'persist' $_ }

# --- 8. POST (worked items + questions only) --------------------------------
try {
    $send = New-Object System.Collections.ArrayList
    foreach ($it in @($state.items)) {
        if (-not $it) { continue }
        if (([string]$it.kind) -eq 'question') { [void]$send.Add($it); continue }
        $rel = if ($it.PSObject.Properties['relevance'] -and $it.relevance) { [string]$it.relevance } else { 'worked' }
        if ($rel -ne 'mentioned') { [void]$send.Add($it) }
    }
    $payload = [ordered]@{
        session    = [string]$state.session
        title      = [string]$state.title
        items      = @($send.ToArray())
        updated_at = $now
    }
    if ($state.summary) { $payload['summary'] = [string]$state.summary }
    $body    = $payload | ConvertTo-Json -Depth 12 -Compress
    $client  = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromMilliseconds(1500)
    $content = New-Object System.Net.Http.StringContent($body, [System.Text.Encoding]::UTF8, 'application/json')
    $t = $client.PostAsync($Endpoint, $content)
    [void]$t.Wait(2000)
    $client.Dispose()
} catch { Add-Err 'post' $_ }

Release-Lock
Write-Log

} catch {
    Add-Err 'fatal' $_
    Release-Lock
    Write-Log
}

exit 0
