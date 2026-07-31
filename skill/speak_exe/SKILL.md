---
name: speak_exe
description: "Low-latency text-to-speech via speak.exe, a self-contained native binary (Pocket TTS, no Python). Use when the user wants to speak text aloud, generate speech audio, convert text to voice, or play spoken output. Shows a pulsing orb on screen while speaking."
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
