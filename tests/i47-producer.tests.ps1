<#
Fixture tests for the i47 producer chain (i47-attention.ps1 + i47-haiku.ps1).

Each case runs the real script in a child pwsh against a throw-away USERPROFILE
(seeded session file, known-repos cache and transcript), with the panel
endpoint pointed at a closed port and, for the Haiku worker, a canned verdict
instead of the model. Nothing touches ~/.claude or the running daemon.

Usage:
  pwsh -NoProfile -File tests\i47-producer.tests.ps1
Exit code = number of failed assertions.
#>
$ErrorActionPreference = 'Stop'
$Root     = Split-Path -Parent $PSScriptRoot
$Producer = Join-Path $Root 'hooks\i47-attention.ps1'
$Haiku    = Join-Path $Root 'hooks\i47-haiku.ps1'
$Utf8     = New-Object System.Text.UTF8Encoding($false)

$script:failed = 0
$script:passed = 0
function Assert([bool]$cond, [string]$what) {
    if ($cond) { $script:passed++; Write-Host "  ok   $what" }
    else { $script:failed++; Write-Host "  FAIL $what" -ForegroundColor Red }
}

function Stamp([double]$hoursAgo) { (Get-Date).ToUniversalTime().AddHours(-$hoursAgo).ToString('yyyy-MM-ddTHH:mm:ssZ') }

function New-Sandbox([string[]]$knownRepos = @('optiwork-app', 'optiwork-mobile', 'optiwork-import')) {
    $sb = Join-Path ([System.IO.Path]::GetTempPath()) ("i47-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $att = Join-Path $sb '.claude\attention'
    New-Item -ItemType Directory -Force -Path $att | Out-Null
    # fresh v2 cache: the producer must not scan the real C:\Dev
    Set-Content -LiteralPath (Join-Path $att 'known-repos.txt') -Value (@('# i47 known-repos v2') + $knownRepos) -Encoding UTF8
    return $sb
}

function Write-Transcript([string]$sb, [object[]]$messages) {
    $path = Join-Path $sb 'transcript.jsonl'
    $lines = foreach ($m in $messages) {
        [ordered]@{
            type = $m.role; timestamp = (Stamp 0)
            message = [ordered]@{ role = $m.role; content = @([ordered]@{ type = 'text'; text = $m.text }) }
        } | ConvertTo-Json -Depth 8 -Compress
    }
    [System.IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"), $Utf8)
    return $path
}

function Write-State([string]$sb, [string]$sid, [object[]]$items) {
    $st = [ordered]@{ session = $sid; title = $null; items = @($items); updated_at = (Stamp 0); _cursor = 0 }
    [System.IO.File]::WriteAllText((Join-Path $sb ".claude\attention\$sid.json"), ($st | ConvertTo-Json -Depth 8), $Utf8)
}

function Read-State([string]$sb, [string]$sid) {
    $raw = [System.IO.File]::ReadAllText((Join-Path $sb ".claude\attention\$sid.json"))
    return ($raw | ConvertFrom-Json -DateKind String)
}

function Invoke-Child([string]$sb, [string[]]$argv, [string]$stdin, [hashtable]$extraEnv = @{}) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Get-Process -Id $PID).Path
    foreach ($a in @('-NoProfile', '-NonInteractive') + $argv) { [void]$psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables['USERPROFILE'] = $sb
    $psi.EnvironmentVariables['I47_PANEL_ENDPOINT'] = 'http://127.0.0.1:9/panel'
    [void]$psi.EnvironmentVariables.Remove('I47_NESTED')
    [void]$psi.EnvironmentVariables.Remove('DEVPILOT_PANE_ID')
    foreach ($k in $extraEnv.Keys) { $psi.EnvironmentVariables[$k] = [string]$extraEnv[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    if ($stdin) { $p.StandardInput.Write($stdin) }
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEnd(); $err = $p.StandardError.ReadToEnd()
    if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch {}; throw 'child timed out' }
    if ($err.Trim()) { Write-Host "    stderr: $($err.Trim())" -ForegroundColor DarkYellow }
    return $p.ExitCode
}

# A non-Stop, non-PostToolUse event: full extraction, no Haiku spawn.
function Invoke-Producer([string]$sb, [string]$sid, [string]$transcript) {
    $hook = [ordered]@{ session_id = $sid; hook_event_name = 'SubagentStop'; cwd = $sb; transcript_path = $transcript } | ConvertTo-Json -Compress
    return (Invoke-Child $sb @('-File', $Producer) $hook)
}

function Keys($state, [string]$kind) {
    @($state.items | Where-Object { $_.kind -eq $kind } | ForEach-Object { '{0}#{1}' -f $_.repo, $_.number })
}

$sandboxes = New-Object System.Collections.ArrayList
try {

# --- 1. per-kind caps: questions and links never evict PRs/issues ----------
Write-Host '1. per-kind caps'
$sb = New-Sandbox; [void]$sandboxes.Add($sb); $sid = 'cap'
$items = @()
for ($i = 1; $i -le 45; $i++) {   # PR i seen i hours ago: 1..40 survive
    $items += [ordered]@{ kind = 'pr'; repo = 'optidatacloud/optiwork-mobile'; number = $i; url = "https://github.com/optidatacloud/optiwork-mobile/pull/$i"; title = ''; status = 'unknown'; relevance = 'worked'; provenance = 'touched'; last_seen = (Stamp (10 + $i)) }
}
for ($i = 1; $i -le 20; $i++) {
    $items += [ordered]@{ kind = 'issue'; repo = 'optidatacloud/optiwork-import'; number = $i; url = "https://github.com/optidatacloud/optiwork-import/issues/$i"; title = ''; status = 'unknown'; relevance = 'worked'; provenance = 'touched'; last_seen = (Stamp (20 + $i)) }
}
for ($i = 1; $i -le 12; $i++) {   # all newer than every PR
    $items += [ordered]@{ kind = 'question'; url = ''; title = "Question number $i ?"; status = 'open'; last_seen = (Stamp ($i / 10)) }
}
for ($i = 1; $i -le 12; $i++) {
    $items += [ordered]@{ kind = 'link'; url = "https://example.com/page$i"; title = "example.com/page$i"; status = 'unknown'; relevance = 'worked'; last_seen = (Stamp ($i / 10)) }
}
for ($i = 1; $i -le 30; $i++) {
    $items += [ordered]@{ kind = 'path'; url = "C:\Dev\field\work\optiwork-app\$i-x"; title = "$i-x"; status = 'unknown'; relevance = 'worked'; last_seen = (Stamp ($i / 10)) }
}
Write-State $sb $sid $items
$tr = Write-Transcript $sb @(@{ role = 'user'; text = 'nothing to extract here' })
[void](Invoke-Producer $sb $sid $tr)
$st = Read-State $sb $sid
$prs = Keys $st 'pr'
Assert ($prs.Count -eq 40) "40 PRs kept (got $($prs.Count))"
Assert ($prs -contains 'optidatacloud/optiwork-mobile#1' -and $prs -contains 'optidatacloud/optiwork-mobile#40') 'the 40 most recent PRs survive'
Assert (-not ($prs -contains 'optidatacloud/optiwork-mobile#41')) 'PR #41 (oldest) evicted by the PR cap only'
Assert ((Keys $st 'issue').Count -eq 20) 'all 20 issues kept'
$qs = @($st.items | Where-Object kind -eq 'question')
Assert ($qs.Count -eq 8) "questions capped at 8 (got $($qs.Count))"
Assert (@($qs | Where-Object title -eq 'Question number 1 ?').Count -eq 1 -and @($qs | Where-Object title -eq 'Question number 9 ?').Count -eq 0) 'newest questions kept'
$links = @($st.items | Where-Object kind -eq 'link'); $paths = @($st.items | Where-Object kind -eq 'path')
Assert ($links.Count -eq 8) "links capped at 8 (got $($links.Count))"
Assert (($links.Count + $paths.Count) -eq 30) "link + path capped at 30 (got $($links.Count + $paths.Count))"

# --- 2. age prune: 7 days for pr/issue, 48 h for the rest -------------------
Write-Host '2. age prune'
$sb = New-Sandbox; [void]$sandboxes.Add($sb); $sid = 'age'
Write-State $sb $sid @(
    [ordered]@{ kind = 'pr'; repo = 'optidatacloud/optiwork-app'; number = 5; url = 'https://github.com/optidatacloud/optiwork-app/pull/5'; title = ''; status = 'unknown'; relevance = 'worked'; provenance = 'touched'; last_seen = (Stamp (24 * 5)) }
    [ordered]@{ kind = 'issue'; repo = 'optidatacloud/optiwork-app'; number = 8; url = 'https://github.com/optidatacloud/optiwork-app/issues/8'; title = ''; status = 'unknown'; relevance = 'worked'; provenance = 'touched'; last_seen = (Stamp (24 * 8)) }
    [ordered]@{ kind = 'path'; url = 'C:\Dev\field\work\optiwork-app\1-x'; title = '1-x'; status = 'unknown'; relevance = 'worked'; last_seen = (Stamp 72) }
    [ordered]@{ kind = 'question'; url = ''; title = 'Old but still checked?'; status = 'open'; last_seen = (Stamp 72); last_checked = (Stamp 1) }
)
$tr = Write-Transcript $sb @(@{ role = 'user'; text = 'nothing' })
[void](Invoke-Producer $sb $sid $tr)
$st = Read-State $sb $sid
Assert ((Keys $st 'pr') -contains 'optidatacloud/optiwork-app#5') 'PR seen 5 days ago kept'
Assert ((Keys $st 'issue').Count -eq 0) 'issue seen 8 days ago pruned'
Assert (@($st.items | Where-Object kind -eq 'path').Count -eq 0) 'path seen 72 h ago pruned'
Assert (@($st.items | Where-Object kind -eq 'question').Count -eq 1) 'question asked 72 h ago but checked 1 h ago kept'

# --- 3. scheme-less GitHub URLs ---------------------------------------------
Write-Host '3. scheme-less URLs'
$sb = New-Sandbox; [void]$sandboxes.Add($sb); $sid = 'url'
$tr = Write-Transcript $sb @(@{ role = 'user'; text = "review github.com/optidatacloud/optiwork-mobile/pull/1050 and www.github.com/optidatacloud/optiwork-import/issues/261, also https://github.com/optidatacloud/optiwork-app/pull/7 but not docs.github.com/foo/bar/pull/9 nor C:/x/github.com/a/b/pull/3" })
[void](Invoke-Producer $sb $sid $tr)
$st = Read-State $sb $sid
$prs = Keys $st 'pr'; $iss = Keys $st 'issue'
Assert ($prs -contains 'optidatacloud/optiwork-mobile#1050') 'github.com/... without scheme -> pr'
Assert ($iss -contains 'optidatacloud/optiwork-import#261') 'www.github.com/... without scheme -> issue'
Assert ($prs -contains 'optidatacloud/optiwork-app#7') 'https URL still works'
Assert (-not ($prs -contains 'foo/bar#9') -and -not ($prs -contains 'a/b#3')) 'host inside another name or path ignored'
$u = @($st.items | Where-Object { $_.kind -eq 'pr' -and $_.number -eq 1050 })[0].url
Assert ($u -eq 'https://github.com/optidatacloud/optiwork-mobile/pull/1050') "stored URL is canonical ($u)"

# --- 4. repo#N aliases ------------------------------------------------------
Write-Host '4. repo#N aliases'
$sb = New-Sandbox; [void]$sandboxes.Add($sb); $sid = 'alias'
Set-Content -LiteralPath (Join-Path $sb '.claude\attention\repo-aliases.txt') -Value @('# alias=repo', 'gw=optiwork-api-gateway') -Encoding UTF8
$tr = Write-Transcript $sb @(@{ role = 'user'; text = 'see mobile#12, app#5487, web#7, chat#3, gw#44 and optiwork-import#261' })
[void](Invoke-Producer $sb $sid $tr)
$st = Read-State $sb $sid
$all = @(Keys $st 'issue') + @(Keys $st 'pr')
Assert ($all -contains 'optidatacloud/optiwork-mobile#12') 'mobile#12 -> optiwork-mobile (built-in alias)'
Assert ($all -contains 'optidatacloud/optiwork-app#5487') 'app#5487 -> optiwork-app (optiwork- prefix of a known repo)'
Assert ($all -contains 'optidatacloud/optiwork-api-gateway#44') 'gw#44 -> repo-aliases.txt mapping'
Assert ($all -contains 'optidatacloud/optiwork-import#261') 'known repo name maps to itself'
Assert (-not ($all -match '/mobile#|/web#|/chat#|optiwork-chat#|optiwork-web#')) "unknown aliases ignored (got: $($all -join ', '))"

# --- 5. Haiku worker: question last_seen is stable ---------------------------
Write-Host '5. haiku question last_seen'
$sb = New-Sandbox; [void]$sandboxes.Add($sb); $sid = 'haiku'
$asked = Stamp 30
Write-State $sb $sid @(
    [ordered]@{ kind = 'question'; url = ''; title = 'Merge the pending pair now?'; status = 'open'; last_seen = $asked }
)
$tr = Write-Transcript $sb @(@{ role = 'assistant'; text = 'Still waiting on you: merge the pending pair now?' })
$open = @('Merge the pending pair now?') + @(1..9 | ForEach-Object { "Extra question $_ ?" })
$verdict = Join-Path $sb 'verdict.json'
[System.IO.File]::WriteAllText($verdict, ([ordered]@{ summary = 'waiting'; items = @(); questions_open = $open; questions_resolved = @() } | ConvertTo-Json -Depth 4), $Utf8)
[void](Invoke-Child $sb @('-File', $Haiku, '-Session', $sid, '-Transcript', $tr) '' @{ I47_HAIKU_VERDICT_FILE = $verdict })
$st = Read-State $sb $sid
$q = @($st.items | Where-Object { $_.kind -eq 'question' -and $_.title -eq 'Merge the pending pair now?' })[0]
Assert ($null -ne $q) 'existing question kept'
Assert ($q -and [string]$q.last_seen -eq $asked) "existing question keeps its first-seen last_seen ($($q.last_seen) vs $asked)"
Assert ($q -and $q.last_checked -and ([datetime]$q.last_checked) -gt ([datetime]$asked)) 'last_checked set to this pass'
$qs = @($st.items | Where-Object kind -eq 'question')
Assert ($qs.Count -eq 8) "haiku caps questions at 8 (got $($qs.Count))"
$new = @($qs | Where-Object title -eq 'Extra question 1 ?')[0]
Assert ($new -and $new.last_seen -and $new.last_checked) 'new question gets last_seen + last_checked'

} finally {
    foreach ($s in $sandboxes) { try { Remove-Item -LiteralPath $s -Recurse -Force -ErrorAction SilentlyContinue } catch {} }
}

Write-Host ''
Write-Host ("{0} passed, {1} failed" -f $script:passed, $script:failed)
exit $script:failed
