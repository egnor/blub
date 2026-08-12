# nRF9151 Feather serial modem firmware build & interface library

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
mise run cell-modem-clean ::: cell-modem-build
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
- `dev.tmp/ncs/workspace/mise.local.toml` - SDK env (see nrf9151_build_setup.py)
- `dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem` - app checkout
- `dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app` - main app source

## Flashing

To (re)flash the entire chip, including bootloader and user registers (UICR):

```sh
mise run cell-modem-flash-all
```

To re-flash the app slot only:

```sh
mise run cell-modem-flash-app
```

Or, you can flash the app slot with the Nordic SDK:

```sh
cd dev.tmp/ncs/workspace/circuitdojo-ncs-serial-modem/app
west flash --runner=probe-rs --domain=app
# WARNING - do not attempt whole-chip flash this way - see below
```

We use [`probe-rs`](https://probe.rs/) to access the onboard CMSIS-DAP USB
interface. ([`pyOCD`](https://pyocd.io/) doesn't support the nRF91xx UICR,
and Nordic's own `nrfjprog`/`nrfutil device` tools
[don't support CMSIS-DAP](https://devzone.nordicsemi.com/f/nordic-q-a/103446/raspberry-pi-debug-probe-cmsis-dap-support-in-nrfutil).)
Heed these quirks if you run `probe-rs` or `west flash` directly:

**`probe-rs` + Nordic quirk: UICR reflashing**

- The [nRF91xx User Information Configuration Register (UICR) block](https://docs.nordicsemi.com/r/bundle/ps_nrf9151/page/uicr.html)
  can only be erased by chipwide ERASEALL (`probe-rs erase --allow-erase-all`);
  then, each UICR word can be programmed only once from an erased state.
- Note, `probe-rs download --chip-erase ...`
  [does NOT use ERASEALL](https://github.com/probe-rs/probe-rs/issues/4225),
  only sector-by-sector erase, which does NOT erase the UICR.
- The whole-flash image (`merged.hex`) includes UICR data, so (re)flashing it
  needs ERASEALL first, which `mise run cell-modem-flash-all` does.
- The app-slot-only image (`zephyr.hex`) does NOT include UICR data, so it
  can be reflashed without ERASEALL (normal sector erase is fine).
- Naive `west flash` (without `--domain=app` or a prior ERASEALL) can fail
  attempting to re-write UICR data even if it didn't change.

**`probe-rs` + Nordic quirk: Debug port locking**

- After ERASEALL, the nRF9151 debug port is
  [only open until the next download operation or hard reset](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/security/ap_protect.html#flow-for-ap-protect-controlled-by-hardware-and-software),
  then it is locked until it is re-opened by firmware or another ERASEALL.
- Nordic setup firmware re-opens the port only if code is programmed AND the
  [APPROTECT UICR register](https://docs.nordicsemi.com/r/bundle/ps_nrf9151/page/uicr.html?section=register.APPROTECT)
  is set to `0x50FA50FA`.
- Therefore, after ERASEALL, both bootloader/app AND the APPROTECT register
  must be flashed in one download operation, which
  `mise run cell-modem-flash-all` does by combining
  `cell_modem/nrf9151_unlock_debug.hex` with the merged image before flashing.

Circuit Dojo has [advice for recovering](https://docs.circuitdojo.com/nrf9151-feather/device-recovery.html) the RP2040 debug probe and the nRF9151 itself:

- [nRF91xx Recovery Tool](https://github.com/circuitdojo/recovery) on GitHub
- [debugprobe-16ms-multiplier.uf2](https://docs.circuitdojo.com/nrf9151-feather/files/debugprobe-16ms-multiplier.uf2) - RP2040 debug probe firmware

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

## Hardware configuration

The firmware builds with Circuit Dojo's config:

- [NSIB (aka `b0`)](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/samples/bootloader/README.html)
\+ [MCUboot](https://docs.mcuboot.com/) bootloaders
- NSIB's provisioning data (key hashes etc.) in the nRF91 UICR at `0xFF8000`

Our build applies some tweaks:

- `cell_modem/nrf9151_serial_modem.overlay` - reroute AT command interface to uart1 (Feather RX/TX), not uart0 (USB via RP2040)
- `cell_modem/nrf9151_serial_modem.conf` - route logs to debug probe, not uart0; set version

The project `mise.toml` sets `$EXTRA_DTC_OVERLAY_FILE` and `$EXTRA_CONF_FILE`
to enable these tweaks. Reset these variables to build without the tweaks,
or install different tweaks.

## Version pinning

- `cell_modem/nrf9151_build_setup.py` pins a commit of the `ncs-serial-modem` fork and
  picks a Nordic toolchain bundle version (`$NCS_VERSION`)
- `ncs-serial-modem` in turn pins NCS itself (`dev.tmp/ncs/workspace/nrf`)
  via `west.yml`
- NCS and toolchain versions must be compatible, eg. for NCS commit
  `v3.4.0-rc1-87-g1c36e48027` we use toolchain `v3.4.0`.

To upgrade

- inspect the NCS commit in `west.yml` and find the corresponding toolchain
- bump the constants in `cell_modem/nrf9151_build_setup.py`
- drop any `UPSTREAM_CHERRY_PICKS` entry the new `APP_REPO_REV` already has
- run `mise run cell-modem-build-clean`
- (optional) clean up old toolchains in `dev.tmp/nordic/toolchains/`
