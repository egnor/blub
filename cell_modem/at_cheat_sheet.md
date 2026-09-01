# nRF9151 Serial Modem AT cheat sheet

Just the commands the MQTT client uses, for coding and for manual noodling over
the Feather TX/RX at 115200 8N1. Full details live in the firmware docs
(`dev.tmp/nordic/workspace/circuitdojo-ncs-serial-modem/doc/app/`) and, for the
non-`#X` commands, in Nordic's *nRF91x1 AT Commands Reference Guide*.

Commands are `AT...` + CR. Every command ends with `OK` or `ERROR`
(or `+CME ERROR: <n>` when extended errors are on). Unsolicited responses (URCs)
can arrive at any time, including between a command and its `OK`.

## Session setup

| Command | Response | Notes |
|---|---|---|
| `AT` | `OK` | Liveness ping |
| `ATE0` | `OK` | Echo off. Do this first; parsing assumes it |
| `AT#XSMVER` | `#XSMVER: "<sm>","<ncs>","blub"` | Third field is our `CONFIG_SM_CUSTOMER_VERSION` — good build sanity check |
| `AT+CGMM` | `nRF9151-LACA` | Model |
| `AT+CGSN=1` | `+CGSN: "<imei>"` | Stable per-device ID; good MQTT client ID |
| `AT#XUUID` | `#XUUID: <uuid>` | Alternative device ID |
| `AT#XCLAC` | list of `#X` commands | What this build actually supports |
| `AT+CMEE=1` | `OK` | Numeric `+CME ERROR: <n>` instead of bare `ERROR` |

## Radio

`%XSYSTEMMODE` can only be set while the modem is deactivated (`CFUN=0`/`4`),
which is where it sits after boot.

| Command | Response | Notes |
|---|---|---|
| `AT+CFUN?` | `+CFUN: <n>` | `0` off, `1` normal, `4` flight |
| `AT+CFUN=1` | `OK` | Radio on. SM intercepts this to re-subscribe `+CGEV` |
| `AT+CFUN=4` | `OK` | Flight mode — the cheap way to force a rescan |
| `AT+CFUN=0` | `OK` | Full off. **Writes NVM** — avoid on the fast path |
| `AT%XSYSTEMMODE=1,0,0,0` | `OK` | LTE-M only (see "RAT choice" below) |
| `AT%XSYSTEMMODE=1,1,0,1` | `OK` | LTE-M + NB-IoT, LTE-M preferred |
| `AT%XSYSTEMMODE?` | `%XSYSTEMMODE: 1,0,0,0` | Read back |
| `AT+CGDCONT=0,"IP","<apn>"` | `OK` | Only if the SIM needs an explicit APN |

Auto-connect is **off** in this build (`CONFIG_SM_AUTO_CONNECT` unset), so the
client owns radio bring-up — including after `#XMODEMRESET` and after a
`#XMODEM: INIT` recovery.

## Registration and signal

| Command | Response | Notes |
|---|---|---|
| `AT+CEREG=5` | `OK` | Subscribe to registration URCs. **Resets on `CFUN=0`** — re-issue after every trip through 0 |
| `AT+CEREG?` | `+CEREG: <n>,<stat>,...` | Ground truth. Read form has `<n>` first; the URC form does not |
| `AT+CESQ` | `+CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>` | Last two are the LTE ones |
| `AT%XMONITOR` | `%XMONITOR: <stat>,<op>,...,<band>,<cell>,...,<rsrp>,<snr>` | One-shot everything; handy for field debug |
| `AT+CNEC=24` | `OK` | Report EMM/ESM reject causes — tells you *why* registration failed |

`<stat>`: `0` not registered / not searching · `1` **registered, home** ·
`2` searching · `3` denied · `4` unknown · `5` **registered, roaming**.
Treat `1` and `5` as up, everything else as down.

`+CESQ` conversions: RSRP dBm = `<rsrp>` − 141 (`255` = unknown),
RSRQ dB = (`<rsrq>` − 40) / 2. Rough read: RSRP better than −100 is
comfortable, −110 is marginal, −120 is trouble.

## TLS credentials

One-time provisioning of the root CA. The modem has **no built-in trust
store** — without this, TLS connects fail. Modem must be offline.

| Command | Response | Notes |
|---|---|---|
| `AT%CMNG=1,<tag>,0` | `%CMNG: <tag>,0,"<sha256>"` | List: check before writing |
| `AT%CMNG=0,<tag>,0,"<PEM>"` | `OK` | Write root CA. Needs `CFUN=0`. **NVM write** — verify first |
| `AT%CMNG=3,<tag>,0` | `OK` | Delete |

Types: `0` root CA · `1` client cert · `2` client private key · `3` PSK ·
`4` PSK identity. For a certbot/Let's Encrypt broker you need only type `0`
holding ISRG Root X1, and no client credentials unless the broker requires
mutual TLS. PEM must use CRLF line endings.

## MQTT

| Command | Response | `ERROR` means | Notes |
|---|---|---|---|
| `AT#XMQTTCFG="<id>",<keepalive>,<clean>` | `OK` | **Already connected**, or bad `<clean>` | Before connecting. `<clean>`: `0` persistent, `1` clean |
| `AT#XMQTTCFG?` | `#XMQTTCFG: "<id>",<ka>,<clean>` | — | |
| `AT#XMQTTCON=1,"<user>","<pass>","<host>",<port>[,<sec_tag>]` | `OK` then `#XMQTTEVT: 0,<r>` | DNS failed, TCP/TLS failed, or already connected | `1` = IPv4, `2` = IPv6. Add `<sec_tag>` for TLS (port 8883) |
| `AT#XMQTTCON=0` | `OK` then `#XMQTTEVT: 1,<r>` | Not connected, **or** the link was already dead — teardown still happened | Disconnect |
| `AT#XMQTTCON?` | `#XMQTTCON: 0` **or** `#XMQTTCON: 1,"<id>","<url>",<port>[,<tag>]` | — | **Not readiness** — see below. Note: the docs' example for this is wrong — the real fields are client_id and url, not username/password |
| `AT#XMQTTSUB="<topic>",<qos>` | `OK` then `#XMQTTEVT: 7,<r>` | Not CONNACKed — *yet*, or *any more* | One at a time — SUBACK carries no topic to correlate on |
| `AT#XMQTTUNSUB="<topic>"` | `OK` then `#XMQTTEVT: 8,<r>` | Not CONNACKed — *yet*, or *any more* | |
| `AT#XMQTTPUB="<topic>","<msg>",<qos>,<retain>` | `OK` | Not CONNACKed, or bad `<qos>`/`<retain>` | Inline. Only safe if the payload has no `,` `"` CR or LF |
| `AT#XMQTTPUB="<topic>","",<qos>,<retain>` | `OK`, enters data mode | as above | Then payload, then terminator |
| `AT#XMQTTPUB="<topic>","",<qos>,<retain>,<len>` | `OK`, enters data mode | as above, plus `<len>` > `CONFIG_SM_DATAMODE_BUF_SIZE` | Counted — no terminator, no escaping. **Needs upstream PR #381** (cherry-picked in `nrf9151_build_setup.py`) |

**It is always a bare `ERROR`, never `+CME ERROR`.** `sm_at_cb_wrapper()` only
reconstructs `+CME`/`+CMS ERROR` when a handler returns a *positive* value (the
`nrf_modem_at_cmd()` encoded-error convention, used by proxying commands like
`AT#XSMS`). Every MQTT handler returns a negative errno, which never reaches
the wire — the `-ENOTCONN`/`-EINVAL`/`-EISCONN` values named above are internal
to the firmware and useful only for reading the source. `AT+CMEE=1` does not
change this; it governs libmodem's own `AT+`/`AT%` errors, not SM's `#X`
handlers.

**"Not CONNACKed — yet, or any more"** is the one ambiguity that matters.
Distinguish by whether you have seen `#XMQTTEVT: 0,0` for the *current* connect
attempt: before that it is the dead zone and you wait; after it, the session is
gone and it is a genuine "reconnect now" signal. A publish that fails on a dead
socket also *causes* the teardown (`client_write()` →
`mqtt_client_disconnect(notify=true)`), so expect `#XMQTTEVT: 1,<errno>` right
behind the `ERROR`.

**No client-side MQTT state.** The firmware does not retransmit or track
packet IDs — QoS > 0 gets you an ack notification, not delivery. Documented
deviation from MQTT v3.1.1.

### Two notions of "connected"

`sm_at_mqtt.c` sets its own `ctx.connected` as soon as `mqtt_connect()`
returns — *before* CONNACK. Zephyr's `verify_tx_state()` gates every packet on
`MQTT_STATE_CONNECTED`, set only when CONNACK arrives **accepted**. They
disagree for a whole round trip:

```
AT#XMQTTCON=1,...           <- blocks: DNS + TCP + TLS handshake + CONNECT write
OK                          <- CONNECT packet is on the wire, nothing more
                            <- #XMQTTCON? now says 1, but PUB/SUB give ERROR
#XMQTTEVT: 0,0              <- CONNACK. NOW it is usable.
```

**Readiness is `#XMQTTEVT: 0,0`, nothing else.** Gate the state machine on
that and never poll `#XMQTTCON?` in the normal path; keep it only as a resync
check for "did the SM reset behind my back."

**The pending-CONNACK dead zone.** In that window `PUB`, `SUB` *and*
`#XMQTTCON=0` all return `ERROR` (`-ENOTCONN`) — `mqtt_disconnect()` checks
`verify_tx_state()` too. There is nothing you can do but wait. Worse,
`do_mqtt_disconnect()` sets `ctx.disconnect_requested` *before* the call that
fails and returns early, so the flag stays set for the life of the connection;
the poll thread then treats a later genuine `POLLNVAL` as an expected
disconnect and exits without `mqtt_abort()`. **Do not try to disconnect out of
this state.**

It is bounded: with no CONNACK, poll times out after the keepalive,
`mqtt_live()` → `mqtt_ping()` fails `-ENOTCONN`, the thread aborts the
connection and emits `#XMQTTEVT: 1,-113`. So the hang is ~1 keepalive.

### Timeouts and blocking

Nothing sets `SO_RCVTIMEO`/`SO_SNDTIMEO`; `getaddrinfo` and `connect` are
plain blocking calls, so DNS and TCP bounds come from the modem firmware and
are not visible in the source. **Our own AT deadline is the real timeout**, and
a blown deadline means a modem not answering its UART — a reset-ladder event,
not something cancellable. One deadline for all commands does not work here:

| Command | Blocks for | Budget |
|---|---|---|
| `#XMQTTCFG`, `#XMQTTCON?`, `#XMQTTSUB`, `#XMQTTUNSUB`, `#XMQTTPUB` | a socket write at most | short |
| `#XMQTTCON=1` | DNS + TCP + **TLS handshake** | tens of seconds |
| `#XMQTTCON=0` | `k_thread_join(..., K_SECONDS(CONFIG_MQTT_KEEPALIVE))` | **> keepalive**, or a clean teardown reads as a wedge |

`CONFIG_MQTT_KEEPALIVE` is commented out in the SM `prj.conf`, so it is
Zephyr's default **60 s**. `CONFIG_MQTT_CLEAN_SESSION=y`.

### Teardown, and who does it

The firmware cleans up after itself in every ordinary case — read error, write
error, `POLLERR`/`POLLHUP`/`POLLNVAL` all close the transport and emit
`#XMQTTEVT: 1,<errno>`. **Do not proactively disconnect on our own view of
network state**; we would only be racing the firmware. Two exceptions:

1. **Before tearing the PDN down ourselves** (`CFUN=0`/`4`, modem reset) — the
   broker gets a clean DISCONNECT instead of a half-open session.
2. **When our watchdog fires before the firmware's does** — the firmware only
   notices a silently blackholed path via missed PINGRESPs, i.e. ~2×keepalive
   (~120 s). Anything shorter is ours to detect, and `#XMQTTCON=0` is the first
   ladder rung.

`ERROR` from `#XMQTTCON=0` on a dead link is **not** a failure to clean up: the
DISCONNECT write fails, which itself triggers full teardown, and only then does
`do_mqtt_disconnect()` return the error without joining the thread.

The one case that does *not* self-clean is `#XMODEM: FAULT`.
`nrf_modem_lib_shutdown()` runs from a work queue in `main.c` with no hook into
the MQTT module: `ctx.connected` stays true over a dead fd. Drop all state and
use the reset ladder — a graceful disconnect is not reliable there.

### `#XMQTTCFG` is the one command that fails when things are going well

`do_mqtt_config()` returns `-EINVAL` if connected. It also calls
`mqtt_client_init()`, which wipes stale client state — so it is exactly what we
want on every reconnect. Config → connect belongs on the disconnected path
only, never in the steady-state poll.

### Session state: use clean sessions

**`session_present` from CONNACK is unreachable.** Zephyr decodes it into
`evt->param.connack.session_present_flag`, but `sm_at_mqtt.c` builds the URC
from `evt->type` and `evt->result` only, so `#XMQTTEVT: 0,0` carries no hint
either way, and no other command exposes it. With `<clean>=0` there is no way
to know whether the broker still holds our subscriptions.

The workaround is to **re-subscribe unconditionally on every connect**.
SUBSCRIBE is idempotent — resubscribing an existing topic just replaces the
subscription and its granted QoS. Since SUBACK carries no correlation info we
serialize them anyway, so the cost is one round trip per topic per reconnect.

But we use `<clean>=1`, for two reasons beyond simplicity:

* A persistent session buys nothing on the uplink side. The firmware keeps no
  packet-ID state and never retransmits, so nothing protects our telemetry
  across a disconnect either way.
* Its one real benefit — the broker queuing QoS ≥ 1 downlink while we are
  offline — is a hazard here. Return from a long outage and the whole backlog
  arrives at once, into a firmware that streams inbound payloads with no size
  bound, through a URC ring that **resets itself on overflow**. A stale command
  from hours ago is rarely worth executing anyway.

Keep the stable IMEI client ID regardless: it keeps broker-side logging and ACLs
sane, and makes a reconnect displace our own stale connection instead of
accumulating ghosts.

### Data mode

Entered by any `#XMQTTPUB` with an empty `<msg>`. Exit by sending the
terminator, or automatically once `<len>` bytes arrive (PR #381 path).

Our terminator is **`!"#$%`**, not the stock `+++`
(`CONFIG_SM_DATAMODE_TERMINATOR` in `nrf9151_serial_modem.conf`) — chosen
because the sequence can't occur inside a JSON string. Exit is confirmed by
`#XDATAMODE: 0` (`-1` = failure). Once `<len>` is available, the terminator
hack can go away.

## URCs

| URC | Meaning |
|---|---|
| `Ready` | SM booted / reset completed. **Invalidate all state** |
| `INIT ERROR` | SM failed to initialize |
| `+CEREG: <stat>,...` | Registration changed (no `<n>` field in URC form) |
| `+CGEV: ...` | Packet-domain events; SM subscribes via `AT+CGEREP=1` on `CFUN=1` |
| `#XMODEM: FAULT,<reason>,<pc>` | Modem crashed |
| `#XMODEM: SHUTDOWN,<r>` | libmodem torn down — MQTT is dead, drop all state |
| `#XMODEM: INIT,<r>` | libmodem back up — **redo radio bring-up and reconnect** |
| `#XDATAMODE: <0\|-1>` | Data mode exited |
| `#XMQTTEVT: <type>,<result>` | See table below. `<result>` 0 = ok, negative = errno — **except CONNACK**, see below |
| `#XMQTTMSG: <topic_len>,<msg_len>` | Inbound message header — see framing note |

### `#XMQTTEVT` types

These are Zephyr's `mqtt_evt_type` values, emitted for *every* MQTT event:

| `<type>` | Fires when | `<result>` |
|---|---|---|
| `0` CONNACK | broker answered our CONNECT | **Special — positive on failure.** See below |
| `1` DISCONNECT | session ended, for any reason | `0` if *we* asked (`#XMQTTCON=0`); otherwise the negative errno that killed it. **The only event where a nonzero result is informational rather than fatal** |
| `2` PUBLISH | inbound message, with `#XMQTTMSG` | **always `0`** — the handler overwrites `evt->result` with `handle_mqtt_publish_evt()`'s unconditional `0`, so a decode failure never surfaces here (it still kills the connection, so watch for `1`) |
| `3` PUBACK | our QoS 1 publish was acked | decode status; `0` in practice |
| `4` PUBREC | QoS 2, step 2 of 4 | **not** the decode status — the result of the PUBREL the firmware sends back |
| `5` PUBREL | QoS 2, step 3 of 4 | **not** the decode status — the result of the PUBCOMP the firmware sends back |
| `6` PUBCOMP | QoS 2, step 4 of 4 | decode status; `0` in practice |
| `7` SUBACK | broker answered a SUBSCRIBE | decode status only — **can lie**, see below |
| `8` UNSUBACK | broker answered an UNSUBSCRIBE | decode status; `0` in practice |
| `9` PINGRESP | keepalive ping answered | **always `0`** — `mqtt_rx.c` never assigns a result for this type |

At QoS 0 you only ever see `0`, `1`, `2`, `7`, `8`, `9`. `9` arrives once per
keepalive interval and is the cheapest proof the whole path (radio → PDN →
broker) is alive.

**The simplifying rule:** for every type *except* `1`, a nonzero `<result>` is
fatal. All of `2`–`9` get their result from a decode function, and `client_read()`
tears the connection down on any negative return from `mqtt_handle_rx()`. So a
nonzero result on those is always followed by `#XMQTTEVT: 1,<errno>`. Treat
"nonzero on anything but DISCONNECT" as "the connection is gone" and don't
bother decoding further.

**CONNACK (`0`) breaks the errno rule.** On failure `<result>` is the *positive*
MQTT return code, not an errno: `1` bad protocol version, `2` identifier
rejected, `3` server unavailable, `4` bad credentials, `5` not authorized.
`4` and `5` mean stop retrying — the config is wrong. `3` means back off.
A rejected connect always arrives as **two** URCs: `#XMQTTEVT: 0,<code>`
followed by `#XMQTTEVT: 1,-111` (`-ECONNREFUSED`) from the library's own
teardown. (A malformed CONNACK gives a *negative* result instead, so sign is
what distinguishes "broker said no" from "packet was garbage".)

**SUBACK (`7`) can lie.** `<result>` is only the *decode* status. The per-topic
return codes land in `param.suback.return_codes`, which `sm_at_mqtt.c` never
reads — so a broker-refused subscription (`0x80`) and a QoS downgrade both
report `#XMQTTEVT: 7,0`, identical to success. If a subscription being live
actually matters, prove it with an application-level round trip.

### Which errno?

**Zephyr/newlib values, not Linux ones.** They come from the Zephyr SDK
toolchain's `sys/errno.h` (matching `zephyr/lib/libc/minimal/include/errno.h`),
which is Linux-*flavored* but diverges exactly where it hurts — the socket
range. Nothing here is 3GPP; those numbers only show up in `+CME ERROR` and
`+CEER`, which this path never produces.

| | | | |
|---|---|---|---|
| `11` EAGAIN | `22` EINVAL | `71` EPROTO | `110` ESHUTDOWN |
| `111` ECONNREFUSED | `113` ECONNABORTED | `114` ENETUNREACH | `115` ENETDOWN |
| `116` ETIMEDOUT | `122` EMSGSIZE | `126` ENETRESET | `127` EISCONN |
| `128` ENOTCONN | | | |

The traps: **ECONNABORTED is 113, not Linux's 103** (this is what a firmware-side
`mqtt_abort()` reports, so it is the one you will see most), ENOTCONN is 128 not
107, and ENETRESET is 126 not 102. Do not decode these with a host `errno.h`.

### Inbound message framing

`#XMQTTMSG` is **not line-oriented**. The payload is raw and may contain CR,
LF, `"`, or NUL. Read it by **byte count**, never by line. Payload size is
unbounded by the firmware — it streams straight through — so cap it
client-side and discard the excess.

The documented order is:

```
#XMQTTMSG: <topic_len>,<msg_len>CRLF
<topic_len bytes>CRLF
<msg_len raw bytes>CRLF
#XMQTTEVT: 2,0
```

**⚠ On our pinned build the header comes LAST.** Confirmed on hardware —
subscribing to `test` and publishing `test` to it produces:

```
test                                     <- topic, no leading delimiter
test                                     <- payload
#XMQTTMSG: 4,4                           <- header, after the data it describes
#XMQTTEVT: 2,0
```

Why: `handle_mqtt_publish_evt()` takes `sm_at_host_lock()`, which increments
`executing_lock`; `is_idle_ctx()` requires that to be `0`; and `urc_send_to()`
on a pipe-specific ctx appends to `ctx->buffered_urcs`, flushing only when idle.
So the header is *queued* inside the lock while topic and payload go straight
out via `data_send()`, and the header lands at unlock.

This is unparseable in general, not merely awkward. The data block has **no
leading delimiter** — the topic just starts — so without a header first there is
nothing to detect the start of a message, and since payloads are raw, one
containing `\r\n#XMQTTEVT: 2,0` is indistinguishable from the real thing.
Trivial payloads only *look* readable.

**Provenance:** a regression, not code that never worked. `git log -L` on the
line: the NCS import (`0b6369c`) had `rsp_send()` — immediate, correct. Nordic's
`62061b1` *"app: Allow targeting responses to a pipe"* (3 Mar 2026) swept it to
`urc_send_to()`. That same commit is the one that *defines* `rsp_send_to()`, so
the correct replacement existed in the changeset that broke it. Upstream
`7c1cb92` (Aug 2026) puts it back.

**Local fix:** one line in `handle_mqtt_publish_evt()`, `urc_send_to` →
`rsp_send_to` for the `#XMQTTMSG` line, alongside the existing PR #381
cherry-pick. Do not cherry-pick `7c1cb92` for this — the fix is entangled with a
poll-callback rewrite and a malloc refactor.

## Upstream drift

We are pinned to the circuitdojo fork. Nordic
[`7c1cb92`](https://github.com/nrfconnect/ncs-serial-modem/commit/7c1cb929e417f22ec5396f5733e591da00c26006)
(*app: Refactor MQTT to use work queue and dynamic memory*, Aug 2026) rewrites
`sm_at_mqtt.c`: the dedicated 2 KB polling thread becomes a one-shot `SO_POLLCB`
callback dispatched onto `sm_work_q`, keepalive becomes a delayable work item,
and buffers/strings move into a single `calloc`'d struct that exists only while
connected. Not in the fork yet. Nothing here forces our hand — accept it in due
course, but know what changes:

**Fixes for us**

* **The inbound framing bug above** (confirmed on hardware). The `#XMQTTMSG`
  header switches back to `rsp_send_to()` so it precedes the payload. This is
  the only reason to care about this commit at all, and it is cheaper to patch
  locally.
* **`#XMQTTCON=0` stops blocking.** `k_thread_join(..., K_SECONDS(CONFIG_MQTT_KEEPALIVE))`
  is gone, replaced by `k_work_cancel_delayable()` and immediate teardown. The
  60-second worst case in *Timeouts and blocking* disappears.

**New behavior to watch for**

* **`#XMQTTCON=1` gains `-ENOMEM`** — the connection struct is allocated per
  connect. Another meaning for `ERROR`.
* **A slow inbound payload holds the AT host lock across work invocations.**
  On `-EAGAIN` the poll handler returns *still holding* `sm_at_host_lock()` and
  re-arms. Framing stays atomic (good), but a large or stalled inbound message
  can now delay our command responses. `mqtt_connection_abort()` has explicit
  code to release the lock if the connection dies mid-drain.

**Unchanged — everything else in this file still applies**

The two notions of "connected", `#XMQTTCON?` reporting pre-CONNACK, the
pending-CONNACK dead zone *including* the `disconnect_requested` poisoning, the
keepalive backstop landing on `#XMQTTEVT: 1,-113`, bare `ERROR` never
`+CME ERROR`, SUBACK unable to report broker refusal, and `session_present`
still dropped on the floor.

## Reset ladder

| Command | Response | Effect |
|---|---|---|
| `AT+CFUN=4` → `AT+CFUN=1` | `OK` | Flight bounce; forces a fresh network scan |
| `AT#XMODEMRESET` | `#XMODEMRESET: 0` | Resets the modem only. **Leaves it at `CFUN=0`** — redo full bring-up |
| `AT#XRESET` | `OK` then `Ready` | Resets the whole SiP |
| (reset line) | `Ready` | Hardware, if wired |

Escalate only, with a hold-down between rungs so a bad cell site doesn't churn
the modem.

## Numbers worth remembering

| | |
|---|---|
| UART | 115200 8N1 = 11,520 B/s ≈ 87 µs/byte |
| MQTT topic | ≤ 128 bytes |
| MQTT client ID | ≤ 64 bytes |
| MQTT control buffer | 512 bytes (excludes payload) |
| Publish payload | keep well under `CONFIG_SM_DATAMODE_BUF_SIZE` = 8192 |
| Inbound payload | unbounded by firmware — cap it yourself |
| SM UART RX slab | 3 × 2048 = 6144 B ≈ 533 ms of drain slack |
| SM URC ring | 8192 B; **resets itself on overflow**, losing queued URCs |
| AT command max | 4096 bytes (a PEM fits) |

The publish limit is the one that matters: exceed the data-mode buffer and SM
transmits mid-stream over LTE, which can stall UART drain for seconds and
silently drop host bytes when hardware flow control is off.
