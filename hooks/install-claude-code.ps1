<#
Installs the i47 attention-panel producer chain into Claude Code.

What it does, idempotently:
  1. copies hooks\*.ps1 into <ClaudeDir>\hooks\ (overwrites; creates the dir)
  2. creates <ClaudeDir>\attention\ (the session files, caches and logs)
  3. merges the `Stop` (matcher *) and `PostToolUse` (matcher Bash) hook entries
     for i47-attention.ps1 into <ClaudeDir>\settings.json and into every
     -ExtraClaudeDirs settings.json that exists, without touching any other
     hook and without duplicating an entry it already installed
  4. registers the `cto-i47-enrich` scheduled task (every minute) unless -SkipTask

Nothing here starts the daemon or restarts a Claude session; see the checklist
it prints, and docs\references\claude-code-integration.md.

Usage:
  pwsh -NoProfile -File hooks\install-claude-code.ps1
  pwsh -NoProfile -File hooks\install-claude-code.ps1 -SkipTask -WhatIf
  pwsh -NoProfile -File hooks\install-claude-code.ps1 -ClaudeDir D:\tmp\.claude -ExtraClaudeDirs @()
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    # The config dir whose hooks\ directory becomes the installed copy, and
    # whose settings.json is the primary one to patch.
    [string]$ClaudeDir = (Join-Path $env:USERPROFILE '.claude'),

    # Extra config dirs (CLAUDE_CONFIG_DIR alternates) whose settings.json must
    # carry the same hook entries. Non-existent dirs are skipped, not created.
    [string[]]$ExtraClaudeDirs = @((Join-Path $env:USERPROFILE '.claude-max')),

    # The speak.exe daemon's speech port. The panel listener is DaemonPort + 1.
    [int]$DaemonPort = 8123,

    [switch]$SkipTask
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$TaskName  = 'cto-i47-enrich'
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$SrcDir    = $PSScriptRoot
$did       = New-Object System.Collections.ArrayList
$todo      = New-Object System.Collections.ArrayList
function Note([string]$m) { [void]$did.Add($m) }
function Todo([string]$m) { [void]$todo.Add($m) }

# --- 1. hook scripts --------------------------------------------------------

$HooksDst = Join-Path $ClaudeDir 'hooks'
if (-not (Test-Path -LiteralPath $HooksDst)) {
    if ($PSCmdlet.ShouldProcess($HooksDst, 'create directory')) {
        New-Item -ItemType Directory -Force -Path $HooksDst | Out-Null
    }
    Note "created $HooksDst"
}

$scripts = @(Get-ChildItem -LiteralPath $SrcDir -Filter '*.ps1' -File |
             Where-Object { $_.Name -ne 'install-claude-code.ps1' })
if ($scripts.Count -eq 0) { throw "no hook scripts next to $SrcDir" }

$panelPort = $DaemonPort + 1
foreach ($s in $scripts) {
    $dst = Join-Path $HooksDst $s.Name
    if ($PSCmdlet.ShouldProcess($dst, 'copy hook script')) {
        Copy-Item -LiteralPath $s.FullName -Destination $dst -Force
        # the scripts POST to the panel listener, which is the daemon port + 1
        if ($panelPort -ne 8124) {
            $raw = [System.IO.File]::ReadAllText($dst)
            $new = $raw.Replace('http://127.0.0.1:8124/panel', "http://127.0.0.1:$panelPort/panel")
            if ($new -ne $raw) { [System.IO.File]::WriteAllText($dst, $new, $Utf8NoBom) }
        }
    }
}
Note ("copied {0} hook script(s) to {1}" -f $scripts.Count, $HooksDst)
if ($panelPort -ne 8124) { Note "rewrote the POST endpoint to port $panelPort" }

$Producer = Join-Path $HooksDst 'i47-attention.ps1'
$Enricher = Join-Path $HooksDst 'i47-enrich.ps1'

# --- 2. state directory -----------------------------------------------------

# The scripts themselves always resolve $env:USERPROFILE\.claude\attention, so
# that is the directory that has to exist; -ClaudeDir only decides where the
# copies and the settings live.
$AttentionDirs = @((Join-Path $ClaudeDir 'attention'))
$RealAttention = Join-Path (Join-Path $env:USERPROFILE '.claude') 'attention'
if ($AttentionDirs -notcontains $RealAttention) { $AttentionDirs += $RealAttention }
foreach ($a in $AttentionDirs) {
    if (Test-Path -LiteralPath $a) { Note "state dir already there: $a"; continue }
    if ($PSCmdlet.ShouldProcess($a, 'create state directory')) {
        New-Item -ItemType Directory -Force -Path $a | Out-Null
    }
    Note "created $a"
}

# --- 3. settings.json -------------------------------------------------------

$HookCommand = 'pwsh -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $Producer
$Wanted = @(
    @{ Event = 'Stop';        Matcher = '*'    },
    @{ Event = 'PostToolUse'; Matcher = 'Bash' }
)

function Get-Prop($obj, [string]$name) {
    if ($obj -and $obj.PSObject.Properties[$name]) { return $obj.PSObject.Properties[$name].Value }
    return $null
}
function Set-Prop($obj, [string]$name, $value) {
    if ($obj.PSObject.Properties[$name]) { $obj.PSObject.Properties[$name].Value = $value }
    else { $obj | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}

# Returns $true when it changed $settings.
function Merge-HookEntry($settings, [string]$event, [string]$matcher, [string]$command) {
    $hooks = Get-Prop $settings 'hooks'
    if (-not $hooks) { $hooks = [pscustomobject]@{}; Set-Prop $settings 'hooks' $hooks }

    $entry = [pscustomobject][ordered]@{
        type    = 'command'
        command = $command
        timeout = 10
    }

    $list = @(Get-Prop $hooks $event)
    $list = @($list | Where-Object { $_ })
    if ($list.Count -eq 0) {
        Set-Prop $hooks $event @([pscustomobject][ordered]@{ matcher = $matcher; hooks = @($entry) })
        return $true
    }

    # an entry for this matcher already exists: add our command to its list
    foreach ($g in $list) {
        if ([string](Get-Prop $g 'matcher') -ne $matcher) { continue }
        $inner = @(Get-Prop $g 'hooks')
        $inner = @($inner | Where-Object { $_ })
        foreach ($h in $inner) {
            # same script, however it is spelled: that is the duplicate test
            if ([string](Get-Prop $h 'command') -match '(?i)i47-attention\.ps1') { return $false }
        }
        Set-Prop $g 'hooks' @($inner + $entry)
        return $true
    }

    # matcher not present: a new group, other groups untouched
    Set-Prop $hooks $event @($list + [pscustomobject][ordered]@{ matcher = $matcher; hooks = @($entry) })
    return $true
}

$targets = @($ClaudeDir) + @($ExtraClaudeDirs | Where-Object { $_ })
$targets = @($targets | Select-Object -Unique)
$stamp   = (Get-Date).ToString('yyyyMMdd-HHmmss')

foreach ($dir in $targets) {
    if (-not (Test-Path -LiteralPath $dir)) { Note "skipped (no such config dir): $dir"; continue }
    $file = Join-Path $dir 'settings.json'

    $settings = $null
    $existed  = Test-Path -LiteralPath $file
    if ($existed) {
        $raw = [System.IO.File]::ReadAllText($file)
        if ($raw.Trim()) {
            try { $settings = $raw | ConvertFrom-Json }
            catch { throw "settings.json at $file is not valid JSON; fix or move it first" }
        }
    }
    if (-not $settings) { $settings = [pscustomobject]@{} }
    if ($settings -isnot [pscustomobject]) { throw "settings.json at $file is not a JSON object" }

    $changed = $false
    foreach ($w in $Wanted) {
        if (Merge-HookEntry $settings $w.Event $w.Matcher $HookCommand) {
            $changed = $true
            Note ("{0}: added the {1} ({2}) entry" -f $file, $w.Event, $w.Matcher)
        }
    }

    if (-not $changed) { Note "${file}: already registered, left untouched"; continue }

    # -EscapeHandling Default keeps quotes and & as themselves; ConvertTo-Json
    # in pwsh 7 indents with two spaces, which is the file's own style.
    $json = ($settings | ConvertTo-Json -Depth 20 -EscapeHandling Default)
    if (-not $PSCmdlet.ShouldProcess($file, 'merge i47 hook entries')) { continue }
    if ($existed) {
        $bak = "$file.bak-i47-$stamp"
        Copy-Item -LiteralPath $file -Destination $bak -Force
        Note "backed up to $bak"
    }
    [System.IO.File]::WriteAllText($file, $json, $Utf8NoBom)
}

# --- 4. scheduled task ------------------------------------------------------

if ($SkipTask) {
    Note "skipped the scheduled task (-SkipTask)"
    Todo "register the enricher yourself, or re-run without -SkipTask"
} else {
    $pwshExe = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
    if (-not $pwshExe) { throw 'pwsh is not on PATH; PowerShell 7 is required' }
    $action  = New-ScheduledTaskAction -Execute $pwshExe `
                 -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}"' -f $Enricher)
    # -Once + -RepetitionInterval with no duration = repeat forever
    $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).Date `
                 -RepetitionInterval (New-TimeSpan -Minutes 1)
    $set     = New-ScheduledTaskSettingsSet -Hidden -StartWhenAvailable `
                 -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                 -MultipleInstances IgnoreNew `
                 -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    $prin    = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" `
                 -LogonType Interactive -RunLevel Limited
    if ($PSCmdlet.ShouldProcess($TaskName, 'register scheduled task')) {
        # -Force rewrites an existing definition, so re-running is safe
        Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
            -Settings $set -Principal $prin -Force | Out-Null
    }
    Note "registered the scheduled task $TaskName (every minute, hidden, 2 min limit)"
}

# --- 5. checklist -----------------------------------------------------------

Todo "start the daemon detached, e.g. Start-Process -FilePath <repo>\speak.exe -ArgumentList '--serve','--port',$DaemonPort -WindowStyle Hidden"
Todo "open (or restart) a Claude Code session: hooks are read at session start"
Todo "verify: curl http://127.0.0.1:$panelPort/panels, and tail $RealAttention\producer.log"

Write-Host ''
Write-Host 'i47 -> Claude Code: done' -ForegroundColor Green
foreach ($m in $did) { Write-Host "  [x] $m" }
Write-Host 'still manual:' -ForegroundColor Yellow
foreach ($m in $todo) { Write-Host "  [ ] $m" }
Write-Host ''
