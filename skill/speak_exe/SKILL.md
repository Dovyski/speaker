---
name: speak_exe
description: "Low-latency text-to-speech via speak.exe, a self-contained native binary (Pocket TTS, no Python). Use when the user wants to speak text aloud, generate speech audio, convert text to voice, or play spoken output. Shows a pulsing orb on screen while speaking, and can point at a window on screen with expanding rings so the user knows which terminal spoke."
---

# speak_exe — Text-to-Speech via speak.exe

Speaks text out loud using a single self-contained native binary. No Python, no
`ffplay` pipeline. A glowing orb appears in the bottom-right corner of the screen
while the audio plays.

Source: `C:\Dev\www\claude-speak` (published at https://github.com/Dovyski/speaker).
This skill lives in that repo under `skill/speak_exe/`; every path below assumes
the checkout is at `C:\Dev\www\claude-speak` — adjust them if it is cloned
elsewhere.

## Speaking

```bash
"C:/Dev/www/claude-speak/speak.exe" "The text to speak."
```

That is the whole thing: it synthesizes, plays through the default audio device,
and animates the orb. The call is synchronous — it returns when playback
finishes, so do not background it if you want to know it completed.

The user can **click the orb to pause** playback and click it again to resume, so
a call may take longer than the audio lasts (it prints `speak: paused` /
`speak: resumed` on stderr). A paused utterance keeps the process alive until it
is resumed, so a call that is killed by a command timeout may simply have been
held — not a failure to investigate.

Model and voice paths resolve relative to the executable, so the working
directory does not matter.

## Latency: keep the daemon warm

`speak.exe` holds no state between calls, so a cold call loads the model
(~5.7 s to first audio). A resident daemon holds the model in memory and drops
that to **~100 ms**.

```bash
# is one running?
"C:/Dev/www/claude-speak/speak.exe" --status     # "daemon ready" / "daemon starting up" / "no daemon"
```

You normally do not need to do anything: a cold call automatically starts a
detached daemon in the background for next time, and speaks the current
utterance in-process meanwhile. But **if you are about to speak several times, or
know you will speak later in the session, start the daemon early** so the first
utterance is fast too:

Start it **detached**, so it outlives the session:

```powershell
Start-Process -FilePath C:\Dev\www\claude-speak\speak.exe -ArgumentList '--serve' -WindowStyle Hidden
```

It is ready ~6 s later; poll `--status` if you need to know when.

**Do not start it as a tracked background command** (`speak.exe --serve &` from
Bash, or `run_in_background`). `--serve` never returns — it *is* the server — so
it stays attached to that task and gets killed when the task is reaped, silently
taking the fast path with it. Use `Start-Process` as above, or just let a cold
call spawn one (those are `DETACHED_PROCESS` and are not affected).

`--stop` shuts it down. One daemon per port; extra `--serve` calls exit harmlessly.

The daemon keeps itself warm by nudging itself every 60 s — without that, the
first call after a few idle minutes takes ~500 ms instead of ~100 ms. Pass
`--no-keepalive` at `--serve` time if you would rather it went idle.

The daemon does not survive a reboot — after one, the first call is slow and
warms a new daemon automatically.

## Pointing at the window you spoke from

Speaking tells the user something happened; it does not tell them **which
terminal** to come back to. To show them, point at the window — rings expand out
of it and fade, three times, then vanish. The overlay is click-through and does
not raise or focus anything.

```bash
# speak, then point at the window whose title contains this text
"C:/Dev/www/claude-speak/speak.exe" --title "reviewer worker" "Tests are green."

# point without speaking
"C:/Dev/www/claude-speak/speak.exe" --point --title "reviewer worker"
```

No model is loaded for pointing, so it costs ~240 ms whether a daemon is running
or not, and `--pulses <n>` (default 3) sets how many rings.

**Work out the target with `--list-targets` first.** It prints every pointable
window as JSON — `hwnd`, `pid`, `process`, `title`, `x`, `y`, `w`, `h`:

```bash
"C:/Dev/www/claude-speak/speak.exe" --list-targets
```

Pick the window whose title matches the session you are running in — terminals
hosting an agent are titled after the task, so your own conversation's subject is
usually right there — then pass that as `--title <substring>` (case-insensitive)
or the exact `--hwnd <n>`. `--at <x,y>` points at a bare screen position.

**Do not rely on the no-target form.** `--point` with no target tries to work out
the calling window from the process tree, and Windows Terminal serves every window
from one process, so it usually cannot tell them apart:

```console
speak: windowsterminal.exe (pid 22788) owns 6 windows — pass --title to say which
```

That is **exit 3**, with the candidate list printed as JSON on stdout — so a
failed `--point` is a usable answer: read the candidates and retry with `--title`.
When the call both speaks and points, a pointing failure never fails the call:
the utterance already happened, so the exit code stays that of the speech and the
reason goes to stderr as `speak: spoke, but could not point (...)`.

Nothing distinguishes *tabs* — pointing is window-level. If several sessions share
one window, the rings say "this window", not "this tab".

### Over HTTP

The daemon serves the same thing on **the next port** (`8124`), loopback only, for
callers that are not running a shell:

```bash
curl -s -X POST http://127.0.0.1:8124/point -d '{"title":"reviewer worker"}'   # {"ok":true}
curl -s http://127.0.0.1:8124/targets                                          # same as --list-targets
```

`POST /point` takes `title`, or `hwnd`, or `x` and `y`, plus optional `pulses` and
`size`; a request **must** name its target, since the daemon cannot tell where the
call came from. It replies when the animation ends (~1.8 s), and concurrent
requests queue.

## Rules

- **One call per utterance.** Gather everything you want to say into a single
  string and make one call. On a cold call especially, splitting a summary into
  several calls pays the model load several times.
- **Quote the text as one argument.** Bare words are joined with spaces, but
  quoting keeps punctuation intact.
- **Write out symbols and abbreviations** the way they should be read: "T T S"
  rather than "TTS", "point" rather than ".", "eight seven six five" for a port
  number. The model reads text literally.
- Accents and non-ASCII work (`"Olá, tudo bem?"`) — arguments are read as UTF-8.
- Add `--timing` when you care about latency; it prints the milliseconds to first
  audio and whether the daemon or the local path served it.

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `--voice <name\|path>` | `jarvis.wav` | Voice sample; bare names resolve inside `voices/` |
| `--save <file.wav>` | — | Also write the audio to a WAV |
| `--no-orb` | — | Speak without the on-screen orb |
| `--orb-style <s>` | `aurora` | `aurora` (glowing ring) or `dot` (solid core) |
| `--orb-size <px>` | `220` | Square size of the overlay |
| `--timing` | — | Report ms to first audio and which path served it |
| `--point` | — | Point at a window; with text, speaks first and points after |
| `--title <substr>` | — | Point at the window whose title contains this (implies `--point`) |
| `--hwnd <n>` / `--at <x,y>` | — | Point at a window handle / screen position (imply `--point`) |
| `--pulses <n>` | `3` | Rings to send out |
| `--point-size <px>` | `320` | Size of the pointer overlay |
| `--list-targets` | — | Print pointable windows as JSON and exit |
| `--serve` / `--status` / `--stop` | — | Manage the resident daemon |
| `--port <n>` | `8123` | Daemon port |
| `--local` | — | Ignore the daemon, synthesize in-process |
| `--temperature <f>` | `0.7` | Sampling temperature |
| `--eos-extra <n>` | `4` | Extra frames after end-of-speech; raise if the last word sounds clipped |
| `--threads <n>` | `0` | Thread budget (0 = half the cores) |

Examples:

```bash
# speak and keep a copy of the audio
"C:/Dev/www/claude-speak/speak.exe" --save C:/tmp/answer.wav "Here is the summary."

# no overlay, and report the latency
"C:/Dev/www/claude-speak/speak.exe" --no-orb --timing "Quietly, without the orb."
```

## If it fails

| Symptom | Cause / fix |
|---|---|
| `could not load models from ...` | `models/` is missing — run `pwsh -File C:\Dev\www\claude-speak\get-models.ps1` |
| `could not start synthesis` | The voice sample is missing — needs a WAV in `C:\Dev\www\claude-speak\voices\` |
| `no audio output device` / `audio init failed` | No default render device available in this session |
| Every call is slow | No daemon; check `--status`, start `--serve` |
| Last word sounds clipped | Raise `--eos-extra` (default 4); the daemon takes it at `--serve` time, not per call |
| `owns N windows — pass --title` (exit 3) | Expected for Windows Terminal; read the JSON candidates that were printed and retry with `--title` |
| `no window title contains '...'` | The window closed or retitled — re-run `--list-targets` |
| `speak.exe` does not exist | Rebuild: `cmd /c C:\Dev\www\claude-speak\build.bat` (needs VS 2022 Build Tools, C++ workload) |
| Build fails with `LNK1104: cannot open file speak.exe` | The daemon is holding the binary — `speak.exe --stop` first |

## Notes

- The orb is an always-on-top layered window; only the ring itself takes clicks
  (to pause), everything around it is click-through. It is invisible to
  GDI screen capture (`CopyFromScreen`, `BitBlt`, even with `CAPTUREBLT`) — to
  screenshot it, use DXGI desktop duplication, e.g.
  `ffmpeg -f lavfi -i ddagrab=0:framerate=10 -vf hwdownload,format=bgra -frames:v 1 shot.png`.
  Use `--dump-orb <file.bmp>` to render a single frame without any capture.
- A voice is just an audio sample (zero-shot cloning). Drop a 5–30 s clean WAV in
  `voices/` and pass `--voice <file>`. To build one from several short takes:
  `pwsh -File C:\Dev\www\claude-speak\make-voice.ps1 -Out voices/mine.wav a.wav b.m4a`
  (the engine conditions on a single file, so clips must be joined first).
- This is unrelated to the `speech` skill, which drives the separate Python
  `pocket-tts-server`. Either can be used; this one has lower latency and no
  Python dependency.
