# nRF9151 Feather serial modem firmware build tools

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

To set up the Nordic SDK and build cell modem firmware (slow the first time):

```sh
mise run cell-modem-build
```

To force a clean build:

```sh
cell-modem-build-clean
```

To REALLY start from
scratch and download the SDK again, delete `dev.tmp/ncs` and re-build.

To noodle around in the Nordic SDK environment:

```sh
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app
west build  # etc.
```

Paths of note:

- `mise.toml` - sets environment variables & defines build tasks
- `dev.tmp/ncs` (`$NRFUTIL_HOME`) - root of everything cell-modem related
- `dev.tmp/ncs/workspace` - `west` (Zephyr build tool) working tree
- `dev.tmp/ncs/workspace/mise.local.toml` - SDK environment (see setup.py)
- `dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem` - app checkout
- `dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app` - main app source

## Flashing

To (re)flash the entire chip, including bootloader and user registers (UICR):

```sh
mise run cell-modem-flash-all
```

To re-flash the app slot only:

```
mise run cell-modem-flash-app
```

Or, you can flash the app slot with the Nordic SDK:

```sh
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app
west flash --runner=probe-rs --domain=app
# WARNING - do not attempt whole-chip flash this way - see below
```

Important notes about using `probe-rs` or `west flash` directly:

- We must use [`probe-rs`](https://probe.rs/) to use
  the onboard RP2040 CMSIS-DAP USB programming/debugging interface.
  (`pyOCD` doesn't support nRF91xx UICR programming, and
  `nrfjprog`/`nrfutil device`
  [doesn't support CMSIS-DAP](https://devzone.nordicsemi.com/f/nordic-q-a/103446/raspberry-pi-debug-probe-cmsis-dap-support-in-nrfutil).)
- The [nRF91xx User Information Configuration Register (UICR) block](https://docs.nordicsemi.com/r/bundle/ps_nrf9151/page/uicr.html)
  can only be erased by chipwide ERASEALL (`probe-rs erase --allow-erase-all`);
  then, each UICR word can be programmed only once from an erased state.
- (Beware, `probe-rs download --chip-erase ...` does NOT use ERASEALL, only
  sector-wise erase which does NOT erase the UICR.)
- The serial modem whole-flash image (`merged.hex`) includes UICR data, so
  reflashing needs ERASEALL first, which `mise run cell-modem-flash-all` does.
- The app-slot-only image (`zephyr.hex`) does NOT include UICR data, so it
  can be reflashed without ERASEALL (normal sector-wise erase is fine).
- So, naive `west flash` (without `--domain=app` or a prior ERASEALL) can fail
  attempting to re-write UICR data even if it didn't change.
- Furthermore, after ERASEALL, default UICR settings lock out debug access once
  the debug session expires (requiring ERASEALL to recover), so
  `mise run cell-modem-flash-all` writes `cell_modem/uicr-approtect-unlock.hex`
  immediately after erasing to enable debug access. (Fortunately those
  registers do not overlap with `merged.hex`.)

## Talking to the board

To get logs from the board: (note, only one `probe-rs` can run at a time)

```sh
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app
probe-rs attach build/app/zephyr/zephyr.elf  # print RTT logs
```

When connected to the Feather TX/RX at 115200 baud (by default),
it should respond to AT commands:

- `AT` + CR -> `OK`
- `AT+CGMM` + CR -> `nRF9151-LACA`
- `AT#XSMVER` + CR -> the Serial Modem version.
- `AT+CFUN=1` + CR -> brings up the radio (needs SIM and antenna)

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
