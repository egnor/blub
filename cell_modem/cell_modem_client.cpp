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

using namespace etl::chrono;
using namespace etl::chrono_literals;

// ISRG Root X1 (https://letsencrypt.org/certificates/)
static char const* const AT_CMNG_SET_ROOT_CERT =
  "AT%CMNG=0,0,0,\""  // 0=set slot=0 type=0=root
  "-----BEGIN CERTIFICATE-----\r\n"
  "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\r\n"
  "-----END CERTIFICATE-----\r\n"
  "\"";

// from "sha256sum" on the certificate above (including CR-LF newlines)
static char const* const ROOT_CERT_SHA256 =
  "4C99356C265EE06C0AE0502E74D38231263513726D001CFE28EA25E70AF2CC7F";

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
    if (state != CommandState::IDLE && now >= state_deadline) {
      OK_ERROR("Command timeout (state=%d), polling", state);
      out_buf.append("\r\n+++\"\r\n");  // unstick modem parser state
      state = CommandState::IDLE;
      next_periodic = {};  // Poll until we get a response
    }

    if (periodic_step < 0 && now >= next_periodic) {
      using namespace etl::chrono;
      OK_DETAIL(
        "⏱️ Periodic poll (%.1f > %.1fs)",
        duration_cast<duration<double>>(now.time_since_epoch()).count(),
        duration_cast<duration<double>>(next_periodic.time_since_epoch()).count()
      );
      next_periodic = now + 10_s;
      periodic_step = 0;
    }

    if (state == CommandState::IDLE && out_complete >= out_buf.size()) {
      state_deadline = now + 1_s;

      // hardware ID (once at startup)
      if (status.hardware.empty()) {
        output_line("AT+CGMM");  // modem model
        state = CommandState::AT_CGMM_WAIT;
      } else if (status.versions[0].empty()) {
        output_line("AT+CGMR");  // modem revision
        state = CommandState::AT_CGMR_WAIT;
      } else if (status.versions[1].empty()) {
        output_line("AT#XSMVER");  // extended serial modem versions
        state = CommandState::OK_WAIT;
      } else if (status.imeisv.empty()) {
        output_line("AT+CGSN=2");  // get IMEI
        state = CommandState::OK_WAIT;

        // cert state (once at startup)
      } else if (cert_state == CertState::UNKNOWN) {
        output_line("AT%CMNG=1,0,0");  // 1=check slot=0 type=0=root
        state = CommandState::OK_WAIT;
        cert_state = CertState::INVALID;  // unless updated by %CMNG: before OK
        cert_radio_off = false;
      } else if (cert_state == CertState::INVALID) {
        output_line("AT+CFUN=4");  // turn off the radio before updating cert
        state = CommandState::OK_WAIT;
        cert_state = CertState::OK_TO_ERASE;
      } else if (cert_state == CertState::OK_TO_ERASE) {
        output_line("AT%CMNG=3,0,0"); // 3=del slot=0 type=0=root
        state = CommandState::OK_WAIT;  // returns OK even if slot was empty
        state_deadline = now + 5_s;  // allow time for NVM write
        cert_state = CertState::OK_TO_WRITE;  // write after deleting
      } else if (cert_state == CertState::OK_TO_WRITE) {
        output_line(AT_CMNG_SET_ROOT_CERT);
        state = CommandState::OK_WAIT;
        state_deadline = now + 5_s;  // allow time for NVM write
        cert_state = CertState::UNKNOWN;  // re-verify after write

        // periodic poll steps
      } else if (periodic_step == 0) {
        output_line("AT+CMEE=1");  // enable extended errors
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 1) {
        output_line("AT%XPDNCFG=1");  // always-on packet network
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 2) {
        output_line("AT+CFUN=1");  // turn on the radio and look for networks
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 3) {
        output_line("AT+CEREG=3");  // network status notifications (after CFUN)
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 4) {
        output_line("AT+CGEREP=1");  // IP status notifications (after CFUN)
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 5) {
        output_line("AT%XMONITOR");  // network and radio status
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 6) {
        output_line("AT+CGPADDR");  // get packet (IP) addresses
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 7) {
        OK_DETAIL("🏁 Periodic poll complete (%d steps)", periodic_step);
        periodic_step = -1;
      }
    }

    while (out_complete < out_buf.size() && serial->availableForWrite() > 0) {
      serial->write(out_buf[out_complete++]);
    }

    return status;
  }

 private:
  enum class CommandState {
    IDLE,
    AT_CGMM_WAIT,
    AT_CGMR_WAIT,
    FAILED,
    OK_WAIT,
  };

  enum class CertState { UNKNOWN, INVALID, OK_TO_ERASE, OK_TO_WRITE, VALID };

  HardwareSerial* const serial;
  etl::string<128> const mqtt_server;
  CellModemStatus status;

  CommandState state = CommandState::IDLE;
  steady_clock::time_point state_deadline = {};
  steady_clock::time_point next_periodic = {};
  int periodic_step = -1;

  CertState cert_state = CertState::UNKNOWN;
  bool cert_radio_off = false;  // radio turned off to update cert

  etl::string<8192> in_buf;
  etl::string<8192> out_buf;  // needs to hold root cert
  int in_expect = 0;
  int out_complete = 0;

  void output_line(etl::string_view line) {
    if (out_complete >= out_buf.size()) {
      OK_DETAIL("▶️ %.*s", line.size(), line.data());
      out_buf = line;
      out_buf.append("\r\n");
      out_complete = 0;
    } else {
      OK_FATAL(  // Should never happen by logic
        "Output overwrite: written=%d < buf=%db\n  new: %.*s",
        out_complete, out_buf.size(), line.size(), line.data()
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
      state = CommandState::IDLE;
      next_periodic = {};  // Initialize immediately
      status.running = true;
      status.registered = false;
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      OK_ERROR("Modem fault (state=%d): %s", state, input_abbr().c_str());
      state = CommandState::FAILED;
      state_deadline = steady_clock::now() + 5_s;
      status.registered = false;
      status.failed = true;
    }

    if (
      eat(&rest, "ERROR") ||
      eat(&rest, "+CME ERROR:") ||
      eat(&rest, "+CMS ERROR:")
    ) {
      OK_ERROR("Modem error (state=%d): %s", state, input_abbr().c_str());
      state = CommandState::IDLE;
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

    if (eat(&rest, "%CMNG:")) {
      int tag, type;
      etl::string_view sha;
      if (
        eat_int(&rest, &tag) && eat(&rest, ",") &&
        eat_int(&rest, &type) && eat(&rest, ",") &&
        eat_quoted(&rest, &sha) && tag == 0 && type == 0
      ) {
        if (sha == ROOT_CERT_SHA256) {
          cert_state = CertState::VALID;
          OK_DETAIL("Root cert correct:\n  %.*s", sha.size(), sha.data());
        } else {
          cert_state = CertState::INVALID;
          OK_ERROR(
            "Root cert mismatch (updating):\n  expect: %s\n  actual: %.*s",
            ROOT_CERT_SHA256, sha.size(), sha.data()
          );
        }
      } else {
        OK_ERROR("Bad %CMNG: %s", input_abbr().c_str());
      }
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
      OK_ERROR("Unexpected reply (state=%d): %s", state, input_abbr().c_str());
      return;
    }

    //
    // "OK" success interpreted by .state
    //

    if (eat(&rest, "OK")) {
      if (!eat(&rest, "")) OK_ERROR("Bad OK: %s", input_abbr().c_str());
      if (state == CommandState::AT_CGMM_WAIT) {
        status.hardware = "-";
      } else if (state == CommandState::AT_CGMR_WAIT) {
        status.versions[0] = "-";
      } else if (state != CommandState::OK_WAIT) {
        OK_ERROR("Unexpected OK (state=%d): %s", state, input_abbr().c_str());
      }
      state = CommandState::IDLE;
      return;
    }

    //
    // Other responses interpreted by .state
    //

    if (state == CommandState::AT_CGMM_WAIT) {
      status.hardware = etl::trim_view_whitespace(rest);
      state = CommandState::OK_WAIT;
      return;
    } else if (state == CommandState::AT_CGMR_WAIT) {
      status.versions[0] = etl::trim_view_whitespace(rest);
      state = CommandState::OK_WAIT;
      return;
    }

    OK_ERROR("Unexpected input (state=%d): %s", state, input_abbr().c_str());
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
