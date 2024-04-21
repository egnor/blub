#include "xbee_monitor.h"

#include <array>

#include <Arduino.h>

using namespace XBeeApi;

class XBeeMonitorDef : public XBeeMonitor {
 public:
  virtual bool maybe_emit_frame(int available, Frame* frame) override {
    if (available < wire_size<ATCommand>(1)) return false;

    long const now = millis();
    Cyclic* next = nullptr;
    for (auto& cycle : cycles) {
      if (cycle.command[0] == 0) continue;  // Disabled
      if (now - cycle.last_millis < 1000) continue;
      if (next == nullptr || cycle.last_millis < next->last_millis) {
        next = &cycle;
      }
    }

    if (next == nullptr) return false;
    auto* command = frame->setup_as<ATCommand>();
    command->frame_id = 100 + (next - &cycles[0]);
    memcpy(command->command, next->command, 2);
    CL_SPAM("XBeeMonitor sending %.2s", command->command);

    next->last_millis = now;
    return true;
  }

  virtual bool maybe_handle_frame(Frame const& frame) override {
    int extra;
    if (auto* r = frame.decode_as<ATCommandResponse>(&extra)) {
      if (r->frame_id < 100 || r->frame_id >= 100 + cycles.size()) return false;
      auto* cyc = &cycles[r->frame_id - 100];
      if (memcmp(r->command, cyc->command, 2)) {
        CL_PROBLEM("XBee replied to %.2s with %.2s", cyc->command, r->command);
        return true;
      }

      if (r->status != 0) {
        CL_PROBLEM("XBee answered %.2s with %s", r->command, r->status_text());
        return true;
      }

      CL_SPAM("XBee replied to %.2s command (%d bytes)", r->command, extra);
      if (cyc->cb) (this->*cyc->cb)(cyc, *r, extra);
      return true;
    }

    return false;
  }

  virtual Status const& status() const override { return stat; }

 private:
  struct Cyclic {
    char command[3];
    void (XBeeMonitorDef::*cb)(Cyclic*, ATCommandResponse const&, int extra);
    long last_millis;
  };

  std::array<Cyclic, 13> cycles{{
    { "AI", &XBeeMonitorDef::handle_assoc },  // Assoc status
    { "MN" },  // Operator
    { "DT" },  // Network Time
    { "OA" },  // Operating APN
    { "OT" },  // Technology
    { "SQ" },  // Signal Quality
    { "SW" },  // Signal Watts
    { "MY" },  // IP address
    { "S#", &XBeeMonitorDef::handle_iccid }, // ICCID
    { "IM", &XBeeMonitorDef::handle_imei },  // IMEI
    { "II", &XBeeMonitorDef::handle_imsi },  // IMSI
    { "HV", &XBeeMonitorDef::handle_hver },  // Hardware Version
    { "VR", &XBeeMonitorDef::handle_fver }   // Firmware Version
  }};

  Status stat;

  void handle_assoc(Cyclic*, ATCommandResponse const& r, int extra) {
    if (extra != 1) {
      CL_PROBLEM("Bad XBee AI reply length (%d != 1 byte)", extra);
      return;
    }
    stat.assoc_status = r.data[0];
    CL_SPAM("XBee assoc status %s", stat.assoc_text());
  }

  void handle_iccid(Cyclic* cycle, ATCommandResponse const& r, int extra) {
    int const len = std::min<int>(extra, sizeof(stat.iccid) - 1);
    strncpy(stat.iccid, (char const*) r.data, len + 1);
    CL_SPAM("XBee ICCID \"%s\"", stat.iccid);
    if (len > 0) cycle->command[0] = 0;  // Disable (it won't change)
  }

  void handle_imei(Cyclic* cycle, ATCommandResponse const& r, int extra) {
    int const len = std::min<int>(extra, sizeof(stat.imei) - 1);
    strncpy(stat.imei, (char const*) r.data, len + 1);
    CL_SPAM("XBee IMEI \"%s\"", stat.imei);
    if (len > 0) cycle->command[0] = 0;  // Disable (it won't change)
  }

  void handle_imsi(Cyclic* cycle, ATCommandResponse const& r, int extra) {
    int const len = std::min<int>(extra, sizeof(stat.imsi) - 1);
    strncpy(stat.imsi, (char const*) r.data, len + 1);
    CL_SPAM("XBee IMSI \"%s\"", stat.imsi);
    if (len > 0) cycle->command[0] = 0;  // Disable (it won't change)
  }

  void handle_hver(Cyclic* cycle, ATCommandResponse const& r, int extra) {
    if (extra != 2) {
      CL_PROBLEM("Bad XBee HV reply length (%d != 2 bytes)", extra);
      stat.hardware_ver = -1;
    } else {
      stat.hardware_ver = (r.data[0] << 8) | r.data[1];
      CL_SPAM("XBee hardware version %04x", stat.hardware_ver);
      cycle->command[0] = 0;  // Disable (it won't change)
    }
  }

  void handle_fver(Cyclic* cycle, ATCommandResponse const& r, int extra) {
    if (extra != 4) {
      CL_PROBLEM("Bad XBee VR reply length (%d != 4 bytes)", extra);
      stat.firmware_ver = -1;
    } else {
      uint8_t const* d = r.data;
      stat.firmware_ver = (d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
      CL_SPAM("XBee firmware version %05x", stat.firmware_ver);
      cycle->command[0] = 0;  // Disable (it won't change)
    }
  }
};

XBeeMonitor* make_xbee_monitor() {
  return new XBeeMonitorDef();
}

char const* XBeeMonitor::Status::assoc_text() {
  switch (assoc_status) {
    case 0x00: return "CONNECTED";
    case 0x22: return "REGISTERING";
    case 0x23: return "CONNECTING";
    case 0x24: return "NO_MODEM";
    case 0x25: return "REGISTRATION_DENIED";
    case 0x2A: return "AIRPLANE_MODE";
    case 0x2B: return "USB_DIRECT";
    case 0x2C: return "POWER_SAVE";
    case 0x2D: return "COMMAND_SHUTDOWN";
    case 0x2E: return "POWER_SHUTDOWN";
    case 0x2F: return "BYPASS_MODE";
    case 0x30: return "UPDATE_ACTIVE";
    case 0x31: return "REGULATORY_TESTING";
    case 0xFF: return "INITIALIZING";
    default: return "UNKNOWN_ASSOC";
  }
}
