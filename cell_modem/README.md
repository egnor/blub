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
west build      # incremental build (add -p for pristine)
west flash      # flash over USB (app only, see below)
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
west build  # if not already built
# download the firmware image, this will perform a full chip erase
probe-rs download --binary-format hex --allow-erase-all build/merged.hex
# prevent the chip from locking out debug access on next boot
probe-rs download --binary-format hex $TOP/cell_modem/uicr-approtect-unlock.hex
# reset to run the app
probe-rs reset
```

To probe the running board: (note, only one `probe-rs` can run at a time)

```sh
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app
probe-rs reset  # reset the target
probe-rs attach build/app/zephyr/zephyr.elf  # print RTT logs
```

Smoke test: `AT` → `OK`, `AT+CGMM` → `nRF9151-LACA`, `AT#XSMVER` → the Serial
Modem version. The radio is off at boot (`AT+CFUN?` → `0`); `AT+CFUN=1` brings
it up once a SIM and antenna are connected.

## Hardware configuration notes

The app builds with Circuit Dojo's sysbuild config: NSIB (`b0`) + MCUboot
bootloaders, with `b0`'s provisioning data (key hashes etc.) in the nRF91 UICR
at `0xFF8000`. `pyOCD` can't write the UICR, so we use `probe-rs` to flash.

The stock firmware looks for AT commands on uart0 which is connected to the
RP2040 USB interface. We add `cell_modem/at-header-uart.overlay` to the device
tree, rerouting the AT command interface to uart1 (connected to Feather RX/TX).

The stock firmware also writes logs to uart0. We add `cell_modem/rtt-logs.conf`
Zephyr configuration to route logs to RTT via debug probe so they can be
read over USB through the onboard RP2040 (see above).

The project `mise.toml` sets `$EXTRA_DTC_OVERLAY_FILE` and `$EXTRA_CONF_FILE`
to enable these tweaks. Reset these variables to build without the tweaks,
or install different tweaks.

## Pinned versions

- `cell_modem/setup.py` pins a commit of the `ncs-serial-modem` fork,
  plus the NCS toolchain bundle version (`$NCS_VERSION`)
- the serial modem fork's `west.yml` pins the NCS code
  (`dev.tmp/ncs/workspace/nrf`) to a commit
- NCS and toolchain-bundle versions must match, eg. for
  `v3.4.0-rc1-87-g1c36e48027` we use toolchain `v3.4.0`.

To upgrade

- bump the constants in `cell_modem/setup.py`
- re-run `cell_modem/setup.py`
- do a pristine build: `west build -p` (in the app dir)
- (optional) clean up old versions in `dev.tmp/ncs/toolchains/`
