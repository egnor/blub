# RP2040 emulator harness

Runs test firmware under [rp2040js](https://github.com/wokwi/rp2040js), the
RP2040 emulator that powers Wokwi's Pi Pico support, so library logic can be
tested without hardware and without a network round trip.

Unlike a host-compiled Arduino shim, this runs the *actual* firmware image built
by `arm-none-eabi-g++` on an emulated Cortex-M0+, so pointer width, struct
layout, alignment, and toolchain codegen all match the real device.

Roughly 25ms from reset to first serial output; a whole test image runs in well
under a second. Emulated idle time (`delay()`, waiting on a timer) costs no wall
clock, because the simulation clock jumps to the next scheduled event. Only
executed instructions are slowed, by about 4x.

## Usage

```sh
node rp2040_emulate.js FIRMWARE.uf2 --expect END-TEST --timeout 30
```

UART0 output goes to stdout; emulator diagnostics go to stderr. Exits 0 when the
`--expect` text appears, 1 on timeout.

Test firmware should print to **`Serial1`** (UART0). `Serial` is USB CDC, which
this harness does not enumerate.

## rp2040-bootrom-b1.bin

The RP2040 boot ROM, revision B1, 16KiB. Required: without it the firmware
faults immediately, because the pico-SDK calls ROM-resident routines (notably
the soft-float helpers).

Extracted from `demo/bootrom.ts` in the rp2040js repository, which is in turn
built from <https://github.com/raspberrypi/pico-bootrom> revision B1
(`00a4a19114195e20fb817bdfbca1165e157eef37`).

`sha256:90b31e108faedfa6bbcfcc33ba05dac61e645108cfbbdc385f8dafc71fde44ae`

## Adding fake hardware

rp2040js is the MCU only -- it has no device models. If a test needs peripherals
on the other end of a bus, write them against these hooks:

- `mcu.uart[n].onByte` / `.feedByte()` -- e.g. a fake modem answering AT commands
- `mcu.gpio[n].addListener()` / `.setInputValue()` -- pin-level devices
- `mcu.i2c`, `mcu.spi`, `mcu.adc`, `mcu.pwm`, `mcu.pio`
- `BasePeripheral` + `mcu.peripherals` -- a custom memory-mapped peripheral

Wokwi's own parts library (displays, sensors) is not open source and cannot be
reused here. For display/screenshot testing, use `wokwi-cli` against the cloud
service instead.
