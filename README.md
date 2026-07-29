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
speak.exe "Hello world."
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

- **One binary** — inference, playback and UI in a single `speak.exe`
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
| `--temperature <f>` | `0.7` | Sampling temperature |
| `--threads <n>` | `0` | Thread budget (`0` = half the cores) |
| `--models-dir <dir>` | `<exe dir>\models` | ONNX model directory |
| `--voices-dir <dir>` | `<exe dir>\voices` | Voice sample directory |

Paths default relative to the executable, not the working directory, so
`speak.exe` works from anywhere once built.

## Performance

INT8 on a 16-core desktop CPU, no GPU:

| | time |
|---|---|
| model load (per process) | ~4.7 s |
| voice conditioning, cached | ~4 ms |
| generation | ~2.4× realtime |
| time to first audio | ~30 ms after generation starts |

The per-process model load dominates short utterances. If you need
back-to-back, instant speech, keep a resident process instead — upstream
PocketTTS.cpp has an HTTP server mode for exactly that.

## How it works

```
speak.exe
├── pocket_tts.cpp  (fetched, pinned)   ONNX Runtime inference, streaming chunks
│      │  ptt_stream_start / ptt_stream_read
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
