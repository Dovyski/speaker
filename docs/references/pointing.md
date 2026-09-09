# Pointing at a window

<p align="center">
  <img src="../pointer.png" width="640" alt="a ring leaving the marked point, two rings in flight, and the tail fading">
</p>
<p align="center"><em>a ring leaving the point → two in flight → the tail fading</em></p>

Speech tells you *that* something finished; it does not tell you **where**. With
several agents running in several terminals, the useful next question is which
window to go back to. So `speak.exe` can also point at one: rings that expand out
of the window's centre and fade, three times over across ~3.5 s, and then nothing.

- [Usage](#usage)
- [Which window? Ask by session id](#which-window-ask-by-session-id)
- [Which window, with nothing to go on?](#which-window-with-nothing-to-go-on)
- [Colours and pacing](#colours-and-pacing)
- [Over HTTP](#over-http)
- [How it works](#how-it-works)
- [Flags](#flags)

## Usage

```bat
speak.exe --point --title "reviewer worker"   rem point at that window
speak.exe --title "reviewer worker" "Tests are green."   rem speak, then point
speak.exe --list-targets                     rem what --title can match, as JSON
speak.exe --point --at 1200,800 --pulses 2    rem a bare screen position
speak.exe --point --title dev --duration 8    rem slower, for a big screen
speak.exe --point --title dev --color red     rem a different kind of attention
speak.exe --point-preview p                   rem render the frames, no screen needed
```

`--point` is the mode; `--pulses`, `--duration`, `--color` and `--size` shape the
gesture (`--point-pulses` and friends are accepted too, if you prefer the prefix).
`--session`, `--title`, `--hwnd` and `--at` each imply `--point`.

The call is synchronous and returns when the animation ends. No model and no
audio device are involved, so a pointing call costs a process start (~240 ms)
whether or not a daemon is running.

The overlay is click-through everywhere — unlike the orb there is nothing on it
to click — and it points at *where the window is*, without raising it or taking
focus. A window that is behind others gets rings drawn over whatever covers it.

When a call both speaks and points, a pointing failure never fails the call: the
utterance already happened, so the exit code stays that of the speech and the
reason goes to stderr as `speak: spoke, but could not point (...)`.

Nothing distinguishes *tabs* — pointing is window-level. If several sessions share
one window, the rings say "this window", not "this tab".

## Which window? Ask by session id

`--title` asks a model to guess a substring of a title it cannot read, and the
title changes every turn. `--session <id>` removes the guess:

```bat
speak.exe --point --session 2c212f58-f956-4be3-ad42-c964cecfba2f
speak.exe --session 2c212f58-… --caption "CI green." "Tests are green."
```

The table it resolves against is the **attention panel's registrations** — the
same one `GET /panels` reports — so nothing extra is maintained for pointing: a
session that has posted a panel is pointable, and one that never has is unknown.
A `SessionStart` hook puts the id in the model's context, and from then on the
target is a fact rather than a search. `--session` implies `--point`, like
`--title`. See
[claude-code-integration.md](claude-code-integration.md) for the hook that
prints it.

The registrations live in the daemon, so a plain `speak.exe` call **asks it**
(one short `GET /panels` on the pointing port, 400 ms budget) rather than
resolving locally; inside the daemon the lookup is local, since asking itself
through its own single-threaded listener would deadlock. Precedence is
`session`, then `title`, then `hwnd`, then `x`/`y`, and an unresolved session is
not an error on its own — it falls through to the `--title` an agent was told to
pass alongside it. Alone, it fails the way a bad title does: exit 3 and the
candidate list.

## Which window, with nothing to go on?

This is the part that needs care, and the reason `--title` exists. With no target
given, `speak.exe` works out where the call came from:

1. **`GetConsoleWindow()`**, for a classic conhost window, which owns a real
   window of its own.
2. Otherwise the **process tree**: under a ConPTY terminal that console is a
   hidden pseudo-console, and a tool child of an agent gets its own conhost
   besides — but the parent chain still reaches the terminal
   (`speak.exe ← bash ← claude ← pwsh ← WindowsTerminal.exe`), so it walks up and
   takes the first ancestor that owns windows. It stops before `explorer.exe` and
   the service layer: pointing at Program Manager or a heap of File Explorer
   windows would be worse than admitting defeat.

If that ancestor owns **more than one** window, the call fails (exit 3) and
prints the candidates as JSON instead of guessing. That is the normal case for
Windows Terminal, which serves every window from one process:

```console
> speak.exe --point
speak: windowsterminal.exe (pid 22788) owns 6 windows — pass --title to say which
[{"hwnd":526232,"pid":22788,"process":"windowsterminal.exe","title":"⠐ Explore speaking with a skill", ...}]
```

Nothing in the process tree, and nothing in Windows Terminal's UI Automation
tree, says which of those windows hosts a given pane — there is no pane or
session id exposed anywhere on the window. The window **title** is the only
discriminator, which is workable because terminals running an agent usually title
themselves after the task. Hence: pick from `--list-targets`, pass `--title`.

`--list-targets` prints every pointable window as JSON — `hwnd`, `pid`,
`process`, `title`, `x`, `y`, `w`, `h` — and `--title` matches case-insensitively
on a substring; `--hwnd <n>` takes an exact handle and `--at <x,y>` a bare screen
position.

## Colours and pacing

Pulses and duration are independent: one is how many rings, the other how long the
whole gesture takes. The built-in pacing is a `1.90 s` ring life and a `0.80 s`
gap — `0.8·pulses + 1.1` seconds — and `--duration` scales both to hit the total
you asked for, keeping their ratio so the rings still read as a sequence rather
than one thick pulse. Duration is what the call blocks for, so pick it for how
long someone needs to notice, not for speed.

<p align="center">
  <img src="../pointer-colours.png" width="640" alt="the same ring in ember, red, green and cyan">
</p>
<p align="center"><em><code>--color</code>: the default ember, then red, green, cyan</em></p>

Everything is derived from that one colour: rings are born white-hot and settle
into it as they expand, the thin line keeps a touch of white so it stays legible,
and the core dot's halo takes it too. Names (`red`, `amber`, `yellow`, `green`,
`cyan`, `azure`, `blue`, `violet`, `magenta`, `pink`, `white`, `steel`, `ember`),
`#rrggbb` and `r,g,b` all work; anything else is a usage error (exit 2) rather
than a silent fallback. So an agent can keep ember for "done" and use red for
"this one needs you".

## Over HTTP

The daemon serves the same thing on **the next port** (`8124` by default), from
its own listener bound to `127.0.0.1` — it moves things on someone's screen, so
it never leaves the machine. `POST /point`, `GET /targets` and `GET /health` are
documented with the rest of the routes in [http-api.md](http-api.md).

Two things it deliberately does not do: raise or focus the window (pointing is a
hint, not a hijack), and point at a *tab*. Windows Terminal's tab headers do turn
up in the UI Automation tree with their own bounding rectangles, so tab-level
pointing is possible later, but it needs UIA in the binary and a way to tell
which tab is which.

## How it works

The same layered-window machinery as the orb, with the opposite personality: a
perfect circle, no audio drive, `WS_EX_TRANSPARENT` for a fully click-through
overlay, and a life measured in rings rather than in samples. Each ring is a
Gaussian band at radius `R(u)` with an ease-out on `u`, a thin bright line riding
a 4.5× wider glow so it survives over a busy window, born `0.80 s` apart and
living `1.90 s`; a hot core dot re-brightens with every ring so the exact spot
stays marked.

One trap worth naming: **DPI.** `GetWindowRect` and overlay placement only agree
if the calling thread is per-monitor aware — a system-DPI-aware process is quietly
handed virtualized rectangles, and on a scaled monitor the rings land beside the
window instead of on it. Rather than change the whole process (the orb was written
against the old behaviour), pointing calls `SetThreadDpiAwarenessContext` with
`PER_MONITOR_AWARE_V2` on their own thread — awareness is per thread, so the two
overlays coexist with different views of the screen.

## Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--point` | — | Point at a window; alone it only points, with text it speaks first |
| `--session <id>` | — | Point at the window this Claude session's panel is bound to; preferred over `--title`, falls back to it (implies `--point`) |
| `--title <substr>` | — | Point at the window whose title contains this (implies `--point`) |
| `--hwnd <n>` / `--at <x,y>` | — | Point at a window handle / a screen position (imply `--point`) |
| `--pulses <n>` | `3` | Rings to send out |
| `--duration <s>` | `3.5` | Seconds the whole gesture lasts; the pacing scales to fit |
| `--color <c>` | `ember` | Colour the rings are built from: a name, `#rrggbb`, or `r,g,b` |
| `--size <px>` | `320` | Square size of the pointer overlay |
| `--point-preview <prefix>` | — | Render a strip of pointer frames and exit |
| `--list-targets` | — | Print the pointable windows as JSON and exit |
| `--point-port <n>` | `port+1` | Daemon: port for the pointing endpoint |
| `--no-point-server` | — | Daemon: do not serve the pointing endpoint |

`--point-pulses`, `--point-duration`, `--point-color` and `--point-size` are
accepted as aliases for the four gesture flags.

Exit codes: `2` for a usage error (an unparseable colour, a missing value), `3`
when the target cannot be resolved — with the candidate list on stdout as JSON,
so a failed `--point` is still a usable answer.
