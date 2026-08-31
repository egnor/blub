#include "cell_modem_client.h"

#include <Arduino.h>
#include <etl/chrono.h>
#include <etl/circular_buffer.h>
#include <etl/format.h>
#include <etl/string.h>
#include <etl/string_utilities.h>
#include <etl/to_arithmetic.h>
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
          handle_input_block();
          in_buf.clear();
          in_expect = 0;
        }
      } else if (ch == '\r' || ch == '\n') {
        if (!in_buf.empty()) {
          OK_DETAIL("Input: %s", input_summary().c_str());
          handle_input_line();
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
      next_periodic = {};  // Poll until we get a response
    }

    if (periodic_step < 0 && now >= next_periodic) {
      next_periodic = now + 10_s;
      periodic_step = 0;
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
      } else if (periodic_step == 0) {
        out_buf = "AT+CMEE=1\r\n";
        state = State::OK_WAIT;
      } else if (periodic_step == 1) {
        out_buf = "AT+CFUN=1\r\n";
        state = State::OK_WAIT;
      } else if (periodic_step == 2) {
        out_buf = "AT%XMONITOR\r\n";
        state = State::AT_XMONITOR_WAIT;
      } else if (periodic_step == 3) {
        periodic_step = -1;
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
    AT_XMONITOR_WAIT,
    AT_XSMVER_WAIT,
    FAILED,
    OK_WAIT,
  };

  State state = State::IDLE;
  time_point state_deadline = {};
  time_point next_periodic = {};
  int periodic_step = -1;
  CellModemStatus status;

  etl::string<8192> in_buf;
  int in_expect = 0;

  etl::string<256> out_buf;
  int out_complete = 0;

  void handle_input_block() {
    OK_ERROR("Unexpected (state=%d): %s", state, input_summary().c_str());
  }

  void handle_input_line() {
    etl::string_view rest(in_buf);

    if (eat(&rest, "Ready")) {
      if (status.running) {
        OK_NOTE("Modem init: %s", input_summary().c_str());
      } else {
        OK_ERROR("Modem reset (state=%d): %s", state, input_summary().c_str());
      }
      state = State::IDLE;
      next_periodic = {};  // Initialize immediately
      status.running = true;
      status.online = false;
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      OK_ERROR("Modem fault (state=%d): %s", state, input_summary().c_str());
      state = State::FAILED;
      state_deadline = etl::chrono::steady_clock::now() + 5_s;
      status.online = false;
      status.failed = true;
    }

    if (
      eat(&rest, "ERROR") ||
      eat(&rest, "+CME ERROR:") ||
      eat(&rest, "+CMS ERROR:")
    ) {
      OK_ERROR("Modem error (state=%d): %s", state, input_summary().c_str());
      state = State::IDLE;
      return;
    }

    if (state == State::AT_CGMM_WAIT) {
      if (eat(&rest, "OK") && etl::trim_view_whitespace(rest).empty()) {
        status.hardware = "-";
        state = State::IDLE;
        return;
      } else if (!eat(&rest, "+") && !eat(&rest, "#")) {
        status.hardware = etl::trim_view_whitespace(rest);
        state = State::OK_WAIT;
        return;
      }
    } else if (state == State::AT_CGMR_WAIT) {
      if (eat(&rest, "OK") && etl::trim_view_whitespace(rest).empty()) {
        status.versions[0] = "-";
        state = State::IDLE;
        return;
      } else if (!eat(&rest, "+") && !eat(&rest, "#")) {
        status.versions[0] = etl::trim_view_whitespace(rest);
        state = State::OK_WAIT;
        return;
      }
    } else if (state == State::AT_CGSN_WAIT) {
      etl::string_view v;
      if (eat(&rest, "+CGSN:") && eat_quoted(&rest, &v)) {
        status.imeisv = v.empty() ? "-" : v;
        state = State::OK_WAIT;
        return;
      }
    } else if (state == State::AT_XMONITOR_WAIT) {
      int reg;
      if (eat(&rest, "%XMONITOR:") && eat_int(&rest, &reg)) {
        state = State::OK_WAIT;
        status.running = (reg == 1 || reg == 2 || reg == 5);
        status.online = (reg == 1 || reg == 5);
        if (reg == 1) status.roaming = false;
        if (reg == 5) status.roaming = true;
        if (reg == 3 || reg == 90) status.failed = true;
        if (status.running) status.failed = false;

        etl::string_view op_full, op_short, op_mcc_mnc;
        etl::string_view cell_tac, cell_id;
        int cell_phys_id;
        int radio_tech, radio_band, radio_earfcn, radio_rsrp, radio_snr;
        etl::string_view power_edrx, power_atime, power_tau_ext, power_tau;
        if (
          eat(&rest, ",") && eat_quoted(&rest, &op_full) &&
          eat(&rest, ",") && eat_quoted(&rest, &op_short) &&
          eat(&rest, ",") && eat_quoted(&rest, &op_mcc_mnc) &&
          eat(&rest, ",") && eat_quoted(&rest, &cell_tac) &&
          eat(&rest, ",") && eat_int(&rest, &radio_tech) &&
          eat(&rest, ",") && eat_int(&rest, &radio_band) &&
          eat(&rest, ",") && eat_quoted(&rest, &cell_id) &&
          eat(&rest, ",") && eat_int(&rest, &cell_phys_id) &&
          eat(&rest, ",") && eat_int(&rest, &radio_earfcn) &&
          eat(&rest, ",") && eat_int(&rest, &radio_rsrp) &&
          eat(&rest, ",") && eat_int(&rest, &radio_snr) &&
          eat(&rest, ",") && eat_quoted(&rest, &power_edrx) &&
          eat(&rest, ",") && eat_quoted(&rest, &power_atime) &&
          eat(&rest, ",") && eat_quoted(&rest, &power_tau_ext) &&
          eat(&rest, ",") && eat_quoted(&rest, &power_tau)
        ) {
          status.op_mcc = etl::to_arithmetic<uint16_t>(op_mcc_mnc.substr(0, 3));
          status.op_mnc = etl::to_arithmetic<uint16_t>(op_mcc_mnc.substr(0, 3));
          status.cell_tac = etl::to_arithmetic<uint16_t>(cell_tac, etl::hex);
          status.cell_phys_id = cell_phys_id;
          status.cell_id = etl::to_arithmetic<uint32_t>(cell_id, etl::hex);
          status.radio_earfcn = radio_earfcn;
          status.radio_tech = radio_tech;
          status.radio_band = radio_band;
          status.radio_rsrp = radio_rsrp;
          status.radio_snr = radio_snr;
        }
        if (!etl::trim_view_whitespace(rest).empty()) {
          OK_ERROR("Bad %%XMONITOR args: %s", input_summary().c_str());
        }
        state = State::OK_WAIT;
        return;
      }
    } else if (state == State::AT_XSMVER_WAIT) {
      etl::string_view v1, v2, v3;
      if (
        eat(&rest, "#XSMVER:") && eat_quoted(&rest, &v1) &&
        eat(&rest, ",") && eat_quoted(&rest, &v2) &&
        eat(&rest, ",") && eat_quoted(&rest, &v3)
      ) {
        status.versions[1] = v1.empty() ? "-" : v1;
        status.versions[2] = v2.empty() ? "-" : v2;
        status.versions[3] = v3.empty() ? "-" : v3;
        state = State::OK_WAIT;
        return;
      }
    } else if (state == State::OK_WAIT) {
      if (eat(&rest, "OK") && etl::trim_view_whitespace(rest).empty()) {
        if (periodic_step >= 0) ++periodic_step;
        state = State::IDLE;
        return;
      }
    } else if (state != State::IDLE) {
      OK_FATAL("Bad state: %d", state);
    }

    OK_ERROR("Unexpected (state=%d): %s", state, input_summary().c_str());
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

  static bool eat(etl::string_view* str, etl::string_view literal) {
    auto view = etl::trim_view_whitespace_left(*str);
    if (!view.starts_with(literal)) return false;
    *str = view.substr(literal.size());
    return true;
  }

  static bool eat_int(etl::string_view* str, int* out) {
    auto view = etl::trim_view_whitespace_left(*str);
    auto const len = view.find_first_not_of("0123456789");
    if (len <= 0) return false;
    *out = etl::to_arithmetic<int>(view.substr(0, len));
    *str = view.substr(len);
    return true;
  }

  static bool eat_quoted(etl::string_view* str, etl::string_view* out) {
    auto view = *str;
    if (!eat(&view, "\"")) return false;
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
