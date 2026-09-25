# ultron — voice sources and candidates

Source clips for an `ultron` voice (James Spader, *Avengers: Age of Ultron*).
No candidate is installed as `voices/ultron.wav` yet — audition first.

```
meteor.wav          23.83 s   44.1 kHz stereo   music bed under the voice
vision-dialog.wav   17.96 s   44.1 kHz stereo   clean, quiet, one speaker
```

Rebuild with:

```powershell
pwsh -File voice-sources/ultron/build.ps1              # A and B
pwsh -File voice-sources/ultron/build.ps1 -All         # A-J
pwsh -File voice-sources/ultron/build.ps1 -All -Render # ...and render samples/ultron-<X>.wav
```

`-Render` plays each line aloud while saving it (`speak.exe --local --save`
has no mute); it does not touch a running daemon.

## Source analysis

**meteor.wav** — "I don't have anyone else. I think a lot about meteors ...
seen hope, seen mercy." One speaker throughout, but a score/ambience bed runs
under the whole clip: `silencedetect` at -40 dB finds no pause at all, the
side channel sits ~19 dB under the mid (a real stereo mix), and the
spectrogram shows steady tonal lines around 17.5–22 s. The bed lives mostly
below ~150 Hz — high-passed at 150 Hz, pauses fall to about -40 dB RMS against
-20 dB speech. Lossy source (hard cut at ~15.5 kHz). Leading 0.62 s is music
before the first word. make-voice's -50 dB trim removes nothing (23.83 s).

**vision-dialog.wav** — Ultron's half of the forest scene with Vision: "You
were supposed to be the last. Stark asked for a savior, and settled for a
slave. I suppose we are. They're doomed. You're unbearably naive." Vision's
lines were already cut out of the source; every remaining line pitches at
110–120 Hz median, so no second speaker was found and no cleaned intermediate
was needed. Near-mono (side -72 dB), silent gaps (-65 dB), quiet (-40 LUFS,
peak -24 dBFS — loudnorm lifts it). One non-speech sound: a short chuckle at
8.06–8.97 s. Pauses at -40 dB / 0.4 s:

```
1.50-2.86  4.35-5.60  7.04-8.17  8.85-10.51  11.28-12.96  14.10-15.54  15.91-16.40  17.42-end
```

make-voice trims it to 17.41 s.

## Chosen voice

**I** = `voices/ultron.wav` (audition 2026-09-25). Ranking by ear: I > E > F; H/J close to E; A/B usable alone; C rejected (sharp cuts at the 30s truncation), D rejected (collapsed pauses), G unimpressive. E kept Ultron's timbre but carried the meteor bed and a footstep-like burst; F's 12 dB denoise was clean but flattened the timbre; I (6 dB) is the middle ground. Untried: nr between 6 and 12, `--temperature`.

## Candidates

| | sources | length | notes |
|---|---|---|---|
| A | meteor alone | 23.83 s | music bed included |
| B | vision-dialog alone | 17.41 s | cleanest material, incl. chuckle |
| C | meteor + vision-dialog | 30.00 s | **truncated** at the cap: 41.5 s joined, keeps vision only up to "Stark asked for a savior" |
| D | both, pauses collapsed | 29.51 s | nothing truncated; every word of both clips kept |
| E | vision-dialog + meteor head | 27.34 s | vision first (17.41 s), 0.25 s gap, meteor 0.616–10.296 s (9.68 s) with 40 ms fades |
| F | vision-dialog + de-bedded meteor head | 27.30 s | as E, meteor pre-filtered `highpass=f=150,afftdn=nf=-25` |
| G | meteor alone, de-bedded | 23.67 s | A with the same F filter; make-voice's -50 dB trim now takes 0.16 s |
| H | vision-dialog + high-passed meteor head | 27.34 s | as E, meteor `highpass=f=150` only (no afftdn) |
| I | vision-dialog + half-denoised meteor head | 27.34 s | as E, meteor `highpass=f=150,afftdn=nr=6:nf=-25` (half F's 12 dB) |
| J | vision-dialog + tracked-denoise meteor head, transient ducked | 27.34 s | as E, meteor `highpass=f=100,afftdn=nr=6:nf=-30:tn=1` + 5.545–5.635 s burst ducked -12 dB |

Audition so far: **B** best, A acceptable, C rejected (sharp cuts at the
collapse and truncation points), D rejected. E–G avoid C's failure
mode: no cut lands outside a pause and every cut is faded. **E** sounds most
like Ultron but carries footstep-like transients and the music bed; **F** is
clean but loses the timbre. H–J sit between them.

**D** runs each clip through `Collapse-Pauses` before make-voice: pauses found
by `silencedetect` (meteor on a 150 Hz high-passed copy at -24 dB, vision at
-45 dB, both ≥ 0.25 s), cut at their midpoints in the original audio leaving
`-KeepPause` (0.2 s) of room tone, and any stretch under `-MinKeep` (0.5 s)
between two pauses dropped. Pause collapsing alone left D at 30.40 s; the
dropped fragments are what get it under the cap:

```
meteor.wav         keep 0.62-1.42 1.56-2.87 2.93-5.18 5.44-7.07 7.38-8.11 8.33-9.22
                        9.35-10.25 10.86-17.31 18.38-21.69 22.24-23.42
                   drop 17.82-18.02 (music swell)                 -> 19.45 s
vision-dialog.wav  keep 0.00-1.79 2.75-4.45 5.49-7.28 10.40-11.53 12.60-14.23
                        15.43-16.08 16.30-17.64
                   drop 8.06-8.51, 8.74-8.97 (chuckle)            ->  9.81 s
```

The collapsed intermediates stay in `work/` with the source spectrograms.

**E/F** cut one excerpt from the head of meteor instead of collapsing it.
Budget = 30 s − B's 17.41 s − 0.25 s gap − 0.1 s margin = 12.24 s. Pauses come
from the same detection as D (150 Hz high-passed copy, -24 dB, ≥ 0.25 s):

```
0.00-0.72  1.32-1.66  2.77-3.03  5.08-5.54  6.97-7.48  8.01-8.43  9.12-9.45
10.15-10.96  17.21-18.48  21.59-22.34  23.32-end
```

The excerpt starts 0.1 s before the first word (0.616 s) and ends 0.15 s into
the last pause that fits the budget (10.146 → **10.296 s**), with a `-Fade`
(40 ms) fade-in and fade-out. The next pause starts at 17.21 s, so ~9.7 s is the
most meteor that fits without cutting mid-phrase ("... I think a lot about
meteors" through the 10.15 s pause). The vision tail and the excerpt meet
across make-voice's 0.25 s silent gap, not a crossfade: the gap is already
silence (-75 dB RMS in E), so a crossfade would only overlap words.

**F/G** run the whole of meteor through `-MeteorClean`
(`highpass=f=150,afftdn=nf=-25`, default afftdn reduction 12 dB) before
cutting, so the denoiser adapts over the full clip; make-voice's `loudnorm`
re-normalizes afterwards. F uses the identical 0.616–10.296 s range. Intermediates:
`work/meteor-head.wav`, `work/meteor-clean.wav`, `work/meteor-clean-head.wav`.
Plain `silenceremove` at -45 dB was tried first and cannot work here: meteor's
bed never drops that low, so it removes nothing.

**H/I/J** are E with a different meteor chain — same 0.616–10.296 s cut, 40 ms
fades, vision first. `Meteor-Variant` takes the chain as a parameter, cleans the
whole clip with it, then cuts the E range (`work/meteor-<X>.wav`,
`work/meteor-<X>-head.wav`). Bed level in the 8.07–8.37 s pause of each head,
against the whole head's mean:

```
E  raw                                   -35.0 / -21.4 dB   13.6 dB below
H  highpass=f=150                        -38.7 / -24.1 dB   14.6 dB below
J  highpass=f=100,afftdn=nr=6:nf=-30:tn=1 -37.9 / -23.2 dB  14.7 dB below
I  highpass=f=150,afftdn=nr=6:nf=-25     -41.4 / -24.5 dB   16.9 dB below
F  highpass=f=150,afftdn=nf=-25          -42.8 / -24.6 dB   18.2 dB below
```

**Transients in the E range.** Searched 0.6–10.3 s on 5 ms / 10 ms RMS
envelopes in four bands (<150 Hz, 150 Hz–1 kHz, >150 Hz, >2 kHz) for short
(≤150 ms) bursts flanked by ≥40 ms of pause-level signal, plus onsets ≥9 dB
over the previous 30 ms and ≥7 dB over a ±100 ms median. Every hit but one is
a word onset or a word-final sibilant. The one isolated burst is **5.545–5.625 s**,
inside the 5.08–5.74 s pause (the -24 dB detection splits that pause at the
burst): ~80 ms, broadband 150 Hz–2.2 kHz with no harmonic lines on the
spectrogram, -22 dB peak (>150 Hz) against a -40 dB floor, 425 ms of pause
before it and 120 ms after, then the next word's sibilant at 5.745 s. Two
weaker candidates were left alone: 7.365–7.390 s (25 ms, only ~6 dB over its
pause floor) and a <150 Hz thump at 1.47–1.50 s (+7 dB, low band only, mostly
removed by the high-pass anyway).

J ducks the -Duck ranges (default `5.545-5.635`) with a `volume` expression
after the denoiser: gain -DuckGain (0.25 = -12 dB) inside the range, 10 ms
linear ramps either side (`-DuckRamp`), `asetnsamples=n=64` so `eval=frame`
follows the ramp at ~1.5 ms. A hard `enable=` switch would click. Measured on
`work/meteor-J.wav`, 5.55–5.62 s drops from -27.2 to -39.2 dB, level with the
surrounding pause (-39.9 dB before, -38.1 dB after); -18 dB was tried first
and left a hole 5 dB under the bed. `agate` was not used: the bed never falls
below the gate's reach in pauses, so it would pump on the music, not the burst.

## Lessons from jarvis that apply

A noisy clip poisoned every jarvis merge it touched, and one long clean take
beat stitched ones. Expect B to be the cleanest; A, C and D all carry
meteor's music bed. If A sounds right but muddy, try a high-passed meteor
(the bed sits under 150 Hz, though Spader's fundamental does too — about
60–110 Hz).
