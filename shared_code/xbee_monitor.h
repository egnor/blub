#pragma once 

#include <stdint.h>

#include "xbee_api.h"

class XBeeMonitor {
 public:
  struct Status {
    uint16_t hardware_ver;
    uint16_t firmware_ver;
    char iccid[21];
    char imei[16];
    char imsi[16];
    uint8_t assoc_status;
    char network_operator[16];
    char access_point[50];
    uint32_t network_time;
    uint8_t technology;
    float rsrp_db;
    float rsrq_db;
    uint32_t device_ip;

    char const* assoc_text();
    char const* technology_text();
  };

  virtual ~XBeeMonitor() = default;
  virtual bool maybe_emit_frame(int available, XBeeApi::Frame*) = 0;
  virtual bool maybe_handle_frame(XBeeApi::Frame const&) = 0;
  virtual Status const& status() const = 0;
};

XBeeMonitor* make_xbee_monitor();
