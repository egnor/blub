// Detection for devices using strapped I2C IO expanders for identification

#pragma once

#include <cstdint>

#include <Arduino.h>
#include <Wire.h>

enum OkDeviceIdPart : uint8_t {
  OKDEV_NO_PART,
  OKDEV_AWINIC_9523_x5B,
  OKDEV_PERICOM_6408_x43,
};

struct OkDeviceId {
  OkDeviceIdPart part;
  uint64_t pins;  // 4b/pin: 0=ground, 1=NC, 2-E=group, E=VCC, F=wild
};

OkDeviceId const& ok_device_id(decltype(Wire)& = Wire, bool rescan = false);
bool ok_device_matches(OkDeviceId const&, OkDeviceId const&);

// List of known devices
// OKDEV_(github user/org)_(name)[_V(version)]
constexpr OkDeviceId
    OKDEV_EGNOR_FEATHER_DOCK{OKDEV_PERICOM_6408_x43, 0x00111111},
    OKDEV_EGNOR_BLUB{OKDEV_PERICOM_6408_x43, 0x10111111},
    OKDEV_EGNOR_BLUB_MINI{OKDEV_PERICOM_6408_x43, 0x01111111},
    OKDEV_PALACEGAMES_CRADLE{OKDEV_AWINIC_9523_x5B, 0x1111EEEEFFFFF111};
