#include "xbee_radio.h"

#include <tuple>

#include <Arduino.h>
#include <CircularBuffer.hpp>

#include <chatty_logging.h>

class XBeeRadioDef : public XBeeRadio {
 public:
  XBeeRadioDef(HardwareSerial* s) : serial(s) {}

  virtual bool poll_api(IncomingFrame* frame) override {
    auto const now = millis();
    auto const to_read = serial->available();
    for (int i = 0; i < to_read; ++i) {
      auto const ch = serial->read();
      if (ch < 0) break;  // Spurious available() seems to happen
      in_buf.push(ch);

      // Return frames immediately to avoid circular buffer overflow
      if (state == API_MODE && frame != nullptr) {
        // Format: <0x7E> <len MSB> <len LSB> <type>+<payload>... <checksum>
        while (in_buf.size() > 1 && in_buf[0] != 0x7E) in_buf.shift();
        if (frame == nullptr || in_buf.size() < 3) continue;

        auto const payload_size = (in_buf[1] << 8 | in_buf[2]) - 1;
        if (payload_size > MAX_API_PAYLOAD) {
          CL_PROBLEM(
              "Incoming XBee frame too big (%d > %d), ignoring",
              payload_size, MAX_API_PAYLOAD);
          in_buf.shift();
          continue;
        }

        if (in_buf.size() < payload_size + 5) continue;

        uint8_t checksum = 0;
        for (int i = 3; i < payload_size + 5; ++i) checksum += in_buf[i];
        if (checksum != 0xFF) {
          CL_PROBLEM(
              "Bad incoming XBee checksum (0x%02x != 0xFF), ignoring frame",
              checksum);
          in_buf.shift();
          continue;
        }

        auto const base_size = base_payload_size(in_buf[3]);
        if (base_size <= 0) {
          CL_SPAM("Unknown incoming XBee frame type: 0x%02x", frame->type);
          for (int i = 0; i < payload_size + 5; ++i) in_buf.shift();
          continue;
        }

        if (payload_size < base_size) {
          CL_PROBLEM(
              "Incoming XBee frame (0x%02x) too short (%d < %d), ignoring",
              frame->type, payload_size, base_size);
          in_buf.shift();
          continue;
        }

        frame->type = in_buf[3];
        frame->extra_size = payload_size - base_size;
        auto* bytes = reinterpret_cast<uint8_t*>(&frame->payload);

        for (int i = 0; i < 4; ++i) in_buf.shift();  // 0x7E, size, type
        for (int i = 0; i < payload_size; ++i) {
          bytes[i] = in_buf.shift();                 // Payload
        }
        in_buf.shift();                              // Checksum

        CL_SPAM(
            "Incoming XBee frame (0x%02x) size=%d+%d=%d",
            frame->type, base_size, frame->extra_size, payload_size);
        return true;
      }
    }

    auto const old_state = state;
    switch (state) {
      case START: {
        CL_SPAM("XBee radio setup starting");
        serial->begin(9600);  // Assume re-begin() is OK
        state = INIT_9600_DELAY_PLUSPLUSPLUS;
        state_millis = now;
        break;
      }

      case INIT_9600_DELAY_PLUSPLUSPLUS: {
        if (now - state_millis > 1100) {
          CL_SPAM("XBee sending +++ at 9600, waiting for OK");
          serial->print("+++");
          state = INIT_9600_EXPECT_OK;
          in_buf.clear();  // Ignore input before +++
        }
        break;
      }

      case INIT_9600_EXPECT_OK: {
        if (eat_ok()) {
          CL_SPAM("XBee got OK to +++ at 9600, switching to 115200");
          serial->print("ATBD7,AC\r");    // 115200 baud, apply changes
          state = INIT_SWITCH_TO_115200;  // Settle baud, then try AT
        } else if (now - state_millis > 1500) {
          CL_SPAM("XBee no OK at 9600, trying 115200");
          serial->begin(115200);  // Assume re-begin() is OK
          state = INIT_115200_DELAY_PLUSPLUSPLUS;
        }
        break;
      }

      case INIT_SWITCH_TO_115200: {
        if (now - state_millis > 100) {
          CL_SPAM("XBee sending AT at 115200, waiting for OK");
          serial->begin(115200);
          serial->print("AT\r");
          state = INIT_115200_EXPECT_OK;
          in_buf.clear();  // Ignore input before AT
        }
        break;
      }

      case INIT_115200_DELAY_PLUSPLUSPLUS: {
        if (now - state_millis > 1100) {
          CL_SPAM("XBee sending +++ at 115200, waiting for OK");
          serial->print("+++");
          state = INIT_115200_EXPECT_OK;
          in_buf.clear();  // Ignore input before +++
        }
        break;
      }

      case INIT_115200_EXPECT_OK: {
        if (eat_ok()) {
          CL_SPAM("XBee got OK at 115200, enabling API mode");
          serial->print("ATAP1,CN\r");  // enable API mode, exit command mode
          state = INIT_API_EXPECT_OKOK;
        } else if (now - state_millis > 1500) {
          CL_PROBLEM("XBee no OK at 115200, retrying init");
          serial->begin(9600);
          state = INIT_9600_DELAY_PLUSPLUSPLUS;
        }
        break;
      }

      case INIT_API_EXPECT_OKOK: {
        if (eat_ok(2)) {  // OK for ATAP1, OK for CN
          CL_SPAM("XBee radio running in API mode");
          state = API_MODE;
        } else if (now - state_millis > 1500) {
          CL_PROBLEM("XBee no OK for API mode, retrying");
          state = INIT_115200_DELAY_PLUSPLUSPLUS;
        }
        break;
      }

      case API_MODE: {
        for (;;) {
          auto const to_write = out_buf.size();
          if (to_write <= 0) break;
          auto const write_space = serial->availableForWrite();
          if (write_space <= 0) break;
          for (int i = 0; i < to_write && i < write_space; ++i) {
            serial->write(out_buf.shift());
          }
        }
        break;
      }

      default:
        CL_FATAL("Bad XBeeRadio state: %d", state);
    }

    if (state != old_state) {
      state_millis = now;
    }
    return false;
  }

  virtual int send_space_available() const override {
    return state == API_MODE ? out_buf.available() - 5 : 0;
  }

  virtual void send_api_frame(OutgoingFrame const& frame) override {
    if (state != API_MODE) {
      CL_PROBLEM("Outgoing XBee frame (0x%02x) before API ready, dropping");
      return;
    }

    auto const base_size = base_payload_size(frame.type);
    if (base_size <= 0) {
      CL_PROBLEM("Bad outgoing XBee frame type (0x%02x), dropping", frame.type);
      return;
    }

    auto const payload_size = base_size + frame.extra_size;
    if (payload_size < base_size) {
      CL_PROBLEM(
          "Outgoing XBee frame (0x%02x) too short (%d < %d), dropping",
          frame.type, payload_size, base_size);
      return;
    }
    if (payload_size > MAX_API_PAYLOAD) {
      CL_PROBLEM(
          "Outgoing XBee frame (0x%02x) too big (%d > %d), dropping",
          frame.type, payload_size, MAX_API_PAYLOAD);
      return;
    }

    auto const available = send_space_available();
    if (payload_size > available) {
      CL_PROBLEM(
          "Outgoing XBee frame too big for buffer (%d > %d), dropping",
          payload_size, available);
      return;
    }

    // Format: <0x7E> <len MSB> <len LSB> <type>+<payload>... <checksum>
    out_buf.push(0x7E);                       // Start delimiter
    out_buf.push((payload_size + 1) >> 8);    // Length (incl. type) MSB
    out_buf.push((payload_size + 1) & 0xFF);  // Length (incl. type) LSB
    out_buf.push(frame.type);                 // Frame type

    uint8_t checksum = frame.type;;
    auto const* bytes = reinterpret_cast<uint8_t const*>(&frame.payload);
    for (int i = 0; i < payload_size; ++i) {
      out_buf.push(bytes[i]);
      checksum += bytes[i];
    }
    out_buf.push(0xFF - checksum);

    CL_SPAM(
        "Outgoing XBee frame (0x%02x) size=%d+%d=%d",
        frame.type, base_size, frame.extra_size, payload_size);
  }

  virtual HardwareSerial* raw_serial() const override {
    return serial;
  }

 private:
  enum Status {
    START,
    INIT_9600_DELAY_PLUSPLUSPLUS,
    INIT_9600_EXPECT_OK,
    INIT_SWITCH_TO_115200,
    INIT_115200_DELAY_PLUSPLUSPLUS,
    INIT_115200_DELAY_AT,
    INIT_115200_EXPECT_OK,
    INIT_API_EXPECT_OKOK,
    API_MODE,
  };

  HardwareSerial* serial = nullptr;
  Status state = START;
  long state_millis = 0;
  CircularBuffer<uint8_t, MAX_API_PAYLOAD + 4> in_buf;
  CircularBuffer<uint8_t, MAX_API_PAYLOAD + 4> out_buf;

  bool eat_ok(int count = 1) {
    for (int i = 0; i < count; ++i) {
      auto const& b = in_buf;
      auto const p = b.size() - 3 * (count - i);
      if (p < 0) return false;
      if (b[p] != 'O' || b[p + 1] != 'K' || b[p + 2] != '\r') return false;
    }
    for (int i = 0; i < 3 * count; ++i) in_buf.shift();
    return true;
  }

  int base_payload_size(uint8_t payload_type) {
    switch (payload_type) {
#define S(x) case x::TYPE: return sizeof(x)
      S(ATCommand);
      S(ATCommandQueue);
      S(ATCommandResponse);
      S(TransmitSMS);
      S(ReceiveSMS);
      S(TransmitIP);
      S(TransmitTLS);
      S(ReceiveIP);
      S(TransmitStatus);
      S(ModemStatus);
      S(RelayToInterface);
      S(RelayFromInterface);
      S(FirmwareUpdate);
      S(FirmwareUpdateResponse);
      S(GnssRequest);
      S(GnssResponse);
      S(GnssNmea);
      S(GnssOneShot);
#undef S
    }
    return -1;  // Type not recognized
  }
};

char const* XBeeRadio::ATCommandResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(ERROR);
    S(BAD_COMMAND);
    S(BAD_PARAM);
#undef S
  }
  return "UNKNOWN";
}

char const* XBeeRadio::TransmitStatus::status_text() const {
  // There's no enum for this one (too many to be worth it)
  switch (status) {
    case 0x0: return "OK";
    case 0x20: return "CONNECTION_NOT_FOUND";
    case 0x21: return "NETWORK_FAILURE";
    case 0x22: return "NETWORK_DISCONNECTED";
    case 0x2C: return "BAD_FRAME";
    case 0x31: return "INTERNAL_ERROR";
    case 0x32: return "RESOURCE_ERROR";
    case 0x74: return "MESSAGE_TOO_LONG";
    case 0x76: return "SOCKET_CLOSED_UNEXPECTEDLY";
    case 0x78: return "BAD_UDP_PORT";
    case 0x79: return "BAD_TCP_PORT";
    case 0x7A: return "BAD_IP_ADDRESS";
    case 0x7B: return "BAD_DATA_MODE";
    case 0x7C: return "BAD_INTERFACE";
    case 0x7D: return "INTERFACE_CLOSED";
    case 0x7E: return "MODEM_UPDATE";
    case 0x80: return "CONNECTION_REFUSED";
    case 0x81: return "CONNECTION_LOST";
    case 0x82: return "NO_SERVER";
    case 0x83: return "SOCKET_CLOSED";
    case 0x84: return "UNKNOWN_SERVER";
    case 0x85: return "UNKNOWN_ERROR";
    case 0x86: return "BAD_TLS_CONFIG";
    case 0x87: return "SOCKET_DISCONNECTED";
    case 0x88: return "SOCKET_UNBOUND";
    case 0x89: return "SOCKET_INACTIVITY_TIMEOUT";
    case 0x8A: return "NETWORK_PDP_DEACTIVATED";
    case 0x8B: return "TLS_AUTHENTICATION_ERROR";
  }
  return "UNKNOWN";
}

char const *XBeeRadio::ModemStatus::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(POWER_UP);
    S(WATCHDOG_RESET);
    S(REGISTERED);
    S(UNREGISTERED);
    S(MANAGER_CONNECTED);
    S(MANAGER_DISCONNECTED);
    S(UPDATE_STARTED);
    S(UPDATE_FAILED);
    S(UPDATE_APPLYING);
#undef S
  }
  return "UNKNOWN";
}

char const* XBeeRadio::FirmwareUpdateResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(IN_PROGRESS);
    S(NOT_STARTED);
    S(SEQUENCE_ERROR);
    S(INTERNAL_ERROR);
    S(RESOURCE_ERROR);
#undef S
  }
  return "UNKNOWN";
}

char const* XBeeRadio::GnssOneShot::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(INVALID);
    S(TIMEOUT);
    S(CANCELLED);
#undef S
  }
  return "UNKNOWN";
}

XBeeRadio* make_xbee_radio(HardwareSerial* serial) {
  CL_ASSERT(serial != nullptr);
  return new XBeeRadioDef(serial);
}
