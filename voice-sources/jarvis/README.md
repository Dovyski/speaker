# jarvis — voice sources and what was learned

Source clips for the `jarvis` voice, kept here so voice experiments start from a
fixed base instead of whatever happens to be in a Downloads folder.

```
jarvis-voice-message.mp3      6.48 s   clean
jarvis-clean-few-seconds.mp3  3.79 s   clean
jarvis-low-battery.mp3        3.89 s   clean
jarvis-as-you-wish-sir.mp3    0.88 s   clean
jarvis-intro-1.mp3           22.71 s   background noise — see below
jarvis-restaurant.wav        25.38 s   synthesized clip, 2026-08-15 — the chosen voice (I)
```

Durations are after trimming. Rebuild with:

```powershell
pwsh -File voice-sources/jarvis/build.ps1              # the chosen voice
pwsh -File voice-sources/jarvis/build.ps1 -All         # every candidate below
pwsh -File voice-sources/jarvis/build.ps1 -All -Render # ...and render a sample of each
```

## What was tried

Each candidate was auditioned on the same lines and judged by ear:

| | sources | length | verdict |
|---|---|---|---|
| A | 2 clean clips | 5.0 s | closest of the first four, but thin |
| B | intro, raw | 22.7 s | lots of artifacts |
| C | intro, denoised | 22.7 s | no better |
| D | intro + clean clips, denoised | 27.9 s | no |
| E | 3 clean clips | 9.05 s | good — best before the new clip arrived |
| F | clean clips first, then denoised intro | 30.0 s | "not good at all" |
| G | 4 clean clips | 15.78 s | former choice — beat E by a small margin |
| H | `voice-message` alone | 6.48 s | very good on its own |
| **I** | **`restaurant` alone** | **25.38 s** | **chosen — beats G, J and K by ear** |
| J | `restaurant` first, then G's clips (capped) | 30.0 s | no — merging diluted it |
| K | G's clips first, then `restaurant` (capped) | 30.0 s | no — merging diluted it |

Candidates I–K added the new `jarvis-restaurant.wav` clip (a synthesized take,
2026-08-15). I isolates it; J and K merge it with G's clips in each order,
since the 30 s cap truncates whatever comes last. I won the audition, so one
long clean clip beat every merge of it with the older, shorter material.

## Conclusions

- **A noisy clip poisons the sample no matter how it is dressed up.** The intro
  was tried raw (B), denoised (C), mixed in (D), and placed after clean anchors so
  conditioning would meet clean audio first (F). Every variant carried audible
  artifacts. The FFT denoiser reduces steady hiss but cannot undo music or
  overlapping sound, and the model imitates what is left.
- **Length helps, but only within clean material.** A (5 s) → E (9 s) → G (15.8 s)
  improved at every step, while every clean-only candidate beat every
  intro-derived one regardless of length. Quality gates, length refines.
- **H is why G works.** H isolated the newest clip on its own and sounded very
  good, which confirms it is the same speaker as the rest — so adding it length-
  ened the sample without blending in a second voice. Worth doing for any new
  clip: audition it alone before merging it, or a mismatch will quietly degrade
  the merged voice in a way that looks like a noise problem but is not.
- **Longer is not additive across takes.** The A→E→G trend suggested more clean
  material always helps, but I (one 25.4 s clip) beat both merges of it with G's
  clips (J, K). A single long, consistent take beats stitched short ones — the
  gaps and delivery changes between takes apparently cost more than the extra
  seconds gain. Next upgrade: an even longer *single* clean take, not more
  short clips.
- Cheap thing to try before hunting more audio: `--temperature` (default 0.7).
  Lower is flatter and more measured, higher is livelier. It changes delivery
  rather than timbre, so it can fix "right voice, wrong performance".

An earlier note claimed D won on length over cleanliness. That came from a
two-line audition; the five-line comparison reversed it.
