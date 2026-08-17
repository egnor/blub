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
    for (int avail = 0; avail || ((avail = serial->available()) > 0); --avail) {
      if (in_buf.full()) {
        OK_ERROR("Dropping long input: %s", input_summary().c_str());
        in_buf.clear();
      }
      int const ch = serial->read();
      if (ch < 0) {
        OK_ERROR("Serial read error: available=%d ch=%d", avail, ch);
        break;
      } else if (in_expect > 0) {
        in_buf.push_back(ch);
        if (in_buf.size() >= in_expect) {
          OK_DETAIL("Input block: %s", input_summary().c_str());
          handle_input();
          in_buf.clear();
          in_expect = 0;
        }
      } else if (ch == '\r' || ch == '\n') {
        if (!in_buf.empty()) {
          OK_DETAIL("Input: %s", input_summary().c_str());
          handle_input();
          in_buf.clear();
        }
      } else if (ch < 32 || ch >= 256) {
        OK_ERROR("Bad input char (state=%d): 0x02x", state, ch);
        in_buf.clear();
      } else {
        in_buf.push_back(ch);
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
      state_deadline = now + 1_s;
      if (status.hardware.empty()) {
        out_buf = "AT+CGMM\r\n";
        state = State::AT_CGMM_WAIT;
      } else if (status.imeisv.empty()) {
        out_buf = "AT+CGSN=2\r\n";
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

    etl::string_view rest(in_buf);
    switch (state) {
      case State::IDLE: {
        break;
      }
      case State::AT_CGMM_WAIT: {
        status.hardware = etl::trim_view_whitespace(rest);
        if (status.hardware.empty()) status.hardware = "-";
        state = State::OK_WAIT;
        break;
      }
      case State::AT_CGMR_WAIT: {
        status.versions[0] = etl::trim_view_whitespace(rest);
        if (status.versions[0].empty()) status.versions[0] = "-";
        state = State::OK_WAIT;
        break;
      }
      case State::AT_CGSN_WAIT: {
        etl::string_view value;
        if (eat_token(&rest, "+CGSN:") && parse_quoted(&rest, &value)) {
          status.imeisv = value.empty() ? "-" : value;
        } else {
          OK_ERROR("Bad CGSN reply: %s", input_summary().c_str());
        }
        state = State::OK_WAIT;
        break;
      }
      case State::AT_XSMVER_WAIT: {
        etl::string_view sm_version, ncs_version, cust_version;
        if (
          eat_token(&rest, "#XSMVER:") && parse_quoted(&rest, &sm_version) &&
          eat_token(&rest, ",") && parse_quoted(&rest, &ncs_version) &&
          eat_token(&rest, ",") && parse_quoted(&rest, &cust_version)
        ) {
          status.versions[1] = sm_version.empty() ? "-" : sm_version;
          status.versions[2] = ncs_version.empty() ? "-" : ncs_version;
          status.versions[3] = cust_version.empty() ? "-" : cust_version;
        } else {
          OK_ERROR("Bad #XSMVER reply: %s", input_summary().c_str());
        }
        state = State::OK_WAIT;
        break;
      }
      case State::OK_WAIT: {
        if (!eat_token(&rest, "OK")) {
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

  static bool eat_token(etl::string_view* str, etl::string_view literal) {
    auto view = etl::trim_view_whitespace_left(*str);
    if (!view.starts_with(literal)) return false;
    *str = view.substr(literal.size());
    return true;
  }

  static bool parse_quoted(etl::string_view* str, etl::string_view* out) {
    auto view = *str;
    if (!eat_token(&view, "\"")) return false;
    auto const end = view.find('"');
    if (end == etl::string_view::npos) return false;
    *out = view.substr(0, end);
    *str = view.substr(end + 1);
    return true;
  }
};

etl::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial, etl::string_view mqtt_server
) {
  OK_FATAL_IF(serial == nullptr);
  return etl::unique_ptr(new CellModemClientDef(serial, mqtt_server));
}
