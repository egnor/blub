#include "ok_device_id.h"
#include "ok_logging.h"

using I2C = decltype(Wire);

static const OkLoggingContext OK_CONTEXT("ok_device_id");

static uint8_t get_pin(OkDeviceId const& id, int pin) {
  OK_FATAL_IF(pin < 0 || pin >= 16);
  return (id.pins >> (pin * 4)) & 0xF;
}

void set_pin(OkDeviceId* id, int pin, uint8_t value) {
  OK_FATAL_IF(pin < 0 || pin >= 16 || value > 0xF);
  id->pins = (id->pins & ~(0xF << (pin * 4))) | (value << (pin * 4));
}

bool ok_device_matches(OkDeviceId const& a, OkDeviceId const&b) {
  if (a.part != b.part) return false;
  if (a.part == OKDEV_NO_PART) return true;
  for (int i = 0; i < 16; ++i) {
    auto const ap = get_pin(a, i), bp = get_pin(b, i);
    if (ap != 0xF && bp != 0xF && ap != bp) return false;
  }
  return true;
}

static bool wbytes(I2C& i2c, uint8_t addr, std::initializer_list<int> b) {
  i2c.beginTransmission(addr);
  for (auto byte : b) i2c.write(byte);
  auto const status = i2c.endTransmission();
  if (status != 0) OK_ERROR("I2C(0x%02X) write failed (%d)", addr, status);
  return (status == 0);
}

static uint8_t rbyte(I2C& i2c, uint8_t addr) {
  i2c.requestFrom(addr, 1);
  if (i2c.available() < 1) OK_ERROR("I2C(0x%02X) read failed (no data)", addr);
  return i2c.read();
}

static bool try_awinic_9523(
    I2C& i2c, uint8_t addr, OkDeviceIdPart part, OkDeviceId* id
) {
  i2c.beginTransmission(addr);
  auto const i2c_status = i2c.endTransmission();
  if (i2c_status == 2) {
    OK_DETAIL("AW9523(0x%02X) not present (NACK)", addr);
    return false;
  } else if (i2c_status != 0) {
    OK_ERROR("AW9523(0x%02X) probe error (%d)", addr, i2c_status);
    return false;
  }

  if (!wbytes(i2c, addr, {0x23})) return false;  // ID register
  uint8_t const id_byte = rbyte(i2c, addr);
  if (id_byte != 0x23) {
    OK_DETAIL("AW9523(0x%02X) not present (ID 0x%02X != 0x23)", addr, id_byte);
    return false;
  }

  if (!wbytes(i2c, addr, {0x7F, 0x00})) return false;  // trigger soft reset

  *id = {part, 0xFFFFFFFFFFFFFFFF};
  wbytes(i2c, addr, {0x11, 0x10});  // P0 push-pull
  wbytes(i2c, addr, {0x04, 0xFF});  // P0 all input
  wbytes(i2c, addr, {0x05, 0xFF});  // P1 all input

  // AW9523 has no pull resistors, use capacitance to detect pin pull
  for (uint8_t port = 0; port < 2; ++port) {
    wbytes(i2c, addr, {0x02 + port, 0x00});  // port all low to preload
    wbytes(i2c, addr, {0x04 + port, 0x00});  // port all output
    wbytes(i2c, addr, {0x04 + port, 0xFF});  // port all input
    wbytes(i2c, addr, {0x00 + port});  // read port pins
    uint8_t const low_readback = rbyte(i2c, addr);

    wbytes(i2c, addr, {0x02 + port, 0xFF});  // port all high to preload
    wbytes(i2c, addr, {0x04 + port, 0x00});  // port all output
    wbytes(i2c, addr, {0x04 + port, 0xFF});  // port all input
    wbytes(i2c, addr, {0x00 + port});  // read port pins
    uint8_t const high_readback = rbyte(i2c, addr);

    for (int pin = 0; pin < 8; ++pin) {
      if ((low_readback & (1 << pin))) set_pin(id, port * 8 + pin, 0xE);
      if (!(high_readback & (1 << pin))) set_pin(id, port * 8 + pin, 0x0);
    }
  }

  // test floating pins that could be tied to each other
  int group = 2;
  for (int port = 0; port < 2; ++port) {
    for (int pin = 0; pin < 8; ++pin) {
      if (get_pin(*id, port * 8 + pin) != 0xFF) continue;  // already determined

      uint8_t test_source[2] = {0x00, 0x00};  // the pin we're testing
      test_source[port] = (1 << pin);

      uint8_t test_targets[2] = {0x00, 0x00};  // pins to test for grouping
      for (int gport = 0; gport < 2; ++gport) {
        for (int gpin = 0; gpin < 8; ++gpin) {
          if (gport == port && gpin == pin) continue;  // skip self
          if (get_pin(*id, gport * 8 + gpin) != 0xFF) continue;  // determined
          test_targets[gport] &= ~(1 << gpin);
        }
      }

      // preload potentially grouped pins low
      wbytes(i2c, addr, {0x02, test_source[0]});  // P0 level
      wbytes(i2c, addr, {0x03, test_source[1]});  // P1 level
      wbytes(i2c, addr, {0x04, ~test_targets[0]});  // P0 dir
      wbytes(i2c, addr, {0x05, ~test_targets[1]});  // P1 dir

      // revert targets to input but drive source high
      wbytes(i2c, addr, {0x05 - port, ~test_source[1 - port]});
      wbytes(i2c, addr, {0x04 + port, ~test_source[port]});

      uint8_t readback[2];
      wbytes(i2c, addr, {0x00});  // read P0
      readback[0] = rbyte(i2c, addr) & test_targets[0];
      wbytes(i2c, addr, {0x01});  // read P1
      readback[1] = rbyte(i2c, addr) & test_targets[1];

      if (readback[0] || readback[1]) {
        set_pin(id, port * 8 + pin, group);  // pin group
        for (int gport = 0; gport < 2; ++gport) {
          for (int gpin = 0; gpin < 8; ++gpin) {
            if ((readback[gport] & (1 << gpin))) {
              set_pin(id, gport * 8 + gpin, group);  // grouped with
            }
          }
        }
      } else {
        set_pin(id, port * 8 + pin, 0x1);  // isolated floating pin
      }
    }
  }

  wbytes(i2c, addr, {0x7F, 0x00});  // trigger soft reset
  OK_DETAIL("AW9523(0x%02X) found, pins=%016llx", addr, id->pins);
  return true;
}

static bool try_pericom_6408(
    I2C& i2c, uint8_t addr, OkDeviceIdPart part, OkDeviceId* id
) {
  i2c.beginTransmission(addr);
  auto const i2c_status = i2c.endTransmission();
  if (i2c_status == 2) {
    OK_DETAIL("PIx6408(0x%02X) not present (NACK)", addr);
    return false;
  } else if (i2c_status != 0) {
    OK_ERROR("PIx6408(0x%02X) probe error (%d)", addr, i2c_status);
    return false;
  }

  if (!wbytes(i2c, addr, {0x01})) return false;  // ID register
  uint8_t const id_byte = rbyte(i2c, addr);
  if ((id_byte & ~0x02) != 0xA0) {
    OK_DETAIL("PIx6408(0x%02X) not present (ID 0x%02X != 0xA0)", addr, id_byte);
    return false;
  }

  OK_DETAIL("PIx6408(0x%02X) found, pins=%016llx", addr, id->pins);
  return false;
}

OkDeviceId const& ok_device_id(I2C& i2c, bool rescan) {
  static OkDeviceId id;
  static bool scanned;
  if (scanned && !rescan) return id;

  scanned = true;
  if (try_awinic_9523(i2c, 0x5B, OKDEV_AWINIC_9523_x5B, &id)) return id;
  if (try_pericom_6408(i2c, 0x43, OKDEV_PERICOM_6408_x43, &id)) return id;
  id = {};
  return id;
}
