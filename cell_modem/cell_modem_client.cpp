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

static const OkLoggingContext OK_CONTEXT("cell_modem_client");

class CellModemClientDef : public CellModemClient {
 public:
  CellModemClientDef(HardwareSerial* s, CellModemConfig const& config)
    : serial(s), config(config) {}

  CellModemStatus const& poll() override {
    for (int avail = 0; avail || ((avail = serial->available()) > 0); --avail) {
      if (in_buf.full()) {
        OK_ERROR("Dropping long input: %s", abbr(in_buf).c_str());
        in_buf.clear();
      }
      int const ch = serial->read();
      if (ch < 0) {
        OK_ERROR("Serial read error: available=%d ch=%d", avail, ch);
        break;
      } else if (in_expect > 0) {
        in_buf.push_back(ch);
        if (in_buf.size() >= in_expect) {
          OK_DETAIL("📦 %s", abbr(in_buf).c_str());
          handle_input_block();
          in_buf.clear();
          in_expect = 0;
        }
      } else if (ch == '\r' || ch == '\n') {
        if (!in_buf.empty()) {
          OK_DETAIL("⬅️ %s", abbr(in_buf).c_str());
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

    auto const now = steady_clock::now();
    if (state != CommandState::IDLE && now >= state_deadline) {
      OK_ERROR("Command timeout (state=%d), polling", state);
      out_buf.append("\r\n+++\"\r\n");  // unstick modem parser state
      state = CommandState::IDLE;
      next_periodic = {};  // Poll until we get a response
    }

    if (periodic_step < 0 && now >= next_periodic) {
      using dsec = duration<double>;
      OK_DETAIL(
        "⏱️ Periodic poll (%.1f > %.1fs)",
        duration_cast<dsec>(now.time_since_epoch()).count(),
        duration_cast<dsec>(next_periodic.time_since_epoch()).count()
      );
      next_periodic = now + 10_s;
      periodic_step = 0;
    }

    if (state == CommandState::IDLE && out_complete >= out_buf.size()) {
      state_deadline = now + 1_s;
      out_buf.clear();
      out_complete = 0;

      // hardware ID (once at startup)
      if (status.hardware.empty()) {
        out_buf = "AT+CGMM\r\n";  // modem model
        state = CommandState::AT_CGMM_WAIT;
      } else if (status.versions[0].empty()) {
        out_buf = "AT+CGMR\r\n";  // modem revision
        state = CommandState::AT_CGMR_WAIT;
      } else if (status.versions[1].empty()) {
        out_buf = "AT#XSMVER\r\n";  // extended serial modem versions
        state = CommandState::OK_WAIT;
      } else if (status.imeisv.empty()) {
        out_buf = "AT+CGSN=2\r\n";  // get IMEI
        state = CommandState::OK_WAIT;

        // cert state (once at startup)
      } else if (cert_state == CertState::UNKNOWN) {
        out_buf = "AT%CMNG=1,0,0\r\n";  // 1=check slot=0 type=0=root
        state = CommandState::OK_WAIT;
        cert_state = config.root_cert.empty()
          ? CertState::VALID : CertState::INVALID;
      } else if (cert_state == CertState::INVALID) {
        out_buf = "AT+CFUN=4\r\n";  // turn off the radio before updating cert
        state = CommandState::OK_WAIT;
        cert_state = CertState::OK_TO_ERASE;
      } else if (cert_state == CertState::OK_TO_ERASE) {
        out_buf = "AT%CMNG=3,0,0\r\n"; // 3=del slot=0 type=0=root
        state = CommandState::OK_WAIT;  // returns OK even if slot was empty
        state_deadline = now + 5_s;  // allow time for NVM write
        cert_state = CertState::OK_TO_WRITE;  // write after deleting
      } else if (cert_state == CertState::OK_TO_WRITE) {
        etl::format_to(out_buf, "AT%CMNG=0,0,0,\"{}\"\r\n", config.root_cert);
        state = CommandState::OK_WAIT;
        state_deadline = now + 5_s;  // allow time for NVM write
        cert_state = CertState::UNKNOWN;  // re-verify after write

        // periodic poll steps
      } else if (periodic_step == 0) {
        out_buf = "AT+CMEE=1\r\n";  // enable extended errors
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 1) {
        out_buf = "AT%XPDNCFG=1\r\n";  // always-on packet network
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 2) {
        out_buf = "AT+CFUN=1\r\n";  // turn on the radio and look for networks
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 3) {
        out_buf = "AT+CEREG=3\r\n";  // registration notifications (after CFUN)
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 4) {
        out_buf = "AT+CGEREP=1\r\n";  // IP status notifications (after CFUN)
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 5) {
        out_buf = "AT%XMONITOR\r\n";  // network and radio status
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 6) {
        out_buf = "AT+CGPADDR\r\n";  // get packet (IP) addresses
        state = CommandState::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 7) {
        out_buf = "AT#XMQTTCON?\r\n";  // get MQTT connection status
        state = CommandState::OK_WAIT;
        periodic_step = -1;  // End of poll steps

        // MQTT connection management
      } else if (mqtt_state == MqttState::OK_TO_DISCONNECT) {
        out_buf = "AT#XMQTTCON=0\r\n";
        state = CommandState::OK_WAIT;
        state_deadline = now + 10_s;
        mqtt_state = MqttState::OK_TO_CONFIG;
      } else if (
        mqtt_state == MqttState::OK_TO_CONFIG && !status.imeisv.empty()
      ) {
        etl::format_to(out_buf, "AT#XMQTTCFG=\"{}\",60,1\r\n", status.imeisv);
        state = CommandState::OK_WAIT;
        mqtt_state = MqttState::OK_TO_CONNECT;
      } else if (
        mqtt_state == MqttState::OK_TO_CONNECT &&
        cert_state == CertState::VALID &&
        !config.mqtt_server.empty() && status.registered && status.ip_attached
      ) {
        etl::format_to(
          out_buf, "AT#XMQTTCON=1,\"{}\",\"{}\",\"{}\",{}{}\r\n",
          config.mqtt_user, config.mqtt_password,
          config.mqtt_server, config.mqtt_port,
          config.root_cert.empty() ? "" : ",0"
        );
        state = CommandState::OK_WAIT;
        state_deadline = now + 60_s;  // DNS, TCP, TLS, etc. handshakes
        mqtt_state = MqttState::CONNECT_WAIT;
      }

      if (!out_buf.empty()) {
        OK_DETAIL("▶️ %s", abbr(out_buf).c_str());
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

  enum class CertState {
    UNKNOWN,
    INVALID,
    OK_TO_ERASE,
    OK_TO_WRITE,
    VALID
  };

  enum class MqttState {
    UNKNOWN,
    OK_TO_DISCONNECT,
    OK_TO_CONFIG,
    OK_TO_CONNECT,
    CONNECT_WAIT,
    CONNECTED,
    SUBSCRIBE_WAIT,
  };

  HardwareSerial* const serial;
  CellModemConfig const config;
  CellModemStatus status;

  CommandState state = CommandState::IDLE;
  steady_clock::time_point state_deadline = {};
  steady_clock::time_point next_periodic = {};
  int periodic_step = -1;

  CertState cert_state = CertState::UNKNOWN;

  MqttState mqtt_state = MqttState::UNKNOWN;
  int mqtt_subscribed = 0;

  etl::string<8192> in_buf;
  etl::string<8192> out_buf;  // needs to hold root cert
  int in_expect = 0;
  int out_complete = 0;

  void handle_input_block() {
    OK_ERROR("Unexpected (state=%d): %s", state, abbr(in_buf).c_str());
  }

  void handle_input_line() {
    etl::string_view rest(in_buf);

    //
    // Generic fault/reset messages
    //

    if (eat(&rest, "Ready")) {
      if (status.running) {
        OK_NOTE("Modem init: %s", abbr(in_buf).c_str());
      } else {
        OK_ERROR("Modem reset (state=%d): %s", state, abbr(in_buf).c_str());
      }
      state = CommandState::IDLE;
      next_periodic = {};  // Initialize immediately
      status.running = true;
      status.registered = false;
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      OK_ERROR("Modem fault (state=%d): %s", state, abbr(in_buf).c_str());
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
      OK_ERROR("Modem error (state=%d): %s", state, abbr(in_buf).c_str());
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
      if (!eat(&rest, "")) OK_ERROR("Bad +CEREG: %s", abbr(in_buf).c_str());
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
      if (!eat(&rest, "")) OK_ERROR("Bad +CGEV: %s", abbr(in_buf).c_str());
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
            status.ip_attached = true;
            status.ip_addr = (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
          } else {
            OK_ERROR("Bad +CGPADDR IPv4: %s", abbr(in_buf).c_str());
          }
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad +CGPADDR: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "+CGSN:")) {
      etl::string_view v;
      if (eat_quoted(&rest, &v)) status.imeisv = v.empty() ? "-" : v;
      if (!eat(&rest, "")) OK_ERROR("Bad +CGSN: %s", abbr(in_buf).c_str());
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
        if (sha == config.root_cert_sha256) {
          cert_state = CertState::VALID;
          OK_DETAIL("Root cert correct:\n  %.*s", sha.size(), sha.data());
        } else {
          cert_state = CertState::INVALID;
          OK_ERROR(
            "Root cert mismatch (updating):\n  expect: %.*s\n  actual: %.*s",
            config.root_cert_sha256.size(), config.root_cert_sha256.data(),
            sha.size(), sha.data()
          );
        }
      } else {
        OK_ERROR("Bad %CMNG: %s", abbr(in_buf).c_str());
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
      if (!eat(&rest, "")) OK_ERROR("Bad %XMONITOR: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "#XMQTTCON:")) {
      if (eat(&rest, "0")) {
        if (mqtt_state <= MqttState::OK_TO_DISCONNECT) {
          mqtt_state = MqttState::OK_TO_CONFIG;
        } else if (mqtt_state >= MqttState::CONNECT_WAIT) {
          OK_ERROR("MQTT disconnected, reconnecting");
          mqtt_state = MqttState::OK_TO_CONFIG;
        }
      } else if (eat(&rest, "1")) {
        etl::string_view cid, host;
        int port, sec_tag = -1;
        if (
          eat(&rest, ",") && eat_quoted(&rest, &cid) &&
          eat(&rest, ",") && eat_quoted(&rest, &host) &&
          eat(&rest, ",") && eat_int(&rest, &port) &&
          ((eat(&rest, ",") && eat_int(&rest, &sec_tag)) || true)
        ) {
          int const config_sec = config.root_cert.empty() ? -1 : 0;
          if (
            host != config.mqtt_server || port != config.mqtt_port ||
            cid != status.imeisv || sec_tag != config_sec
          ) {
            OK_ERROR(
              "Bad MQTT host:\n  %.*s:%d[%d] (%.*s) !=\n  %.*s:%d[%d] (%.*s)",
              host.size(), host.data(), port, sec_tag, cid.size(), cid.data(),
              config.mqtt_server.size(), config.mqtt_server.data(),
              config.mqtt_port, config_sec,
              status.imeisv.size(), status.imeisv.data()
            );
            mqtt_state = MqttState::OK_TO_DISCONNECT;
          }
        } else {
          OK_ERROR("Bad #XMQTTCON data: %s", abbr(in_buf).c_str());
        }
      } else {
        OK_ERROR("Bad #XMQTTCON status: %s", abbr(in_buf).c_str());
      }
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
      if (!eat(&rest, "")) OK_ERROR("Bad AT#XSMVER: %s", abbr(in_buf).c_str());
      return;
    }

    if (rest.starts_with("+") || rest.starts_with("#")) {
      OK_ERROR("Unexpected (state=%d): %s", state, abbr(in_buf).c_str());
      return;
    }

    //
    // "OK" success interpreted by .state
    //

    if (eat(&rest, "OK")) {
      if (!eat(&rest, "")) OK_ERROR("Bad OK: %s", abbr(in_buf).c_str());
      if (state == CommandState::AT_CGMM_WAIT) {
        status.hardware = "-";
      } else if (state == CommandState::AT_CGMR_WAIT) {
        status.versions[0] = "-";
      } else if (state != CommandState::OK_WAIT) {
        OK_ERROR("Unexpected OK (state=%d): %s", state, abbr(in_buf).c_str());
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

    OK_ERROR("Unexpected data (state=%d): %s", state, abbr(in_buf).c_str());
  }

  static etl::string<40> abbr(etl::string_view str) {
    etl::string<40> out;
    for (auto const ch : str) {
      if (out.size() > out.max_size() - 10) {
        etl::format_to(etl::back_insert_iterator(out), "...{}b", str.size());
        break;
      } else if (ch == 10) {
        out.append("\\n");
      } else if (ch == 13) {
        out.append("\\r");
      } else if (ch < 32 || ch > 126) {
        etl::format_to(etl::back_insert_iterator(out), "\\x{:02x}", ch);
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
  arduino::HardwareSerial* serial, CellModemConfig const& config
) {
  OK_FATAL_IF(serial == nullptr);
  return etl::unique_ptr(new CellModemClientDef(serial, config));
}
