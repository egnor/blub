# nRF9151 Feather — Serial Modem firmware build tools

Scripts to build Nordic's [Serial Modem][sm] AT-command firmware for the
[Circuit Dojo nRF9151 Feather][feather], using [Circuit Dojo's fork][fork] of
`ncs-serial-modem` (pulling in [nfed][nfed] to support
`circuitdojo_feather_nrf9151`), all based on Nordic's
[nRF Connect SDK (NCS)][ncs].

[sm]: https://nrfconnectdocs.nordicsemi.com/addons/addon-serial_modem/latest/index.html
[feather]: https://www.circuitdojo.com/products/nrf9151-feather
[fork]: https://github.com/circuitdojo/ncs-serial-modem
[nfed]: https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers
[ncs]: https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK

## Building

Make sure `mise` is activated (or run commands with `mise exec -- ...`), then

```bash
cell_modem/setup.py  # download tools, prepare build
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app  # main app source
west build -p   # pristine (clean) build
west build      # incremental build
west flash      # flash over USB
```

Paths of note:

- `dev.tmp/ncs` (`$NRFUTIL_HOME`) - root of everything cell-modem related
- `dev.tmp/ncs/workspace` - `west` (Zephyr build tool) working tree
- `dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem` - app checkout
- `dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app` - main app source

Environment settings come from top-level `mise.toml` and also `mise.local.toml`
in the workspace (generated from `nrfutil sdk-manager toolchain env`).

## Flashing details

The `west flash` alias does an app-only update. To reprogram from scratch:

```sh
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app
probe-rs erase --allow-erase-all
probe-rs download --binary-format hex build/merged.hex
probe-rs reset
```

To probe the running board: (note, `probe-rs` is single-client)

```sh
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app
probe-rs reset  # reset the target
probe-rs attach build/app/zephyr/zephyr.elf  # RTT logs
```

To talk to the running app:

```bash
okserial term cmsis-dap 115200
```

Smoke test: `AT` → `OK`, `AT+CGMM` → `nRF9151-LACA`, `AT#XSMVER` → the Serial
Modem version. The radio is off at boot (`AT+CFUN?` → `0`); `AT+CFUN=1` brings
it up once a SIM and antenna are connected.

Note, the stock app config sends logs to uart0, not RTT; see below.

## Hardware notes

The app builds with Circuit Dojo's sysbuild config: NSIB (`b0`) + MCUboot
bootloaders, with `b0`'s provisioning data (key hashes etc.) in the nRF91
UICR at `0xFF8000`. `pyOCD` can't program the UICR, so we use `probe-rs`.
Rewriting the UICR requires erasing the entire chip.

The AT interface is nRF9151 uart0 (TX P0.11 / RX P0.10, no flow control),
which on the Feather is wired to both the RP2040 USB-serial bridge
(`/dev/ttyACM*`) and header TX/RX pins. The same firmware works over USB and
with TTL wiring to a sibling MCU (cross TX/RX, common ground, 3.3 V).

In the stock config, the Zephyr console and log backend also use uart0, so the
boot banner and any log messages share the AT port. For clean separation, build
with an RTT logging fragment and read logs with `probe-rs attach`:

```bash
printf 'CONFIG_USE_SEGGER_RTT=y\nCONFIG_LOG_BACKEND_RTT=y\n' > /tmp/rtt.conf
west build -p -- -DEXTRA_CONF_FILE=/tmp/rtt.conf
```

## Tweaking configuration (pinouts etc.)

NCS config is layered:

- `app/prj.conf` — application baseline.
- `app/boards/circuitdojo_feather_nrf9151_ns.{conf,overlay}` — included board
  files with Feather defaults (uart0 as AT UART, nPM1300, CR termination).
- For changes worth keeping, put a `.conf` (Kconfig) or `.overlay` (devicetree)
  file **here in `cell_modem/`**, check it in, and pass it at build time
  (absolute path; builds run from the app dir):

  ```bash
  EXTRA_DTC_OVERLAY_FILE=$PWD/cell_modem/my-pins.overlay west build -p
  EXTRA_CONF_FILE=$PWD/cell_modem/my-tweaks.conf west build -p
  ```

  Use the `EXTRA_*` forms (not `CONF_FILE`/`DTC_OVERLAY_FILE`, which *replace*
  the auto-included board files instead of adding to them).

Caveat: the fork's `app/overlay-external-mcu.overlay` is pin-mapped for the
nRF9151 **DK** and collides with the Feather's uart0 (P0.10/P0.11) — don't use
it as-is. For a sibling MCU, just wire to the header TX/RX (same uart0, no
overlay needed).

## Pinned versions

- `cell_modem/setup.py` pins a commit of the `ncs-serial-modem` fork,
  plus the NCS toolchain bundle version (`$NCS_VERSION`)
- the fork's `west.yml` pins an NCS tree (`dev.tmp/ncs/workspace/nrf`) commit
- eg. for `v3.4.0-rc1-87-g1c36e48027` we use toolchain `v3.4.0`.

To upgrade

- bump the constants in `cell_modem/setup.py`
- re-run `cell_modem/setup.py`
- do a pristine build: `west build -p` (in the app dir)
- (optional) clean up old versions in `dev.tmp/ncs/toolchains/`
