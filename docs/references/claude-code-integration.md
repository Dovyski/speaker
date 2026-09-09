# Claude Code integration (the attention panel's producer side)

Audience: an agent with this repo checked out, asked to make the attention panel
work on a machine where it has never run. `speak.exe` draws the card; everything
that decides *what is on it* lives in `hooks/`. This document is the map between
the two.

## What the panel is

A small card parked inside the bottom-right corner of one terminal window,
listing what the Claude Code session running in that window is working on: open
pull requests, issues, work directories, and questions the agent is waiting on an
answer for. Rows are clickable — PRs and issues open in the browser, paths in
Explorer, a question copies itself to the clipboard. See
[The attention panel](../../README.md#the-attention-panel) in the README for the
card itself, its tabs, the collapsed pill and the `POST /panel` contract. This
document covers only the producer chain that feeds it.

## The chain, end to end

```
Claude Code session, in a Windows Terminal window titled "◑ <what it is doing>"
  │
  ├─ Stop hook (matcher *)              once per assistant turn
  └─ PostToolUse hook (matcher Bash)    every Bash call; ignored unless `gh pr|gh issue`
        │  hook JSON on stdin
        ▼
   hooks/i47-attention.ps1  — the producer (regex, no model)
        ├─ reads the transcript .jsonl from `_cursor` to EOF (the delta only)
        ├─ regex → items: GitHub URLs, owner/repo#N, repo#N, bare #N, work paths
        ├─ resolves its own hosting terminal title (see below)
        ├─ writes ~/.claude/attention/<session_id>.json
        ├─ POSTs the `worked` items + questions to http://127.0.0.1:8124/panel
        └─ on Stop only: spawns the async worker and does not wait
                │
                ▼
   hooks/i47-haiku.ps1  — detached, hidden pwsh, ~10-40 s
        ├─ `claude -p --model haiku` over the transcript delta since `_haiku_cursor`
        ├─ returns summary, per-item relevance (worked|mentioned), questions_open/resolved
        ├─ rewrites the session json (adds `summary`, `question` rows)
        └─ re-POSTs

   hooks/i47-enrich.ps1  — scheduled task `cto-i47-enrich`, every minute
        ├─ every session json touched in the last 48 h
        ├─ `gh pr view` / `gh issue view --json …`, cached in status-cache.json
        ├─ writes back `status` and empty `title`s
        └─ re-POSTs the files it changed
                │
                ▼
   speak.exe --serve  →  POST /panel on the daemon port + 1 (8124 by default)
        └─ resolver every 500 ms: title → hwnd → the card is shown, moved or hidden
```

Three independent writers, one file per session, one endpoint. The producer is
the only one that must be fast (its hook budget is 5 s on `Stop`, 3 s on
`PostToolUse`, and it truncates its own delta scan 800 ms before that); the other
two are free to be slow because nothing waits for them.

## How a session finds its window

There is no session or pane id anywhere in Windows Terminal — not in the process
tree, not in its UI Automation tree. The only handle is the window title, and
Claude Code sets it to a spinner glyph plus a short description of the turn.

- **A WT window has tabs, and its title is the active tab's title.** So a
  session's card is visible only while its tab is in front. `/panel` therefore
  registers a *session*, not a window; the daemon re-resolves title → hwnd every
  ~500 ms and shows, hides or rebinds the card accordingly. This is normal
  operation, not an error state: `resolved:false` just means "that tab is in the
  background right now".
- **Claude Code rotates the leading glyph** (`◐ ◑ ◒ ◓ ✳`), and the title was
  captured at one instant and read at another, so the daemon strips a leading
  non-ASCII glyph *and the space behind it* from both sides before matching.
- **The hook has to find its own console.** `GetConsoleWindow()` is useless under
  ConPTY and `[Console]::Title` is wrong when the hook runs under the Bash tool
  (it reports bash's own title). `i47-attention.ps1` walks its parent chain
  (`Win32_Process`, up to 12 hops, stopping at `windowsterminal.exe` /
  `openconsole.exe` / `conhost.exe` / `explorer.exe`), then for each ancestor
  nearest-first calls `FreeConsole()` + `AttachConsole(pid)` +
  `GetConsoleTitleW`. `FreeConsole` first, or `AttachConsole` fails with GLE 5;
  attaching to `WindowsTerminal.exe` itself fails with GLE 6, which is why the
  chain matters. `GetConsoleTitleW`'s buffer tail is uninitialised and may carry
  a BEL, so the result is truncated to the returned length and stripped of
  control characters. `Test-Title` then rejects the obvious impostors
  (`pwsh`, `bash`, `mingw…`, anything ending in `.exe`, anything that is a path).
- `hooks/i47-terminal-title.ps1` is the P0 spike that established all of the
  above. It is not needed at runtime; it is kept because it logs every method it
  tried and matches the result against `speak.exe --list-targets`, which is the
  fastest way to debug a title that will not bind. It is the one script with a
  hardcoded path (`$SpeakExe = 'C:\Dev\www\claude-speak\speak.exe'`) — edit that
  line before using it elsewhere.

## The hook JSON

Claude Code writes one JSON object to the hook's stdin. The producer reads it
with `[Console]::In.ReadToEnd()` guarded by `[Console]::IsInputRedirected`, and
uses four fields — plus one more on `PostToolUse`:

```json
{
  "session_id": "2c212f58-f956-4be3-ad42-c964cecfba2f",
  "transcript_path": "C:\\Users\\<user>\\.claude\\projects\\C--Dev-field-cto\\2c212f58-….jsonl",
  "cwd": "C:\\Dev\\field\\cto",
  "hook_event_name": "Stop",
  "tool_input": { "command": "gh pr view 1375 --json title,state" }
}
```

| Field | Used for |
|---|---|
| `session_id` | the state file's name and the `/panel` registration key |
| `transcript_path` | the `.jsonl` the delta is read from; opened `FileShare.ReadWrite` because Claude Code is still appending to it |
| `cwd` | infers the repo for a bare `#N` when the cwd is `…\Dev\field\work\<repo>\<slug>` |
| `hook_event_name` | `Stop` gets the 5 s budget and spawns the Haiku worker; `PostToolUse` gets 3 s and no worker |
| `tool_input.command` | `PostToolUse` only: anything not matching `\bgh\s+(pr|issue)\b` exits immediately (`skip:not-gh`) |

Everything else in the payload is ignored. The producer never writes to stdout,
never blocks and always exits 0 — a hook that fails must not break the session.

## `I47_NESTED`

`i47-haiku.ps1` classifies the transcript by running `claude -p --model haiku`.
That nested run is itself a Claude Code session, so its own `Stop` hook would
fire `i47-attention.ps1`, which would spawn another worker, which would run
another nested `claude -p`. Two guards stop that:

- the nested process is started with `I47_NESTED=1` in its environment, and both
  `i47-attention.ps1` and `i47-haiku.ps1` `exit 0` on line one if that variable
  is set;
- the nested run also passes `--safe-mode` (no hooks, no `CLAUDE.md`, no skills,
  no MCP) together with `--no-session-persistence`, `--tools ''`,
  `--permission-prompts none`, `--output-format json` and `--json-schema`.

`--safe-mode` alone would be enough today; `I47_NESTED` is the guard that does
not depend on a CLI flag keeping its meaning. The worker uses Claude Code's own
authentication, so no API key is needed; it falls back to a direct
`api.anthropic.com` call with `$env:ANTHROPIC_API_KEY` and
`claude-haiku-4-5-20251001` only if the CLI is unavailable.

## The files

All under `~/.claude/attention/`. The scripts resolve that path from
`$env:USERPROFILE` regardless of which config dir the session came from, so
there is exactly one state directory per user even with several
`CLAUDE_CONFIG_DIR`s.

### `<session_id>.json` — the state, and the POST body

```json
{
  "session": "2c212f58-f956-4be3-ad42-c964cecfba2f",
  "title": "◑ Floating window for PR context display",
  "summary": "i47 — wiring the panel into the daemon",
  "items": [
    {
      "kind": "pr",
      "repo": "optidatacloud/laravel-opticloud",
      "number": 1375,
      "url": "https://github.com/optidatacloud/laravel-opticloud/pull/1375",
      "title": "feat: calendar event reminder as a bottom-right toast (issue #1372)",
      "status": "merged",
      "relevance": "worked",
      "last_seen": "2026-09-09T12:04:24Z"
    },
    { "kind": "question", "url": "", "title": "Merge the pending pair now or wait for D50?",
      "status": "open", "last_seen": "2026-09-09T12:04:24Z" },
    { "kind": "path", "url": "C:\\Dev\\field\\work\\laravel-opticloud\\1372-toast",
      "title": "1372-toast", "status": "unknown", "last_seen": "2026-09-09T12:04:24Z" }
  ],
  "updated_at": "2026-09-09T12:04:24Z",
  "_cursor": 481233,
  "_haiku_cursor": 481233
}
```

- It is the `POST /panel` body plus three private keys. `_cursor` is the
  producer's byte offset into the transcript, `_haiku_cursor` the worker's own —
  each writer preserves the other's, and a cursor past EOF resets to 0 so a
  rotated transcript is re-read rather than skipped.
- `relevance` is the producer's confidence: `worked` for the strong rules (a real
  URL, an explicit `owner/repo#N`, a work path), `mentioned` for the weak bare
  `#N` rule. **Only `worked` items and `question` rows are POSTed**; `mentioned`
  ones stay in the file for audit. An item with no `relevance` counts as
  `worked`. A strong sighting promotes a weak row; nothing ever demotes one
  except the Haiku verdict.
- Items are pruned at 48 h since `last_seen` and capped at 30, most recent first.
- `summary` and `question` rows only ever come from the Haiku worker.

### `status-cache.json` — the enricher's GitHub cache

```json
{
  "optidatacloud/infra#issue#36": {
    "fetched_at": "2026-09-09T12:11:03Z",
    "status": "open",
    "_missing": 0,
    "url": "https://github.com/optidatacloud/infra/issues/36",
    "title": "infra: ajustar recursos, probes e HPA do rollout call"
  }
}
```

Keyed `repo#kind#number`. TTL is 60 s while open and 30 min once closed or
merged, at most 40 `gh` calls a pass, 4 in parallel, 15 s per call. `_missing`
counts consecutive failures; at 3 the item is given up on (this is what retires a
repo name the regex invented). Entries no session references and not refreshed
for 7 days are dropped. `is_pr` records that a number the producer guessed was an
issue is really a pull request — `gh issue view` says so in its error, and the
enricher then re-fetches it as a PR and fixes `kind` in the session file.

### Logs — one compact JSON line per run

| File | Written by | Fields worth reading |
|---|---|---|
| `producer.log` | `i47-attention.ps1` | `event`, `delta_bytes`, `items_total`, `items_sent`, `title`, `post_status`, `haiku`, `ms`, `errors` |
| `haiku.log` | `i47-haiku.ps1` | `model`, `delta_chars`, `attempts`, `summary`, `worked`, `mentioned`, `q_open`, `cost_usd`, `ms` |
| `enricher.log` | `i47-enrich.ps1` | `sessions`, `items`, `gh_calls`, `changed`, `ms`, `errors` |
| `p0-spike.log` | `i47-terminal-title.ps1` | `chain`, `methods`, `title_source`, `hwnd`, `candidates` |

```json
{"ts":"2026-09-09T09:15:58.01-03:00","session":"2c212f58-…","event":"PostToolUse","delta_bytes":0,
 "items_total":0,"items_new":0,"items_sent":0,"title":"◑ Floating window for PR context display",
 "post_status":200,"haiku":null,"ms":1322,"errors":[]}
```

`post_status` is the HTTP code, or `refused` (no daemon), `timeout`, or a
`skip:*` reason. A healthy `Stop` run is 1.0–1.6 s; a cold enricher pass ~3 s and
a cached one under 0.5 s. The logs are append-only and nothing rotates them.

## Installing

Prerequisites:

- Windows 11 and **Windows Terminal** — the whole title→window mechanism assumes
  it. A session in a bare conhost window has no card.
- **PowerShell 7** (`pwsh` on `PATH`). The hooks use `-DateKind String`,
  `ArgumentList`, `-EscapeHandling` and ternaries; Windows PowerShell 5.1 will
  not run them.
- **`gh` authenticated** (`gh auth status`) for the enricher, with access to the
  repositories the sessions talk about.
- **`speak.exe` built**, with its `models/` and `voices/` populated — see the
  README's [Build](../../README.md#build), [Get the models](../../README.md#get-the-models)
  and [Get a voice](../../README.md#get-a-voice). The panel itself needs no
  model, but the daemon that draws it loads one at `--serve` time.
- **`claude` on `PATH`** for the optional Haiku layer. Without it the panel still
  works; it just has no `summary`, no `Pending` tab and no relevance pruning.

Then:

```powershell
pwsh -NoProfile -File hooks\install-claude-code.ps1
```

It copies `hooks\*.ps1` into `<ClaudeDir>\hooks\`, creates
`<ClaudeDir>\attention\`, merges the two hook entries into every settings file it
was pointed at, registers the scheduled task, and prints what it did and what is
left to do by hand.

| Parameter | Default | |
|---|---|---|
| `-ClaudeDir` | `$env:USERPROFILE\.claude` | where the scripts are installed and the primary `settings.json` |
| `-ExtraClaudeDirs` | `@("$env:USERPROFILE\.claude-max")` | further config dirs whose `settings.json` gets the same entries; a dir that does not exist is skipped, never created |
| `-DaemonPort` | `8123` | the speech port; the panel listener is this **+ 1**, and a non-default value is patched into the copied scripts' endpoint |
| `-SkipTask` | — | do not touch the scheduled task |

It also honours `-WhatIf`. Note that `-ExtraClaudeDirs @()` needs
`pwsh -NoProfile -Command "& '…\install-claude-code.ps1' -ExtraClaudeDirs @()"`;
with `-File` every argument is a string, so pass `-ExtraClaudeDirs ''` instead.

What the merge writes into each `settings.json`, alongside whatever hooks are
already there:

```json
"Stop": [
  { "matcher": "*",
    "hooks": [ { "type": "command",
                 "command": "pwsh -NoProfile -ExecutionPolicy Bypass -File \"C:\\Users\\<user>\\.claude\\hooks\\i47-attention.ps1\"",
                 "timeout": 10 } ] }
],
"PostToolUse": [
  { "matcher": "Bash",
    "hooks": [ { "type": "command",
                 "command": "pwsh -NoProfile -ExecutionPolicy Bypass -File \"C:\\Users\\<user>\\.claude\\hooks\\i47-attention.ps1\"",
                 "timeout": 10 } ] }
]
```

Both entries point at the `-ClaudeDir` copy, even in the extra config dirs —
there is one installed copy of the scripts, not one per config dir. The file is
backed up to `settings.json.bak-i47-<yyyyMMdd-HHmmss>` before the first change
and rewritten as UTF-8 without BOM at two-space indentation. Re-running changes
nothing and leaves no new backup: the duplicate test is "does any hook in this
matcher's list mention `i47-attention.ps1`", so a hand-edited command line is
recognised too. **`settings.json` is read at session start**, so an already-open
session will not pick the hooks up.

The scheduled task is `cto-i47-enrich`: `pwsh -NoProfile -WindowStyle Hidden
-ExecutionPolicy Bypass -File <ClaudeDir>\hooks\i47-enrich.ps1`, one trigger
repeating every minute forever, hidden, interactive logon, `Limited` run level,
2-minute execution limit, `IgnoreNew` so a slow pass is never overlapped.
Registration uses `-Force`, so re-running rewrites the definition rather than
failing.

Start the daemon detached — a job-controlled or agent-background launch dies when
its task is reaped, and `--serve` never returns:

```powershell
Start-Process -FilePath C:\Dev\www\claude-speak\speak.exe -ArgumentList `
  '--serve','--port','8123','--voice','jarvis.wav', `
  '--models-dir','C:\Dev\www\claude-speak\models', `
  '--voices-dir','C:\Dev\www\claude-speak\voices', `
  '--eos-extra','4','--keepalive','60' -WindowStyle Hidden
```

`--models-dir` and `--voices-dir` default to the directories beside the exe, so
they are only needed when the daemon is started from elsewhere.

### Verifying

```powershell
speak.exe --status                                  # the daemon is up
curl.exe http://127.0.0.1:8124/panels               # what is registered
speak.exe --panel-demo --title "<part of a title>"  # a sample card for 20 s, no producer
Get-Content $env:USERPROFILE\.claude\attention\producer.log -Tail 3
```

`GET /panels` is the first thing to look at when a card is missing — it reports
`hwnd`, `resolved`, the item count, `collapsed`, `tab` and `updated_at` per
session, refreshed by the resolver at most half a second ago:

```json
[{"session":"2c212f58-…","title":"◑ Floating window for PR context display",
  "hwnd":721546,"resolved":true,"items":9,"collapsed":false,"tab":"all",
  "expanded_all":false,"speaking":false,"updated_at":"2026-09-09T12:14:36Z"}]
```

Then open a Claude session, let it finish a turn, and check `producer.log` shows
`post_status: 200` and a non-null `title`. `--panel-preview <prefix>` renders the
card to PNGs with no daemon and no producer at all, which separates "the producer
is wrong" from "the card is wrong".

## Operations

**Rebuilding `speak.exe` while the daemon runs.** The image is locked. Stop the
daemon (`speak.exe --stop`), and if the linker still cannot write it, rename the
old image to `speak-old-daemon.exe`, link, then delete the rename. Start the new
daemon with the `Start-Process` line above. Registrations are in-process, so
every card is gone until each session's next `POST`.

**Re-firing the producer by hand** (no need to wait for a turn to end):

```powershell
'{"session_id":"<id>","transcript_path":"<…\\<id>.jsonl>","cwd":"C:\\Dev\\field\\cto","hook_event_name":"Stop"}' |
  pwsh -NoProfile -ExecutionPolicy Bypass -File $env:USERPROFILE\.claude\hooks\i47-attention.ps1
```

Keep a payload like that on disk (the live machine has
`~/.claude/attention/hook-test.json`) — it is the whole hook interface. Two
caveats: the run resolves *your* console's title, not the session's, so it will
bind the card to the window you ran it from; and it advances `_cursor`, so the
next real hook sees a smaller delta. To re-read a transcript from the start,
delete the session json, or set `_cursor` to 0.

**Re-running the enricher now:** `pwsh -NoProfile -File
$env:USERPROFILE\.claude\hooks\i47-enrich.ps1 -Verbose`, optionally
`-Session <id>`. Deleting `status-cache.json` forces a cold pass.

**Removing a card:** `curl.exe -X DELETE "http://127.0.0.1:8124/panel?session=<id>"`,
or a `POST` with empty `items`. Registrations expire 48 h after their last
`POST` anyway.

## Troubleshooting

| Symptom | Cause and check |
|---|---|
| No card, `producer.log` shows `post_status: 200` | The session's tab is not the active one in its WT window — the title is the active tab's title. Bring the tab forward; `GET /panels` will flip `resolved` to `true`. |
| No card, `resolved:false` while the tab *is* in front | Title mismatch. Compare the registered `title` in `/panels` against `speak.exe --list-targets`. Matching strips one leading non-ASCII glyph *and its space* from both sides, then tries exact before containing. |
| No card, and `/panels` is empty | The daemon is a different build, or was restarted: registrations do not survive it. Check `speak.exe --status`, then wait for the next turn or re-fire the producer. |
| `producer.log` never gets a line for a session | The hook is not registered in the config dir *that session* uses. A session started with `CLAUDE_CONFIG_DIR=…\.claude-max` reads that `settings.json`, not the default one — that is what `-ExtraClaudeDirs` is for. Also: hooks are read at session start, so restart the session. |
| `post_status: refused` | No daemon on the panel port. Start it; the producer keeps its state file either way, so nothing is lost. |
| `title: null` in `producer.log` | The console-title resolver failed. Run `i47-terminal-title.ps1` with the same payload and read `chain` / `methods` in `p0-spike.log`. A session launched with `CreateNoWindow` or `DETACHED_PROCESS` in its ancestry has no console to attach to. |
| The whole mouse pointer feels late | Something installed a lifetime `WH_MOUSE_LL` hook. Never do that: every mouse event on the machine round-trips through the installing thread. The panel installs one only while the pointer is over a scrollable card, and its callback does nothing but `PostMessage`. |
| Rows for things the session never worked on | The producer scrapes the whole transcript, including text *about* other sessions and agent briefs, so a coordinating session collects other people's numbers. Bare `#N` is the weakest rule and is born `mentioned`, which is not POSTed; the Haiku layer demotes the rest. Without `claude` on `PATH` there is no pruning at all. |
| A row for an identifier that does not exist | Fake identifiers in prompts (`does-not-exist#1`) are indistinguishable from real ones to a regex. The enricher gives up after 3 failed lookups; keep invented numbers out of live prompts. |
| A question row that is already answered | The Haiku worker clears questions when the user has spoken since, unless the model re-lists them as open. Check `q_open` / `q_resolved` in `haiku.log`. |
| `haiku.log` says `skip:locked` | A previous worker is still running (`<session>.haiku.lock`, considered stale after 5 minutes). `skip:no-verdict` after `cli: timeout` means the model call exceeded 40 s and both attempts failed — harmless, the next turn tries again. |

## What is machine-specific

Everything here is a constant in a script, not a parameter. Change them before
using the chain outside Fernando's machine.

| Where | What |
|---|---|
| `i47-attention.ps1` | `$DefaultOwner = 'optidatacloud'` — the owner assumed for a bare `repo#N` |
| `i47-attention.ps1` | the known-repo scan globs: `C:\Dev\field\cto\repos\*` (three depths), `C:\Dev\field\work\*`, `C:\Dev\www\*`, cached 24 h in `known-repos.txt`. Depth-limited on purpose: a `-Recurse` walk of `C:\Dev` costs seconds inside a 5 s budget |
| `i47-attention.ps1` | the work-path rule `<drive>:\Dev\field\work\<repo>\<slug>` — two segments deep, and the leaf becomes the row's title |
| `i47-attention.ps1`, `i47-enrich.ps1`, `i47-haiku.ps1` | `$Endpoint = 'http://127.0.0.1:8124/panel'`; the installer rewrites it for a non-default `-DaemonPort` |
| `i47-terminal-title.ps1` | `$SpeakExe = 'C:\Dev\www\claude-speak\speak.exe'` |
| `i47-haiku.ps1` | `$ApiModel = 'claude-haiku-4-5-20251001'` (API fallback only), and prompts that name the user |
| `install-claude-code.ps1` | the task name `cto-i47-enrich`, and `.claude-max` as the extra config dir |
| all | `~/.claude/attention` as the state directory, resolved from `$env:USERPROFILE` |
