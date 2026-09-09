# Installing speak.exe from a release

You do not need Visual Studio, CMake or Python to run `speak.exe` — only to
build it. A release ships the compiled binary; this page is the path from a
downloaded zip to a warm daemon speaking in ~100 ms.

Two things are not in the zip:

- **the models** (~200 MB) — third-party weights, fetched by `get-models.ps1`
- **ffmpeg** — only needed by `make-voice.ps1`, if you build your own voice

Requirements: Windows 10/11 x64, PowerShell 7 (`pwsh`) or Windows PowerShell,
and ~250 MB of disk.

## 1. Download

Grab `speak-win-x64.zip` from the
[latest release](https://github.com/Dovyski/speaker/releases/latest).

```powershell
$dest = "$env:LOCALAPPDATA\speak"
$zip  = "$env:TEMP\speak-win-x64.zip"

Invoke-WebRequest -Uri 'https://github.com/Dovyski/speaker/releases/latest/download/speak-win-x64.zip' -OutFile $zip
Expand-Archive $zip -DestinationPath $dest -Force
cd $dest
```

`%LOCALAPPDATA%\speak` is a suggestion, not a requirement — nothing in the
release is path-dependent, so unzip wherever you like. Pick somewhere permanent
though: the models land next to the binary, and you do not want to download
them twice.

### Verifying the download

Each release also carries `SHA256SUMS.txt`. Compare it against the zip before
unzipping anything you care about:

```powershell
Invoke-WebRequest -Uri 'https://github.com/Dovyski/speaker/releases/latest/download/SHA256SUMS.txt' -OutFile "$env:TEMP\SHA256SUMS.txt"

$expected = (Get-Content "$env:TEMP\SHA256SUMS.txt").Split()[0]
$actual   = (Get-FileHash "$env:TEMP\speak-win-x64.zip" -Algorithm SHA256).Hash.ToLower()

if ($actual -eq $expected) { "ok" } else { "MISMATCH: $actual" }
```

### The unsigned-binary warning

`speak.exe` is not code-signed, so the first run trips SmartScreen: *"Windows
protected your PC"*, or a warning that the publisher is unknown. Unblocking it
is a per-file property, and the zip's mark-of-the-web propagates to everything
extracted from it:

```powershell
Get-ChildItem $dest -Recurse | Unblock-File
```

Signing needs a certificate this project does not have. If that is not
acceptable in your environment, [build from source](../../README.md#build) —
the binary you compile locally is not marked at all.

## 2. Get the models

```powershell
pwsh -File get-models.ps1
```

Downloads six ONNX files (~200 MB) into `models/` from
[KevinAHM/pocket-tts-onnx](https://huggingface.co/KevinAHM/pocket-tts-onnx).
The script is resumable in the sense that it skips files it already has, so a
dropped connection just means running it again.

They are kept out of the release deliberately: they are someone else's weights
under their own terms, they dwarf the 1 MB binary, and they change on a
different schedule than this project does.

## 3. First run

```powershell
.\speak.exe "Hello world."
```

This first call is slow — around 5 seconds — because no daemon is up yet, so it
loads the model in-process. It also starts a daemon in the background on the way
out, so the *next* call is fast without you doing anything.

The zip includes the default voice, `voices\jarvis.wav`. To use your own, drop a
5–30 s clip of speech in `voices\` and pass `--voice mine.wav`; see
[Get a voice](../../README.md#get-a-voice) for how to join several takes with
`make-voice.ps1` (that one needs ffmpeg on your PATH).

## 4. Start the daemon

The daemon holds the model in memory, which is what gets a line of speech down
to ~100 ms. Start it detached so it does not die with your shell:

```powershell
Start-Process -FilePath "$env:LOCALAPPDATA\speak\speak.exe" -ArgumentList '--serve' -WindowStyle Hidden
```

Check on it, and stop it, with:

```powershell
.\speak.exe --status     # "daemon ready" / "daemon starting up" / "no daemon"
.\speak.exe --stop
```

Do not launch `--serve` from a shell job (`speak.exe --serve &`) or from an
agent's background task: `--serve` never returns, so the launcher keeps it
attached and it dies when that job is cleaned up.

### Starting it at logon

Two ways, both without admin rights. A **scheduled task** is the more reliable
one — it survives sign-out and can be queried:

```powershell
$exe = "$env:LOCALAPPDATA\speak\speak.exe"

$action    = New-ScheduledTaskAction -Execute $exe -Argument '--serve' -WorkingDirectory (Split-Path $exe)
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit 0
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive

Register-ScheduledTask -TaskName 'speak-daemon' -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force
```

`-ExecutionTimeLimit 0` matters: the default three-day limit would otherwise
kill a daemon that has been up too long. `-LogonType Interactive` matters too —
the daemon draws the orb on screen, so it has to run in your desktop session,
not as a service.

Manage it with:

```powershell
Start-ScheduledTask   -TaskName 'speak-daemon'
Get-ScheduledTaskInfo -TaskName 'speak-daemon'
Unregister-ScheduledTask -TaskName 'speak-daemon' -Confirm:$false
```

Or, if you would rather keep it simple, a **Startup-folder shortcut**:

```powershell
$exe = "$env:LOCALAPPDATA\speak\speak.exe"
$lnk = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\speak-daemon.lnk"

$s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
$s.TargetPath       = $exe
$s.Arguments        = '--serve'
$s.WorkingDirectory = Split-Path $exe
$s.WindowStyle      = 7            # minimized
$s.Save()
```

Honestly, you can also skip both: any `speak.exe "text"` call spawns a detached
daemon when none is listening, so the only thing autostart buys you is that the
first line after a reboot is fast too.

## 5. Claude Code hooks and the agent skill

The zip carries both, ready to install:

```
hooks\                        the attention-panel producer chain
  install-claude-code.ps1     installs the hooks and patches settings.json
  i47-attention.ps1           …and the rest of the chain
skill\speak\SKILL.md          the skill that teaches an agent to drive speak.exe
```

Install the hooks with:

```powershell
pwsh -NoProfile -File hooks\install-claude-code.ps1
```

Make the skill visible to Claude Code by linking it (a junction needs no admin
rights), so it tracks the release you unzipped:

```powershell
New-Item -ItemType Junction `
  -Path   "$env:USERPROFILE\.claude\skills\speak" `
  -Target "$env:LOCALAPPDATA\speak\skill\speak"
```

What the hooks do, what they write, and how the attention panel is wired is in
[claude-code-integration.md](claude-code-integration.md).

## 6. Upgrading

The models live in `models\` and voices in `voices\`, and neither is in the zip
except for the default voice — so an upgrade is: stop the daemon, overwrite the
binary, start it again. Nothing to re-download.

```powershell
$dest = "$env:LOCALAPPDATA\speak"

& "$dest\speak.exe" --stop                      # the daemon holds speak.exe open

Invoke-WebRequest -Uri 'https://github.com/Dovyski/speaker/releases/latest/download/speak-win-x64.zip' -OutFile "$env:TEMP\speak-win-x64.zip"
Expand-Archive "$env:TEMP\speak-win-x64.zip" -DestinationPath $dest -Force
Get-ChildItem $dest -Recurse | Unblock-File

Start-Process -FilePath "$dest\speak.exe" -ArgumentList '--serve' -WindowStyle Hidden
& "$dest\speak.exe" --version
```

`--stop` first is not optional: a running daemon has `speak.exe` and
`onnxruntime.dll` locked, and `Expand-Archive -Force` will fail on them.

`-Force` overwrites `voices\jarvis.wav` with the release's copy and re-copies the
hooks and skill, but leaves `models\`, `voices\.cache\` and any voice of your own
alone. If you replaced `jarvis.wav` with your own recording under that name,
back it up first.

If you installed the Claude Code hooks, re-run the installer after upgrading —
it copies the hook scripts into `~\.claude\hooks\`, so the ones in `hooks\` are
the source, not the installed copy:

```powershell
pwsh -NoProfile -File "$dest\hooks\install-claude-code.ps1"
```

## Uninstalling

There is no installer and nothing in the registry, so:

```powershell
& "$env:LOCALAPPDATA\speak\speak.exe" --stop
Unregister-ScheduledTask -TaskName 'speak-daemon' -Confirm:$false -ErrorAction SilentlyContinue
Remove-Item "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\speak-daemon.lnk" -ErrorAction SilentlyContinue
Remove-Item "$env:USERPROFILE\.claude\skills\speak" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:LOCALAPPDATA\speak" -Recurse -Force
```

The Claude Code hooks are the exception — `install-claude-code.ps1` edits
`~\.claude\settings.json`, so remove the `speak` entries there by hand.

## Troubleshooting

| symptom | cause |
|---|---|
| `--status` says "no daemon" right after `--serve` | the daemon takes a few seconds to load the model; it reports "daemon starting up" meanwhile |
| the daemon exits immediately | another one already holds the port — one instance per port, guarded by a named mutex |
| "could not load model" | `get-models.ps1` was not run, or was run somewhere other than next to the binary |
| a voice takes seconds on first use | conditioning cost, cached afterwards in `voices\.cache\`; a 30 s sample costs more than a 5 s one |
| the orb never appears | the daemon is running outside your desktop session — check that the scheduled task uses `-LogonType Interactive` |

Building from source instead is documented in the
[README](../../README.md#build).
