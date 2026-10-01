# The daemon's HTTP API

Everything `speak.exe --serve` listens on. All of it is bound to `127.0.0.1`:
these routes move things on someone's screen, so they never leave the machine.

## Ports

| Port | What | Flag |
|---|---|---|
| `8123` | speech — upstream PocketTTS.cpp's `TTSServer` | `--port <n>` |
| `8124` | pointing, the attention panel and remote speech, a separate listener on its own thread | `--point-port <n>`, default `port+1` |
| `8125` | the speaking beacon: UDP, one fixed-size datagram every other frame (~30 Hz) | `point_port + 1` |

`--no-point-server` starts the daemon with neither the pointing/panel listener
nor the beacon.

## Speech: `POST /tts` (port 8123)

Upstream PocketTTS.cpp's own endpoint, used by every `speak.exe` client as the
fast path: the response is chunked PCM, and the client hands partial chunks to
WASAPI as they arrive. The daemon's keepalive nudge goes through this same route
so it serializes against real requests. Its routing is hardcoded in a source file
CMake downloads at a pinned SHA, which is why everything below is a separate
listener rather than more routes on it.

`speak.exe --status` reports on this port: `daemon ready`, `daemon starting up`
or `no daemon`.

## Pointing (port 8124)

```console
$ curl -s -X POST http://127.0.0.1:8124/point -d '{"title":"reviewer worker"}'
{"ok":true}

$ curl -s -X POST http://127.0.0.1:8124/point -d '{"session":"2c212f58-…"}'
{"ok":true}

$ curl -s http://127.0.0.1:8124/targets      # same list as --list-targets
$ curl -s http://127.0.0.1:8124/health
{"ok":true,"service":"speak-pointer"}
```

`POST /point` takes `session`, or `title`, or `hwnd`, or `x` and `y`, plus
optional `pulses`, `duration`, `color` and `size` — the flags' JSON
counterparts, same defaults, and an unparseable colour is a `400` rather than a
silent ember. It **must** be told a target: the caller is at the other end of a
socket, so the daemon's own process tree says nothing about where the request
came from. It answers when the animation has finished (~3.5 s for three rings),
and requests queue rather than overlapping.

`GET /targets` returns the pointable windows as JSON — `hwnd`, `pid`, `process`,
`title`, `x`, `y`, `w`, `h`. See [pointing.md](pointing.md).

## Remote speech: `POST /speak` (port 8124)

For a caller that cannot run `speak.exe` itself — a Claude Code agent on a Linux
runner reaching this desktop through an SSH reverse tunnel (`RemoteForward 8124`).
The daemon runs its own binary as a client with the matching flags, so the
utterance gets the orb, the caption and pointing exactly as from the CLI.

```console
$ curl -s -X POST http://127.0.0.1:8124/speak -d '{
    "text": "Remote speech works now, sir.",
    "caption_title": "claude-runner-2", "caption": "tunnel speak e2e",
    "caption_variant": "light", "title": "reviewer worker"}'
{"ok":true,"exit":0}
```

| Field | Flag |
|---|---|
| `text` (required) | the positional text |
| `voice`, `caption_title`, `caption`, `caption_variant`, `caption_icon`, `subtitle` | `--voice`, `--caption-title`, … |
| `caption_opacity` (number or numeric string) | `--caption-opacity` |
| `no_orb` (`true`) | `--no-orb` |
| `title`, `session` | `--title`, `--session`: point at that window while speaking |

It answers when playback ends: `{"ok":true,"exit":0}`, or `{"ok":false,"exit":N}`
with a `500` when the client failed (an unknown `caption_variant` exits `2`).
`400` for a missing `text`; `500` if the client could not start or ran past 120 s.
Requests queue, one utterance at a time. Each runs on its own thread, so
`/panels` (which `--session` resolves against) and `/point` keep answering
while it speaks; synthesis goes to `8123`, served by a different thread.

## The attention panel (port 8124)

`POST /panel`, `DELETE /panel` and `GET /panels` share the pointing listener.

```console
$ curl -s -X POST http://127.0.0.1:8124/panel -d '{
    "session": "0a7f…",
    "title": "◑ Floating window for PR context",
    "summary": "i47 — wiring the panel into the daemon",
    "items": [
      {"kind":"pr","repo":"optidatacloud/laravel-opticloud","number":1375,
       "url":"https://github.com/optidatacloud/laravel-opticloud/pull/1375",
       "title":"feat: calendar event reminder as a toast","status":"checks_failing"},
      {"kind":"question","title":"Merge the partners PR before or after the gateway one?"},
      {"kind":"path","url":"C:\\Dev\\field\\work\\laravel-opticloud\\1372-toast",
       "title":"1372-toast"}
    ]}'
{"ok":true,"hwnd":1968562,"resolved":true}
```

| Route | |
|---|---|
| `POST /panel` | Register or update a session's panel. Always succeeds unless the body is wrong: `{"ok":true,"hwnd":N,"resolved":true}`, or `{"ok":true,"hwnd":null,"resolved":false,"error":"…","candidates":[…]}` when the title matches nothing (or several) at that moment. `400` for a body that is not a JSON object, a missing `session`, a missing `title` or malformed `items`. Empty `items` **and** an empty `summary` removes the registration |
| `DELETE /panel?session=<id>` | Remove one registration at once |
| `GET /panels` | What is registered: `session`, `title`, `hwnd` or `null`, `resolved`, the item count, `collapsed`, `hidden`, `tab`, `expanded_all`, `speaking`, `updated_at`. A snapshot published by the resolver, so at most half a second stale. This is also the table `--session` resolves a window against |
| `GET /panels?debug=1` | The same registrations seen from the panel thread: each card's rectangle, the monitor it is on, the DPI it was drawn at, the row count, and the **last cursor position and hit** the thread acted on. What to reach for when a card is in the wrong place or a hover does nothing |

Item fields, the `details` object the popover reads, and every rule behind
`resolved` are in [attention-panel.md](attention-panel.md).
Registrations expire 48 hours after their last `POST`.

## The speaking beacon (UDP, port 8125)

Not a route: while an utterance is aimed at a window, the one-shot client
broadcasts one fixed-size UDP datagram every other frame (~30 Hz) to loopback,
each carrying the whole state — target window, level, caption title, caption. The daemon uses it to
glow the card parked in that window. No setup, no teardown, no session: the
client sends a last few with `active` clear when the voice stops, and a *gap* in
the datagrams ends it too, so a client killed mid-sentence cannot leave a card
glowing forever. UDP because a stream of tiny HTTP requests at that rate would
have queued `/panel` behind it.
