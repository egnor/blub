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
        OK_ERROR("Dropping long input: %s", input_abbr().c_str());
        in_buf.clear();
      }
      int const ch = serial->read();
      if (ch < 0) {
        OK_ERROR("Serial read error: available=%d ch=%d", avail, ch);
        break;
      } else if (in_expect > 0) {
        in_buf.push_back(ch);
        if (in_buf.size() >= in_expect) {
          OK_DETAIL("📦 %s", input_abbr().c_str());
          handle_input_block();
          in_buf.clear();
          in_expect = 0;
        }
      } else if (ch == '\r' || ch == '\n') {
        if (!in_buf.empty()) {
          OK_DETAIL("⬅️ %s", input_abbr().c_str());
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
      // lambda to convert time_point to milliseconds since epoch for logging
      auto const msec = [](auto const& tp) -> int64_t {
        auto const d = tp.time_since_epoch();
        return etl::chrono::duration_cast<etl::chrono::milliseconds>(d).count();
      };

      using ms = etl::chrono::milliseconds;
      OK_DETAIL(
        "⏱️ Periodic poll (%lld > %lldmsec)", msec(now), msec(next_periodic)
      );
      next_periodic = now + 10_s;
      periodic_step = 0;
    }

    if (state == State::IDLE && out_complete >= out_buf.size()) {
      state_deadline = now + 1_s;
      if (status.hardware.empty()) {
        output_line("AT+CGMM");  // modem model
        state = State::AT_CGMM_WAIT;
      } else if (status.versions[0].empty()) {
        output_line("AT+CGMR");  // modem revision
        state = State::AT_CGMR_WAIT;
      } else if (status.versions[1].empty()) {
        output_line("AT#XSMVER");  // extended serial modem versions
        state = State::OK_WAIT;
      } else if (status.imeisv.empty()) {
        output_line("AT+CGSN=2");  // get IMEI
        state = State::OK_WAIT;
      } else if (periodic_step == 0) {
        output_line("AT+CMEE=1");  // enable extended errors
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 1) {
        output_line("AT%XPDNCFG=1");  // always-on packet network
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 2) {
        output_line("AT+CFUN=1");  // turn on the radio and look for networks
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 3) {
        output_line("AT+CEREG=3");  // network status notifications (after CFUN)
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 4) {
        output_line("AT+CGEREP=1");  // IP status notifications (after CFUN)
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 5) {
        output_line("AT%XMONITOR");  // network and radio status
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 6) {
        output_line("AT+CGPADDR");  // get packet (IP) addresses
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 7) {
        OK_DETAIL("🏁 Periodic poll complete (%d steps)", periodic_step);
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

  void output_line(etl::string_view line) {
    if (out_complete >= out_buf.size()) {
      OK_DETAIL("▶️ %.*s", line.size(), line.data());
      out_buf = line;
      out_buf.append("\r\n");
      out_complete = 0;
    } else {
      OK_ERROR(
        "Output overwrite:\n  old (%db sent): %.*s\n  new: %.*s",
        out_complete, out_buf.size(), out_buf.data(),
        line.size(), line.data()
      );
    }
  }

  void handle_input_block() {
    OK_ERROR("Unexpected (state=%d): %s", state, input_abbr().c_str());
  }

  void handle_input_line() {
    etl::string_view rest(in_buf);

    //
    // Generic fault/reset messages
    //

    if (eat(&rest, "Ready")) {
      if (status.running) {
        OK_NOTE("Modem init: %s", input_abbr().c_str());
      } else {
        OK_ERROR("Modem reset (state=%d): %s", state, input_abbr().c_str());
      }
      state = State::IDLE;
      next_periodic = {};  // Initialize immediately
      status.running = true;
      status.registered = false;
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      OK_ERROR("Modem fault (state=%d): %s", state, input_abbr().c_str());
      state = State::FAILED;
      state_deadline = etl::chrono::steady_clock::now() + 5_s;
      status.registered = false;
      status.failed = true;
    }

    if (
      eat(&rest, "ERROR") ||
      eat(&rest, "+CME ERROR:") ||
      eat(&rest, "+CMS ERROR:")
    ) {
      OK_ERROR("Modem error (state=%d): %s", state, input_abbr().c_str());
      state = State::IDLE;
      return;
    }

    //
    // Generic success
    //

    if (eat(&rest, "OK")) {
      if (!eat(&rest, "")) OK_ERROR("Bad OK: %s", input_abbr().c_str());
      if (state != State::OK_WAIT) {
        OK_ERROR("Unexpected OK (state=%d): %s", state, input_abbr().c_str());
      }
      state = State::IDLE;
      return;
    }

    //
    // Identifiable message responses
    //

    if (eat(&rest, "+CEREG:")) {
      int reg;
      if (eat_int(&rest, &reg)) {
        status.running = (reg == 1 || reg == 2 || reg == 5);
        status.registered = (reg == 1 || reg == 5);
        if (reg == 1) status.roaming = false;
        if (reg == 5) status.roaming = true;
        if (reg == 3 || reg == 90) status.failed = true;
        if (status.running) status.failed = false;
        if (status.registered) status.reject_cause = 0;

        etl::string_view cell_tac, cell_id;
        int act;
        if (
          eat(&rest, ",") && eat_quoted(&rest, &cell_tac) &&
          eat(&rest, ",") && eat_quoted(&rest, &cell_id) &&
          eat(&rest, ",") && eat_int(&rest, &act)
        ) {
          auto const tac = etl::to_arithmetic<uint16_t>(cell_tac, etl::hex);
          auto const id = etl::to_arithmetic<uint32_t>(cell_id, etl::hex);
          auto const& st = status;
          if (tac != st.cell_tac || id != st.cell_id || act != st.radio_tech) {
            status.op_mcc = status.op_mnc = 0;  // Unknown from +CEREG
            status.cell_tac = tac;
            status.cell_id = id;
            status.radio_tech = act;
            // Registration changed; reset radio status fields until next poll
            status.cell_phys_id = 0;
            status.radio_earfcn = 0;
            status.radio_band = 0;
            status.radio_rsrp = status.radio_snr = -0x8000;
            next_periodic = {};  // Trigger a poll to get status faster
          }

          int cause_type, reject_cause;
          if (
            eat(&rest, ",") && eat_int(&rest, &cause_type) &&
            eat(&rest, ",") && eat_int(&rest, &reject_cause) &&
            cause_type == 0 && !status.registered
          ) {
            status.reject_cause = reject_cause;
          }
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad +CEREG: %s", input_abbr().c_str());
      return;
    }

    if (eat(&rest, "+CGEV:")) {
      if (eat(&rest, "NW") || eat(&rest, "ME")) {
        if (eat(&rest, "PDN ACT")) {
          status.ip_attached = true;
          next_periodic = {};  // Trigger a poll to get IP addresses, etc.
          return;  // Don't bother parsing further
        } else if (
          eat(&rest, "PDN DEACT") || eat(&rest, "DETACH") ||
          eat(&rest, "OVERHEATED")
        ) {
          if (status.ip_attached) next_periodic = {};  // Trigger a poll
          status.ip_attached = false;
          status.ip_addr = 0;
          return;  // Don't bother parsing further
        } else if (
          eat(&rest, "ACT") || eat(&rest, "DEACT") ||
          eat(&rest, "BATTERY LOW") || eat(&rest, "MODIFY")
        ) {
          return;  // Ignore these, don't bother parsing further
        }
      } else if (
        eat(&rest, "IPV6") || eat(&rest, "RESTR") ||
        eat(&rest, "APNARATECTRL") || eat(&rest, "EXCE")
      ) {
        return;  // Ignore these, don't bother parsing further
      }
      if (!eat(&rest, "")) OK_ERROR("Bad +CGEV: %s", input_abbr().c_str());
      return;
    }

    if (eat(&rest, "+CGPADDR:")) {
      int cid;
      etl::string_view a1, a2;
      if (eat_int(&rest, &cid)) {
        if (eat(&rest, ",")) eat_quoted(&rest, &a1);
        if (eat(&rest, ",")) eat_quoted(&rest, &a2);
        if (a1.empty()) {
          status.ip_addr = 0;
        } else {
          int b1, b2, b3, b4;
          if (
            eat_int(&a1, &b1) && eat(&a1, ".") &&
            eat_int(&a1, &b2) && eat(&a1, ".") &&
            eat_int(&a1, &b3) && eat(&a1, ".") &&
            eat_int(&a1, &b4) && eat(&a1, "")
          ) {
            status.ip_addr = (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
          } else {
            OK_ERROR("Bad +CGPADDR IPv4: %s", input_abbr().c_str());
          }
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad +CGPADDR: %s", input_abbr().c_str());
      return;
    }

    if (eat(&rest, "+CGSN:")) {
      etl::string_view v;
      if (eat_quoted(&rest, &v)) status.imeisv = v.empty() ? "-" : v;
      if (!eat(&rest, "")) OK_ERROR("Bad +CGSN: %s", input_abbr().c_str());
      return;
    }

    if (eat(&rest, "%XMONITOR:")) {
      int reg;
      if (eat_int(&rest, &reg)) {
        status.running = (reg == 1 || reg == 2 || reg == 5);
        status.registered = (reg == 1 || reg == 5);
        if (reg == 1) status.roaming = false;
        if (reg == 5) status.roaming = true;
        if (reg == 3 || reg == 90) status.failed = true;
        if (status.running) status.failed = false;
        if (status.registered) status.reject_cause = 0;

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
          status.op_mnc = etl::to_arithmetic<uint16_t>(op_mcc_mnc.substr(3, 3));
          status.cell_tac = etl::to_arithmetic<uint16_t>(cell_tac, etl::hex);
          status.cell_phys_id = cell_phys_id;
          status.cell_id = etl::to_arithmetic<uint32_t>(cell_id, etl::hex);
          status.radio_earfcn = radio_earfcn;
          status.radio_tech = radio_tech;
          status.radio_band = radio_band;
          status.radio_rsrp = radio_rsrp == 255 ? -0x8000 : radio_rsrp - 141;
          status.radio_snr = radio_snr == 127 ? -0x8000 : radio_snr - 25;
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad %XMONITOR: %s", input_abbr().c_str());
      return;
    }

    if (eat(&rest, "#XSMVER:")) {
      etl::string_view v1, v2, v3;
      if (
        eat_quoted(&rest, &v1) &&
        eat(&rest, ",") && eat_quoted(&rest, &v2) &&
        eat(&rest, ",") && eat_quoted(&rest, &v3)
      ) {
        status.versions[1] = v1.empty() ? "-" : v1;
        status.versions[2] = v2.empty() ? "-" : v2;
        status.versions[3] = v3.empty() ? "-" : v3;
      }
      if (!eat(&rest, "")) OK_ERROR("Bad AT#XSMVER: %s", input_abbr().c_str());
      return;
    }

    if (rest.starts_with("+") || rest.starts_with("#")) {
      OK_ERROR("Unexpected input: %s", input_abbr().c_str());
      return;
    }

    //
    // Responses identifiable only by .state
    //

    if (state == State::AT_CGMM_WAIT) {
      status.hardware = etl::trim_view_whitespace(rest);
      if (status.hardware.empty()) status.hardware = "-";
      state = State::OK_WAIT;
    } else if (state == State::AT_CGMR_WAIT) {
      status.versions[0] = etl::trim_view_whitespace(rest);
      if (status.versions[0].empty()) status.versions[0] = "-";
      state = State::OK_WAIT;
    } else if (state == State::IDLE) {
      OK_ERROR("Unexpected input: %s", input_abbr().c_str());
    } else if (state == State::OK_WAIT) {
      // Actual OK handled above
      OK_ERROR("Bad reply: %s", input_abbr().c_str());  // Keep waiting for OK
    } else {
      OK_FATAL("Bad state: %d", state);
    }
  }

  etl::string<40> input_abbr() const {
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
    if (literal.empty()) return view.empty();  // special case for EOL
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
