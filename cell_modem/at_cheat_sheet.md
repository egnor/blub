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

| Command | Response | Notes |
|---|---|---|
| `AT#XMQTTCFG="<id>",<keepalive>,<clean>` | `OK` | Before connecting. `<clean>`: `0` persistent, `1` clean |
| `AT#XMQTTCFG?` | `#XMQTTCFG: "<id>",<ka>,<clean>` | |
| `AT#XMQTTCON=1,"<user>","<pass>","<host>",<port>[,<sec_tag>]` | `OK` then `#XMQTTEVT: 0,<r>` | `1` = IPv4, `2` = IPv6. Add `<sec_tag>` for TLS (port 8883) |
| `AT#XMQTTCON=0` | `OK` then `#XMQTTEVT: 1,<r>` | Disconnect |
| `AT#XMQTTCON?` | `#XMQTTCON: 0` **or** `#XMQTTCON: 1,"<id>","<url>",<port>[,<tag>]` | Ground truth for the watchdog. Note: the docs' example for this is wrong — the real fields are client_id and url, not username/password |
| `AT#XMQTTSUB="<topic>",<qos>` | `OK` then `#XMQTTEVT: 7,<r>` | One at a time — SUBACK carries no topic to correlate on |
| `AT#XMQTTUNSUB="<topic>"` | `OK` then `#XMQTTEVT: 8,<r>` | |
| `AT#XMQTTPUB="<topic>","<msg>",<qos>,<retain>` | `OK` | Inline. Only safe if the payload has no `,` `"` CR or LF |
| `AT#XMQTTPUB="<topic>","",<qos>,<retain>` | `OK`, enters data mode | Then payload, then terminator |
| `AT#XMQTTPUB="<topic>","",<qos>,<retain>,<len>` | `OK`, enters data mode | Counted — no terminator, no escaping. **Needs upstream PR #381** (cherry-picked in `nrf9151_build_setup.py`) |

`SUB`/`UNSUB`/`PUB` return `ERROR` (`-ENOTCONN`) when not connected, so a
failed publish is a reliable "reconnect now" signal.

**No client-side MQTT state.** The firmware does not retransmit or track
packet IDs — QoS > 0 gets you an ack notification, not delivery. Documented
deviation from MQTT v3.1.1.

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
| `#XMQTTEVT: <type>,<result>` | See table below. `<result>` 0 = ok, negative = errno |
| `#XMQTTMSG: <topic_len>,<msg_len>` | Inbound message header — see framing note |

### `#XMQTTEVT` types

These are Zephyr's `mqtt_evt_type` values, emitted for *every* MQTT event:

| | | | |
|---|---|---|---|
| `0` CONNACK | `1` DISCONNECT | `2` PUBLISH (inbound) | `3` PUBACK (QoS 1) |
| `4` PUBREC (QoS 2) | `5` PUBREL (QoS 2) | `6` PUBCOMP (QoS 2) | `7` SUBACK |
| `8` UNSUBACK | `9` PINGRESP | | |

At QoS 0 you only ever see `0`, `1`, `2`, `7`, `8`, `9`. `9` arrives once per
keepalive interval and is the cheapest proof the whole path (radio → PDN →
broker) is alive.

### Inbound message framing

`#XMQTTMSG` is **not line-oriented**. After the header line comes:

```
#XMQTTMSG: <topic_len>,<msg_len>CRLF
<topic_len bytes>CRLF
<msg_len raw bytes>CRLF
#XMQTTEVT: 2,0
```

The payload is raw and may contain CR, LF, `"`, or NUL. Read it by **byte
count**, never by line. Payload size is unbounded by the firmware — it streams
straight through — so cap it client-side and discard the excess.

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
