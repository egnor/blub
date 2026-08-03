# nRF9151 Feather — Serial Modem firmware build tools

Scripts to build Nordic's [Serial Modem][sm] AT-command firmware for the
[Circuit Dojo nRF9151 Feather][feather], using [Circuit Dojo's fork][fork] of
`ncs-serial-modem` (the fork pulls in [nfed][nfed], which provides the
`circuitdojo_feather_nrf9151` board, plus ready-made board overlay files),
all based on Nordic's [nRF Connect SDK (NCS)][ncs].

[sm]: https://nrfconnectdocs.nordicsemi.com/addons/addon-serial_modem/latest/index.html
[feather]: https://www.circuitdojo.com/products/nrf9151-feather
[fork]: https://github.com/circuitdojo/ncs-serial-modem
[nfed]: https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers
[ncs]: https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK

The actual build happens in `dev.tmp/ncs`:

- bin/         - nrfutil's self-install + command plugins (on PATH via mise)
- downloads/   - NCS toolchain bundle download cache
- toolchains/  - NCS toolchain bundle (compiler, west, python)
- tmp/         - temporary downloads etc
- workspace/   - west workspace for building (manifest/app repo + NCS source)
(plus other nrfutil housekeeping: bootstrap/, cache/, config/, logs/, ...)

## Cheat sheet

`cell_modem/ncs.sh` runs any command inside the pinned NCS toolchain environment, with
the Serial Modem app directory (`dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app`)
as the working directory. The board target
(`circuitdojo_feather_nrf9151/nrf9151/ns`) is preset via `west config
build.board`, so plain `west build` does the right thing.

```bash
cell_modem/ncs.sh west build                   # incremental build
cell_modem/ncs.sh west build -p                # pristine (clean) build
cell_modem/ncs.sh west flash                   # reflash the app over USB (probe-rs, CMSIS-DAP)
cell_modem/ncs.sh probe-rs reset --chip nRF9151_xxAA                        # reset the target
cell_modem/ncs.sh probe-rs attach --chip nRF9151_xxAA build/app/zephyr/zephyr.elf  # RTT logs
cell_modem/ncs.sh                              # interactive shell in the toolchain env
```

(All target access goes through probe-rs and the Feather's onboard RP2040
CMSIS-DAP probe — no J-Link needed. The probe is single-client: quit an
`attach` before flashing/resetting. Note the stock app config sends logs to
uart0, not RTT — see Hardware notes.)

## Flashing details

The app builds with Circuit Dojo's sysbuild config: NSIB (`b0`) + MCUboot
bootloaders, with `b0`'s provisioning data (key hashes etc.) in the nRF91
**UICR** at `0xFF8000`. That has two consequences:

- **pyOCD can't flash this image** — its nRF9160 pack can't program UICR
  (`flash program page failure (address 0x00ff8000)`), so flashing uses
  **probe-rs** instead (installed by `mise`)
- **UICR can't be rewritten in place**, so the bootloader + provisioning
  images only flash onto an erased chip.

`setup.py` therefore sets a west alias so that plain `west flash` means
`flash --runner probe-rs --domain app` — it reflashes just the (signed) app
image, which is all everyday iteration needs. To provision a blank board, or
after changing bootloader/sysbuild config, erase and flash everything:

```bash
cell_modem/ncs.sh probe-rs erase --chip nRF9151_xxAA --allow-erase-all
cell_modem/ncs.sh probe-rs download --binary-format hex --chip nRF9151_xxAA build/merged.hex
cell_modem/ncs.sh probe-rs reset --chip nRF9151_xxAA
```

If probe-rs reports `Core 0 is locked` (APPROTECT can end up engaged, e.g.
after a failed flash), the same `probe-rs erase --allow-erase-all` recovers
the chip — at the cost of a full erase, so re-provision afterwards.

Talking AT to the modem (115200 8N1, **CR** line ending; the blub venv already
has pyserial):

```bash
python -m serial.tools.miniterm --eol CR \
    /dev/serial/by-id/usb-Raspberry_Pi_Debug_Probe*-if01* 115200
```

Smoke test: `AT` → `OK`, `AT+CGMM` → `nRF9151-LACA`, `AT#XSMVER` → the Serial
Modem version. The radio is off at boot (`AT+CFUN?` → `0`); `AT+CFUN=1` brings
it up once a SIM and antenna are connected.

## Hardware notes

The AT interface is nRF9151 **uart0** (TX P0.11 / RX P0.10, no flow control),
which on the Feather is wired to **both** the RP2040 USB-serial bridge
(`/dev/ttyACM*`) **and** the header TX/RX pins — so the same firmware serves
USB bench use and TTL wiring to a sibling MCU (cross TX/RX, common ground,
3.3 V) with no rebuild.

In the stock config the Zephyr console and log backend also use uart0, so the
boot banner (and any log spew) shares the AT port. For clean separation, build
with an RTT logging fragment and read logs with `probe-rs attach`:

```bash
printf 'CONFIG_USE_SEGGER_RTT=y\nCONFIG_LOG_BACKEND_RTT=y\n' > /tmp/rtt.conf
cell_modem/ncs.sh west build -p -- -DEXTRA_CONF_FILE=/tmp/rtt.conf
```

## Tweaking configuration (pinouts etc.)

NCS config is layered; don't fork source to configure things:

- `app/prj.conf` — application baseline.
- `app/boards/circuitdojo_feather_nrf9151_ns.{conf,overlay}` — auto-included
  board files; where the Feather defaults live (uart0 as AT UART, nPM1300,
  CR termination, ...). Fine for local experiments, but note the checkout in
  `dev.tmp/` is disposable.
- For changes worth keeping, put a `.conf` (Kconfig) or `.overlay` (devicetree)
  file **here in `cell_modem/`**, check it in, and pass it at build time (absolute
  path, since builds run from the app dir):

  ```bash
  EXTRA_DTC_OVERLAY_FILE=$PWD/cell_modem/my-pins.overlay cell_modem/ncs.sh west build -p
  EXTRA_CONF_FILE=$PWD/cell_modem/my-tweaks.conf cell_modem/ncs.sh west build -p
  ```

  Use the `EXTRA_*` forms (not `CONF_FILE`/`DTC_OVERLAY_FILE`, which *replace*
  the auto-included board files instead of adding to them).

Caveat: the fork's `app/overlay-external-mcu.overlay` is pin-mapped for the
nRF9151 **DK** and collides with the Feather's uart0 (P0.10/P0.11) — don't use
it as-is. For a sibling MCU, just wire to the header TX/RX (same uart0, no
overlay needed).

## Pinned versions

`cell_modem/setup.py` pins a specific commit of the fork, plus the NCS toolchain
bundle version to build it with (`NCS_VERSION`, duplicated in `ncs.sh`). The
fork's `west.yml` pins the NCS tree itself, nowadays to a bare SHA rather than a
tag — resolve it against `dev.tmp/ncs/workspace/nrf` (`git describe --tags <sha>`) to
find which toolchain bundle to ask for. As of the current pin that's
`v3.4.0-rc1-87-g1c36e48027`, hence toolchain `v3.4.0`.

To upgrade: bump the constants, rerun `cell_modem/setup.py`, and do a
pristine build. A toolchain bump means a fresh ~6 GB download; the old bundle
stays in `dev.tmp/ncs/toolchains/` until you delete it.
