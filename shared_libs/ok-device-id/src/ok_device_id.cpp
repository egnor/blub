#include "ok_device_id.h"

#include <array>

#include "ok_logging.h"

using I2C = decltype(Wire);

static const OkLoggingContext OK_CONTEXT("ok_device_id");

bool OkDeviceId::matches(OkDeviceId const& other) const {
  if (part != other.part) return false;
  if (part == OKDEV_NO_PART) return true;
  for (int i = 0; i < 16; ++i) {
    auto const ap = pin(i), bp = other.pin(i);
    if (ap != 0xF && bp != 0xF && ap != bp) return false;
  }
  return true;
}

void OkDeviceId::set_pin(int pin, uint8_t value) {
  OK_FATAL_IF(pin < 0 || pin >= 16 || value > 0xF);
  pins = pins & ~(uint64_t(0xF) << (pin * 4)) | (uint64_t(value) << (pin * 4));
}

static bool wbytes(I2C& i2c, uint8_t addr, std::initializer_list<int> bytes) {
  i2c.beginTransmission(addr);
  for (auto b : bytes) i2c.write(b);
  auto const status = i2c.endTransmission();
  if (status != 0) OK_ERROR("I2C(0x%02X) write failed (%d)", addr, status);
  return (status == 0);
}

static uint8_t wrbyte(I2C& i2c, uint8_t addr, std::initializer_list<int> w) {
  wbytes(i2c, addr, w);
  i2c.requestFrom(addr, 1);
  if (i2c.available() < 1) OK_ERROR("I2C(0x%02X) read failed (no data)", addr);
  return i2c.read();
}

static OkDeviceId try_awinic_9523(OkDeviceIdPart part, I2C& i2c, uint8_t addr) {
  i2c.beginTransmission(addr);
  auto const i2c_status = i2c.endTransmission();
  if (i2c_status == 2) {
    OK_DETAIL("AW9523(0x%02X) not present (NACK)", addr);
    return {};
  } else if (i2c_status != 0) {
    OK_ERROR("AW9523(0x%02X) probe error (%d)", addr, i2c_status);
    return {};
  }

  if (!wbytes(i2c, addr, {0x10})) return {};  // ID register
  auto const id_byte = wrbyte(i2c, addr, {});
  if (id_byte != 0x23) {
    OK_DETAIL("AW9523(0x%02X) not present (ID 0x%02X != 0x23)", addr, id_byte);
    return {};
  }

  if (!wbytes(i2c, addr, {0x7F, 0x00})) return {};  // trigger soft reset

  OkDeviceId id = {part, 0xFFFFFFFFFFFFFFFF};
  wbytes(i2c, addr, {0x11, 0x13});  // P0 push-pull, LED mode min current range
  wbytes(i2c, addr, {0x04, 0xFF, 0xFF});  // P0 & P1 all input
  wbytes(i2c, addr, {0x20, 1, 1, 1, 1, 1, 1, 1, 1});  // LED mode min current
  wbytes(i2c, addr, {0x28, 1, 1, 1, 1, 1, 1, 1, 1});

  // Briefly use low current LED cathode drive as a pull-down alternative
  wbytes(i2c, addr, {0x12, 0x00, 0x00});   // P0 & P1 LED drive
  wbytes(i2c, addr, {0x12, 0xFF, 0xFF});   // P0 & P1 back to GPIO (input)
  wbytes(i2c, addr, {0x00});               // read port pins
  uint8_t const lo_read[] = { wrbyte(i2c, addr, {0}), wrbyte(i2c, addr, {1}) };

  // Briefly drive pins high as a pull-up alternative
  wbytes(i2c, addr, {0x02, 0xFF, 0xFF, 0x00, 0x00});  // P0 & P1 high output
  wbytes(i2c, addr, {0x04, 0xFF, 0xFF});              // P0 & P1 back to input
  wbytes(i2c, addr, {0x00});                          // read port pins
  uint8_t const hi_read[] = { wrbyte(i2c, addr, {0}), wrbyte(i2c, addr, {1}) };

  for (int port = 0; port < 2; ++port) {
    for (int pos = 0; pos < 8; ++pos) {
      if ((lo_read[port] & (1 << pos))) id.set_pin(port * 8 + pos, 0xE);
      if (!(hi_read[port] & (1 << pos))) id.set_pin(port * 8 + pos, 0x0);
    }
  }

  // test floating pins that could be tied to each other
  int group = 2;
  for (int port = 0; port < 2; ++port) {
    for (int pos = 0; pos < 8; ++pos) {
      if (id.pin(port * 8 + pos) != 0xF) continue;  // skip if already known
      uint8_t source[2] = {0x00, 0x00};  // the pin we're testing
      source[port] = (1 << pos);

      uint8_t targets[2] = {0x00, 0x00};  // pins to test for grouping
      for (int tport = 0; tport < 2; ++tport) {
        for (int tpos = 0; tpos < 8; ++tpos) {
          if (id.pin(tport * 8 + tpos) != 0xF) continue;  // skip if known
          if (tport != port || tpos != pos) targets[tport] |= (1 << tpos);
        }
      }

      // GPIO-drive source pin high, briefly LED-drive target pins low
      wbytes(i2c, addr, {0x02, 0xFF, 0xFF, ~source[0], ~source[1]});
      wbytes(i2c, addr, {0x12, ~targets[0], ~targets[1]});  // LED drive
      wbytes(i2c, addr, {0x12, 0xFF, 0xFF});  // revert all to GPIO
      uint8_t const read[] = { wrbyte(i2c, addr, {0}), wrbyte(i2c, addr, {1}) };
      wbytes(i2c, addr, {0x04, 0xFF, 0xFF});  // revert to all input

      if ((read[0] & targets[0]) || (read[1] & targets[1])) {
        id.set_pin(port * 8 + pos, group);  // pin group
        for (int tport = 0; tport < 2; ++tport) {
          for (int tpos = 0; tpos < 8; ++tpos) {
            if ((read[tport] & targets[tport] & (1 << tpos))) {
              id.set_pin(tport * 8 + tpos, group);  // grouped with
            }
          }
        }
        if (group < 0xD) ++group;
      } else {
        id.set_pin(port * 8 + pos, 0x1);  // isolated floating pin
      }
    }
  }

  wbytes(i2c, addr, {0x7F, 0x00});  // trigger soft reset
  OK_DETAIL("AW9523(0x%02X) found, pins=%016llx", addr, id.pins);
  return id;
}

static OkDeviceId try_pericom_6408(
    OkDeviceIdPart part, I2C& i2c, uint8_t addr
) {
  i2c.beginTransmission(addr);
  auto const i2c_status = i2c.endTransmission();
  if (i2c_status == 2) {
    OK_DETAIL("PIx6408(0x%02X) not present (NACK)", addr);
    return {};
  } else if (i2c_status != 0) {
    OK_ERROR("PIx6408(0x%02X) probe error (%d)", addr, i2c_status);
    return {};
  }

  uint8_t const id_byte = wrbyte(i2c, addr, {0x01});  // ID register
  if ((id_byte & ~0x02) != 0xA0) {
    OK_DETAIL("PIx6408(0x%02X) not present (ID 0x%02X != 0xA0)", addr, id_byte);
    return {};
  }

  if (!wbytes(i2c, addr, {0x01, 0x01})) return {};  // trigger soft reset

  OkDeviceId id = {part, 0xFFFFFFFF};
  wbytes(i2c, addr, {0x03, 0x00});  // all input
  wbytes(i2c, addr, {0x07, 0x00});  // disable high-Z
  wbytes(i2c, addr, {0x0D, 0x00});  // pull downward
  wbytes(i2c, addr, {0x0B, 0xFF});  // enable pull
  uint8_t const lo_read = { wrbyte(i2c, addr, {0x0F}) };  // read pins

  wbytes(i2c, addr, {0x0D, 0xFF});  // pull upward
  uint8_t const hi_read = { wrbyte(i2c, addr, {0x0F}) };  // read pins
  for (int pin = 0; pin < 8; ++pin) {
    if ((lo_read & (1 << pin))) id.set_pin(pin, 0xE);  // external pull up
    if (!(hi_read & (1 << pin))) id.set_pin(pin, 0x0);  // external pull down
  }

  // test floating pins that could be tied to each other
  int group = 2;
  for (int pin = 0; pin < 8; ++pin) {
    if (id.pin(pin) != 0xF) continue;  // skip if already known
    uint8_t source = 1 << pin;  // the pin we're testing

    uint8_t targets = 0x00;  // pins to test for grouping
    for (int tpin = 0; tpin < 8; ++tpin) {
      if (tpin != pin && id.pin(tpin) != 0xF) targets |= (1 << tpin);
    }

    // GPIO-drive source pin high, pull down target pins
    wbytes(i2c, addr, {0x0B, targets});  // pull targets only
    wbytes(i2c, addr, {0x0D, 0x00});     // pull downward
    wbytes(i2c, addr, {0x05, 0xFF});     // output high
    wbytes(i2c, addr, {0x03, source});   // output on source only
    uint8_t const read = wrbyte(i2c, addr, {0x0F});  // read pins
    wbytes(i2c, addr, {0x03, 0x00});     // revert to input

    if (read & targets) {
      id.set_pin(pin, group);  // pin group
      for (int tpin = 0; tpin < 8; ++tpin) {
        if ((read & targets & (1 << tpin))) id.set_pin(tpin, group);
      }
      if (group < 0xD) ++group;
    } else {
      id.set_pin(pin, 0x1);  // isolated floating pin
    }
  }

  wbytes(i2c, addr, {0x01, 0x01});  // trigger soft reset
  OK_DETAIL("PIx6408(0x%02X) found, pins=%016llx", addr, id.pins);
  return id;
}

static OkDeviceId try_device(I2C& i2c, OkDeviceIdPart part) {
  switch (part) {
    case OKDEV_AWINIC_9523_x5B: return try_awinic_9523(part, i2c, 0x5B);
    case OKDEV_PERICOM_6408_x43: return try_pericom_6408(part, i2c, 0x43);
    default: OK_FATAL("Bad OkDeviceIdPart: %d", part); return {};
  }
}

OkDeviceId const& ok_device_id(
    I2C& i2c, bool rescan, std::initializer_list<OkDeviceIdPart> parts
) {
  static OkDeviceId id;
  static bool scanned;
  if (scanned && !rescan) return id;

  if (parts.size() > 0) {
    for (auto part : parts) {
      if ((id = try_device(i2c, part))) break;
    }
  } else {
    for (int part = OKDEV_NO_PART + 1; part < OKDEV_PART_MAX; ++part) {
      if ((id = try_device(i2c, (OkDeviceIdPart) part))) break;
    }
  }

  scanned = true;
  return id;
}
