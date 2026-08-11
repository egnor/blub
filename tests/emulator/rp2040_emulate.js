#!/usr/bin/env node
// Runs an RP2040 .uf2 image under the rp2040js emulator.
// Emulator diagnostics go to stderr, uart0 (arduino Serial1) output to stdout.

const fs = require('fs');
const path = require('path');
const { Simulator, ConsoleLogger, LogLevel } = require('rp2040js');

const FLASH_START = 0x10000000;
const BOOTROM = path.join(__dirname, 'rp2040-bootrom-b1.bin');

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
}

if (process.argv.length != 3) throw new Error("Usage: node [script] fw.uf2");

const simulator = new Simulator();
const mcu = simulator.rp2040;
mcu.logger = new ConsoleLogger(LogLevel.Error);

const rom = fs.readFileSync(BOOTROM);
mcu.loadBootrom(new Uint32Array(rom.buffer, rom.byteOffset, rom.length / 4));
loadUF2(process.argv[2], mcu);

// Relay uart0 (arduino Serial1) output to stdout.
mcu.uart[0].onByte = (val) => process.stdout.write(String.fromCharCode(val));
mcu.core.PC = FLASH_START;
simulator.execute();
