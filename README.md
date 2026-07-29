# speaker

A single, self-contained Windows binary that speaks text out loud and shows a
glowing orb on screen while it talks.

<p align="center">
  <img src="docs/orb.png" width="180" alt="the orb, pulsing with the voice">
</p>

No Python. No server to start. No `ffplay` to pipe into. One `speak.exe` (plus
`onnxruntime.dll`) that loads the [Pocket TTS](https://github.com/kyutai-labs/pocket-tts)
model, renders audio straight to your speakers through WASAPI, and pulses a
click-through overlay in the corner of the screen in time with the voice.

```
speak.exe --serve             &:: once: resident daemon holding the model
speak.exe "Hello world."      &:: ~80 ms to first audio
```

## Why

Local TTS setups usually end up as a Python server plus a shell pipeline into an
audio player. That means a Python install, a virtualenv, a process to keep alive
and a port to remember — a lot of moving parts to say one sentence. This is the
same thing as one native binary you can copy anywhere.

It also gives you something to *look at*: when a machine starts talking, a
small visual cue tells you where the sound is coming from and that it is still
going. The orb fades in with the first sample, pulses with the actual amplitude
of what the speakers are playing right now, and fades out when the audio drains.

## Features

- **One binary** — inference, playback, daemon and UI in a single `speak.exe`
- **~80 ms to first audio** with the resident daemon (vs ~5.7 s loading per call)
- **No Python at build time or run time** — pre-exported ONNX weights, fetched by a script
- **Voice cloning** — any short WAV/MP3/FLAC sample becomes the voice
- **Audio-reactive orb** — per-pixel-alpha layered window, click-through, always on top, parked above the taskbar
- **Streaming** — audio starts playing while the rest of the sentence is still being generated
- **Optional WAV output** — `--save out.wav` alongside (or instead of) playback
- **UTF-8 / accents** — arguments are read as wide chars, so `"Olá, tudo bem?"` works

## Requirements

- Windows 10/11 x64
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with the
  **Desktop development with C++** workload (ships the CMake and Ninja used by `build.bat`)
- ~200 MB of disk for the models

## Build

```bat
build.bat
```

That is the whole setup. CMake fetches ONNX Runtime 1.23.2 (prebuilt),
SentencePiece and dr_libs, downloads the pinned `pocket_tts.cpp` inference
engine, and links everything into `speak.exe`. First build takes a few minutes;
later builds are seconds.

If your Build Tools live somewhere else, edit `VSBT` at the top of `build.bat`.

### Get the models

```powershell
pwsh -File get-models.ps1
```

Downloads the six INT8 ONNX files (~200 MB) into `models/` from
[KevinAHM/pocket-tts-onnx](https://huggingface.co/KevinAHM/pocket-tts-onnx).
This is what keeps Python out of the picture — the upstream project generates
these with a torch-based export script, and this repo just uses the exported
result.

### Get a voice

Voice cloning is zero-shot, so a voice is just an audio file in `voices/`:

```
voices/
└── alba.wav        ← 5–30 s of clean speech, any WAV/MP3/FLAC
```

Point at it with `--voice alba.wav` (or pass an absolute path to any file).
The default is `alba.wav`. First use of a voice costs a few hundred ms of
conditioning; after that it is cached in `voices/.cache/` and reloads in ~4 ms.

## Usage

```bat
speak.exe "Hello world."
speak.exe --voice narrator.wav "A different voice."
speak.exe --save out.wav "Speak and keep a copy."
speak.exe --no-orb "Speak with no overlay."
speak.exe --dump-orb orb.bmp          rem render one orb frame and exit
```

| Flag | Default | Description |
|------|---------|-------------|
| `--voice <name\|path>` | `alba.wav` | Voice sample; bare names resolve inside `--voices-dir` |
| `--save <file.wav>` | — | Also write the audio to a 32-bit float WAV |
| `--no-orb` | — | Skip the on-screen indicator |
| `--dump-orb <file.bmp>` | — | Render a single orb frame to a BMP and exit |
| `--timing` | — | Report milliseconds to first audio, and which path served it |
| `--serve` | — | Run as the resident daemon (see below) |
| `--status` / `--stop` | — | Inspect or stop the daemon |
| `--port <n>` | `8123` | Daemon port |
| `--local` | — | Never use the daemon; always synthesize in-process |
| `--no-auto-serve` | — | Do not start a daemon in the background on a cold call |
| `--temperature <f>` | `0.7` | Sampling temperature |
| `--threads <n>` | `0` | Thread budget (`0` = half the cores) |
| `--models-dir <dir>` | `<exe dir>\models` | ONNX model directory |
| `--voices-dir <dir>` | `<exe dir>\voices` | Voice sample directory |

Paths default relative to the executable, not the working directory, so
`speak.exe` works from anywhere once built.

## Low latency: the daemon

Loading the model costs ~5 s, and a one-shot process pays it *every* call. So
`speak.exe` can also be the resident process that holds the model, using
upstream PocketTTS.cpp's HTTP server:

```bat
speak.exe --serve            rem loads, warms up, primes the voice, then serves
speak.exe --status           rem "daemon ready" / "daemon starting up" / "no daemon"
speak.exe --stop
```

Speaking needs no extra flags — every call tries the daemon first:

```
speak.exe "text"
   │
   ├── daemon answers on 127.0.0.1:8123  →  stream PCM, play, ~80 ms  ← the fast path
   │
   └── nothing listening
         ├── start a detached `--serve` daemon in the background (for next time)
         └── synthesize this one in-process (~5.7 s), so nothing blocks on the warm-up
```

That means you never have to remember to start anything: the first call after a
reboot is slow, everything after it is fast. `--local` opts out of the daemon
entirely; `--no-auto-serve` keeps the fast path but never spawns anything.

Measured on this machine (INT8, 16 cores, no GPU) — time from launching the
command to hearing the first sample:

| | first audio |
|---|---|
| cold, in-process (`--local`) | ~5 700 ms |
| **warm, via daemon** | **~80 ms** |

Roughly 70× less latency. Three things get it that low:

- The daemon calls `warmup()` **and** synthesizes one throwaway phrase with the
  default voice at startup, so ONNX kernels and the voice's KV state are already
  hot when the first real request lands.
- The client hands partial HTTP chunks to WASAPI as soon as they arrive rather
  than waiting for a full chunk, and sets `TCP_NODELAY`.
- `onnxruntime.dll` (14 MB) is **delay-loaded**, so the client process never maps
  the inference library at all — worth ~30 ms of the budget on its own.

To have it always warm, run `speak.exe --serve` at login — e.g. drop a shortcut
to it in `shell:startup`. The daemon is a single instance per port, guarded by a
named mutex, so extra `--serve` calls exit harmlessly.

## Performance

INT8 on a 16-core desktop CPU, no GPU:

| | time |
|---|---|
| model load (per process) | ~4.6 s |
| daemon warm-up + voice priming | ~0.6 s |
| voice conditioning, cached | ~4 ms |
| generation | ~2.4× realtime |
| first audio, daemon | ~80 ms |

## How it works

```
speak.exe
├── pocket_tts.cpp  (fetched, pinned)   compiled into this binary, two ways in:
│      │                                 · ptt_* C API      → local synthesis
│      │                                 · TTSServer        → `--serve` daemon
│      ▼
├── PcmSource                           "hand me the next chunk", either from
│      │                                 LocalSource (in-process generator) or
│      │                                 DaemonSource (chunked HTTP POST /tts)
│      ▼
├── PlayStream()                        WASAPI shared mode, 24 kHz mono float
│      │                                 · writes chunks as buffer space frees
│      │                                 · records a peak envelope per 256 frames
│      │                                 · publishes the live playback position
│      ▼
└── OrbThread()                         layered window, 60 fps
                                         · reads envelope at the playing position
                                         · so the orb tracks what you *hear*,
                                           not what was just generated
```

Playback does not know or care which source it is draining, so the orb, the WAV
saving and the timing all behave identically on both paths.

Two details worth knowing:

- The orb is driven by `frames_written - GetCurrentPadding()`, i.e. the frame the
  speakers are actually on. Driving it from the generator instead would make it
  pulse ahead of the sound.
- `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` lets the audio engine resample 24 kHz
  mono float to whatever the device mix format is, so there is no resampler here.

## Credits

- [kyutai-labs/pocket-tts](https://github.com/kyutai-labs/pocket-tts) — the model (CC-BY-4.0 weights)
- [VolgaGerm/PocketTTS.cpp](https://github.com/VolgaGerm/PocketTTS.cpp) — the single-file C++ inference runtime this links against (MIT)
- [KevinAHM/pocket-tts-onnx](https://huggingface.co/KevinAHM/pocket-tts-onnx) — pre-exported ONNX weights

## License

MIT — see [LICENSE](LICENSE). The model weights carry their own license (CC-BY-4.0).
