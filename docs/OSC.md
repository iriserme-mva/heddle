# Weft OSC protocol

Transport: **OSC 1.0 over UDP** (the same framing works over TCP).
Types used: `f` (float32), `i` (int32), `s` (string), `b` (blob).

The Weft host listens on a configurable port (default `9001`) and optionally
sends replies to the client's source address.

## Addressing

### Commands (client → host)

| Address | Args | Meaning |
|---|---|---|
| `/chain/query` | (none) | list slots → reply `/reply/chain` |
| `/chain/<slot>/<param>` | `f` value | **set param by name** (value normalized 0..1) |
| `/chain/<slot>/<param>` | `i` id, `f` value | **set param by id** (first arg is the param id) |
| `/chain/<slot>/reset` | (none) | reset that slot's params to plugin defaults |
| `/chain/reset` | (none) | reset all slots |
| `/chain/apply` | `s` path | (re)load a config file; hot-apply (no reload of unchanged plugins) |
| `/chain/config` | `b` json blob | (re)load config from a raw JSON blob |

**Slot token** `<slot>` is either the slot `id` from the config (e.g. `comp`)
or the numeric index as a string (e.g. `0`). Resolution is done by `Chain`.

**Disambiguation of set-by-name vs set-by-id:** with one float argument the
message sets by *param name*; with `int, float` it sets by *param id*.

### Replies (host → client)

| Address | Args | Meaning |
|---|---|---|
| `/reply/chain` | `s` | one line per slot: `index<TAB>id<TAB>path<TAB>enabled` |
| `/reply/<slot>` | `s` per param | all params of the slot, one message each addressed to `/reply/<slot>/<param>`: line = `id<TAB>title<TAB>value<TAB>min<TAB>max<TAB>steps<TAB>kind`, plus one extra tab-separated column per `listItems` label (enum/chord params) |
| `/reply/<slot>/<param>` | `f` | ack of a set: the param's current normalized value |
| `/status` | `s` | host status string (e.g. `loaded: 3 slots`) |

### Pushes (host → client, unsolicited)

| Address | Args | Meaning |
|---|---|---|
| `/change/<slot>/<param>` | `f` | a plugin changed its own param (UI touch, internal automation); current normalized value |

## Examples

```
# set "Threshold" of slot "comp" to 0.6
/chain/comp/Threshold  f(0.6)

# set param id 17 of slot index 0 to 0.25
/chain/0/Cutoff  i(17) f(0.25)

# reset slot "fx2"
/chain/fx2/reset

# hot-apply a new config file
/chain/apply  s("/home/me/rigs/main.json")

# hot-apply an inline config
/chain/config  b('{"sample_rate":48000,"block_size":512,"chain":[]}')
```

## Implementation notes

- `OscUdp` (core/include/weft/osc_io.hpp): `bind()` to listen, `openOut()` to
  send, `poll()` is non-blocking — the host calls it from its timer.
 `poll(msg, &src)` captures the sender address of the last packet and
 `sendTo(msg, src)` replies to that explicit address (client-reply routing).
- `osc_addr::classify()` maps a raw `OscMsg` to a `ParamAction`; unknown
  prefixes classify as `Unknown` (ignored, no error push).
- The decoder rejects malformed input (unterminated strings, missing tag
  separator) and returns `false` — no partial state is applied.
- The *core* library owns the message model + addressing; the *app* owns the
  UDP sockets (split by design: wrapper describes params, app transports).
