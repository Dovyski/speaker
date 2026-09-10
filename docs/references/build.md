# Building from source

For running a release build instead, see [install.md](install.md).

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

The ONNX weights are pre-exported and fetched by a script, so the toolchain
above is everything the build and the binary need.

## Get the models

```powershell
pwsh -File get-models.ps1
```

Downloads the six INT8 ONNX files (~200 MB) into `models/` from
[KevinAHM/pocket-tts-onnx](https://huggingface.co/KevinAHM/pocket-tts-onnx).
This is what keeps Python out of the picture — the upstream project generates
these with a torch-based export script, and this repo just uses the exported
result.

`--models-dir <dir>` points elsewhere; the default is `models/` beside the
executable.

## Get a voice

A voice is just an audio sample — see [speech.md](speech.md#voices) for the
`voices/` directory, `--voice`, and `make-voice.ps1` for joining several takes.

## What ships next to the binary

| | |
|---|---|
| `speak.exe` | inference, playback, daemon and UI |
| `onnxruntime.dll` | 14 MB, **delay-loaded** — a client process never maps it, which is worth ~30 ms of the cold-start budget |
| `models/` | the six INT8 ONNX files, from `get-models.ps1` |
| `voices/` | voice samples; `voices/.cache/` holds conditioned state |
| `hooks/` | the Claude Code producer and the status enricher the daemon runs ([claude-code-integration.md](claude-code-integration.md)) |

## Dev notes

- **Stop the daemon before rebuilding.** A build that fails with
  `LNK1104: cannot open file speak.exe` is a resident daemon holding the binary:
  `speak.exe --stop` first.
- **The overlays cannot be screenshotted with GDI.** Layered windows are invisible
  to `CopyFromScreen` / `BitBlt`, so the previews render the same pixels straight
  to a file instead: `--dump-orb`, `--orb-preview`, `--point-preview` and
  `--panel-preview` (see
  [attention-panel.md](attention-panel.md#seeing-it-without-a-daemon) — every
  panel image in these docs was made that way). DXGI desktop duplication is the
  alternative if you need a real capture:
  `ffmpeg -f lavfi -i ddagrab=0:framerate=10 -vf hwdownload,format=bgra -frames:v 1 shot.png`.
- **`--panel-demo --title <window>`** parks a sample panel in any window for 20 s
  with no daemon and no producer, which is how the card's binding, movement and
  clicks get checked.
- The upstream engine is downloaded at a pinned SHA and its HTTP routing lives in
  that file, which is why `/point` and `/panel` are a separate listener rather
  than extra routes ([http-api.md](http-api.md)).

## Credits

- [kyutai-labs/pocket-tts](https://github.com/kyutai-labs/pocket-tts) — the model (CC-BY-4.0 weights)
- [VolgaGerm/PocketTTS.cpp](https://github.com/VolgaGerm/PocketTTS.cpp) — the single-file C++ inference runtime this links against (MIT)
- [KevinAHM/pocket-tts-onnx](https://huggingface.co/KevinAHM/pocket-tts-onnx) — pre-exported ONNX weights
- [primer/octicons](https://github.com/primer/octicons) — the panel's row icons (MIT)
