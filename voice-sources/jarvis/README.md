# jarvis — voice sources and what was learned

Source clips for the `jarvis` voice, kept here so voice experiments start from a
fixed base instead of whatever happens to be in a Downloads folder.

```
as-you-wish-sir-jarvis.mp3    0.88 s   clean
clean-few-seconds.mp3         3.79 s   clean
jarvis_low_battery.mp3        3.89 s   clean
jarvis-intro-1.mp3           22.71 s   background noise — see below
```

Durations are after trimming. Rebuild with:

```powershell
pwsh -File voice-sources/jarvis/build.ps1              # the chosen voice
pwsh -File voice-sources/jarvis/build.ps1 -All         # every candidate below
pwsh -File voice-sources/jarvis/build.ps1 -All -Render # ...and render a sample of each
```

## What was tried

Six candidates, each auditioned on the same five lines and judged by ear:

| | sources | length | verdict |
|---|---|---|---|
| A | 2 clean clips | 5.0 s | closest of the first four, but thin |
| B | intro, raw | 22.7 s | lots of artifacts |
| C | intro, denoised | 22.7 s | no better |
| D | intro + clean clips, denoised | 27.9 s | no |
| **E** | **3 clean clips** | **9.05 s** | **chosen** |
| F | clean clips first, then denoised intro | 30.0 s | "not good at all" |

## Conclusions

- **A noisy clip poisons the sample no matter how it is dressed up.** The intro
  was tried raw (B), denoised (C), mixed in (D), and placed after clean anchors so
  conditioning would meet clean audio first (F). Every variant carried audible
  artifacts. The FFT denoiser reduces steady hiss but cannot undo music or
  overlapping sound, and the model imitates what is left.
- **Length only helps within clean material.** E beat A purely by having 9 s
  instead of 5 s of the same kind of audio. But every clean-only candidate beat
  every intro-derived one regardless of length, so length never outranks quality.
- **Therefore: grow the clean pile.** A few more short, dry clips would take E
  toward 15–20 s of clean material, which is the axis that has actually worked.
- Cheap thing to try before hunting more audio: `--temperature` (default 0.7).
  Lower is flatter and more measured, higher is livelier. It changes delivery
  rather than timbre, so it can fix "right voice, wrong performance".

An earlier note claimed D won on length over cleanliness. That was from a
two-line audition; the full five-line comparison reversed it.
