# speaker

A single, self-contained Windows binary that speaks text out loud, shows a
glowing orb while it talks, points at the terminal that spoke, and parks a
clickable list of that terminal's pull requests, issues and questions in its
corner.

<p align="center">
  <img src="docs/hero-strip.png" width="880" alt="the orb going from silence to speaking and back, and the pointer's rings expanding out of a window">
</p>
<p align="center">
  <img src="docs/panel-expanded.png" width="480" alt="the attention panel: a summary header, the All/Issues/PRs/Pending tabs, two question rows and four PR and issue rows">
</p>

No Python. No server to start. No `ffplay` to pipe into. One `speak.exe` (plus
`onnxruntime.dll`) that loads the [Pocket TTS](https://github.com/kyutai-labs/pocket-tts)
model, renders audio straight to your speakers through WASAPI, and pulses a
click-through overlay in the corner of the screen in time with the voice.

```
speak.exe --serve             &:: once: resident daemon holding the model
speak.exe "Hello world."      &:: ~100 ms to first audio
```

## What it does

**Speech** — [speech.md](docs/references/speech.md)

- **One binary**: inference, playback, daemon and UI in a single `speak.exe`
- **~100 ms to first audio** with the resident daemon, against ~5.7 s loading per call
- **Voice cloning**: any short WAV/MP3/FLAC sample becomes the voice; `make-voice.ps1` joins several takes into one
- **Streaming**: audio starts playing while the rest of the sentence is still being generated
- Optional WAV output, UTF-8 arguments, and no Python at build or run time

**On screen** — [overlay.md](docs/references/overlay.md)

- **Audio-reactive orb**: a per-pixel-alpha layered window, always on top, that pulses with what the speakers are actually playing
- **Caption toast**: an icon, a title and a line of context beside the orb, so you also see *what* it is about
- **Subtitle**: `--subtitle` puts the same line there bare — white text with a dark contour, no card
- **Click the orb to pause**, click again to resume from the same word

**Pointing** — [pointing.md](docs/references/pointing.md)

- **Expanding rings** out of one window's centre: "over here", without raising or focusing it
- **By Claude session id** (`--session`) or by window title (`--title`), by flag or over HTTP

**Attention panel for Claude Code** — [attention-panel.md](docs/references/attention-panel.md)

- A **per-terminal card** listing what the session in that window is working on, with each row marked by GitHub's own icon and status colour
- **Clickable rows**: pull requests and issues open in the browser, work directories in Explorer
- **Tabs** — `All`, `Issues`, `PRs`, `Pending` — six rows by default, `+N more` to expand everything, and a **one-line pill** when collapsed
- **Pending questions**: what an agent is waiting on you for, sorted to the top; click to copy
- **Glows with the voice** when speech is aimed at its window, carrying the caption in its header
- **Hover popover** with the full title, labels, assignees, reviewers and their verdicts, and the check counts
- Filled by the included Claude Code hooks, with an optional Haiku relevance pass and a headless GitHub status enricher the daemon runs

## Quick start

```powershell
# 1. the binary
Invoke-WebRequest https://github.com/Dovyski/speaker/releases/latest/download/speak-win-x64.zip -OutFile speak.zip
Expand-Archive speak.zip -DestinationPath speaker; cd speaker

# 2. the model weights (~200 MB, into models/)
pwsh -File get-models.ps1

# 3. say something
.\speak.exe "Hello world."
```

The first call is slow (~5.7 s) and starts a daemon in the background, so every
call after it is ~100 ms. Full instructions, including what to put on `PATH`, are
in [install.md](docs/references/install.md); to build from source instead, see
[build.md](docs/references/build.md).

## Install into Claude Code

The panel needs something to tell it what a session is working on. For Claude
Code that is `hooks/`: a `Stop` and a `PostToolUse` hook that mine the
transcript, a `SessionStart` hook that hands the model its session id, an
optional Haiku pass that prunes noise and collects open questions, and a status
enricher the daemon runs while any panel is registered.

```powershell
pwsh -NoProfile -File hooks\install-claude-code.ps1
```

[claude-code-integration.md](docs/references/claude-code-integration.md) explains
the chain end to end, the file formats, the operations and how to debug it.
[`skill/`](skill/README.md) is the agent skill that teaches a coding agent to
drive the binary — when to speak, when to point, and how to phrase what it says.

## Documentation

| | |
|---|---|
| [install.md](docs/references/install.md) | Getting a release build onto a machine |
| [build.md](docs/references/build.md) | `build.bat`, the models, dev notes |
| [speech.md](docs/references/speech.md) | Engine, voices, the daemon, speech flags |
| [overlay.md](docs/references/overlay.md) | Orb, pause, captions, subtitles |
| [pointing.md](docs/references/pointing.md) | Rings, targets, colours, the DPI trap |
| [attention-panel.md](docs/references/attention-panel.md) | The card, its tabs, its popover, the `/panel` contract |
| [claude-code-integration.md](docs/references/claude-code-integration.md) | The hooks that fill the panel |
| [http-api.md](docs/references/http-api.md) | Every port and route the daemon serves |

## Requirements

- Windows 10/11 x64
- ~200 MB of disk for the model weights
- To build from source: [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/)
  with the **Desktop development with C++** workload
- For the attention panel: [Claude Code](https://claude.com/claude-code), PowerShell 7
  (`pwsh`) and the [GitHub CLI](https://cli.github.com/) (`gh`) authenticated

## Credits

- [kyutai-labs/pocket-tts](https://github.com/kyutai-labs/pocket-tts) — the model (CC-BY-4.0 weights)
- [VolgaGerm/PocketTTS.cpp](https://github.com/VolgaGerm/PocketTTS.cpp) — the single-file C++ inference runtime this links against (MIT)
- [KevinAHM/pocket-tts-onnx](https://huggingface.co/KevinAHM/pocket-tts-onnx) — pre-exported ONNX weights
- [primer/octicons](https://github.com/primer/octicons) — the panel's row icons (MIT)

## License

MIT — see [LICENSE](LICENSE). The model weights carry their own license (CC-BY-4.0).
