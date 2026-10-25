# heddle-proxy — OSC fan-out router

`heddle-proxy` routes **one control surface** (e.g. TouchOSC on a phone/tablet)
to **up to 5 Heddle instances** at once:

```
        OSC                    OSC (rewritten)
surface ───────►  proxy  ──────────────►  target 1  (Heddle host A)
(TouchOSC)       (router)                target 2  (Heddle host B)
                                          target 3 …  (up to 5)
```

It exists to solve the "one surface, many hosts" case: a single TouchOSC layout
drives several Heddle instances (different machines, or different slot-chains on
the same box) by re-addressing each surface control to a
`/chain/<slot>/<param>` set on a chosen target.

There are **two forms of the same engine**:

| Form | What it is | Audio |
|------|-----------|-------|
| **Passthrough VST3** (`HeddleProxy.vst3`) | Drop into any DAW track; routes OSC; audio passes through **unchanged** | dry passthrough |
| **Standalone exe** (`heddle-proxy`) | Config-file / CLI front-end, no audio | none |

Both share the JUCE-free engine in `core/` (`ProxyEngine` + `ProxyConfig`), so
the OSC logic is identical and unit-tested in both. Audio passthrough and any
GUI belong to the *apps*; the engine owns only the OSC side.

## Routing modes (the 3-state proxy)

`ProxyConfig.mode` selects how incoming messages are handled:

| Mode | Behaviour |
|------|-----------|
| `off` | **Drop everything.** The proxy still binds its listen port (so a surface sees it as alive and the counters still count dropped traffic) but forwards nothing. In the VST form this is pure audio passthrough. |
| `only-control` | **Forward only mapped messages.** An incoming message is forwarded only if its address exactly equals a configured route's `srcAddr`; it is rewritten to `/chain/<slot>/<param>` on that route's target with the value mapped per the route (see [Value mapping](#value-mapping-arg--range--curve)). Unmapped messages are dropped. |
| `full` | **Forward every message, address as-is**, to every *enabled* target (no rewriting, no filtering). For a surface whose addresses already match the target's expectations. |

`only-control` is the default — it is the safe, explicit routing mode that lets
one layout fan out to differently-named slots/params on each target.

> **Address matching is exact.** A message matches a route only when its address
> **equals** `route.srcAddr`. No normalization (no trimming a leading `/k/`, no
> case folding) is done: TouchOSC address layouts vary, so you write the exact
> address your surface sends.

## Config schema (JSON)

```jsonc
{
  "listen_port": 9100,            // the port the SURFACE (TouchOSC) sends to
  "listen_host": "0.0.0.0",       // bind address (127.0.0.1 for loopback)
  "mode": "only-control",          // "off" | "only-control" | "full"
  "targets": [                     // up to 5
    { "ip": "127.0.0.1", "port": 9001, "enabled": true },
    { "ip": "192.168.1.40", "port": 9001, "enabled": false }
  ],
  "routes": [                      // only-control routing table
    { "src": "/k/0", "target": 0, "slot": "smoke1", "param": "Gain" },
    { "src": "/xy", "target": 1, "slot": "chain_a", "param": "Threshold",
      "arg": 1, "in_min": -1.0, "in_max": 1.0, "curve": "exp" }
  ]
}
```

- `targets` — destination Heddle instances (ip + port). `enabled:false` targets
  are skipped by `full` mode and by any `only-control` route pointing at them;
  an output socket is still opened so a target can be re-enabled live without
  re-binding.
- `routes` — mapping rows. `src` = the exact surface address; `target` = index
  into `targets`; `slot`/`param` = the Heddle slot token + param title the set
  is rewritten to. One `src` may map to several `(target, slot, param)` rows —
  each is forwarded as its own rewritten message (this is the fan-out).
- Route value mapping (`arg`, `in_min`, `in_max`, `curve`) — all optional,
  defaults keep the classic "first float, 0..1, as-is" behaviour. See
  [Value mapping](#value-mapping-arg--range--curve).

Validation: listen port 1–65535; ≤5 targets each with a non-empty ip and port
1–65535; every route's `src` starts with `/`, references an existing target,
has non-empty `slot` and `param`, and a non-negative `arg`.

The VST persists this table in plugin state (DAW save/restore); the standalone
exe reads it from a file (or builds it from CLI flags, below).

## Value mapping (arg, range, curve)

Surfaces do not all send what you expect: TouchOSC pads a slider with extra
floats, sends `-1..1` instead of `0..1`, or flips its range. Each route can
therefore pick **which float** of the surface message is the value, **rescale**
it, and **shape** it before it is forwarded:

```
raw = surface.float[arg]
x   = clamp01((raw - in_min) / (in_max - in_min))
set = /chain/<slot>/<param>   ( curve(x) )     <- exactly ONE float
```

| Key | Default | Meaning |
|-----|---------|---------|
| `arg` | `0` | Index of the float that carries the value (`float[0]`, `float[1]`, …). If the message has fewer floats, the route emits **nothing** (the message is counted, the set is skipped) instead of guessing. |
| `in_min` | `0.0` | Surface-side range start. |
| `in_max` | `1.0` | Surface-side range end. Set `in_min > in_max` to **invert** (flipped slider). `in_min == in_max` passes the value through unchanged (surface already 0..1). |
| `curve` | `identity` | Shape applied after rescaling: `identity` (= `linear`), `exp` (x², slow-out), `invexp` (1−(1−x)², snappy start), `scurve` (smoothstep S around 0.5). |

The rewritten set carries **exactly one float** — the mapped value — because a
Heddle set reads `float[0]` as the value and `float[1]`/`float[2]` as
optional delay/duration (docs/OSC.md). Surfaces with extra values (X/Y knobs)
must not leak those into the delay slot, which is why the value is picked
explicitly by `arg`. With the defaults, a single-float surface behaves exactly
as before (first float, 0..1, as-is), so old configs are unchanged.

Example — a TouchOSC X/Y pad whose **y** axis (`float[1]`, range `-1..1`)
drives a gain, fading in slowly:

```jsonc
{ "src": "/pad/xy", "target": 0, "slot": "comp", "param": "Gain",
  "arg": 1, "in_min": -1.0, "in_max": 1.0, "curve": "exp" }
```

## Learning the surface addresses

You do not have to know what your surface sends. The engine records every
received message (in **all** modes, including `off`) in a small recent list —
deduped by address, newest first, bounded to 24 entries — exposed as
`ProxyEngine::recent()`. The VST3 editor shows it as the **RECEIVED** panel
(top-right of the route table): double-click a line to fill the next empty
route row's SURFACE ADDR (and a sensible ARG when the message carries several
floats), then pick slot/param (or DISCOVER) and press APPLY.

## Standalone usage

```
heddle-proxy [config.json] [options]
  --listen <port>                    override listen port
  --mode off|only-control|full       override mode
  --target <ip:port>                 add a target (repeatable, max 5)
  --route <src> <tgt> <slot> <param> add a route (repeatable)
  --query <ip:port>                  discover a target's slots/params, print, exit
  --timeout <ms>                     discovery timeout (default 1500)
  --stats <sec>                      stats line period on stderr (0 = off)
  -h, --help                         this text
exit: 0 ok; 1 bad config/usage; 2 socket error
```

The CLI `--route` takes the four core fields; the value mapping (`arg`,
`in_min`, `in_max`, `curve`) is set in the JSON config file or the VST editor
(adding it to the CLI would make the positional config file ambiguous).

A config file and the flags can be combined (flags override the file). Example —
loopback fan-out of `/k/0` to two hosts' Gain, full stats every 5 s:

```
heddle-proxy \
  --listen 9100 --mode only-control \
  --target 127.0.0.1:9001 \
  --target 127.0.0.1:9002 \
  --route "/k/0" 0 smoke1 Gain \
  --route "/k/0" 1 smoke1 Gain \
  --stats 5
```

## VST3 usage

1. Install `HeddleProxy.vst3` into the DAW's plugin folder.
2. Put it on a track (it is **audio-dry**: input == output, no DSP).
3. In the editor:
   - Set the **listen port** (where the surface sends).
   - Fill in up to 5 **targets** (ip + port, per-target enable).
   - Build the **routes** (surface addr → target → slot → param), or press
     **DISCOVER** against a target to list its slots/params and double-click a
     row to insert a route.
   - For each route, set the value **mapping**: ARG (which float of the surface
     message is the value), MIN / MAX (surface range to rescale; MIN > MAX
     inverts), and CURVE (identity / exp / invexp / scurve). Defaults leave a
     plain 0..1 slider untouched.
   - Watch the **RECEIVED** panel: every address the surface actually sends
     shows up there (newest first). Double-click a line to fill the next empty
     route row's SURFACE ADDR, then press APPLY.
   - Choose the **mode** (3-state: OFF / ONLY-CONTROL / FULL).
   - **APPLY** to (re)bind. The status line shows mode + live received /
     forwarded / dropped counters.

The editor drives the engine on a `juce::Timer` (~200 Hz) on the **message
thread**; the audio callback never touches the engine.

## Discovery (auto-filling routes)

`DISCOVER` (or `--query`) speaks the Heddle OSC protocol to a target (docs/OSC.md):

1. Sends `/chain/query`.
2. On the `/reply/chain` reply (one string, `\n`-separated lines, each
   `index<TAB>id<TAB>path<TAB>enabled`), reads each slot **id** (column 2).
3. Sends a bare `/chain/<slotId>` per slot to list its params.
4. Collects each `/reply/<slotId>/<title>` param line
   (`id<TAB>title<TAB>value<TAB>min<TAB>max<TAB>steps<TAB>kind…`).

The result (`ProxyDiscovered`) carries, per param: the slot **id** (the token
`resolveSlot()` accepts and that keys the reply), the param **title** (what
`findByName()` matches and what a `/chain/<slot>/<param>` set needs), and the
raw VST3 param id (for reference). Error replies (`"error: …"`, `"(no params)"`)
arrive as strings with no tab / a non-numeric id and are filtered out.

## Threading & transport model

- **One owner thread.** `start()` binds the listen socket and opens one output
  socket per target; `pump()` is **non-blocking** — it drains whatever arrived
  and forwards it. No internal threads or locks: the VST3 editor calls `pump()`
  from a `juce::Timer`; the standalone runs its own loop.
- **Fire-and-forget UDP** to each enabled target (`OscUdp::send` / `sendTo`).
- **No reply echo.** The proxy does **not** echo target replies back to the
  surface (that would loop a surface that also listens on its own output).
  Surfaces that need a set-ack should talk to their target's own reply port,
  not through the proxy.

## Files

- `core/include/heddle/proxy.hpp` — `ProxyConfig`, `ProxyTarget`, `ProxyRoute`,
  `ProxyDiscovered`, `ProxyCurve` + `proxyCurveEval/FromName/Name`,
  `proxy::parse/parseFile/dump`.
- `core/include/heddle/proxy_engine.hpp` + `core/src/proxy.cpp` — `ProxyEngine`
  (JUCE-free; `start`/`stop`/`pump`/`discover`/`recent` + config parse/dump).
- `core/tests/test_proxy.cpp` — loopback tests (config round-trip incl. the
  mapping fields, only-control rewrite + arg pick + rescale + curve, inverted
  range, out-of-range drop, multi-target fan-out, learn list, off-drop,
  discovery parse) against a fake target that answers in the exact Heddle
  reply formats.
- `core/tools/proxy_main.cpp` — the standalone `heddle-proxy` exe.
- `host/proxy_plugin/plugin.cpp` — the passthrough VST3 (`HeddleProxy`).
