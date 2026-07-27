# nRF9151 Feather — Serial Modem firmware (NCS workspace)

Builds Nordic's [Serial Modem][sm] AT-command firmware for the
[Circuit Dojo nRF9151 Feather][feather], using [Circuit Dojo's fork][fork] of
`ncs-serial-modem` (only the fork pulls in [nfed][nfed], which provides the
`circuitdojo_feather_nrf9151` board, plus ready-made board overlay files).

[sm]: https://nrfconnectdocs.nordicsemi.com/addons/addon-serial_modem/latest/index.html
[feather]: https://www.circuitdojo.com/products/nrf9151-feather
[fork]: https://github.com/circuitdojo/ncs-serial-modem
[nfed]: https://github.com/circuitdojo/nrf9160-feather-examples-and-drivers

Only this directory (two scripts and this file) is checked in. The actual
workspace — nrfutil, the NCS v3.2.1 toolchain bundle, and the west module tree,
~11 GB total — is constructed under `dev.tmp/ncs/` (git-ignored) by:

```bash
ncs/setup             # idempotent; reruns are quick, --update refreshes modules
```

## Cheat sheet

`ncs/ncs` runs any command inside the pinned NCS toolchain environment, with
the Serial Modem app directory (`dev.tmp/ncs/west/circuitdojo-ncs-serial-modem/app`)
as the working directory. The board target
(`circuitdojo_feather_nrf9151/nrf9151/ns`) is preset via `west config
build.board`, so plain `west build` does the right thing.

```bash
ncs/ncs west build                      # incremental build
ncs/ncs west build -p                   # pristine (clean) build
ncs/ncs west flash                      # flash over USB (pyOCD, CMSIS-DAP)
ncs/ncs pyocd rtt -t nRF9160_xxAA       # RTT console: boot banner, logs, errors
ncs/ncs pyocd reset -t nRF9160_xxAA     # reset the target
ncs/ncs                                 # interactive shell in the toolchain env
```

(pyOCD targets `nRF9160_xxAA` because the nRF9151 isn't in the CMSIS packs yet;
the 9160 is register-compatible for flashing. The Feather's onboard RP2040 is
the debug probe — no J-Link needed.)

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
3.3 V) with no rebuild. App console/logs go to RTT, not the UART.

## Tweaking configuration (pinouts etc.)

NCS config is layered; don't fork source to configure things:

- `app/prj.conf` — application baseline.
- `app/boards/circuitdojo_feather_nrf9151_ns.{conf,overlay}` — auto-included
  board files; where the Feather defaults live (uart0 as AT UART, nPM1300,
  CR termination, ...). Fine for local experiments, but note the checkout in
  `dev.tmp/` is disposable.
- For changes worth keeping, put a `.conf` (Kconfig) or `.overlay` (devicetree)
  file **here in `ncs/`**, check it in, and pass it at build time (absolute
  path, since builds run from the app dir):

  ```bash
  EXTRA_DTC_OVERLAY_FILE=$PWD/ncs/my-pins.overlay ncs/ncs west build -p
  EXTRA_CONF_FILE=$PWD/ncs/my-tweaks.conf ncs/ncs west build -p
  ```

  Use the `EXTRA_*` forms (not `CONF_FILE`/`DTC_OVERLAY_FILE`, which *replace*
  the auto-included board files instead of adding to them).

Caveat: the fork's `app/overlay-external-mcu.overlay` is pin-mapped for the
nRF9151 **DK** and collides with the Feather's uart0 (P0.10/P0.11) — don't use
it as-is. For a sibling MCU, just wire to the header TX/RX (same uart0, no
overlay needed).

## Pinned versions

`ncs/setup` pins NCS `v3.2.1` (matching the fork's `west.yml`) and a specific
commit of the fork; bump the constants at the top of the script to upgrade,
then rerun `ncs/setup --update` and do a pristine build.
