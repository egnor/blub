#include "cell_modem_client.h"

#include <Arduino.h>
#include <etl/chrono.h>
#include <etl/circular_buffer.h>
#include <etl/format.h>
#include <etl/string.h>
#include <etl/string_utilities.h>
#include <memory>
#include <ok_logging.h>

using namespace etl::chrono_literals;

static const OkLoggingContext OK_CONTEXT("cell_modem_client");

class CellModemClientDef : public CellModemClient {
 public:
  CellModemClientDef(HardwareSerial* s, etl::string_view mqtt)
    : serial(s), mqtt_server(mqtt) {}

  CellModemStatus const& poll() override {
    auto const avail = serial->available();
    while (avail > 0) {
      if (in_buf.full()) {
        OK_ERROR("Dropping big input: %s", input_summary().c_str());
        in_buf.clear();
      }
      auto const ch = serial->read();
      if (ch < 0) {
        OK_ERROR("Serial read error: available=%d ch=%d", avail, ch);
        break;
      }
      in_buf.push_back(ch);
      if (in_expect > 0 ? in_buf.size() >= in_expect : in_buf.back() == '\n') {
        OK_DETAIL("Input: %s", input_summary().c_str());
        handle_input();
        in_buf.clear();
        in_expect = 0;
      }
    }

    auto const now = etl::chrono::steady_clock::now();
    if (state != State::IDLE && now >= state_deadline) {
      OK_ERROR("Command timeout (state=%d), polling", state);
      state = State::IDLE;
      next_status_poll = {};  // Poll until we get a response
    }

    if (state == State::IDLE && out_complete >= out_buf.size()) {
      out_complete = 0;
      out_buf.clear();
      if (status.hardware.empty()) {
        out_buf = "AT+CGMM\r\n";
        state = State::AT_CGMM_WAIT;
      } else if (status.imeisv.empty()) {
        out_buf = "AT+CGSN=1\r\n";
        state = State::AT_CGSN_WAIT;
      } else if (status.versions[0].empty()) {
        out_buf = "AT+CGMR\r\n";
        state = State::AT_CGMR_WAIT;
      } else if (status.versions[1].empty()) {
        out_buf = "AT#XSMVER\r\n";
        state = State::AT_XSMVER_WAIT;
      }
      // TODO: more outgoing
    }

    while (out_complete < out_buf.size() && serial->availableForWrite() > 0) {
      serial->write(out_buf[out_complete++]);
    }

    return status;
  }

 private:
  using duration = etl::chrono::steady_clock::duration;
  using time_point = etl::chrono::steady_clock::time_point;

  HardwareSerial* const serial;
  etl::string<128> const mqtt_server;

  enum class State {
    IDLE,
    AT_CGMM_WAIT,
    AT_CGMR_WAIT,
    AT_CGSN_WAIT,
    AT_XSMVER_WAIT,
    OK_WAIT,
  };

  State state = State::IDLE;
  time_point state_deadline = {};
  time_point next_status_poll = {};
  CellModemStatus status;

  etl::string<8192> in_buf;
  int in_expect = 0;

  etl::string<256> out_buf;
  int out_complete = 0;

  void handle_input() {
    if (in_expect > 0) {
      // TODO: handle
      return;
    }

    auto const trimmed = etl::trim_view_whitespace(in_buf);
    switch (state) {
      case State::IDLE: {
        break;
      }

      case State::AT_CGMM_WAIT: {
        status.hardware = trimmed.empty() ? "-" : trimmed;
        state = State::OK_WAIT;
        break;
      }

      case State::AT_CGMR_WAIT: {
        status.versions[0] = trimmed.empty() ? "-" : trimmed;
        state = State::OK_WAIT;
        break;
      }

      case State::AT_CGSN_WAIT: {
        status.imeisv = trimmed.empty() ? "-" : trimmed;
        state = State::OK_WAIT;
        break;
      }

      case State::AT_XSMVER_WAIT: {
        // TODO: parse XSMVER reply
        state = State::OK_WAIT;
        break;
      }

      case State::OK_WAIT: {
        if (trimmed != "OK") {
          OK_ERROR("Bad input (state=%d): %s", state, input_summary().c_str());
        }
        state = State::IDLE;
        break;
      }
    }
  }

  etl::string<40> input_summary() const {
    etl::string<40> out;
    for (auto const ch : in_buf) {
      if (out.size() > out.max_size() - 10) {
        etl::format_to(etl::back_inserter(out), "...{}b", in_buf.size());
        break;
      } else if (ch < 32 || ch > 126) {
        etl::format_to(etl::back_inserter(out), "\\x{:02x}", ch);
      } else {
        out.push_back(ch);
      }
    }
    return out;
  }
};

etl::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial, etl::string_view mqtt_server
) {
  OK_FATAL_IF(serial == nullptr);
  return etl::unique_ptr(new CellModemClientDef(serial, mqtt_server));
}
