# Speech: the engine, voices and the daemon

How `speak.exe` turns text into audio, where the voice comes from, and how the
resident daemon gets first audio down to ~100 ms.

- [Usage](#usage)
- [Voices](#voices)
- [Low latency: the daemon](#low-latency-the-daemon)
- [Performance](#performance)
- [How it works](#how-it-works)
- [Do not clip the last word](#do-not-clip-the-last-word)
- [Flags](#flags)

## Usage

```bat
speak.exe "Hello world."
speak.exe --voice narrator.wav "A different voice."
speak.exe --save out.wav "Speak and keep a copy."
speak.exe --no-orb "Speak with no overlay."
speak.exe --caption-title "o-cli #85" --caption "CI green." "The pull request is ready."
speak.exe --subtitle "Rebasing the forms PRs on dev." "Give me a minute."
```

Click the orb to pause while it is speaking, and again to resume — see
[overlay.md](overlay.md) for the on-screen side of a call.

Paths default relative to the executable, not the working directory, so
`speak.exe` works from anywhere once built. Arguments are read as wide chars, so
accents and non-ASCII work (`"Olá, tudo bem?"`).

Audio starts playing while the rest of the sentence is still being generated, and
`--save out.wav` keeps a 32-bit float WAV alongside (or instead of) playback.

## Voices

Voice cloning is zero-shot, so a voice is just an audio file in `voices/`:

```
voices/
└── jarvis.wav      ← 5–30 s of speech, any WAV/MP3/FLAC
```

Point at it with `--voice jarvis.wav` (or pass an absolute path to any file).
The default is `jarvis.wav`. First use of a voice costs conditioning time —
a few hundred ms for a short sample, several seconds for a 20 s+ one — after
which it is cached in `voices/.cache/` and reloads in ~4 ms.

**Several short recordings?** The engine conditions on one file (and uses at most
30 s of it), so join them first:

```powershell
pwsh -File make-voice.ps1 -Out voices/mine.wav take1.wav take2.m4a take3.mp3
```

That resamples each clip to 24 kHz mono, trims leading/trailing silence,
loudness-normalizes them so takes recorded at different levels do not fight each
other, joins them with a 0.25 s gap, and caps the result at 30 s. Add `-Denoise`
for sources with background hiss — an FFT denoiser, which will not remove music
or a second voice.

Aim for at least ~5 s of speech. Length matters: in a four-way blind listen, a
28 s denoised sample beat a 5 s clean one, so do not throw away a longer take
just because it is noisier — build both and compare by ear.

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

Measured on one machine (INT8, 16 cores, no GPU) — time from launching the
command to hearing the first sample:

| | first audio |
|---|---|
| cold, in-process (`--local`) | ~5 700 ms |
| **warm, via daemon** | **~100 ms** |

Roughly 55× less latency. Three things get it that low:

- The daemon calls `warmup()` **and** synthesizes one throwaway phrase with the
  default voice at startup, so ONNX kernels and the voice's KV state are already
  hot when the first real request lands.
- The client hands partial HTTP chunks to WASAPI as soon as they arrive rather
  than waiting for a full chunk, and sets `TCP_NODELAY`.
- `onnxruntime.dll` (14 MB) is **delay-loaded**, so the client process never maps
  the inference library at all — worth ~30 ms of the budget on its own.

Start the daemon detached, so it is not tied to whatever shell launched it:

```powershell
Start-Process -FilePath .\speak.exe -ArgumentList '--serve' -WindowStyle Hidden
```

`--serve` never returns — it *is* the server — so a job-controlled background
launch (`speak.exe --serve &`, or an agent's background task) keeps it attached
and it dies when that job is cleaned up. Auto-spawned daemons use
`DETACHED_PROCESS` and are immune to this.

The daemon is a single instance per port, guarded by a named mutex, so extra
`--serve` calls exit harmlessly. It also owns the pointing endpoint and the
attention panel — see [http-api.md](http-api.md) for every port and route it
serves.

### Keepalive

An *idle* daemon gets slow again — the CPU clocks down and its working set gets
paged out, so the first call after a few quiet minutes measured 350–660 ms
instead of ~100 ms. Text length is not the factor here; idleness is.

So the daemon nudges itself with a throwaway word every 60 s (`--keepalive <sec>`,
`--no-keepalive` to turn it off). The nudge goes through the daemon's own HTTP
endpoint rather than calling the engine directly, so the server serializes it
against real requests instead of racing them. Each tick costs a fraction of a
second of CPU, and shows up in the daemon's log as a normal `POST /tts`.

The daemon does not survive a reboot — after one, the first call is slow and
warms a new daemon automatically.

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

## Do not clip the last word

Two independent things clip the tail of an utterance, and both are handled here:

- **The model stops too early.** With upstream's automatic setting, generation
  ends while the final consonant is still sounding — measured over the last 50 ms
  of "…is instant": `-29.8 dB` of residual energy, i.e. audio cut mid-sound. The
  default here is `--eos-extra 4`, which brings that to `-64.3 dB`, a real decay
  into silence, for ~160 ms more audio. Raise it further if you still hear
  clipping; the daemon takes it at `--serve` time, not per call.
- **Playback stops too early.** The device used to stop the instant the last
  sample was consumed, which cuts whatever the audio engine had not pushed out
  yet. Playback now appends 250 ms of silence and drains that before stopping.

Two more details worth knowing:

- The orb is driven by `frames_written - GetCurrentPadding()`, i.e. the frame the
  speakers are actually on. Driving it from the generator instead would make it
  pulse ahead of the sound.
- `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` lets the audio engine resample 24 kHz
  mono float to whatever the device mix format is, so there is no resampler here.

## Flags

Speech, voice and daemon flags. The on-screen ones are in
[overlay.md](overlay.md), pointing in [pointing.md](pointing.md), the panel in
[attention-panel.md](attention-panel.md).

| Flag | Default | Description |
|------|---------|-------------|
| `--voice <name\|path>` | `jarvis.wav` | Voice sample; bare names resolve inside `--voices-dir` |
| `--save <file.wav>` | — | Also write the audio to a 32-bit float WAV |
| `--timing` | — | Report milliseconds to first audio, and which path served it |
| `--serve` | — | Run as the resident daemon |
| `--status` / `--stop` | — | Inspect or stop the daemon |
| `--port <n>` | `8123` | Daemon port |
| `--keepalive <sec>` | `60` | Daemon: nudge itself every N seconds so it stays fast when idle |
| `--no-keepalive` | — | Daemon: let it go cold between calls |
| `--local` | — | Never use the daemon; always synthesize in-process |
| `--no-auto-serve` | — | Do not start a daemon in the background on a cold call |
| `--temperature <f>` | `0.7` | Sampling temperature |
| `--eos-extra <n>` | `4` | Extra frames generated after end-of-speech (`-1` = upstream auto) |
| `--eos-threshold <f>` | `-4.0` | End-of-speech threshold; lower cuts off later |
| `--threads <n>` | `0` | Thread budget (`0` = half the cores) |
| `--models-dir <dir>` | `<exe dir>\models` | ONNX model directory |
| `--voices-dir <dir>` | `<exe dir>\voices` | Voice sample directory |

## If it fails

| Symptom | Cause / fix |
|---|---|
| `could not load models from ...` | `models/` is missing — run `pwsh -File get-models.ps1` (see [build.md](build.md)) |
| `could not start synthesis` | The voice sample is missing — needs a WAV in `voices/` |
| `no audio output device` / `audio init failed` | No default render device available in this session |
| Every call is slow | No daemon; check `--status`, start `--serve` |
| Last word sounds clipped | Raise `--eos-extra` (default 4) |
