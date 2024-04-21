#include "xbee_radio.h"

#include <tuple>

#include <Arduino.h>
#include <CircularBuffer.hpp>

#include <chatty_logging.h>

class XBeeRadioDef : public XBeeRadio {
 public:
  XBeeRadioDef(HardwareSerial* s) : serial(s) {}

  virtual bool poll_api(XBeeApi::Frame* frame) override {
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
        if (frame == nullptr || in_buf.size() < 5) continue;

        auto const size = (in_buf[1] << 8 | in_buf[2]) - 1;  // Without type
        if (size > XBeeApi::MAX_PAYLOAD) {
          CL_PROBLEM(
              "Incoming XBee frame too big (%d > %d), ignoring",
              size, XBeeApi::MAX_PAYLOAD);
          in_buf.shift();
          continue;
        }

        if (in_buf.size() < size + 5) continue;

        uint8_t checksum = 0;
        for (int i = 3; i < size + 5; ++i) checksum += in_buf[i];
        if (checksum != 0xFF) {
          CL_PROBLEM(
              "Bad incoming XBee frame checksum (0x%02x != 0xFF), ignoring",
              checksum);
          in_buf.shift();
          continue;
        }

        frame->type = in_buf[3];
        frame->payload_size = size;

        for (int i = 0; i < 4; ++i) in_buf.shift();  // 0x7E, size, type
        for (int i = 0; i < size; ++i) frame->payload[i] = in_buf.shift();
        in_buf.shift();                              // Checksum

        CL_SPAM("Incoming XBee frame (0x%02x) %d bytes", frame->type, size);
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

  virtual int send_available() const override {
    return state == API_MODE ? out_buf.available() : 0;
  }

  virtual void send_api_frame(XBeeApi::Frame const& frame) override {
    if (state != API_MODE) {
      CL_PROBLEM("Outgoing XBee frame (0x%02x) before API ready, dropping");
      return;
    }

    if (frame.payload_size > XBeeApi::MAX_PAYLOAD) {
      CL_PROBLEM(
          "Outgoing XBee frame (0x%02x) too big (%d > %d), dropping",
          frame.type, frame.payload_size, XBeeApi::MAX_PAYLOAD);
      return;
    }

    auto const available = send_available();
    if (frame.payload_size + 5 > available) {
      CL_PROBLEM(
          "Outgoing XBee frame too big for buffer (%d > %d), dropping",
          frame.payload_size + 5, available);
      return;
    }

    // Format: <0x7E> <len MSB> <len LSB> <type>+<payload>... <checksum>
    out_buf.push(0x7E);                             // Start delimiter
    out_buf.push((frame.payload_size + 1) >> 8);    // Length (incl. type) MSB
    out_buf.push((frame.payload_size + 1) & 0xFF);  // Length (incl. type) LSB
    out_buf.push(frame.type);                       // Frame type

    uint8_t checksum = frame.type;
    for (int i = 0; i < frame.payload_size; ++i) {
      out_buf.push(frame.payload[i]);
      checksum += frame.payload[i];
    }
    out_buf.push(0xFF - checksum);

    CL_SPAM(
        "Outgoing XBee frame (0x%02x) %d bytes",
        frame.type, frame.payload_size);
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
  CircularBuffer<uint8_t, XBeeApi::MAX_PAYLOAD + 5> in_buf;
  CircularBuffer<uint8_t, XBeeApi::MAX_PAYLOAD + 5> out_buf;

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
};

XBeeRadio* make_xbee_radio(HardwareSerial* serial) {
  CL_ASSERT(serial != nullptr);
  return new XBeeRadioDef(serial);
}
