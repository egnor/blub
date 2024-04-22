#pragma once

#include <stdint.h>

#include "xbee_api.h"

class XBeeMonitor {
 public:
  enum CarrierProfile : uint8_t {
    AUTODETECT = 0, NO_PROFILE = 1, ATT = 2, VERIZON = 3, AUSTRALIA = 4,
    UNKNOWN_PROFILE = 0xFF,
  };

  enum AssociationStatus : uint8_t {
    CONNECTED = 0x00, REGISTERING = 0x22, CONNECTING = 0x23,
    NO_HARDWARE = 0x24, REGISTRATION_DENIED = 0x25,
    AIRPLANE_MODE = 0x2A, USB_DIRECT_MODE = 0x2B, POWER_SAVE_MODE = 0x2C,
    COMMANDED_SHUTDOWN = 0x2D, LOW_VOLTAGE_SHUTDOWN = 0x2E,
    BYPASS_MODE = 0x2F, FIRMWARE_UPDATE = 0x30,
    REGULATORY_TESTING = 0x31, INITIALIZING = 0xFF,
  };

  enum Technology : uint16_t {
    GSM = 0, LTE_M = 8, NB_IOT = 9, UNKNOWN_TECH = 0xFFFF,
  };

  struct Status {
    uint16_t hardware_ver;
    uint32_t firmware_ver;
    char iccid[21];
    char imei[16];
    char imsi[16];
    CarrierProfile carrier_profile = UNKNOWN_PROFILE;
    AssociationStatus assoc_status = INITIALIZING;
    char network_operator[16];
    char requested_apn[50];
    char operating_apn[50];
    uint32_t network_time;
    long network_time_millis;
    Technology technology = UNKNOWN_TECH;
    float received_power;
    float received_quality;
    uint8_t ip_address[4];

    char const* carrier_profile_text() const;
    char const* assoc_text() const;
    char const* technology_text() const;
  };

  virtual ~XBeeMonitor() = default;
  virtual void handle_frame(XBeeAPI::Frame const&) = 0;
  virtual bool maybe_emit_frame(int available, XBeeAPI::Frame*) = 0;

  virtual Status const& status() const = 0;
  virtual void configure_carrier(CarrierProfile) = 0;
  virtual void configure_apn(char const*) = 0;
};

XBeeMonitor* make_xbee_monitor();
