# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semantic versioning](https://semver.org/spec/v2.0.0.html).

The version a binary reports (`speak.exe --version`) comes from `SPEAK_VERSION`
in `version.h`, and a `v*` tag ships whatever that header says.

## [Unreleased]

### Changed

- **The speaking card carries a mini orb instead of a white halo.** While an
  utterance is aimed at a window, the card in that window's corner now shows the
  orb itself, a glyph wide, animated from the same per-frame amplitude beacon:
  the same outline churned by the voice, the same ember→white→azure ramp
  travelling round it, the same idle breath between words — the real orb math,
  scaled, rather than a second animation that also pulses. On the collapsed pill
  it takes over the leading icon's slot, so a pill that starts talking does not
  change shape; on an open card it opens the header line and the title moves over
  by a glyph, cut to what is left. The white halo around the outline is gone, and
  with it the baked halo layer. The captions and the orb now leave together at
  the end of the fade, rather than the header snapping back while the card was
  still lit. `--panel-preview` gained `<prefix>-speaking-pill.png`.

## [1.0.0] — 2026-09-09

First release with a prebuilt binary. Everything below already existed in the
repo; this is the point at which it stopped requiring Visual Studio to use.

### Speech

- **One binary.** Inference, WASAPI playback, the daemon and all the on-screen UI
  in a single `speak.exe`. No Python, no server, no ffplay. The engine is
  upstream [PocketTTS.cpp](https://github.com/VolgaGerm/PocketTTS.cpp)
  (pinned) compiled into the same translation unit, so both its `ptt_*` C API
  and its HTTP server are available from one executable.
- **The resident daemon** (`--serve`, `--status`, `--stop`). It loads the model
  once, warms the ONNX kernels and primes the default voice at startup, so a
  line of speech costs ~100 ms to first audio instead of the ~5.7 s a one-shot
  process pays. A keepalive nudge every 60 s stops an idle daemon from clocking
  down and losing that. `onnxruntime.dll` is delay-loaded, so the client path
  never maps the 14 MB inference library at all.
- **No setup step.** Every call tries the daemon first and, finding none,
  synthesizes in-process while spawning a detached daemon for next time — so the
  first line after a reboot is slow and everything after it is fast, without
  anyone remembering to start something. `--local` and `--no-auto-serve` opt out.
- **Voice cloning** from any 5–30 s WAV/MP3/FLAC sample dropped in `voices/`,
  cached after first conditioning. `make-voice.ps1` joins several takes into one
  usable sample: resamples, trims silence, loudness-normalizes so takes recorded
  at different levels do not fight, and caps the result at 30 s.
- **Streaming**, so audio starts playing while the rest of the sentence is still
  being generated, plus `--save out.wav` and UTF-8 arguments read as wide chars.

### On screen

- **The audio-reactive orb** — a per-pixel-alpha layered window, always on top,
  parked above the taskbar, pulsing with the audio it is playing. Click it to
  pause mid-sentence, click again to resume from the same word.
- **Captions** (`--caption`) — an icon, a title and one short line of context in
  a card beside the orb, in Bootstrap-style variants, so you see *what* the
  speech is about and not just that something spoke.
- **Subtitles** (`--subtitle`) — the same line bare: white text with a dark
  contour, no card.

### Pointing

- **`--point`** rings out a window so a human can find which terminal wants
  them: expanding rings leaving the window, by flag or over HTTP.
- **`--title`** picks the window by matching its title, `--list-targets` shows
  what is matchable as JSON, and a session id can be used instead when the
  title is ambiguous or absent.

### The attention panel

- **A card parked in a terminal's corner** listing the PRs, issues and work dirs
  that session is on, as a clickable list.
- **Tabs** across its groups, a **collapsed pill** for when it should stay out of
  the way, and a **glow** that pulses with the voice — plus the live caption —
  whenever speech is aimed at that window.
- **Pending**: questions waiting on a human are called out separately, so
  "something is blocked on you" is visible without reading the list.
- **A hover popover** with the detail behind a row: title, labels, assignees,
  reviewers and cached avatars.
- It **follows its window** as that window moves, and survives the window's title
  changing underneath it.

### Claude Code integration

- **`hooks/`** — the producer chain that fills the panel from a live Claude Code
  session (`Stop`, `PostToolUse` and `SessionStart` hooks).
- **`hooks/install-claude-code.ps1`** — idempotently copies the hooks into place
  and merges only its own entries into `settings.json`, leaving other hooks
  untouched.
- **The headless enricher** — the daemon runs it once a minute while a panel is
  registered, with `CREATE_NO_WINDOW`, to fetch issue and PR details and avatars.
  It replaces an earlier scheduled task that flashed a console window on screen
  every minute; the installer unregisters that task if it finds it.
- **`skill/speak/`** — the agent skill that teaches a coding agent how and when
  to drive `speak.exe`.

### Distribution

- `scripts/package-release.ps1` and a tag-triggered GitHub Actions workflow
  produce `speak-win-x64.zip` with the binary, `onnxruntime.dll`, the default
  voice, the hooks, the skill and the model fetcher, plus `SHA256SUMS.txt`.
  The models stay out of it: ~200 MB of third-party weights, fetched by
  `get-models.ps1`. See [docs/references/install.md](docs/references/install.md).
- `speak.exe --version`.

[Unreleased]: https://github.com/Dovyski/speaker/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Dovyski/speaker/releases/tag/v1.0.0
