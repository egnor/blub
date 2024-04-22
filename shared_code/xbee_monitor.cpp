#include "xbee_monitor.h"

#include <array>

#include <Arduino.h>

#include "tagged_logging.h"

static const TaggedLoggingContext TL_CONTEXT("xbee_monitor");

using namespace XBeeAPI;

class XBeeMonitorDef : public XBeeMonitor {
 public:
  virtual void handle_frame(Frame const& frame) override {
    int extra;
    if (auto* r = frame.decode_as<ATCommandResponse>(&extra)) {
      if (r->frame_id < 128 || r->frame_id >= 128 + cyclics.size()) {
        return;
      }

      auto* cyc = &cyclics[r->frame_id - 128];
      if (memcmp(r->command, cyc->command, 2)) {
        TL_PROBLEM("XBee replied to %.2s with %.2s", cyc->command, r->command);
        return;
      }

      if (r->status != 0) {
        TL_PROBLEM("XBee answered %.2s with %s", r->command, r->status_text());
        return;
      }

      if (cyc->cb) (this->*cyc->cb)(cyc, *r, extra);
      return;
    }

    if (auto* modem = frame.decode_as<ModemStatus>()) {
      TL_SPAM("XBee modem status %s", modem->status_text());

      // Immediately update the status, then re-poll for the "proper" status
      switch (modem->status) {
        case ModemStatus::POWER_UP:
        case ModemStatus::WATCHDOG_RESET:
          stat.assoc_status = INITIALIZING;
          break;
        case ModemStatus::REGISTERED:
          stat.assoc_status = CONNECTED;
          break;
        case ModemStatus::UNREGISTERED:
          stat.assoc_status = REGISTERING;
          break;
        case ModemStatus::UPDATE_APPLYING:
          stat.assoc_status = FIRMWARE_UPDATE;
          break;
      }

      for (auto& cyc : cyclics) cyc.next_millis -= 10000;  // Re-poll
      return;
    }
  }

  virtual bool maybe_emit_frame(int available, Frame* frame) override {
    if (available < wire_size_of<ATCommand>(1)) return false;

    if (conf_carrier != UNKNOWN_PROFILE) {
      auto* command = frame->setup_as<ATCommand>(1);
      command->frame_id = 0;  // No ack, we'll just poll immediately after
      memcpy(command->command, "CP", 2);
      command->data[0] = conf_carrier;
      conf_carrier = UNKNOWN_PROFILE;

      TL_ASSERT(!memcmp(cyclics[0].command, "CP", 2));
      cyclics[0].next_millis = 0;  // Check immediately after setting
      cyclics[0].enabled = true;
      return true;
    }

    if (conf_apn[0]) {
      TL_SPAM("Requesting XBee APN \"%s\"", conf_apn);
      auto const apn_size = strlen(conf_apn);
      auto* command = frame->setup_as<ATCommand>(apn_size);
      command->frame_id = 0;  // No ack, we'll just poll immediately after
      memcpy(command->command, "AN", 2);
      memcpy(command->data, conf_apn, apn_size);
      conf_apn[0] = 0;

      TL_ASSERT(!memcmp(cyclics[1].command, "AN", 2));
      cyclics[1].next_millis = 0;  // Check immediately after setting
      cyclics[1].enabled = true;
      return true;
    }

    long const now = millis();
    Cyclic* next = nullptr;
    for (auto& cyc : cyclics) {
      if (!cyc.enabled) continue;
      if (cyc.next_millis - now > 0) continue;
      if (next == nullptr || next->next_millis - cyc.next_millis > 0) {
        next = &cyc;
      }
    }

    if (next != nullptr) {
      auto* command = frame->setup_as<ATCommand>();
      command->frame_id = 128 + (next - &cyclics[0]);
      memcpy(command->command, next->command, sizeof(command->command));
      next->next_millis = now + 10000;  // 20s poll (or status change)
      return true;
    }

    return false;
  }

  virtual Status const& status() const override { return stat; }

  virtual void configure_carrier(CarrierProfile carrier) {
    conf_carrier = carrier;
    cyclics[0].next_millis = 0;  // Force immediate set
  }

  virtual void configure_apn(char const* apn) {
    copy_text(apn, strlen(apn), conf_apn);
    cyclics[1].next_millis = 0;  // Force immediate set
  }

 private:
  struct Cyclic {
    char command[3];
    void (XBeeMonitorDef::*cb)(Cyclic*, ATCommandResponse const&, int extra);
    bool enabled = true;
    long next_millis = 0;
  };

  std::array<Cyclic, 15> cyclics{{
    { "CP", &XBeeMonitorDef::handle_config_cp },        // Must be [0]
    { "AN", &XBeeMonitorDef::handle_config_apn },  // Must be [1]
    { "HV", &XBeeMonitorDef::handle_hver },
    { "VR", &XBeeMonitorDef::handle_fver },
    { "S#", &XBeeMonitorDef::handle_iccid },
    { "IM", &XBeeMonitorDef::handle_imei },
    { "II", &XBeeMonitorDef::handle_imsi },
    { "AI", &XBeeMonitorDef::handle_assoc },
    { "MN", &XBeeMonitorDef::handle_operator },
    { "DT", &XBeeMonitorDef::handle_time },
    { "OA", &XBeeMonitorDef::handle_operating_apn },
    { "OT", &XBeeMonitorDef::handle_technology },
    { "SQ", &XBeeMonitorDef::handle_rsrq },
    { "SW", &XBeeMonitorDef::handle_rsrp },
    { "MY", &XBeeMonitorDef::handle_ip },
  }};

  Status stat = {};

  CarrierProfile conf_carrier = UNKNOWN_PROFILE;
  char conf_apn[50] = "";

  template <int N>
  void copy_text(void const* from, int from_size, char (&to)[N]) {
    int const len = std::min(from_size, N - 1);
    memcpy(to, from, len);
    to[len] = 0;
  }

  void handle_hver(Cyclic* cyc, ATCommandResponse const& r, int extra) {
    if (extra == 2) {
      stat.hardware_ver = *reinterpret_cast<uint16_be const*>(r.data);
      TL_SPAM("XBee hardware version %04x", stat.hardware_ver);
      cyc->enabled = false;  // Won't change
    } else {
      TL_PROBLEM("Bad XBee reply length (MV: %d != 2 bytes)", extra);
    }
  }

  void handle_fver(Cyclic* cyc, ATCommandResponse const& r, int extra) {
    if (extra == 4) {
      stat.firmware_ver = *reinterpret_cast<uint32_be const*>(r.data);
      TL_SPAM("XBee firmware version %05x", stat.firmware_ver);
      cyc->enabled = false;  // Won't change
    } else {
      TL_PROBLEM("Bad XBee reply length (VR: %d != 4 bytes)", extra);
    }
  }

  void handle_iccid(Cyclic* cyc, ATCommandResponse const& r, int extra) {
    copy_text(r.data, extra, stat.iccid);
    TL_SPAM("XBee ICCID \"%s\"", stat.iccid);
    if (stat.iccid[0]) cyc->enabled = false;  // Disable (it won't change)
  }

  void handle_imei(Cyclic* cyc, ATCommandResponse const& r, int extra) {
    copy_text(r.data, extra, stat.imei);
    TL_SPAM("XBee IMEI \"%s\"", stat.imei);
    if (stat.imei[0]) cyc->enabled = false;  // Disable (it won't change)
  }

  void handle_imsi(Cyclic* cyc, ATCommandResponse const& r, int extra) {
    copy_text(r.data, extra, stat.imsi);
    if (stat.imsi[0]) cyc->enabled = false;  // Disable (it won't change)
  }

  void handle_config_cp(Cyclic* cyc, ATCommandResponse const& r, int extra) {
    if (extra == 1) {
      stat.carrier_profile = (CarrierProfile) r.data[0];
      TL_SPAM("XBee carrier profile %s", stat.carrier_profile_text());
      cyc->enabled = false;  // Won't change (unless we change it)
    } else if (extra != 0) {
      TL_PROBLEM("Bad XBee reply length (CP: %d != 1 byte)", extra);
    }
  }

  void handle_assoc(Cyclic*, ATCommandResponse const& r, int extra) {
    if (extra == 1) {
      stat.assoc_status = (AssociationStatus) r.data[0];
      TL_SPAM("XBee assoc status %s", stat.assoc_text());
    } else {
      TL_PROBLEM("Bad XBee reply length (AI: %d != 1 byte)", extra);
    }
  }

  void handle_operator(Cyclic*, ATCommandResponse const& r, int extra) {
    copy_text(r.data, extra, stat.network_operator);
    TL_SPAM("XBee network operator \"%s\"", stat.network_operator);
  }

  void handle_time(Cyclic*, ATCommandResponse const& r, int extra) {
    if (extra == 4) {
      uint8_t const* d = r.data;
      uint32_t const value = *reinterpret_cast<uint32_be const*>(d);
      stat.network_time = value;
      stat.network_time_millis = millis();
      uint32_t const offset = value - stat.network_time_millis / 1000;
      TL_SPAM("XBee network time %lu (offset %lu)", value, offset);
    } else if (extra != 0) {
      TL_PROBLEM("Bad XBee reply length (DT: %d != 4 bytes)", extra);
    }
  }

  void handle_config_apn(Cyclic* cyc, ATCommandResponse const& r, int extra) {
    copy_text(r.data, extra, stat.requested_apn);
    TL_SPAM("XBee requested APN \"%s\"", stat.requested_apn);
    cyc->enabled = false;  // Won't change (unless we change it)
  }

  void handle_operating_apn(Cyclic*, ATCommandResponse const& r, int extra) {
    copy_text(r.data, extra, stat.operating_apn);
    TL_SPAM("XBee operating APN \"%s\"", stat.operating_apn);
  }

  void handle_technology(Cyclic*, ATCommandResponse const& r, int extra) {
    if (extra == 2) {
      uint16_t const value = *reinterpret_cast<uint16_be const*>(r.data);
      stat.technology = (Technology) value;
      TL_SPAM("XBee technology %s", stat.technology_text());
    } else if (extra != 0) {
      TL_PROBLEM("Bad XBee reply length (OT: %d != 1 byte)", extra);
    }
  }

  void handle_rsrq(Cyclic*, ATCommandResponse const& r, int extra) {
    if (extra == 2) {
      uint16_t const value = *reinterpret_cast<uint16_be const*>(r.data);
      stat.received_quality = value * -0.1f;
      TL_SPAM("XBee signal quality %.1fdbm", stat.received_quality);
    } else if (extra != 0) {
      TL_PROBLEM("Bad XBee reply length (SQ: %d != 1 byte)", extra);
    }
  }

  void handle_rsrp(Cyclic*, ATCommandResponse const& r, int extra) {
    if (extra == 2) {
      uint16_t const value = *reinterpret_cast<uint16_be const*>(r.data);
      stat.received_power = value * -0.1f;
      TL_SPAM("XBee signal power %.1fdbm", stat.received_power);
    } else if (extra != 0) {
      TL_PROBLEM("Bad XBee reply length (SW: %d != 1 byte)", extra);
    }
  }

  void handle_ip(Cyclic*, ATCommandResponse const& r, int extra) {
    if (extra == 4) {
      uint8_t const* ip = r.data;
      memcpy(stat.ip_address, ip, 4);
      TL_SPAM("XBee IP %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    } else if (extra != 0) {
      TL_PROBLEM("Bad XBee reply length (MY: %d != 4 bytes)", extra);
    }
  }
};

XBeeMonitor* make_xbee_monitor() {
  return new XBeeMonitorDef();
}

char const* XBeeMonitor::Status::carrier_profile_text() const {
  switch (carrier_profile) {
#define S(x) case x: return #x
    S(AUTODETECT);
    S(NO_PROFILE);
    S(ATT);
    S(VERIZON);
    S(AUSTRALIA);
#undef S
    default: return "UNKNOWN_PROFILE";
  }
}

char const* XBeeMonitor::Status::assoc_text() const {
  switch (assoc_status) {
#define S(x) case x: return #x
    S(CONNECTED);
    S(REGISTERING);
    S(CONNECTING);
    S(NO_HARDWARE);
    S(REGISTRATION_DENIED);
    S(AIRPLANE_MODE);
    S(USB_DIRECT_MODE);
    S(POWER_SAVE_MODE);
    S(COMMANDED_SHUTDOWN);
    S(LOW_VOLTAGE_SHUTDOWN);
    S(BYPASS_MODE);
    S(FIRMWARE_UPDATE);
    S(REGULATORY_TESTING);
    S(INITIALIZING);
#undef S
    default: return "UNKNOWN_ASSOC";
  }
}

char const* XBeeMonitor::Status::technology_text() const {
  switch (technology) {
#define S(x) case x: return #x
    S(GSM);
    S(LTE_M);
    S(NB_IOT);
#undef S
    default: return "UNKNOWN_TECH";
  }
}
