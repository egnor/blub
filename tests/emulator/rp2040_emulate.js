#!/usr/bin/env node
// Runs an RP2040 .uf2 image under the rp2040js emulator (the same emulator
// core Wokwi uses) and streams UART0 output to stdout.
//
//   node rp2040_emulate.js FIRMWARE.uf2 [--expect TEXT] [--timeout SECONDS]
//
// Exits 0 when --expect text appears in the output (or when the firmware runs
// to the timeout with no --expect given), 1 otherwise. Emulator diagnostics go
// to stderr so stdout is exactly the device's serial output.

const fs = require('fs');
const path = require('path');
const { Simulator, ConsoleLogger, LogLevel } = require('rp2040js');

const FLASH_START = 0x10000000;
const BOOTROM = path.join(__dirname, 'rp2040-bootrom-b1.bin');

function parseArgs(argv) {
  const opts = { firmware: null, expect: null, timeout: 30 };
  for (let i = 0; i < argv.length; i++) {
    // Accepts both "--opt value" and "--opt=value".
    const [name, inlineValue] = argv[i].startsWith('--')
      ? [argv[i].split('=', 1)[0], argv[i].includes('=') ? argv[i].slice(argv[i].indexOf('=') + 1) : null]
      : [null, null];
    const value = () => (inlineValue !== null ? inlineValue : argv[++i]);

    if (name === '--expect') opts.expect = value();
    else if (name === '--timeout') opts.timeout = Number(value());
    else if (name) throw new Error(`unknown option: ${name}`);
    else if (!opts.firmware) opts.firmware = argv[i];
    else throw new Error(`unexpected argument: ${argv[i]}`);
  }
  if (!opts.firmware) throw new Error('usage: rp2040_emulate.js FIRMWARE.uf2 [--expect TEXT] [--timeout SECONDS]');
  return opts;
}

// Minimal UF2 reader: 512-byte blocks, first/second magic per the UF2 spec.
function loadUF2(filename, mcu) {
  const image = fs.readFileSync(filename);
  let blocks = 0;
  for (let off = 0; off + 512 <= image.length; off += 512) {
    const block = image.subarray(off, off + 512);
    if (block.readUInt32LE(0) !== 0x0a324655) continue;  // "UF2\n"
    if (block.readUInt32LE(4) !== 0x9e5d5157) continue;  // magic 2
    const address = block.readUInt32LE(12);
    const length = block.readUInt32LE(16);
    mcu.flash.set(block.subarray(32, 32 + length), address - FLASH_START);
    blocks++;
  }
  if (!blocks) throw new Error(`no UF2 blocks found in ${filename}`);
  return blocks;
}

const opts = parseArgs(process.argv.slice(2));
const started = process.hrtime.bigint();

const simulator = new Simulator();
const mcu = simulator.rp2040;
mcu.logger = new ConsoleLogger(LogLevel.Error);

const bootrom = fs.readFileSync(BOOTROM);
mcu.loadBootrom(new Uint32Array(bootrom.buffer, bootrom.byteOffset, bootrom.length / 4));
loadUF2(opts.firmware, mcu);

let pendingLine = '';

function finish(code, why) {
  const wallMs = Number(process.hrtime.bigint() - started) / 1e6;
  const simMs = simulator.clock.micros / 1000;
  simulator.stop();
  process.stderr.write(
    `rp2040_emulate: ${why} after ${wallMs.toFixed(0)}ms wall / ${simMs.toFixed(0)}ms emulated\n`
  );
  process.exit(code);
}

// Serial1 on the Arduino RP2040 core is UART0. (Serial is USB CDC, which this
// harness does not enumerate -- test firmware should print to Serial1.)
// Matching happens on line boundaries, so a matched line is written out whole
// before the emulator stops.
mcu.uart[0].onByte = (value) => {
  const char = String.fromCharCode(value);
  process.stdout.write(char);
  if (!opts.expect) return;
  if (char !== '\n') {
    pendingLine += char;
    return;
  }
  const line = pendingLine;
  pendingLine = '';
  if (line.includes(opts.expect)) finish(0, `matched ${JSON.stringify(opts.expect)}`);
};

setTimeout(
  () => finish(opts.expect ? 1 : 0, opts.expect ? 'TIMED OUT waiting for expected text' : 'timeout reached'),
  opts.timeout * 1000
);

mcu.core.PC = FLASH_START;
simulator.execute();
