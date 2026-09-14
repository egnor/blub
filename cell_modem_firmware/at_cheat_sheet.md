# nRF9151 Serial Modem AT cheat sheet

Just the commands the MQTT client uses, for coding and for manual noodling over
the Feather TX/RX at 115200 8N1. Full details live in the firmware docs
(`dev.tmp/nordic/workspace/circuitdojo-ncs-serial-modem/doc/app/`) and, for the
non-`#X` commands, in Nordic's *nRF91x1 AT Commands Reference Guide*.

Commands are `AT...` + CR. Every command ends with `OK` or `ERROR`
(or `+CME ERROR: <n>` when extended errors are on). Unsolicited responses (URCs)
never interleave with a command and its response: the firmware queues them
while the host is busy and flushes them only once the line is idle, so a URC
can arrive **arbitrarily late** — see *URC delivery is deferred* below.

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
mutual TLS.

PEM line endings: LF-only is fine. Nordic's own `modem_key_mgmt_write()` passes
the PEM through `nrf_modem_at_printf()` verbatim, and every cert in the NCS
tree is LF-only; a DevZone thread reports both the original file and a
hand-stripped one-line version importing successfully. The modem stores the
bytes exactly as written (`<CR>`, `<LF>` and all), and the SHA-256 that
`%CMNG=1` reports is over those stored bytes, so **whichever line ending you
pick, `cert_sha256` in the config must be computed over the same bytes** —
`test_blub_cert_sha256` checks this. Over the SM's UART, termination characters
are ignored inside double quotes, so both CR and LF survive the trip either
way. We currently use CRLF; that is a choice, not a requirement.

## Command termination

This build has `CONFIG_SM_CR_TERMINATION=y` (from the Circuit Dojo board
config, "CR only for PuTTY"). In `cmd_rx_handler()` a bare CR outside double
quotes dispatches the command at once; LF is not a terminator. **Send `\r`
only.** CRLF happens to work for ordinary commands because the parser discards
everything until it sees `AT`, so the stray LF is dropped. It does *not* work
for `#XMQTTPUB`: `enter_datamode()` runs synchronously inside the handler, so
the LF after the CR becomes the first payload byte, the message gains a leading
newline, and the last real byte is left over in command mode. Termination
characters are ignored inside quotes, which is why a PEM with CRLF line endings
passes through `AT%CMNG=0` intact; that requirement comes from the modem's PEM
parser, not from the AT line syntax. Responses and URCs always arrive CRLF
terminated regardless.

## MQTT

| Command | Response | `ERROR` means | Notes |
|---|---|---|---|
| `AT#XMQTTCFG="<id>",<keepalive>,<clean>` | `OK` | **Already connected**, or bad `<clean>` | Before connecting. `<clean>`: `0` persistent, `1` clean |
| `AT#XMQTTCFG?` | `#XMQTTCFG: "<id>",<ka>,<clean>` | — | |
| `AT#XMQTTCON=1,"<user>","<pass>","<host>",<port>[,<sec_tag>]` | `OK` then `#XMQTTEVT: 0,<r>` | DNS failed, TCP/TLS failed, or already connected | `1` = IPv4, `2` = IPv6. Add `<sec_tag>` for TLS (port 8883) |
| `AT#XMQTTCON=0` | `OK` then `#XMQTTEVT: 1,0` | Not connected, in the pending-CONNACK dead zone, **or** the link was already dead (then `#XMQTTEVT: 1,<errno>` follows — teardown still happened) | Disconnect. Returns immediately; the event is a deferred URC. See *What `#XMQTTCON=0` does per state* |
| `AT#XMQTTCON?` | `#XMQTTCON: 0` **or** `#XMQTTCON: 1,"<id>","<url>",<port>[,<tag>]` | — | **Not readiness** — see below. Note: the docs' example for this is wrong — the real fields are client_id and url, not username/password |
| `AT#XMQTTSUB="<topic>",<qos>` | `OK` then `#XMQTTEVT: 7,<r>` | Not CONNACKed — *yet*, or *any more* | One at a time — SUBACK carries no topic to correlate on |
| `AT#XMQTTUNSUB="<topic>"` | `OK` then `#XMQTTEVT: 8,<r>` | Not CONNACKed — *yet*, or *any more* | |
| `AT#XMQTTPUB="<topic>","<msg>",<qos>,<retain>` | `OK` | Not CONNACKed, or bad `<qos>`/`<retain>` | Inline. Only safe if the payload has no `,` `"` CR or LF |
| `AT#XMQTTPUB="<topic>","",<qos>,<retain>` | `OK`, enters data mode | as above | Then payload, then terminator |
| `AT#XMQTTPUB="<topic>","",<qos>,<retain>,<len>` | `OK`, enters data mode, then `#XDATAMODE: 0` once `<len>` bytes are in | as above, plus `<len>` > `CONFIG_SM_DATAMODE_BUF_SIZE` (8192 exactly is fine) | Counted — no terminator, no escaping. `<len>` = 0 falls back to terminator mode. Upstream PR #381, now in the fork. See *Data mode* for the idle-timer trap |

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
the poll work handler then treats a later genuine `POLLNVAL` as an expected
disconnect and returns without `mqtt_abort()`. **Do not try to disconnect out of
this state.** (Fixed by upstream PR #435, not yet in the fork — see *What
`#XMQTTCON=0` does per state*.)

It is bounded: with no CONNACK, the keepalive work fires after one keepalive
interval, `mqtt_live()` → `mqtt_ping()` fails `-ENOTCONN`,
`mqtt_connection_abort()` runs and emits `#XMQTTEVT: 1,-113`. So the hang is
~1 keepalive.

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
| `#XMQTTCON=0` | one DISCONNECT socket write | short (17 ms measured). The `#XMQTTEVT: 1,0` that follows is a deferred URC, not part of the response |

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
   notices a silently blackholed path via missed PINGRESPs. The keepalive work
   fires once per keepalive interval of silence, aborts when *more than one*
   PINGREQ is unanswered, so detection is the third firing: ~3×keepalive
   (~180 s at 60 s), reported as `#XMQTTEVT: 1,-113`. Anything shorter is
   ours to detect (`#XMQTTEVT: 9,0` per PINGRESP is the free signal to time),
   and `#XMQTTCON=0` is the first ladder rung.

`ERROR` from `#XMQTTCON=0` on a dead link is **not** a failure to clean up: the
DISCONNECT write fails, which itself triggers full teardown, and only then does
`do_mqtt_disconnect()` return the error without joining the thread.

The one case that does *not* self-clean is `#XMODEM: FAULT`.
`nrf_modem_lib_shutdown()` runs from a work queue in `main.c` with no hook into
the MQTT module: `ctx.connected` stays true over a dead fd. Drop all state and
use the reset ladder — a graceful disconnect is not reliable there.

### What `#XMQTTCON=0` does per state

`do_mqtt_disconnect()` is short and every exit looks different on the wire:

| Firmware state | Response | `#XMQTTEVT: 1,<r>` follows? |
|---|---|---|
| Not connected (`#XMQTTCON?` says `0`) | `ERROR` (`-ENOTCONN`) | **No.** Nothing to tear down, nothing to report |
| CONNACKed, socket alive | `OK` | **Yes, `1,0`.** Zephyr's `mqtt_disconnect()` fires the callback *synchronously inside the command*, but the URC is queued, so it surfaces after `OK` once the line idles |
| CONNACKed, socket dead | `ERROR` | **Yes, `1,<errno>`.** The DISCONNECT write fails, `client_write()` tears down with notify, and only then does the handler return the error |
| Connected, no CONNACK yet (dead zone) | `ERROR` (`-ENOTCONN` from `verify_tx_state()`) | **No**, and `disconnect_requested` is now poisoned — see above |

Rule: after `OK`, exactly one `1,0` is guaranteed and waiting for it is safe.
After `ERROR`, poll `#XMQTTCON?`: `0` means there was nothing to do, go to
`#XMQTTCFG`; `1` means the dead zone — wait for `#XMQTTEVT: 0,x` (CONNACK) or
`1,-113` (keepalive backstop) and do not retry `#XMQTTCON=0`. The dead-socket
`1,<errno>` arrives on the next idle flush and should be consumed, not treated
as news about whatever connection you have started since.

**After upstream PR #435** (`bc527c6`, merged 2026-09-07, **not yet in the
fork**) the two `ERROR` rows for a live `ctx.connected` collapse: when
`mqtt_disconnect()` fails, `do_mqtt_disconnect()` falls back to
`mqtt_connection_abort()` and returns `OK`. So:

| Firmware state | Response | `#XMQTTEVT: 1,<r>` follows? |
|---|---|---|
| Not connected | `ERROR` | no (unchanged) |
| CONNACKed, socket alive | `OK` | `1,0` (unchanged) |
| CONNACKed, socket dead | `OK` | `1,<errno>` from the write failure; the abort after it is a no-op |
| No CONNACK yet (dead zone) | `OK` | **`1,-113`** — the abort path always reports `ECONNABORTED` |

The rule becomes simpler: `OK` means the connection is closed and exactly one
`1,x` is owed, where `x` may be `0`, `-113` or a write errno — **do not read
a nonzero `x` after your own `#XMQTTCON=0` as a failure**. The
`disconnect_requested` poisoning is gone, since the connection it would have
poisoned no longer exists. The abort path also releases `mqtt_conn`, so the
leak below no longer applies to `#XMQTTCON=0`; it still applies to a
`#XMQTTPUB`/`#XMQTTSUB` write failure, after which `#XMQTTCON=0` returns
`ERROR` (`ctx.connected` is already false) without releasing anything.

**Leak on library-initiated teardown.** When the Zephyr client tears the
session down itself (a failed write, from `#XMQTTCON=0`, `#XMQTTPUB` or
`#XMQTTSUB` on a dead socket) the DISCONNECT callback only clears
`ctx.connected`; `mqtt_conn_release()` is not called and the keepalive work is
not cancelled. The next `#XMQTTCON=1` does `mqtt_conn = calloc(...)` without
checking, so the previous ~1.5 KB struct leaks each time. `mqtt_connection_abort()`
(the poll/keepalive path) and the `OK` path both release properly. Worth
fixing upstream; until then, prefer letting the firmware notice a dead link
(POLLHUP/keepalive) over writing to it.

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

Entered by any `#XMQTTPUB` with an empty `<msg>`. The `OK` is sent *after*
`enter_datamode()` runs inside the command handler, so payload bytes may follow
the `OK` immediately. Exit by sending the terminator, or automatically once
`<len>` bytes arrive (PR #381 path). Either way the exit is announced by
`#XDATAMODE: <r>` via `rsp_send()` (immediate, not queued): `0` means the
payload was handed to `mqtt_publish()`, `-1` means the send failed. There is
no further `OK`.

Counted mode, verified in `sm_at_host.c` / `sm_at_mqtt.c`:

* `<len>` is checked against `CONFIG_SM_DATAMODE_BUF_SIZE` up front and
  rejected with `ERROR` if larger. Up to and including 8192 is accepted.
* The whole payload accumulates in the data-mode ring (reset to empty on
  entry) and goes out as **one** `mqtt_publish()` when the count hits zero.
  So the exact bound is 8192, not "well under" — that advice is for
  terminator mode, where a full ring flushes mid-stream with `MORE_DATA`
  set, which the MQTT handler rejects with `-EOVERFLOW`.
* Terminate the command with a bare CR. Any byte after the CR — an LF in
  particular — is already payload. See *Command termination*.
* `<len>` = 0 is *not* an empty publish: it selects terminator mode. An empty
  MQTT payload cannot be published through the counted path.
* **⚠ The data-mode inactivity timer still runs in counted mode.** Every
  received byte restarts `data_inactivity_timer`; when it fires with data in
  the ring, `raw_send_scheduled()` publishes whatever has arrived as a
  complete MQTT message, and the rest becomes a *second* message when the
  count completes. The period is
  `CONFIG_SM_UART_RX_BUF_SIZE × 10 bits × 1000 / baud + UART_RX_MARGIN_MS`
  = 2048 × 10 × 1000 / 115200 + 10 ≈ **187 ms**. The host must never pause
  longer than that between payload bytes, and nothing reports the split
  afterwards. (`AT#XDATACTRL=<ms>` can only raise it.)

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

### URC delivery is deferred

Every URC — libmodem's (`+CEREG`, `+CGEV`) via `urc_send()`, and the `#X`
modules' via `urc_send_to()` — is **queued, not written**. `sm_at_host.c`
flushes the queue only when the host is *idle*: no command executing
(`executing_lock == 0`) **and** the idle timer expired. The idle timer restarts
on **every received byte** and is stopped when a full command line arrives; a
URC that finds the host busy re-arms a 100 ms retry (`URC_RETRY_DELAY`, or
`CONFIG_SM_URC_DELAY_WITH_INCOMPLETE_ECHO_MS` = 1 s if echo is on and a partial
command is pending). `rsp_send()`, `rsp_send_to()` and `data_send()` also flush
the queue first, but only if the host is idle at that moment, which it is not
while a command is executing.

Consequences:

* A URC is **never** delivered between a command and its final `OK`/`ERROR`.
* There are two queues: a global ring for libmodem URCs (`urc_send()`) and a
  per-pipe list for `#X` module URCs (`urc_send_to()`). Order is preserved
  within each, and a flush drains the global ring first, so a `+CEREG` and a
  `#XMQTTEVT` can swap places relative to each other. Nothing is lost short of
  global ring overflow, which resets the *whole* ring.
* A host that sends the next command as soon as it sees `OK` **starves the
  queue indefinitely**. The URC comes out after whichever command finally
  leaves the line quiet, up to ~100 ms later.
* A URC can therefore be *older* than a response received before it. A
  `+CEREG` URC generated before `AT%XMONITOR` ran can arrive after
  `%XMONITOR`'s answer and describe a state that answer already superseded.

Observed on hardware, back-to-back commands with no idle gap:

```
6.639  AT#XMQTTCON=0            (tearing down a session left from the last run)
6.656  OK                        <- #XMQTTEVT: 1,0 was queued inside this command
6.656  AT#XMQTTCFG=...
6.665  OK
6.665  AT#XMQTTCON=1,...         <- blocks ~5 s in DNS + TCP + TLS
11.819 OK                        <- CONNECT written
11.920 #XMQTTEVT: 1,0            <- the 6.6 s disconnect, 100 ms after the line idled
12.451 #XMQTTEVT: 0,0            <- CONNACK for the 6.665 connect
```

A client that reads the `1,0` as "the new session died" will issue
`#XMQTTCFG` (`ERROR`, `-EINVAL`: connected) and `#XMQTTCON=1` (`ERROR`,
`-EISCONN`) and then be rescued by the CONNACK. It works by accident.

How to live with it, without sleeping between commands:

* **Treat poll responses as truth and URCs as triggers.** `%XMONITOR`,
  `+CGPADDR`, `#XMQTTCON?` describe the state at the moment they ran. A URC
  says "something changed, re-poll" — do not copy its payload into state
  as if it were newer than the last response.
* **Count MQTT events, don't interpret them by current state.** Each
  `#XMQTTCON=1` that returned `OK` owes exactly one `0,x`; each session owes
  exactly one `1,x`; each `#XMQTTSUB` that returned `OK` owes one `7,x`. Match
  each event to the oldest outstanding debt. A `1,x` arriving while a `1,x`
  is owed for a *previous* session is that session's, not this one's.
* **Wait for what is owed before moving on.** After `#XMQTTCON=0` → `OK`,
  wait for the `1,0` before sending `#XMQTTCFG`. This costs one idle window
  (~100 ms) and removes the ambiguity; it is an event wait, not a sleep.
* There is no command that forces a flush: a no-op `AT` just defers the
  queue again. Only an idle gap does it. If some transition truly needs the
  queue drained, leave the line quiet for one retry window (100 ms) and
  accept that it is a sleep; the debt model above is how to avoid needing
  one.

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
unbounded by the firmware — it streams straight through in
`MQTT_MAX_TOPIC_LEN` (128 B) chunks — so cap it client-side, but keep
counting through the excess to stay in sync. The topic is truncated to 128
bytes by the firmware, so `<topic_len>` never exceeds that.

The whole block — header, topic, CRLF, payload, CRLF, `#XMQTTEVT: 2,0` — is
emitted while `mqtt_poll_work_handler()` holds `sm_at_host_lock()`. Nothing
interleaves: no URC, no command response. A command in flight simply gets its
response after the block, so a large inbound message (8 KB ≈ 711 ms of UART
time) can blow a short command deadline. Size deadlines by bytes moved on the
wire, not by command.

Framing (correct since the fork rebased past upstream `7c1cb92`; older fork
builds get it backwards — see below):

```
#XMQTTMSG: <topic_len>,<msg_len>CRLF
<topic_len bytes>CRLF
<msg_len raw bytes>CRLF
#XMQTTEVT: 2,0
```

Verified on hardware: subscribing to `test` and publishing `test message body`
gives `#XMQTTMSG: 4,17`, then `test`, then the 17 payload bytes, then
`#XMQTTEVT: 2,0`.

**⚠ Fork builds before `268e839` (Sep 2026) get this backwards** — check
`AT#XSMVER` before trusting one. There, `handle_mqtt_publish_evt()` takes
`sm_at_host_lock()`, which increments
`executing_lock`; `is_idle_ctx()` requires that to be `0`; and `urc_send_to()`
on a pipe-specific ctx appends to `ctx->buffered_urcs`, flushing only when idle.
So the header is *queued* inside the lock while topic and payload go straight
out via `data_send()`, and the header lands at unlock, after the bytes it
describes:

```
test
test message body
#XMQTTMSG: 4,17          <- flushed at unlock, too late to be useful
#XMQTTEVT: 2,0
```

That is unparseable in general, not merely awkward. The data block has **no
leading delimiter** — the topic just starts — so without a header first there is
nothing to detect the start of a message, and since payloads are raw, one
containing `\r\n#XMQTTEVT: 2,0` is indistinguishable from the real thing.
Trivial payloads only *look* readable.

**Provenance:** a regression, not code that never worked. `git log -L` on the
line: the NCS import (`0b6369c`) had `rsp_send()` — immediate, correct. Nordic's
`62061b1` *"app: Allow targeting responses to a pipe"* (3 Mar 2026) swept it to
`urc_send_to()`. That same commit is the one that *defines* `rsp_send_to()`, so
the correct replacement existed in the changeset that broke it. Upstream
`7c1cb92` (Aug 2026) puts it back and the fork now carries it, so the local
patch we used to apply in `nrf9151_build_setup.py` is gone (`LOCAL_PATCHES` is
empty).

## Upstream drift

We are pinned to the circuitdojo fork, currently `268e839` (Sep 2026), which
has rebased onto Nordic's Aug 2026 work-queue refactors. Everything in this file
describes that tree. Two of those commits changed behavior we care about:

[`7c1cb92`](https://github.com/nrfconnect/ncs-serial-modem/commit/7c1cb929e417f22ec5396f5733e591da00c26006)
*app: Refactor MQTT to use work queue and dynamic memory* — the dedicated
polling thread became a one-shot `SO_POLLCB` callback dispatched onto
`sm_work_q`, keepalive became a delayable work item, and buffers/strings live in
one `calloc`'d struct that exists only while connected. Effects:

* **The inbound framing bug is fixed** (`#XMQTTMSG` header via `rsp_send_to()`).
* **`#XMQTTCON=0` no longer blocks** on a thread join; it returns after the
  DISCONNECT write.
* **`#XMQTTCON=1` gains `-ENOMEM`** as another meaning for `ERROR`.
* **A slow inbound payload holds the AT host lock across work invocations**,
  so a large or stalled inbound message can delay our command responses.
* **The teardown leak** described under *What `#XMQTTCON=0` does per state*.

[`68c9897`](https://github.com/nrfconnect/ncs-serial-modem/commit/68c9897e8c6cbdf67310baa10dc5f285e33c78eb)
*app: Add work queue for long blocking operations* — only nRF Cloud uses it so
far. `#XMQTTCON=1` still runs on `sm_work_q`, so while it blocks in DNS/TCP/TLS
**nothing else on that queue runs**: no other AT command, no MQTT poll work, no
URC flush.

Pending, merged upstream but not in the fork: #431 (`928c805`, data-mode
`<data_len>` after a send failure) and #435 (`bc527c6`, `#XMQTTCON=0` always
closes). Both are small and could go into `LOCAL_PATCHES` if the fork lags.

Unchanged: the two notions of "connected", `#XMQTTCON?` reporting pre-CONNACK,
the pending-CONNACK dead zone including the `disconnect_requested` poisoning,
the keepalive backstop landing on `#XMQTTEVT: 1,-113`, bare `ERROR` never
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
| Publish payload (counted) | ≤ `CONFIG_SM_DATAMODE_BUF_SIZE` = 8192 exactly; larger is `ERROR`; 0 is terminator mode |
| Publish payload, max gap between bytes | ≈ 187 ms, else the idle timer splits it into two messages |
| Inbound payload | unbounded by firmware — cap it yourself, but count through the rest |
| Inbound topic | truncated to 128 by firmware |
| SM UART RX slab | 3 × 2048 = 6144 B ≈ 533 ms of drain slack |
| SM UART TX buffer | 256 B; beyond that `data_send()` blocks the work queue at UART rate |
| SM URC ring | 8192 B; **resets itself on overflow**, losing queued URCs |
| AT command max | 4096 bytes (a PEM fits) |
| Longest non-payload command we send | `#XMQTTCON=1` ≈ 223 B with max user/pass/host |

Host side, arduino-pico 6.0.0 `Serial1` on the RP2040 (no flow-control wiring):

| | |
|---|---|
| RX | 32 B hardware FIFO + 32 B IRQ-fed software queue by default ≈ **5.5 ms** of slack, then silent loss (`Serial1.overflow()` is the only tell). Call `Serial1.setFIFOSize(n)` before `begin()`; 2048 ≈ 178 ms |
| TX | no software buffer; `write()` goes straight into the 32 B hardware FIFO, `availableForWrite()` is 0/1. A non-blocking writer moves ≤ 32 B per `poll()`, so an 8 KB publish takes ≥ 256 polls — and must not pause > 187 ms |

Two consequences follow. Terminator-mode publishes that exceed the data-mode
buffer are transmitted mid-stream over LTE, which can stall UART drain for
seconds and silently drop host bytes. And a publish in flight while an inbound
message is streaming has the SM work queue blocked on outbound UART, so host
bytes pile into the 6 KB RX slab; keep outbound payloads comfortably inside
that if both directions can be busy at once.
