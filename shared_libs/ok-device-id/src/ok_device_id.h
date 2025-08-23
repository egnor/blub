// Detection for devices using strapped I2C IO expanders for identification

#pragma once

#include <cstdint>
#include <initializer_list>

#include <Arduino.h>
#include <Wire.h>

enum OkDeviceIdPart : uint8_t {
  OKDEV_NO_PART,
  OKDEV_AWINIC_9523_x5B,
  OKDEV_PERICOM_6408_x43,
  OKDEV_PART_MAX,
};

struct OkDeviceId {
  OkDeviceIdPart part;
  uint64_t pins;  // 4b/pin: 0=ground, 1=NC, 2-0xD=group, 0xE=high, 0xF=wild

  operator bool() const { return part != OKDEV_NO_PART; }
  bool matches(OkDeviceId const& other) const;
  uint8_t pin(int pin) const { return (pins >> (pin * 4)) & 0x0F; }
  void set_pin(int pin, uint8_t value);
};

OkDeviceId const& ok_device_id(
    decltype(Wire)& = Wire, bool rescan = false,
    std::initializer_list<OkDeviceIdPart> = {}
);

// List of known devices
// OKDEV_(github user/org)_(name)[_V(version)]
constexpr OkDeviceId
    OKDEV_EGNOR_FEATHER_DOCK{OKDEV_PERICOM_6408_x43, 0x00111111},
    OKDEV_EGNOR_BLUB{OKDEV_PERICOM_6408_x43, 0x10111111},
    OKDEV_EGNOR_BLUB_MINI{OKDEV_PERICOM_6408_x43, 0x01111111},
    OKDEV_PALACEGAMES_CRADLE{OKDEV_AWINIC_9523_x5B, 0x1111EEEEFFF11111};
