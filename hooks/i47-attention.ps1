# i47 P1 producer: Claude Code hook (Stop + PostToolUse/Bash) that mines the
# session transcript delta for PRs, issues and work paths, keeps
# ~/.claude/attention/<session_id>.json in the POST /panel contract shape,
# resolves the hosting Windows Terminal title (P0 resolver) and POSTs the file
# to the speak.exe daemon.
#
# Never writes to stdout, never blocks, always exits 0.
# Log: ~/.claude/attention/producer.log (one JSON line per invocation).

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

# The P4 worker runs a nested `claude -p`; that run must never re-enter here.
if ($env:I47_NESTED) { exit 0 }

$Dir      = Join-Path $env:USERPROFILE '.claude\attention'
$LogFile  = Join-Path $Dir 'producer.log'
$RepoCache = Join-Path $Dir 'known-repos.txt'
$Endpoint = 'http://127.0.0.1:8124/panel'
$DefaultOwner = 'optidatacloud'
$MaxItems = 30
$MaxAgeHours = 48

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$errors = New-Object System.Collections.ArrayList
function Add-Err([string]$where, $e) {
    try { [void]$errors.Add(("{0}: {1}" -f $where, (($e | Out-String).Trim() -replace '\s+', ' '))) } catch {}
}

$log = [ordered]@{
    ts = (Get-Date).ToString('o'); session = $null; event = $null
    delta_bytes = 0; items_total = 0; items_new = 0; items_sent = 0
    title = $null; post_status = $null; haiku = $null; ms = $null; errors = @()
}

function Write-Log {
    try {
        $log.ms = [int]$sw.ElapsedMilliseconds
        $log.errors = @($errors)
        if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Force -Path $Dir | Out-Null }
        $line = ($log | ConvertTo-Json -Depth 6 -Compress)
        $s = New-Object System.IO.StreamWriter($LogFile, $true, (New-Object System.Text.UTF8Encoding($false)))
        try { $s.WriteLine($line) } finally { $s.Dispose() }
    } catch {}
}

try {

# --- 0. hook payload ---------------------------------------------------------
$hook = $null
try {
    if ([Console]::IsInputRedirected) {
        $raw = [Console]::In.ReadToEnd()
        if ($raw) { $hook = $raw | ConvertFrom-Json }
    }
} catch { Add-Err 'stdin' $_ }
if (-not $hook) { $log.post_status = 'skip:no-payload'; Write-Log; exit 0 }

$sessionId = [string]$hook.session_id
$event     = [string]$hook.hook_event_name
$cwd       = [string]$hook.cwd
$transcript = [string]$hook.transcript_path
$log.session = $sessionId
$log.event   = $event

if (-not $sessionId) { $log.post_status = 'skip:no-session'; Write-Log; exit 0 }

# PostToolUse only matters for `gh pr|issue` Bash calls.
if ($event -eq 'PostToolUse') {
    $cmd = ''
    try { if ($hook.tool_input) { $cmd = [string]$hook.tool_input.command } } catch {}
    if ($cmd -notmatch '(?i)\bgh\s+(pr|issue)\b') { $log.post_status = 'skip:not-gh'; Write-Log; exit 0 }
}
$budgetMs = if ($event -eq 'Stop') { 5000 } else { 3000 }

# --- 1. session file --------------------------------------------------------
$File = Join-Path $Dir ("{0}.json" -f $sessionId)
$state = $null
try { if (Test-Path $File) { $state = Get-Content -LiteralPath $File -Raw -Encoding UTF8 | ConvertFrom-Json } } catch { Add-Err 'load-state' $_ }

$cursor = 0
$items = [ordered]@{}   # key -> ordered hashtable
if ($state) {
    try { if ($state._cursor) { $cursor = [int64]$state._cursor } } catch {}
    try {
        foreach ($it in @($state.items)) {
            if (-not $it) { continue }
            $h = [ordered]@{}
            foreach ($p in $it.PSObject.Properties) { $h[$p.Name] = $p.Value }
            $k = switch ([string]$h.kind) {
                'path'     { 'path|' + ([string]$h.url).ToLower() }
                'question' { 'question|' + (([string]$h.title).ToLower() -replace '[^a-z0-9]+', ' ').Trim() }
                default    { "{0}|{1}|{2}" -f $h.kind, $h.repo, $h.number }
            }
            $items[$k] = $h
        }
    } catch { Add-Err 'load-items' $_ }
}
$preExisting = @($items.Keys)

# --- 2. known repos (for `repo#N` without an owner) -------------------------
$knownRepos = @{}
try {
    $fresh = (Test-Path $RepoCache) -and (((Get-Date) - (Get-Item $RepoCache).LastWriteTime).TotalHours -lt 24)
    if (-not $fresh) {
        # depth-limited globs only: a -Recurse walk of C:\Dev costs seconds
        $names = New-Object System.Collections.ArrayList
        $specs = @(
            'C:\Dev\field\cto\repos\*', 'C:\Dev\field\cto\repos\*\*', 'C:\Dev\field\cto\repos\*\*\*',
            'C:\Dev\field\work\*', 'C:\Dev\www\*'
        )
        foreach ($spec in $specs) {
            foreach ($d in (Get-Item -Path $spec -ErrorAction SilentlyContinue)) {
                if ($d.PSIsContainer) { [void]$names.Add($d.Name) }
            }
        }
        $uniq = @($names | Where-Object { $_ -and $_.Length -gt 2 -and $_ -notmatch '^\d' -and $_ -match '[A-Za-z]' } | Sort-Object -Unique)
        if ($uniq.Count -gt 0) { Set-Content -LiteralPath $RepoCache -Value $uniq -Encoding UTF8 }
    }
    if (Test-Path $RepoCache) {
        foreach ($n in (Get-Content -LiteralPath $RepoCache -Encoding UTF8)) { if ($n) { $knownRepos[$n.Trim().ToLower()] = $true } }
    }
} catch { Add-Err 'known-repos' $_ }

# --- 3. transcript delta ----------------------------------------------------
$deltaLines = @()
$newCursor = $cursor
try {
    if ($transcript -and (Test-Path -LiteralPath $transcript)) {
        $fs = [System.IO.File]::Open($transcript, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            if ($cursor -gt $fs.Length) { $cursor = 0 }        # truncated / rotated
            [void]$fs.Seek($cursor, [System.IO.SeekOrigin]::Begin)
            $ms = New-Object System.IO.MemoryStream
            $fs.CopyTo($ms)
            $bytes = $ms.ToArray()
            $ms.Dispose()
            $log.delta_bytes = $bytes.Length
            # keep only whole lines; the cursor stops after the last newline
            $lastNl = -1
            for ($i = $bytes.Length - 1; $i -ge 0; $i--) { if ($bytes[$i] -eq 10) { $lastNl = $i; break } }
            if ($lastNl -ge 0) {
                $newCursor = $cursor + $lastNl + 1
                $text = [System.Text.Encoding]::UTF8.GetString($bytes, 0, $lastNl + 1)
                $deltaLines = $text -split "`n"
            } else {
                $newCursor = $cursor
            }
        } finally { $fs.Dispose() }
    }
} catch { Add-Err 'transcript' $_ }

# --- 4. extraction ----------------------------------------------------------
$rxUrl       = [regex]'https?://github\.com/([A-Za-z0-9._-]+)/([A-Za-z0-9._-]+)/(pull|issues)/(\d{1,7})'
$rxOwnerRepo = [regex]'(?<![A-Za-z0-9._/#-])([A-Za-z0-9][A-Za-z0-9._-]*)/([A-Za-z0-9][A-Za-z0-9._-]*)#(\d{1,5})(?![0-9A-Za-z])'
$rxRepoHash  = [regex]'(?<![A-Za-z0-9._/#-])([A-Za-z][A-Za-z0-9._-]*)#(\d{1,5})(?![0-9A-Za-z])'
$rxBare      = [regex]'(?<![A-Za-z0-9._#:/-])(?:(?<kw>PR|pull request|issue)\s+)?#(?<n>\d{1,5})(?![0-9A-Za-z])'
$rxPath      = [regex]'(?i)([A-Za-z]:[\\/]Dev[\\/]field[\\/]work[\\/][A-Za-z0-9._-]+[\\/][A-Za-z0-9._-]+)'
$rxMdLink    = [regex]'\[([^\]\r\n]{2,140})\]\((https?://github\.com/[^\s)]+)\)'
$rxJsonTitle = [regex]'"title"\s*:\s*"((?:[^"\\]|\\.){2,200})"'
$rxGhTitle   = [regex]'(?m)^\s*title:\s*(\S.{1,200}?)\s*$'

function Flatten-Strings($o, [int]$depth = 0) {
    if ($null -eq $o -or $depth -gt 6) { return @() }
    if ($o -is [string]) { return @($o) }
    if ($o -is [bool] -or $o -is [int] -or $o -is [long] -or $o -is [double]) { return @() }
    if ($o -is [System.Collections.IDictionary]) {
        $r = @(); foreach ($v in $o.Values) { $r += Flatten-Strings $v ($depth + 1) }; return $r
    }
    if ($o -is [System.Collections.IEnumerable]) {
        $r = @(); foreach ($i in $o) { $r += Flatten-Strings $i ($depth + 1) }; return $r
    }
    if ($o.PSObject -and $o.PSObject.Properties) {
        $r = @(); foreach ($p in $o.PSObject.Properties) { $r += Flatten-Strings $p.Value ($depth + 1) }; return $r
    }
    return @()
}

function Get-MessageText($j) {
    $parts = New-Object System.Collections.ArrayList
    $m = $j.message
    if ($m) {
        $c = $m.content
        if ($c -is [string]) { [void]$parts.Add($c) }
        elseif ($c) {
            foreach ($b in @($c)) {
                if ($null -eq $b) { continue }
                if ($b -is [string]) { [void]$parts.Add($b); continue }
                switch ([string]$b.type) {
                    'text'        { if ($b.text) { [void]$parts.Add([string]$b.text) } }
                    'thinking'    { }
                    'tool_use'    { foreach ($s in (Flatten-Strings $b.input)) { [void]$parts.Add($s) } }
                    'tool_result' {
                        $rc = $b.content
                        if ($rc -is [string]) { [void]$parts.Add($rc) }
                        else { foreach ($s in (Flatten-Strings $rc)) { [void]$parts.Add($s) } }
                    }
                    default       { foreach ($s in (Flatten-Strings $b)) { [void]$parts.Add($s) } }
                }
            }
        }
    }
    if ($j.toolUseResult) { foreach ($s in (Flatten-Strings $j.toolUseResult)) { [void]$parts.Add($s) } }
    return ($parts -join "`n")
}

# repo inferred from the cwd, when the cwd is a work activity directory
$cwdRepo = $null
try {
    if ($cwd -and ($cwd -match '(?i)[A-Za-z]:[\\/]Dev[\\/]field[\\/]work[\\/]([A-Za-z0-9._-]+)')) { $cwdRepo = $Matches[1] }
} catch {}

# $relevance: 'worked' for the strong rules (a real URL, an explicit owner/repo#N,
# a work path), 'mentioned' for the weak bare-`#N` rule. Haiku (P4) may revise it;
# an item without the property is treated as 'worked' everywhere downstream.
function Add-Item($kind, $repo, $number, $url, $title, $ts, $relevance = 'worked') {
    $key = if ($kind -eq 'path') { 'path|' + ([string]$url).ToLower() } else { "{0}|{1}|{2}" -f $kind, $repo, $number }
    if ($items.Contains($key)) {
        $h = $items[$key]
        $h['last_seen'] = $ts
        if ((-not $h['title']) -and $title) { $h['title'] = $title }
        if ((-not $h['url']) -and $url) { $h['url'] = $url }
        # a strong sighting promotes a row the weak rule created; never demote
        if ($relevance -eq 'worked' -and -not $h['relevance']) { $h['relevance'] = 'worked' }
    } else {
        $h = [ordered]@{ kind = $kind }
        if ($kind -ne 'path') { $h['repo'] = $repo; $h['number'] = [int]$number }
        $h['url'] = $url
        $h['title'] = if ($title) { [string]$title } else { '' }
        $h['status'] = 'unknown'
        $h['relevance'] = $relevance
        $h['last_seen'] = $ts
        $items[$key] = $h
    }
    return $key
}

$processed = 0
foreach ($line in $deltaLines) {
    if (-not $line -or $line.Length -lt 2) { continue }
    if ($sw.ElapsedMilliseconds -gt ($budgetMs - 800)) { Add-Err 'budget' 'delta scan truncated'; break }
    $j = $null
    try { $j = $line | ConvertFrom-Json } catch { continue }
    if (-not $j) { continue }
    $t = [string]$j.type
    if ($t -notin @('user', 'assistant', 'attachment', 'system')) { continue }
    $text = ''
    try { $text = Get-MessageText $j } catch { continue }
    if (-not $text) { continue }
    $processed++

    $ts = $null
    try { if ($j.timestamp) { $ts = ([datetime]$j.timestamp).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } } catch {}
    if (-not $ts) { $ts = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }

    # titles available in this message
    $mdTitles = @{}
    foreach ($m in $rxMdLink.Matches($text)) { $mdTitles[$m.Groups[2].Value] = $m.Groups[1].Value.Trim() }
    # a `"title": "..."` / `title:\tX` pair counts only in gh output context,
    # otherwise arbitrary JSON (e.g. a terminal title in a log line) leaks in
    $loneTitle = $null
    if ($text -match '(?i)gh\s+(pr|issue)\s+(view|create|list)') {
        $mm = $rxGhTitle.Match($text)
        if ($mm.Success) { $loneTitle = $mm.Groups[1].Value }
        if (-not $loneTitle) { $mm = $rxJsonTitle.Match($text); if ($mm.Success) { $loneTitle = $mm.Groups[1].Value } }
    }
    if ($loneTitle) {
        $loneTitle = ($loneTitle -replace '\\"', '"' -replace '\\n', ' ').Trim()
        # reject spinner-prefixed window titles and paths
        if ($loneTitle -match '^[^\p{L}\p{N}"(\[]' -or $loneTitle.Length -lt 4) { $loneTitle = $null }
    }

    $work = $text
    $reposHere = New-Object System.Collections.ArrayList
    $touched = New-Object System.Collections.ArrayList

    # 4a. full GitHub URLs
    foreach ($m in $rxUrl.Matches($text)) {
        $owner = $m.Groups[1].Value; $repo = $m.Groups[2].Value
        $kind = if ($m.Groups[3].Value -eq 'pull') { 'pr' } else { 'issue' }
        $num  = $m.Groups[4].Value
        $url  = "https://github.com/$owner/$repo/$($m.Groups[3].Value)/$num"
        $ttl  = $null
        foreach ($k in $mdTitles.Keys) { if ($k -like ($url + '*')) { $ttl = $mdTitles[$k]; break } }
        [void]$touched.Add((Add-Item $kind "$owner/$repo" $num $url $ttl $ts))
        [void]$reposHere.Add("$owner/$repo")
    }
    $work = $rxUrl.Replace($work, ' ')

    # 4b. owner/repo#N
    foreach ($m in $rxOwnerRepo.Matches($work)) {
        $owner = $m.Groups[1].Value; $repo = $m.Groups[2].Value; $num = $m.Groups[3].Value
        if ($owner -match '(?i)^(https?|www)$') { continue }
        $kind = 'issue'
        $url = "https://github.com/$owner/$repo/issues/$num"
        [void]$touched.Add((Add-Item $kind "$owner/$repo" $num $url $null $ts))
        [void]$reposHere.Add("$owner/$repo")
    }
    $work = $rxOwnerRepo.Replace($work, ' ')

    # 4c. repo#N (owner implied: optidatacloud), only for repos we know
    foreach ($m in $rxRepoHash.Matches($work)) {
        $repo = $m.Groups[1].Value; $num = $m.Groups[2].Value
        if (-not $knownRepos.ContainsKey($repo.ToLower())) { continue }
        $url = "https://github.com/$DefaultOwner/$repo/issues/$num"
        [void]$touched.Add((Add-Item 'issue' "$DefaultOwner/$repo" $num $url $null $ts))
        [void]$reposHere.Add("$DefaultOwner/$repo")
    }
    $work = $rxRepoHash.Replace($work, ' ')

    # 4d. bare #N / PR #N / issue #N, only with an inferable repo
    # ambiguous when the message names several repos: skip rather than guess
    $distinctRepos = @($reposHere | Select-Object -Unique)
    $inferRepo = $null
    if ($distinctRepos.Count -eq 1) { $inferRepo = $distinctRepos[0] }
    elseif ($distinctRepos.Count -eq 0 -and $cwdRepo) { $inferRepo = "$DefaultOwner/$cwdRepo" }
    if ($inferRepo) {
        foreach ($m in $rxBare.Matches($work)) {
            $num = $m.Groups['n'].Value
            if ($num.Length -gt 1 -and $num.StartsWith('0')) { continue }   # #007 → not an issue
            $kw = [string]$m.Groups['kw'].Value
            $kind = if ($kw -match '(?i)^(pr|pull request)$') { 'pr' } else { 'issue' }
            # do not fork a second row when the other kind is already tracked
            $other = if ($kind -eq 'pr') { 'issue' } else { 'pr' }
            $otherKey = ("{0}|{1}|{2}" -f $other, $inferRepo, $num)
            if ($items.Contains($otherKey)) { $kind = $other }
            $seg = if ($kind -eq 'pr') { 'pull' } else { 'issues' }
            $url = "https://github.com/$inferRepo/$seg/$num"
            # weakest rule: the repo is inferred, so this row is noise until proven
            [void]$touched.Add((Add-Item $kind $inferRepo $num $url $null $ts 'mentioned'))
        }
    }

    # 4e. work paths
    foreach ($m in $rxPath.Matches($text)) {
        $p = ($m.Groups[1].Value -replace '/', '\')
        $leaf = ($p -split '\\')[-1]
        [void]$touched.Add((Add-Item 'path' $null $null $p $leaf $ts))
    }

    # a single item in a message claims that message's lone title
    if ($loneTitle) {
        $prIssue = @($touched | Select-Object -Unique | Where-Object { $_ -and -not $_.StartsWith('path|') })
        if ($prIssue.Count -eq 1) {
            $h = $items[$prIssue[0]]
            if ($h -and -not $h['title']) { $h['title'] = $loneTitle }
        }
    }
}

# --- 5. prune + cap ---------------------------------------------------------
try {
    $cut = (Get-Date).ToUniversalTime().AddHours(-$MaxAgeHours)
    $kept = @()
    foreach ($k in @($items.Keys)) {
        $h = $items[$k]
        $ls = $null
        try { $ls = ([datetime]$h['last_seen']).ToUniversalTime() } catch { $ls = (Get-Date).ToUniversalTime() }
        if ($ls -lt $cut) { continue }
        $kept += [pscustomobject]@{ key = $k; when = $ls }
    }
    $kept = @($kept | Sort-Object when -Descending | Select-Object -First $MaxItems)
    $final = [ordered]@{}
    foreach ($e in $kept) { $final[$e.key] = $items[$e.key] }
    $items = $final
} catch { Add-Err 'prune' $_ }

$itemList = @()
foreach ($k in $items.Keys) { $itemList += ,([pscustomobject]$items[$k]) }
$log.items_total = $itemList.Count
$log.items_new = @($items.Keys | Where-Object { $preExisting -notcontains $_ }).Count

# --- 6. hosting terminal title (P0 resolver) --------------------------------
$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class I47P1 {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
        public static extern uint GetConsoleTitleW(StringBuilder sb, uint size);
    public static string ConTitle() {
        var sb = new StringBuilder(1024);
        uint n = GetConsoleTitleW(sb, (uint)sb.Capacity);
        if (n == 0) return null;
        string s = sb.ToString();
        if (n < (uint)s.Length) s = s.Substring(0, (int)n);   // buffer tail is uninitialised
        int z = s.IndexOf('\0');
        if (z >= 0) s = s.Substring(0, z);
        var o = new StringBuilder(s.Length);
        foreach (char c in s) { if (!char.IsControl(c)) o.Append(c); }  // ConPTY leaves BEL etc.
        s = o.ToString().Trim();
        return s.Length == 0 ? null : s;
    }
}
'@
function Test-Title([string]$v) {
    if (-not $v) { return $false }
    if ($v -in @('Windows PowerShell', 'Administrator: Windows PowerShell', 'Command Prompt')) { return $false }
    if ($v -match '(?i)\.(exe|cmd|bat|ps1)$') { return $false }
    if ($v -match '(?i)^(pwsh|powershell|cmd|node|bash)$') { return $false }
    if ($v -match '(?i)^(mingw|msys)') { return $false }         # Bash-tool child title
    if ($v -match '^(/|[A-Za-z]:\\)') { return $false }
    return $true
}
$termTitle = $null
try {
    if (-not ('I47P1' -as [type])) { Add-Type -TypeDefinition $sig -Language CSharp | Out-Null }
    $chain = @()
    $cur = $PID
    $stopAt = @('windowsterminal.exe', 'openconsole.exe', 'conhost.exe', 'explorer.exe')
    for ($i = 0; $i -lt 12; $i++) {
        $p = $null
        try { $p = Get-CimInstance Win32_Process -Filter "ProcessId=$cur" -ErrorAction Stop } catch {}
        if (-not $p) { break }
        if ($p.ProcessId -ne $PID) { $chain += [int]$p.ProcessId }
        if ($stopAt -contains $p.Name.ToLower()) { break }
        if (-not $p.ParentProcessId -or $p.ParentProcessId -eq 0 -or $p.ParentProcessId -eq $cur) { break }
        $cur = [int]$p.ParentProcessId
    }
    # AttachConsole to each ancestor (nearest first); FreeConsole first or GLE 5.
    foreach ($apid in $chain) {
        try {
            [void][I47P1]::FreeConsole()
            if ([I47P1]::AttachConsole([uint32]$apid)) {
                $v = [I47P1]::ConTitle()
                if (Test-Title $v) { $termTitle = $v; break }
            }
        } catch {}
    }
    try { [void][I47P1]::FreeConsole() } catch {}
    if (-not $termTitle) { try { if (Test-Title ([Console]::Title)) { $termTitle = [Console]::Title } } catch {} }
} catch { Add-Err 'title' $_ }
$log.title = $termTitle

# --- 7. persist -------------------------------------------------------------
$payload = [ordered]@{
    session    = $sessionId
    title      = $termTitle
    items      = $itemList
    updated_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
}
if ($state -and $state.summary) { $payload['summary'] = [string]$state.summary }

try {
    $onDisk = [ordered]@{}
    foreach ($k in $payload.Keys) { $onDisk[$k] = $payload[$k] }
    $onDisk['_cursor'] = $newCursor
    # P4 keeps its own cursor over the same transcript; never clobber it
    if ($state -and $state.PSObject.Properties['_haiku_cursor']) { $onDisk['_haiku_cursor'] = $state._haiku_cursor }
    if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Force -Path $Dir | Out-Null }
    $json = $onDisk | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($File, $json, (New-Object System.Text.UTF8Encoding($false)))
} catch { Add-Err 'persist' $_ }

# --- 8. POST ----------------------------------------------------------------
# The panel only ever shows what this session actually worked on plus the
# questions waiting on Fernando. `mentioned` rows stay in the file for audit;
# applying the filter here too stops the card flashing noise before the P4
# worker lands its own POST a few seconds later.
try {
    $send = @()
    foreach ($it in $itemList) {
        if (([string]$it.kind) -eq 'question') { $send += ,$it; continue }
        $rel = 'worked'
        try { if ($it.PSObject.Properties['relevance'] -and $it.relevance) { $rel = [string]$it.relevance } } catch {}
        if ($rel -ne 'mentioned') { $send += ,$it }
    }
    $payload['items'] = @($send)
    $log.items_sent = @($send).Count
} catch { Add-Err 'filter' $_ }

try {
    $body = $payload | ConvertTo-Json -Depth 8 -Compress
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromMilliseconds(1000)
    $content = New-Object System.Net.Http.StringContent($body, [System.Text.Encoding]::UTF8, 'application/json')
    $t = $client.PostAsync($Endpoint, $content)
    if ($t.Wait(1200)) { $log.post_status = [int]$t.Result.StatusCode } else { $log.post_status = 'timeout' }
    $client.Dispose()
} catch {
    $m = $_.Exception.Message
    if ($m -match '(?i)refused|actively refused|No connection') { $log.post_status = 'refused' }
    else { $log.post_status = 'error'; Add-Err 'post' $_ }
}

# --- 9. P4: spawn the Haiku worker, do not wait -----------------------------
# The Stop hook has a 5 s budget and the model call costs seconds, so the
# worker is a detached hidden pwsh. It re-reads the file, revises `relevance`,
# writes `summary` / question rows and POSTs again on its own.
if ($event -eq 'Stop') {
    try {
        $worker = Join-Path $PSScriptRoot 'i47-haiku.ps1'
        if (Test-Path -LiteralPath $worker) {
            $ps = 'pwsh'
            try { $p0 = (Get-Process -Id $PID).Path; if ($p0 -and $p0 -match '(?i)pwsh(\.exe)?$') { $ps = $p0 } } catch {}
            # ProcessStartInfo.ArgumentList, not Start-Process -ArgumentList:
            # the latter joins on spaces and splits "C:\Users\Fernando Bevilacqua\..."
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = $ps
            foreach ($a in @('-NoProfile', '-NonInteractive', '-File', $worker, '-Session', $sessionId)) { [void]$psi.ArgumentList.Add($a) }
            if ($transcript) { [void]$psi.ArgumentList.Add('-Transcript'); [void]$psi.ArgumentList.Add($transcript) }
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow  = $true
            $psi.WorkingDirectory = $env:TEMP
            $child = [System.Diagnostics.Process]::Start($psi)
            $child.Dispose()   # detach: never wait, the hook has a 5 s budget
            $log.haiku = 'spawned'
        } else { $log.haiku = 'missing' }
    } catch { $log.haiku = 'error'; Add-Err 'haiku-spawn' $_ }
}

Write-Log

} catch {
    Add-Err 'fatal' $_
    Write-Log
}

exit 0
