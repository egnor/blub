# nRF9151 Feather serial modem firmware build

Build setup for Nordic's
[Serial Modem](https://nrfconnectdocs.nordicsemi.com/addons/addon-serial_modem/latest/index.html)
AT-command firmware on the
[Circuit Dojo nRF9151 Feather](https://www.circuitdojo.com/products/nrf9151-feather).

This directory contains the `nrf9151_build_setup.py` script, and is also a
[west manifest repository](https://docs.zephyrproject.org/latest/develop/west/manifest.html#multiple-repository-model)
(`west.yml`) and a
[Zephyr module](https://docs.zephyrproject.org/latest/develop/modules.html)
(`zephyr/module.yml`) that adds overlays to the build.

Other boards use the client library (`../cell_modem_client_lib/`)
to talk to this firmware.

## Building

To install the Nordic SDK and build cell modem firmware (slow the first time):

```sh
mise run cell-modem-build
```

To force a clean build:

```sh
mise run cell-modem-clean ::: cell-modem-build
```

To REALLY start from scratch, including SDK download:

```sh
rm -rf dev.tmp/nordic
mise run cell-modem-build
```

To noodle around in the Nordic SDK environment:

```sh
cd dev.tmp/nordic/workspace/ncs-serial-modem/app
west build  # etc.
```

Paths of note:

- `mise.toml` - sets environment variables & defines build tasks
- `dev.tmp/nordic` (`$NRFUTIL_HOME`) - root of cell modem firmware build
- `dev.tmp/nordic/toolchains` - Nordic toolchain bundle(s)
- `dev.tmp/nordic/workspace` - [west workspace](https://docs.zephyrproject.org/latest/develop/west/workspaces.html)
- `dev.tmp/nordic/workspace/mise.local.toml` - SDK env (see `nrf9151_build_setup.py`)
- `dev.tmp/nordic/workspace/cell_modem_firmware` - symlink to this directory
- `dev.tmp/nordic/workspace/ncs-serial-modem/app` - Nordic's
  [serial modem app](https://github.com/nrfconnect/ncs-serial-modem/tree/main/app)
- `dev.tmp/nordic/workspace/nfed` - Circuit Dojo's
  [nRF91xx Feather board support](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/)
- `dev.tmp/nordic/workspace/nrf` - Nordic SDK core

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
cd dev.tmp/nordic/workspace/ncs-serial-modem/app
west flash --runner=probe-rs --domain=app
# WARNING - do not attempt whole-chip flash this way - see below
```

## Talking to the board

The Feather should respond to [AT commands](at_cheat_sheet.md)
on the TX/RX pins (2.8V TTL) at 115200 baud:

- `AT` + CR -> `OK`
- `AT+CGMM` + CR -> `nRF9151-LACA`
- `AT#XSMVER` + CR -> the Serial Modem version.
- `AT+CFUN=1` + CR -> brings up the radio (needs SIM and antenna)

To stream log messages from the Feather's onboard CMSIS-DAP USB interface with
[RTT](https://www.segger.com/products/debug-probes/j-link/technology/about-real-time-transfer/):

```sh
mise run cell-modem-logs
```

## Quirks and Details

**The nRF9151 Feather's USB port**

- The nRF9151 Feather has an onboard CMSIS-DAP USB debug probe. (Yay!)
- However, Nordic's `nrfjprog`/`nrfutil device` tools
  [don't support CMSIS-DAP](https://devzone.nordicsemi.com/f/nordic-q-a/103446/raspberry-pi-debug-probe-cmsis-dap-support-in-nrfutil).
- Also, the popular [`pyOCD`](https://pyocd.io/) doesn't support the nRF91xx UICR.
- Therefore, we use `probe-rs` to flash and debug the board.

**`probe-rs` issues: nRF91xx UICR reflashing and debug port access**

- The [nRF91xx UICR block](https://docs.nordicsemi.com/r/bundle/ps_nrf9151/page/uicr.html)
  (config registers) can only be reset by the
  [CTRL-AP](https://docs.nordicsemi.com/r/bundle/ps_nrf9151/page/chapters/dif/ctrl-ap.html)'s chipwide
  [ERASEALL](https://docs.nordicsemi.com/r/bundle/ps_nrf9151/page/chapters/dif/ctrl-ap.html?section=register.ERASEALL)
  operation; then, each UICR word can be programmed only once.
- The nRF9151 debug port is
  [locked by default](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/security/ap_protect.html#flow-for-ap-protect-controlled-by-hardware-and-software)
  and only open for one download after ERASEALL, or if firmware is loaded
  and the
  [APPROTECT](https://docs.nordicsemi.com/r/bundle/ps_nrf9151/page/uicr.html?section=register.APPROTECT)
  UICR register is set appropriately.
- So, initial setup must ERASEALL, then write firmware and APPROTECT in one go.
- However, neither `probe-rs erase` nor
  `probe-rs download --chip-erase` use ERASEALL; they use sector
  erase and [skip the UICR](https://github.com/probe-rs/probe-rs/issues/4225)
  (in 0.32; fixed in [#4226](https://github.com/probe-rs/probe-rs/pull/4226)).
- But, `probe-rs --allow-erase-all` *does* use ERASEALL on attach if the debug
  port is locked.
- Therefore, `mise run cell-modem-flash-all` runs two steps to ensure success:
  - `probe-rs erase --allow-erase-all` (locks the chip if it wasn't already)
  - `probe-rs download --allow-erase-all` (programs UICR, bootloader, and app)
- After that, `mise run cell-modem-flash-app` can program the app slot without
  touching UICR.
- ⚠️ Naive `west flash` (without `--domain=app`) can fail trying to
  re-write the UICR.

See also:

- [Circuit Dojo recovery advice](https://docs.circuitdojo.com/nrf9151-feather/device-recovery.html)
- [Circuit Dojo nRF91xx Recovery Tool](https://github.com/circuitdojo/recovery)
- [debugprobe-16ms-multiplier.uf2](https://docs.circuitdojo.com/nrf9151-feather/files/debugprobe-16ms-multiplier.uf2) - RP2040 debug probe firmware image

## Hardware configuration

The firmware builds with Nordic's standard serial modem sysbuild setup:

- [nRF Secure Immutable Bootloader](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/samples/bootloader/README.html)
  (NSIB aka B0) + [MCUboot](https://docs.mcuboot.com/) bootloaders
- NSIB provisioning data (key hashes etc.) in the nRF91 UICR at `0xFF8000`

Upstream ncs-serial-modem only knows Nordic's own boards, so `sysbuild/`
includes board-specific configuration and our preferences:

- `partitions.dtsi` - flash layout for B0 + updatable MCUboot + TF-M
  (same as Nordic's nRF9151 DK layout; the nfed board default differs),
  shared by all three overlays below
- `app.conf`, `app.overlay` - serial modem app: AT command interface on
  uart1 (Feather RX/TX, 115200 baud) rather than uart0 (USB via RP2040);
  logs to the debug probe via RTT; buffer sizes; unused features off;
  Feather peripherals (nPM1300, SPI flash, GNSS antenna); SRAM layout
- `mcuboot.conf`, `mcuboot.overlay` - MCUboot: code partition, SRAM layout,
  unneeded board drivers off
- `b0.conf`, `b0.overlay` - NSIB: likewise, plus workarounds (see below)

These files are attached to the build by `sysbuild/CMakeLists.txt` via
`..._EXTRA_CONF_FILE` / `..._EXTRA_DTC_OVERLAY_FILE` CMake cache variables.
These files originated in [Circuit Dojo's fork][fork] of ncs-serial-modem.

[fork]: https://github.com/circuitdojo/ncs-serial-modem

Workarounds, to drop once the fixes reach the pinned versions:

- `b0.conf` sets `CONFIG_CIRCUITDOJO_FEATHER_NRF9151_PMIC_STARTUP=n`
  because nfed's board startup code is wrongly built into B0 and fails to
  link; see [nfed PR #24](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/pull/24).
  Once that is in the pinned nfed, this line must go (assigning an
  undefined Kconfig symbol is a build error).

## Version pinning

- `west.yml` (here) pins `nrfconnect/ncs-serial-modem` (Serial Modem app) and
  `circuitdojo/nrf9160-feather-examples-and-drivers` ("nfed")
- ncs-serial-modem's [`west.yml`](https://github.com/nrfconnect/ncs-serial-modem/blob/main/west.yml)
  in turn pins `nrfconnect/sdk-nrf` (SDK core)
- circuitdojo's [`west.yml`](https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers/blob/v3.4.x/west.yml)
  pins a different SDK core version; we do NOT import this manifest
- `nrf9151_build_setup.py` (here) picks a Nordic toolchain bundle
  (`SDK_VERSION`) which should match the SDK core

To upgrade

- pick a new `ncs-serial-modem` tag or commit and update our `west.yml`
- find the `sdk-nrf` version pinned by ncs-serial-modem's `west.yml`
  - find the corresponding toolchain (see `nrfutil sdk-manager search`)
    - set `SDK_VERSION` in `nrf9151_build_setup.py`
  - find the corresponding `nrf9160-feather-examples-and-drivers` commit
    - set `revision:` under `name: nfed` in our `west.yml`
- run `mise run cell-modem-clean ::: cell-modem-build`
