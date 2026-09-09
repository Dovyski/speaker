<#
i47 P2 enricher: polls GitHub for the status of the PRs and issues that the P1
producer recorded in ~/.claude/attention/<session_id>.json, writes `status`
(and `title` when empty) back into each session file, reconciles items the
producer could only guess as issues but that are really pull requests, and
re-POSTs the changed files to the speak.exe daemon (POST /panel).

Never writes to stdout except with -Verbose, always exits 0.
Log: ~/.claude/attention/enricher.log (one JSON line per run).
Cache: ~/.claude/attention/status-cache.json (per item, keyed repo#kind#number).

Usage:
  pwsh -NoProfile -File i47-enrich.ps1                # one pass over every session file
  pwsh -NoProfile -File i47-enrich.ps1 -Session <id>  # only that session file
#>
[CmdletBinding()]
param(
    [switch]$Once,
    [string]$Session
)

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

$Dir       = Join-Path $env:USERPROFILE '.claude\attention'
$LogFile   = Join-Path $Dir 'enricher.log'
$CacheFile = Join-Path $Dir 'status-cache.json'
$Endpoint  = 'http://127.0.0.1:8124/panel'

$MaxGhCalls     = 40
$MaxSessionAgeH = 48
$TtlOpenSec     = 60
$TtlClosedSec   = 1800
$MaxMisses      = 3
$Throttle       = 4
$GhTimeoutMs    = 15000

$sw     = [System.Diagnostics.Stopwatch]::StartNew()
$errors = New-Object System.Collections.ArrayList
function Add-Err([string]$where, $e) {
    try { [void]$errors.Add(("{0}: {1}" -f $where, (($e | Out-String).Trim() -replace '\s+', ' '))) } catch {}
}

$log = [ordered]@{
    ts = (Get-Date).ToString('o'); sessions = 0; items = 0
    gh_calls = 0; changed = 0; ms = $null; errors = @()
}
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

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Read-JsonString([string]$raw) {
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) {
        return ($raw | ConvertFrom-Json -DateKind String)
    }
    return ($raw | ConvertFrom-Json)
}
function Read-JsonFile([string]$path) {
    $raw = [System.IO.File]::ReadAllText($path)
    if (-not $raw.Trim()) { return $null }
    # -DateKind String: never turn last_seen/updated_at into [datetime] on the
    # round-trip, so the producer's exact timestamp text survives the rewrite
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) {
        return ($raw | ConvertFrom-Json -DateKind String)
    }
    return ($raw | ConvertFrom-Json)
}
function Write-JsonFileAtomic([string]$path, $obj) {
    $json = $obj | ConvertTo-Json -Depth 12
    $tmp  = "$path.enrich.tmp"
    [System.IO.File]::WriteAllText($tmp, $json, $Utf8NoBom)
    Move-Item -LiteralPath $tmp -Destination $path -Force
}
function Get-Mtime([string]$path) {
    try { return (Get-Item -LiteralPath $path -ErrorAction Stop).LastWriteTimeUtc.Ticks } catch { return 0 }
}
function Parse-Utc($v) {
    if (-not $v) { return $null }
    try { return ([datetime]$v).ToUniversalTime() } catch { return $null }
}
function Now-Utc { (Get-Date).ToUniversalTime() }
function Stamp { (Now-Utc).ToString('yyyy-MM-ddTHH:mm:ssZ') }

try {

if (-not (Test-Path $Dir)) { Write-Log; exit 0 }

# --- 1. session files -------------------------------------------------------
$skipNames = @('status-cache.json')
$files = @()
if ($Session) {
    $p = Join-Path $Dir ("{0}.json" -f $Session)
    if (Test-Path -LiteralPath $p) { $files = @((Get-Item -LiteralPath $p)) }
} else {
    $files = @(Get-ChildItem -LiteralPath $Dir -Filter '*.json' -File -ErrorAction SilentlyContinue |
               Where-Object { $skipNames -notcontains $_.Name })
}

$cut      = (Now-Utc).AddHours(-$MaxSessionAgeH)
$sessions = New-Object System.Collections.ArrayList   # @{ path; obj }
$index    = @{}                                       # key -> @{ repo; kind; number; last_seen }

function Add-ToIndex([string]$repo, [string]$kind, [int]$num, $lastSeen) {
    $key = '{0}#{1}#{2}' -f $repo, $kind, $num
    if ($index.ContainsKey($key)) {
        if ($lastSeen -and ((-not $index[$key].last_seen) -or $lastSeen -gt $index[$key].last_seen)) { $index[$key].last_seen = $lastSeen }
    } else {
        $index[$key] = @{ repo = $repo; kind = $kind; number = $num; last_seen = $lastSeen }
    }
    return $key
}

foreach ($f in $files) {
    $obj = $null
    try { $obj = Read-JsonFile $f.FullName } catch { Add-Err ('read:' + $f.Name) $_; continue }
    if (-not $obj) { continue }
    # a session file carries a `session` property; anything else is not ours
    if (-not $obj.PSObject.Properties['session']) { continue }
    $upd = Parse-Utc $obj.updated_at
    if ($upd -and $upd -lt $cut) { continue }
    [void]$sessions.Add(@{ path = $f.FullName; obj = $obj })

    foreach ($it in @($obj.items)) {
        if (-not $it) { continue }
        $kind = [string]$it.kind
        if ($kind -ne 'pr' -and $kind -ne 'issue') { continue }
        $repo = [string]$it.repo
        $num  = 0
        try { $num = [int]$it.number } catch {}
        if (-not $repo -or $num -le 0) { continue }
        [void](Add-ToIndex $repo $kind $num (Parse-Utc $it.last_seen))
    }
}
$log.sessions = $sessions.Count
$log.items    = $index.Count
if ($index.Count -eq 0) { Write-Log; exit 0 }

# --- 2. cache ---------------------------------------------------------------
$cache = @{}
try {
    if (Test-Path -LiteralPath $CacheFile) {
        $c = Read-JsonFile $CacheFile
        if ($c) {
            foreach ($p in $c.PSObject.Properties) {
                $e = @{}
                foreach ($q in $p.Value.PSObject.Properties) { $e[$q.Name] = $q.Value }
                $cache[$p.Name] = $e
            }
        }
    }
} catch { Add-Err 'cache-load' $_ }

function Test-NeedsFetch([string]$key) {
    $e = $cache[$key]
    if (-not $e) { return $true }
    $misses = 0; try { $misses = [int]$e['_missing'] } catch {}
    if ($misses -ge $MaxMisses) { return $false }
    $when = Parse-Utc $e['fetched_at']
    if (-not $when) { return $true }
    $st  = [string]$e['status']
    $ttl = if ($st -eq 'merged' -or $st -eq 'closed') { $TtlClosedSec } else { $TtlOpenSec }
    return (((Now-Utc) - $when).TotalSeconds -ge $ttl)
}

# --- 3. gh fetch (small parallelism, hard per-call timeout) -----------------
$ghExePath = $null
try { $ghExePath = (Get-Command gh -ErrorAction Stop).Source } catch { Add-Err 'gh-missing' $_ }
$ghBudget = $MaxGhCalls

function Invoke-GhFetch([string[]]$keys) {
    if (-not $ghExePath) { return @() }
    $keys = @($keys | Where-Object { $_ } | Select-Object -Unique)
    if ($keys.Count -eq 0) { return @() }
    if ($keys.Count -gt $script:ghBudget) { $keys = @($keys | Select-Object -First $script:ghBudget) }
    if ($keys.Count -eq 0) { return @() }
    $script:ghBudget -= $keys.Count
    $jobs = foreach ($k in $keys) {
        [pscustomobject]@{ key = $k; repo = $index[$k].repo; kind = $index[$k].kind; number = $index[$k].number }
    }
    return @($jobs | ForEach-Object -ThrottleLimit $Throttle -Parallel {
        $ghExe   = $using:ghExePath
        $timeout = $using:GhTimeoutMs
        $j = $_
        $fields = if ($j.kind -eq 'pr') { 'state,isDraft,reviewDecision,statusCheckRollup,mergedAt,title,url' } else { 'state,title,url' }
        $sub    = if ($j.kind -eq 'pr') { 'pr' } else { 'issue' }
        $out = ''; $err = ''; $code = -1
        try {
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = $ghExe
            foreach ($a in @($sub, 'view', [string]$j.number, '--repo', $j.repo, '--json', $fields)) { $psi.ArgumentList.Add($a) }
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError  = $true
            $psi.UseShellExecute        = $false
            $psi.CreateNoWindow         = $true
            $p  = [System.Diagnostics.Process]::Start($psi)
            $so = $p.StandardOutput.ReadToEndAsync()
            $se = $p.StandardError.ReadToEndAsync()
            if (-not $p.WaitForExit($timeout)) {
                try { $p.Kill($true) } catch {}
                $err = 'timeout'
            } else {
                $code = $p.ExitCode
                if ($so.Wait(2000)) { $out = $so.Result }
                if ($se.Wait(2000)) { $err = $se.Result }
            }
            $p.Dispose()
        } catch { $err = $_.Exception.Message }
        [pscustomobject]@{ key = $j.key; kind = $j.kind; code = $code; out = $out; err = $err }
    })
}

# --- 4. status mapping ------------------------------------------------------
function Get-PrStatus($d) {
    $state = ([string]$d.state).ToUpper()
    if ($d.mergedAt -or $state -eq 'MERGED') { return 'merged' }
    if ($state -eq 'CLOSED') { return 'closed' }
    if ($d.isDraft -eq $true) { return 'draft' }
    $bad = @('FAILURE', 'ERROR', 'CANCELLED', 'TIMED_OUT')
    foreach ($c in @($d.statusCheckRollup)) {
        if (-not $c) { continue }
        $v = ''
        if ($c.PSObject.Properties['conclusion']) { $v = ([string]$c.conclusion).ToUpper() }
        if ((-not $v) -and $c.PSObject.Properties['state']) { $v = ([string]$c.state).ToUpper() }
        if ($bad -contains $v) { return 'checks_failing' }
    }
    $rd = ([string]$d.reviewDecision).ToUpper()
    if ($rd -eq 'CHANGES_REQUESTED') { return 'changes_requested' }
    if ($rd -eq 'APPROVED') { return 'approved' }
    return 'open'
}

# returns the issue keys discovered to really be pull requests
function Update-CacheFromResults($results) {
    $prFound = New-Object System.Collections.ArrayList
    foreach ($r in $results) {
        $e = $cache[$r.key]
        if (-not $e) { $e = @{}; $cache[$r.key] = $e }
        $data = $null
        if ($r.code -eq 0 -and $r.out) { try { $data = $r.out | ConvertFrom-Json } catch { $data = $null } }

        # `owner/repo#N` is recorded as an issue by the producer; GitHub numbers
        # issues and PRs in one sequence, so N may be a PR. `gh issue view` either
        # answers with a /pull/N url or refuses with "is a pull request".
        $isPr = $false
        if ($r.kind -eq 'issue') {
            if ($data -and ([string]$data.url) -match '/pull/\d+\s*$') { $isPr = $true }
            elseif ((-not $data) -and ($r.err -match '(?i)is a pull request|use .?gh pr')) { $isPr = $true }
            if ($isPr) { $e['is_pr'] = $true; [void]$prFound.Add($r.key) }
        }

        if ($data) {
            if ($r.kind -eq 'pr') {
                $e['status'] = Get-PrStatus $data
            } else {
                $e['status'] = if (([string]$data.state).ToUpper() -eq 'CLOSED') { 'closed' } else { 'open' }
            }
            $e['title']      = [string]$data.title
            $e['url']        = [string]$data.url
            $e['_missing']   = 0
            $e['fetched_at'] = Stamp
            if ($e.ContainsKey('last_error')) { $e.Remove('last_error') }
        } elseif ($isPr) {
            # not a miss: the number exists, it is simply the other kind
            $e['_missing']   = 0
            $e['fetched_at'] = Stamp
            if (-not $e['status']) { $e['status'] = 'unknown' }
        } else {
            $m = 0; try { $m = [int]$e['_missing'] } catch {}
            $e['_missing']   = $m + 1
            $e['fetched_at'] = Stamp
            if (-not $e['status']) { $e['status'] = 'unknown' }
            if ($null -eq $e['title']) { $e['title'] = '' }
            $t = (($r.err | Out-String).Trim() -replace '\s+', ' ')
            if ($t) { $e['last_error'] = $t.Substring(0, [Math]::Min(180, $t.Length)) }
        }
    }
    return @($prFound)
}

# pass 1: everything stale, most recently seen first
$todo = @($index.Keys | Where-Object { Test-NeedsFetch $_ } |
          Sort-Object -Property @{ Expression = { $d = $index[$_].last_seen; if ($d) { $d } else { [datetime]::MinValue } }; Descending = $true })
$calls   = 0
$results = Invoke-GhFetch $todo
$calls  += $results.Count
$prFound = Update-CacheFromResults $results

# pass 2: an issue that turned out to be a PR needs the PR status once
$prKeys = New-Object System.Collections.ArrayList
foreach ($k in @($index.Keys | Where-Object { $index[$_].kind -eq 'issue' -and $cache.ContainsKey($_) -and $cache[$_]['is_pr'] })) {
    $e = $index[$k]
    $pk = Add-ToIndex $e.repo 'pr' $e.number $e.last_seen
    if (Test-NeedsFetch $pk) { [void]$prKeys.Add($pk) }
}
if ($prKeys.Count -gt 0) {
    $r2 = Invoke-GhFetch @($prKeys)
    $calls += $r2.Count
    [void](Update-CacheFromResults $r2)
}
$log.gh_calls = $calls
$log.items    = $index.Count

# an item we know about but could not resolve still needs a publishable status
foreach ($k in @($index.Keys)) {
    if ($cache.ContainsKey($k) -and (-not $cache[$k]['status'])) { $cache[$k]['status'] = 'unknown' }
}

try {
    # drop cache entries no session references any more and not refreshed for 7 days
    $stale = (Now-Utc).AddDays(-7)
    foreach ($k in @($cache.Keys)) {
        if ($index.ContainsKey($k)) { continue }
        $w = Parse-Utc $cache[$k]['fetched_at']
        if ((-not $w) -or $w -lt $stale) { $cache.Remove($k) }
    }
    $out = [ordered]@{}
    foreach ($k in ($cache.Keys | Sort-Object)) { $out[$k] = $cache[$k] }
    Write-JsonFileAtomic $CacheFile ([pscustomobject]$out)
} catch { Add-Err 'cache-save' $_ }

# --- 5. reconcile issue→pr, dedupe -----------------------------------------
function Set-Prop($o, [string]$name, $value) {
    if ($o.PSObject.Properties[$name]) { $o.$name = $value } else { $o | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}
function Reconcile-Items($obj) {
    $changed = $false
    $keep    = New-Object System.Collections.ArrayList
    $seen    = @{}
    foreach ($it in @($obj.items)) {
        if (-not $it) { continue }
        $kind = [string]$it.kind
        $num  = 0; try { $num = [int]$it.number } catch {}
        if ($kind -eq 'issue' -and $it.repo -and $num -gt 0) {
            $ik = '{0}#issue#{1}' -f [string]$it.repo, $num
            if ($cache.ContainsKey($ik) -and $cache[$ik]['is_pr']) {
                Set-Prop $it 'kind' 'pr'
                Set-Prop $it 'url' ('https://github.com/{0}/pull/{1}' -f [string]$it.repo, $num)
                $kind = 'pr'
                $changed = $true
            }
        }
        # `question` rows (P4) carry no repo/number: key them by text or they
        # would all collapse onto one row here
        $mk = switch ($kind) {
            'path'     { 'path|' + ([string]$it.url).ToLower() }
            'question' { 'question|' + (([string]$it.title).ToLower() -replace '[^a-z0-9]+', ' ').Trim() }
            default    { '{0}|{1}|{2}' -f $kind, [string]$it.repo, $num }
        }
        if ($seen.ContainsKey($mk)) {
            $tgt = $seen[$mk]
            $a = Parse-Utc $tgt.last_seen; $b = Parse-Utc $it.last_seen
            if ($b -and ((-not $a) -or $b -gt $a)) { Set-Prop $tgt 'last_seen' ([string]$it.last_seen) }
            $fa = Parse-Utc $tgt.first_seen; $fb = Parse-Utc $it.first_seen
            if (-not $fa) { $fa = $a }
            if (-not $fb) { $fb = $b }
            if ($fb -and ((-not $fa) -or $fb -lt $fa)) {
                if ($it.PSObject.Properties['first_seen'] -or $tgt.PSObject.Properties['first_seen']) {
                    Set-Prop $tgt 'first_seen' ($fb.ToString('yyyy-MM-ddTHH:mm:ssZ'))
                }
            }
            if ((-not [string]$tgt.title) -and [string]$it.title) { Set-Prop $tgt 'title' ([string]$it.title) }
            if ((-not [string]$tgt.url) -and [string]$it.url)     { Set-Prop $tgt 'url'   ([string]$it.url) }
            $changed = $true
        } else {
            $seen[$mk] = $it
            [void]$keep.Add($it)
        }
    }
    if ($changed) { $obj.items = @($keep.ToArray()) }
    return $changed
}

function Apply-ToObject($obj) {
    $changed = Reconcile-Items $obj
    foreach ($it in @($obj.items)) {
        if (-not $it) { continue }
        $kind = [string]$it.kind
        if ($kind -ne 'pr' -and $kind -ne 'issue') { continue }
        $num = 0; try { $num = [int]$it.number } catch {}
        if ((-not $it.repo) -or $num -le 0) { continue }
        $key = '{0}#{1}#{2}' -f [string]$it.repo, $kind, $num
        $e = $cache[$key]
        if (-not $e -or (-not $e['status'])) { continue }
        $st = [string]$e['status']
        if (-not $it.PSObject.Properties['status']) {
            $it | Add-Member -NotePropertyName status -NotePropertyValue $st
            $changed = $true
        } elseif ([string]$it.status -ne $st) {
            $it.status = $st
            $changed = $true
        }
        $ct = [string]$e['title']
        if ($ct) {
            if (-not $it.PSObject.Properties['title']) {
                $it | Add-Member -NotePropertyName title -NotePropertyValue $ct
                $changed = $true
            } elseif (-not [string]$it.title) {
                $it.title = $ct
                $changed = $true
            }
        }
    }
    return $changed
}

$changedFiles = New-Object System.Collections.ArrayList
foreach ($s in $sessions) {
    try {
        $path = $s.path
        $obj  = $s.obj
        $mt   = Get-Mtime $path
        $chg  = Apply-ToObject $obj
        if (-not $chg) { continue }
        if ((Get-Mtime $path) -ne $mt) {
            # the producer wrote while we were fetching: re-read and re-apply once
            $obj = Read-JsonFile $path
            if (-not $obj) { continue }
            $chg = Apply-ToObject $obj
            if (-not $chg) { continue }
        }
        Write-JsonFileAtomic $path $obj
        [void]$changedFiles.Add($obj)
    } catch { Add-Err 'writeback' $_ }
}
$log.changed = $changedFiles.Count

foreach ($obj in $changedFiles) {
    try {
        $post = Read-JsonString ($obj | ConvertTo-Json -Depth 12)
        if ($post.PSObject.Properties['_cursor']) { $post.PSObject.Properties.Remove('_cursor') }
        if ($post.PSObject.Properties['_haiku_cursor']) { $post.PSObject.Properties.Remove('_haiku_cursor') }
        # same filter the producer and the P4 worker apply: `mentioned` rows stay
        # in the file for audit but never reach the panel
        $post.items = @(@($post.items) | Where-Object {
            $_ -and (([string]$_.kind) -eq 'question' -or
                     (-not $_.PSObject.Properties['relevance']) -or
                     (-not $_.relevance) -or ([string]$_.relevance) -ne 'mentioned')
        })
        $body    = $post | ConvertTo-Json -Depth 12 -Compress
        $client  = New-Object System.Net.Http.HttpClient
        $client.Timeout = [TimeSpan]::FromMilliseconds(1000)
        $content = New-Object System.Net.Http.StringContent($body, [System.Text.Encoding]::UTF8, 'application/json')
        $t = $client.PostAsync($Endpoint, $content)
        [void]$t.Wait(1200)
        $client.Dispose()
    } catch {}   # daemon absent / 404 is expected while P3 lands
}

Write-Log

} catch {
    Add-Err 'fatal' $_
    Write-Log
}

exit 0
