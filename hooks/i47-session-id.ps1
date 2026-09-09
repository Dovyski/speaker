# i47 session-id announcer: Claude Code `SessionStart` hook that tells the model
# its own session id, so it can point and speak with `speak.exe --session <id>`
# instead of guessing a window title.
#
# SessionStart is the one hook whose stdout is injected into the model's
# context, so the single line below is the whole delivery mechanism.
#
# Also stamps the id into ~/.claude/attention/<session_id>.json as `session`
# when the producer has already created that file (it never creates it).
#
# Always exits 0, under 200 ms, and prints nothing but that one line.

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

# The P4 worker runs a nested `claude -p`; that run must stay silent.
if ($env:I47_NESTED) { exit 0 }

try {
    $sessionId = $null
    if ([Console]::IsInputRedirected) {
        $raw = [Console]::In.ReadToEnd()
        if ($raw) {
            try { $sessionId = [string](([string]$raw | ConvertFrom-Json).session_id) }
            catch {
                # a payload we cannot parse still carries the id in plain sight
                $m = [regex]::Match([string]$raw, '"session_id"\s*:\s*"([^"]+)"')
                if ($m.Success) { $sessionId = $m.Groups[1].Value }
            }
        }
    }
    if (-not $sessionId) { exit 0 }

    # the line carries an em dash; do not let the console mangle it
    try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch {}
    Write-Output ("Speak session id: {0} — pass it as --session to speak.exe when speaking or pointing." -f $sessionId)

    # Best effort: keep the attention file's `session` key in sync. The producer
    # owns that file; if it is not there yet, there is nothing to do.
    try {
        $file = Join-Path $env:USERPROFILE ('.claude\attention\{0}.json' -f $sessionId)
        if (Test-Path -LiteralPath $file) {
            $state = Get-Content -LiteralPath $file -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($state -and [string]$state.session -ne $sessionId) {
                if ($state.PSObject.Properties['session']) { $state.session = $sessionId }
                else { $state | Add-Member -NotePropertyName session -NotePropertyValue $sessionId }
                $json = ($state | ConvertTo-Json -Depth 20 -EscapeHandling Default)
                [System.IO.File]::WriteAllText($file, $json, (New-Object System.Text.UTF8Encoding($false)))
            }
        }
    } catch {}
} catch {}

exit 0
