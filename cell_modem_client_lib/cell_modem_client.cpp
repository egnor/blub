#include "cell_modem_client.h"

#include <Arduino.h>
#include <etl/chrono.h>
#include <etl/circular_buffer.h>
#include <etl/string.h>
#include <etl/string_utilities.h>
#include <etl/to_string.h>
#include <etl/to_arithmetic.h>
#include <memory>
#include <ok_logging.h>

#include <blub_clock_util.h>

using namespace etl::chrono;
using namespace etl::chrono_literals;

static const OkLoggingContext OK_CONTEXT("cell_modem_client");

class CellModemClientDef : public CellModemClient {
 public:
  CellModemClientDef(
    HardwareSerial* serial, int en_pin,
    MqttServerConfig const& server,
    etl::span<etl::string_view const> subs
  ) : serial(serial), enable_pin(en_pin), mqtt_server(server), mqtt_subs(subs) {
    // Catch things that would cause crashes or bad command syntax
    OK_FATAL_IF(serial == nullptr);
    if (server.cert.find_first_of("\"") != etl::string_view::npos)  // CRLF OK
      OK_FATAL("Bad MQTT cert: %s", abbr(server.cert).c_str());
    if (server.host.find_first_of("\"\r\n") != etl::string_view::npos)
      OK_FATAL("Bad MQTT host: %s", abbr(server.host).c_str());
    if (server.user.find_first_of("\"\r\n") != etl::string_view::npos)
      OK_FATAL("Bad MQTT user: %s", abbr(server.user).c_str());
    if (server.password.find_first_of("\"\r\n") != etl::string_view::npos)
      OK_FATAL("Bad MQTT password: %s", abbr(server.password).c_str());
    for (int i = 0; i < subs.size(); ++i) {
      if (subs[i].find_first_of("\"\r\n") != etl::string_view::npos)
        OK_FATAL("Bad MQTT sub #%d: %s", i, abbr(subs[i]).c_str());
    }

    if (server.cert.empty()) cert_state = CertState::VALID;  // no cert
  }

  CellModemStatus const& poll() override {
    auto const poll_time = steady_clock::now();
    auto const elapsed = poll_time - last_poll;
    if (elapsed > 50_ms && last_poll > steady_clock::time_point{}) {
      OK_ERROR("Slow poll (%.3fs)", raw_count<secd>(elapsed));
    }

    if (!mqtt_incoming.topic.empty()) {
      OK_ERROR("Unhandled MQTT message: %s", abbr(mqtt_incoming.topic).c_str());
      mqtt_incoming = {};  // Depends on counted buffer which will be lost
    }

    // hard modem reset at startup or if MQTT fails to thrive
    if (enable_pin >= 0 && poll_time >= next_hard_reset) {
      OK_NOTE("📴 Resetting modem (p%d LOW)", enable_pin);
      pinMode(enable_pin, OUTPUT);
      digitalWrite(enable_pin, LOW);
      next_hard_reset = poll_time + 300_s;
      last_serial_traffic = poll_time;
      in_counted_remain = -1;
      in_buf.clear();
      out_bufs.clear();
      state = State::RESET_WAIT;
      status.running = status.registered = status.ip_attached = false;
      do_probe = true;  // Once reset completes, probe for liveness
    }

    //
    // Read serial input
    //

    if (in_counted_remain == 0) {
      in_buf.clear();  // Counted buffer remains for one poll cycle
      in_counted_remain = -1;
    }

    if (in_counted_remain < 0) mqtt_in_topic_size = mqtt_in_payload_size = -1;

    for (int av = 0; av || ((av = serial->available()) > 0); --av) {
      last_serial_traffic = poll_time;
      int const ch = serial->read();
      if (ch < 0) {
        OK_ERROR("Serial read error: avail=%d ch=%d", av, ch);
        break;
      }

      if (in_counted_remain > 0) {
        if (!in_buf.full()) in_buf.push_back(ch);
        if (--in_counted_remain == 0) {
          handle_counted_input();
          break;  // buffer will be valid until the next poll() (see above)
        }
      } else if (ch == '\r' || ch == '\n') {
        if (!in_buf.empty()) {
          handle_input_line();
          in_buf.clear();
        }
      } else if (ch < 32 || ch > 255) {
        OK_ERROR("Bad input char (state=%d): 0x%02x", state, ch);
        in_buf.clear();
      } else {
        if (in_buf.full()) {
          OK_ERROR("Dropping long input: %s", abbr(in_buf).c_str());
          in_buf.clear();
        }
        in_buf.push_back(ch);
      }
    }

    //
    // State timeouts
    //

    auto const elapsed_quiet = poll_time - last_serial_traffic;
    if (in_counted_remain > 0) {
      if (elapsed_quiet > 60_s) {
        OK_ERROR("Data timeout: %.1fs > 60s", raw_count<secd>(elapsed_quiet));
        in_counted_remain = -1;
        in_buf.clear();
        out_bufs.clear();
        do_probe = true;
        state = State::IDLE;  // any pending command was surely lost
      }
    } else if (state == State::RESET_WAIT) {
      if (elapsed_quiet >= 100_ms) {
        OK_NOTE("🔛 Ending modem reset (p%d PULLUP)", enable_pin);
        pinMode(enable_pin, INPUT_PULLUP);
        digitalWrite(enable_pin, HIGH);
        last_serial_traffic = poll_time;
        state = State::IDLE;
      }
    } else if (state == State::PROBE_DRAIN) {
      if (elapsed_quiet >= 100_ms) {
        OK_DETAIL("✅️ Startup probe complete");
        next_periodic = {};
        state = State::IDLE;
      }
    } else if (state != State::IDLE) {
      auto const allow = (state == State::AT_XMQTTCON_WAIT) ? 60_s : 5_s;
      if (elapsed_quiet >= allow) {
        OK_ERROR(
          "Command timeout (state=%d): %.1f > %.1fs", state,
          raw_count<secd>(elapsed_quiet), raw_count<secd>(allow)
        );
        state = State::IDLE;
        do_probe = true;
      }
    }

    //
    // Command sending (transitions from idle)
    //

    if (state == State::IDLE && in_counted_remain < 0 && out_bufs.empty()) {
      // initial command probe (once at startup)
      if (do_probe) {
        out_bufs.push("\r+++\"\rAT\r");  // unstick modem parser, ask for OK
        state = State::PROBE_WAIT;  // wait for OK, then drain buffers
        do_probe = false;

      // hardware ID (once at startup)
      } else if (status.hardware.empty()) {
        out_bufs.push("AT+CGMM\r");  // modem model
        state = State::AT_CGMM_WAIT;
      } else if (status.versions[0].empty()) {
        out_bufs.push("AT+CGMR\r");  // modem revision
        state = State::AT_CGMR_WAIT;
      } else if (status.versions[1].empty()) {
        out_bufs.push("AT#XSMVER\r");  // extended serial modem versions
        state = State::OK_WAIT;
      } else if (status.imeisv.empty()) {
        out_bufs.push("AT+CGSN=2\r");  // get IMEI
        state = State::OK_WAIT;

        // cert state (once at startup)
      } else if (cert_state == CertState::UNKNOWN) {
        out_bufs.push("AT%CMNG=1,0,0\r");  // 1=check slot=0 type=0=root
        state = State::OK_WAIT;
        cert_state = CertState::INVALID;
      } else if (cert_state == CertState::INVALID) {
        out_bufs.push("AT+CFUN=4\r");  // 4=radio-off before cert update
        state = State::OK_WAIT;
        cert_state = CertState::OK_TO_ERASE;  // erase (radio should be off)
      } else if (cert_state == CertState::OK_TO_ERASE) {
        out_bufs.push("AT%CMNG=3,0,0\r");  // 3=del slot=0 type=0=root
        state = State::OK_WAIT;
        cert_state = CertState::OK_TO_WRITE;  // write (radio off, cert erased)
      } else if (cert_state == CertState::OK_TO_WRITE) {
        out_bufs.push("AT%CMNG=0,0,0,\"");
        out_bufs.push(mqtt_server.cert);
        out_bufs.push("\"\r");
        state = State::OK_WAIT;
        cert_state = CertState::UNKNOWN;  // re-verify after write

        // periodic poll steps
      } else if (periodic_step < 0 && poll_time >= next_periodic) {
        OK_DETAIL(
          "⏱️ Periodic timer: %.1f > %.1fs",
          raw_count<secd>(poll_time), raw_count<secd>(next_periodic)
        );
        next_periodic = poll_time + 10_s;
        out_bufs.push("AT+CMEE=1\r");  // enable extended errors
        state = State::OK_WAIT;
        periodic_step = 1;
        do_poll_radio = do_poll_ip = do_poll_mqtt = true;
      } else if (periodic_step == 1) {
        out_bufs.push("AT%XPDNCFG=1\r");  // always-on packet network
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 2) {
        out_bufs.push("AT+CFUN=1\r");  // enable radio
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 3) {
        out_bufs.push("AT+CEREG=1\r");  // radio events (after CFUN)
        state = State::OK_WAIT;
        ++periodic_step;
      } else if (periodic_step == 4) {
        out_bufs.push("AT+CGEREP=1\r");  // data events (after CFUN)
        state = State::OK_WAIT;
        periodic_step = -1;  // End of periodic poll

        // triggered poll steps
      } else if (do_poll_radio) {
        out_bufs.push("AT%XMONITOR\r");  // network and radio status
        state = State::OK_WAIT;
        do_poll_radio = false;
      } else if (do_poll_ip) {
        out_bufs.push("AT+CGPADDR\r");  // get packet (IP) addresses
        state = State::AT_CGPADDR_WAIT;
        do_poll_ip = false;
      } else if (do_poll_mqtt) {
        out_bufs.push("AT#XMQTTCON?\r");  // get MQTT connection status
        state = State::OK_WAIT;
        do_poll_mqtt = false;

        // soft radio restart if it doesn't seem to be running
      } else if (poll_time >= next_soft_reset) {
        out_bufs.push("AT+CFUN=4\r");  // turn radio off
        state = State::OK_WAIT;
        next_soft_reset = poll_time + 300_s;  // time for it to work
        next_periodic = {};  // poll right away to turn radio back on

        // MQTT connection management
      } else if (
        mqtt_state == MqttState::OK_TO_DISCONNECT ||
        (mqtt_state >= MqttState::CONNACK_WAIT && !status.ip_attached)
      ) {
        if (mqtt_state != MqttState::OK_TO_DISCONNECT) {
          OK_DETAIL("💬 %d -> OK_TO_DISCONNECT (IP down)", mqtt_state);
        }
        out_bufs.push("AT#XMQTTCON=0\r");
        OK_DETAIL("💬 OK_TO_DISCONNECT -> OK_TO_CONFIG");
        state = State::OK_WAIT;
        mqtt_state = MqttState::OK_TO_CONFIG;
      } else if (mqtt_state == MqttState::OK_TO_CONFIG &&
                 !status.imeisv.empty()) {
        out_bufs.push("AT#XMQTTCFG=\"");
        out_bufs.push(status.imeisv);
        out_bufs.push("\",60,1\r");
        OK_DETAIL("💬 OK_TO_CONFIG -> OK_TO_CONNECT");
        state = State::OK_WAIT;
        mqtt_state = MqttState::OK_TO_CONNECT;
      } else if (mqtt_state == MqttState::OK_TO_CONNECT &&
                 status.ip_attached && poll_time >= mqtt_backoff &&
                 !mqtt_server.host.empty()) {
        out_bufs.push("AT#XMQTTCON=1,\"");
        out_bufs.push(mqtt_server.user);
        out_bufs.push("\",\"");
        out_bufs.push(mqtt_server.password);
        out_bufs.push("\",\"");
        out_bufs.push(mqtt_server.host);
        out_bufs.push("\",");
        etl::to_string(mqtt_server.port, out_scratch);
        out_bufs.push(out_scratch);
        out_bufs.push(mqtt_server.cert.empty() ? "\r" : ",0\r");
        OK_DETAIL("💬 OK_TO_CONNECT -> CONNACK_WAIT");
        state = State::AT_XMQTTCON_WAIT;
        mqtt_state = MqttState::CONNACK_WAIT;
        mqtt_backoff = poll_time + 30_s;  // limit reconnection attempts
      } else if (mqtt_state == MqttState::CONNECTED &&
                 mqtt_subscribed < mqtt_subs.size()) {
        auto const sub = mqtt_subs[mqtt_subscribed];
        out_bufs.push("AT#XMQTTSUB=\"");
        out_bufs.push(sub);
        out_bufs.push("\",0\r");
        state = State::OK_WAIT;
        OK_DETAIL("💬 CONNECTED -> SUBACK_WAIT");
        mqtt_state = MqttState::SUBACK_WAIT;
      } else if (mqtt_state == MqttState::CONNECTED &&
                 !mqtt_outgoing.topic.empty()) {
        out_bufs.push("AT#XMQTTPUB=\"");
        out_bufs.push(mqtt_outgoing.topic);
        out_bufs.push("\",\"\",1,0,");
        etl::to_string(mqtt_outgoing.payload.size(), out_scratch);
        out_bufs.push(out_scratch);
        out_bufs.push("\r");
        OK_DETAIL("💬 CONNECTED -> PUBACK_WAIT");
        state = State::AT_XMQTTPUB_WAIT;
        mqtt_state = MqttState::PUBACK_WAIT;
      }

      if (!out_bufs.empty()) OK_DETAIL("▶️ %s", abbr(out_bufs).c_str());
    }

    while (!out_bufs.empty()) {
      auto* const buf = &out_bufs.front();
      for (int av = 0; av || ((av = serial->availableForWrite()) > 0); --av) {
        if (buf->empty()) break;
        serial->write(buf->front());
        buf->remove_prefix(1);
        last_serial_traffic = poll_time;
      }
      if (!buf->empty()) break;
      out_bufs.pop();
    }

    if (mqtt_state < MqttState::CONNECTED && !mqtt_outgoing.topic.empty()) {
      OK_ERROR("MQTT publish lost: %s", abbr(mqtt_outgoing.topic).c_str());
      mqtt_outgoing = {};
    }

    status.mqtt_ready = (
      mqtt_state >= MqttState::CONNECTED &&
      mqtt_subscribed >= mqtt_subs.size()
    );
    status.mqtt_publish_busy = !mqtt_outgoing.topic.empty();
    status.mqtt_receive_ready = !mqtt_incoming.topic.empty();

    last_poll = poll_time;
    return status;
  }

  void publish(MqttMessage pub) override {
    if (!mqtt_outgoing.topic.empty()) {
      OK_ERROR("MQTT publish while busy: %s", abbr(pub.topic).c_str());
    } else if (mqtt_state != MqttState::CONNECTED) {
      OK_ERROR("MQTT publish while unready: %s", abbr(pub.topic).c_str());
    } else if (pub.topic.empty() || pub.topic.size() > 128) {
      OK_ERROR("Bad MQTT topic size (%s): %db", abbr(pub.topic).c_str(),
               pub.topic.size());
    } else if (pub.payload.empty() || pub.payload.size() > 8192) {
      OK_ERROR("Bad MQTT payload size (%s): %db",
               abbr(pub.topic).c_str(), pub.payload.size());
    } else {
      OK_DETAIL("💬 Publish request: %s", abbr(pub.topic).c_str());
      mqtt_outgoing = pub;
      status.mqtt_publish_busy = true;
    }
  }

  MqttMessage receive() override {
    if (mqtt_incoming.topic.empty()) OK_ERROR("MQTT receive while empty");
    status.mqtt_receive_ready = false;
    auto const temp = mqtt_incoming;
    mqtt_incoming = {};
    return temp;
  }

 private:
  enum class State {
    IDLE,
    RESET_WAIT,
    PROBE_WAIT,
    PROBE_DRAIN,
    AT_CGMM_WAIT,
    AT_CGMR_WAIT,
    AT_CGPADDR_WAIT,
    AT_XMQTTCON_WAIT,
    AT_XMQTTPUB_DATA,
    AT_XMQTTPUB_WAIT,
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
    OK_TO_DISCONNECT,
    OK_TO_CONFIG,
    OK_TO_CONNECT,
    CONNACK_WAIT,
    CONNECTED,
    SUBACK_WAIT,
    PUBACK_WAIT,
    PUBLISH_PENDING,
  };

  HardwareSerial* const serial;
  int const enable_pin;
  MqttServerConfig const mqtt_server;
  etl::span<etl::string_view const> const mqtt_subs;
  CellModemStatus status;

  State state = State::IDLE;
  steady_clock::time_point last_poll = {};
  steady_clock::time_point last_serial_traffic = {};
  steady_clock::time_point next_periodic = {};
  steady_clock::time_point next_soft_reset = {};
  steady_clock::time_point next_hard_reset = {};
  int periodic_step = -1;
  bool do_probe = true;
  bool do_poll_radio = true;
  bool do_poll_ip = true;
  bool do_poll_mqtt = true;

  etl::string<2048> in_buf;
  int in_counted_remain = -1;  // -1=linemode, 0=buffered, >0=buffering

  etl::circular_buffer<etl::string_view, 16> out_bufs;
  etl::string<10> out_scratch;

  CertState cert_state = CertState::UNKNOWN;
  MqttState mqtt_state = MqttState::OK_TO_DISCONNECT;
  steady_clock::time_point mqtt_backoff = {};
  MqttMessage mqtt_outgoing = {};
  MqttMessage mqtt_incoming = {};
  int mqtt_in_topic_size = -1;
  int mqtt_in_payload_size = -1;
  int mqtt_subscribed = 0;

  void handle_input_line() {
    OK_DETAIL("⬅️ %s", abbr(in_buf).c_str());
    etl::string_view rest(in_buf);

    //
    // Generic fault/reset messages
    //

    if (state == State::RESET_WAIT) {
      OK_ERROR("Unexpected data in reset: %s", abbr(in_buf).c_str());
      return;
    }

    // "Ready" is often prefixed with \xFF because... UART init or something?
    if (eat(&rest, "Ready") || eat(&rest, "\xffReady")) {
      if (!status.running) {
        OK_NOTE("Modem init: %s", abbr(in_buf).c_str());
      } else {
        OK_ERROR("Modem restart: %s", abbr(in_buf).c_str());
        status.failed = true;
      }
      state = State::IDLE;
      status.running = status.registered = status.ip_attached = false;
      next_periodic = {};  // initialize immediately
      out_bufs.clear();  // stop anything we were doing
      do_probe = true;
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      OK_ERROR("Modem fault (state=%d): %s", state, abbr(in_buf).c_str());
      state = State::OK_WAIT;  // really waiting for "Ready"
      status.running = status.registered = status.ip_attached = false;
      status.failed = true;
      out_bufs.clear();  // stop anything we were doing
      do_probe = true;
      return;
    }

    if (eat(&rest, "ERROR") ||
        eat(&rest, "+CME ERROR:") ||
        eat(&rest, "+CMS ERROR:")) {
      OK_ERROR("Modem error (state=%d): %s", state, abbr(in_buf).c_str());
      state = State::IDLE;
      return;
    }

    //
    // Identifiable message responses
    //

    if (eat(&rest, "+CEREG:")) {
      do_poll_radio = do_poll_ip = true;  // This could be stale; trigger a poll
      return;
    }

    if (eat(&rest, "+CGEV:")) {
      do_poll_ip = true;  // This could be stale; trigger a poll
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
          if (eat_int(&a1, &b1) && eat(&a1, ".") &&
              eat_int(&a1, &b2) && eat(&a1, ".") &&
              eat_int(&a1, &b3) && eat(&a1, ".") &&
              eat_int(&a1, &b4) && eat(&a1, "")) {
            status.ip_attached = true;
            status.ip_addr = (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
            if (state == State::AT_CGPADDR_WAIT) state = State::OK_WAIT;
          } else {
            OK_ERROR("Bad IPv4: %s", abbr(in_buf).c_str());
          }
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "+CGSN:")) {
      etl::string_view v;
      if (eat_quoted(&rest, &v)) status.imeisv = v.empty() ? "-" : v;
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "%CMNG:")) {
      int tag, type;
      etl::string_view sha;
      if (eat_int(&rest, &tag) && eat(&rest, ",") &&
          eat_int(&rest, &type) && eat(&rest, ",") &&
          eat_quoted(&rest, &sha) && tag == 0 && type == 0) {
        if (sha == mqtt_server.cert_sha256) {
          cert_state = CertState::VALID;
        } else {
          cert_state = CertState::INVALID;
          OK_ERROR(
            "Cert mismatch:\n  expect: %.*s\n  actual: %.*s",
            mqtt_server.cert_sha256.size(), mqtt_server.cert_sha256.data(),
            sha.size(), sha.data()
          );
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "#XDATAMODE:")) {
      int err;
      if (eat_int(&rest, &err)) {
        if (state == State::AT_XMQTTPUB_DATA) {
          if (err != 0) {
            OK_ERROR("MQTT publish failed: %s", abbr(in_buf).c_str());
            OK_DETAIL("💬 %d -> OK_TO_DISCONNECT", mqtt_state);
            mqtt_state = MqttState::OK_TO_DISCONNECT;  // reconnect, retry
          }
          state = State::IDLE;
        } else {
          OK_ERROR(
            "Unexpected #XDATAMODE (state=%d): %s", state, abbr(in_buf).c_str()
          );
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "%XMONITOR:")) {
      int reg;
      if (eat_int(&rest, &reg)) {
        if (reg != 0) next_soft_reset = last_poll + 60_s;  // push forward
        status.running = (reg == 1 || reg == 2 || reg == 5);
        status.registered = (reg == 1 || reg == 5);
        status.roaming = (reg == 5);
        if (reg == 3 || reg == 90) status.failed = true;
        if (status.running) status.failed = false;
        if (!status.registered) status.ip_attached = false;

        etl::string_view op_full, op_short, op_mcc_mnc;
        etl::string_view cell_tac, cell_id;
        int cell_phys_id;
        int radio_tech, radio_band, radio_earfcn, radio_rsrp, radio_snr;
        etl::string_view power_edrx, power_atime, power_tau_ext, power_tau;
        if (eat(&rest, ",") && eat_quoted(&rest, &op_full) &&
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
            eat(&rest, ",") && eat_quoted(&rest, &power_tau)) {
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
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "#XMQTTCON:")) {
      etl::string_view cid, host;
      int port, sec_tag = -1;
      if (eat(&rest, "0")) {
        if (mqtt_state == MqttState::OK_TO_DISCONNECT) {
          OK_DETAIL("💬 OK_TO_DISCONNECT -> OK_TO_CONFIG");
          mqtt_state = MqttState::OK_TO_CONFIG;
        } else if (mqtt_state >= MqttState::CONNACK_WAIT) {
          OK_ERROR("MQTT not connected, reconnecting");
          OK_DETAIL("💬 %d -> OK_TO_CONFIG", mqtt_state);
          mqtt_state = MqttState::OK_TO_CONFIG;
        }
      } else if (eat(&rest, "1") &&
                 eat(&rest, ",") && eat_quoted(&rest, &cid) &&
                 eat(&rest, ",") && eat_quoted(&rest, &host) &&
                 eat(&rest, ",") && eat_int(&rest, &port) &&
                 ((eat(&rest, ",") && eat_int(&rest, &sec_tag)) || true)) {
        int const config_sec = mqtt_server.cert.empty() ? -1 : 0;
        if (mqtt_state > MqttState::OK_TO_DISCONNECT &&
            mqtt_state < MqttState::CONNACK_WAIT) {
          OK_ERROR("Unexpected MQTT connection: %s", abbr(in_buf).c_str());
          OK_DETAIL("💬 %d -> OK_TO_DISCONNECT", mqtt_state);
          mqtt_state = MqttState::OK_TO_DISCONNECT;
        } else if (host != mqtt_server.host || port != mqtt_server.port ||
                   cid != status.imeisv || sec_tag != config_sec) {
          OK_ERROR(
            "Bad MQTT link:\n  %.*s:%d[%d] (%.*s) !=\n  %.*s:%d[%d] (%.*s)",
            host.size(), host.data(), port, sec_tag, cid.size(), cid.data(),
            mqtt_server.host.size(), mqtt_server.host.data(),
            mqtt_server.port, config_sec,
            status.imeisv.size(), status.imeisv.data()
          );
          OK_DETAIL("💬 %d -> OK_TO_DISCONNECT", mqtt_state);
          mqtt_state = MqttState::OK_TO_DISCONNECT;
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "#XMQTTEVT:")) {
      int type, err;
      if (eat_int(&rest, &type) && eat(&rest, ",") && eat_int(&rest, &err)) {
        // Hard reset after 5 minutes without any MQTT signal
        if (type != 1 && err == 0) next_hard_reset = last_poll + 300_s;

        if (type == 0) {  // CONNACK (or connection failed)
          if (err != 0) {
            // Connection failed - this could be stale; trigger a poll
            OK_ERROR("MQTT connect failed: %s", abbr(in_buf).c_str());
            if (mqtt_state >= MqttState::CONNACK_WAIT) do_poll_mqtt = true;
          } else if (mqtt_state == MqttState::CONNACK_WAIT) {
            OK_DETAIL("💬 CONNACK_WAIT -> CONNECTED (CONNACK)");
            mqtt_state = MqttState::CONNECTED;  // CONNACK confirmed!
            mqtt_subscribed = 0;
          } else {
            OK_ERROR("Unexpected MQTT CONNACK: %s", abbr(in_buf).c_str());
            OK_DETAIL("💬 %d -> OK_TO_DISCONNECT", mqtt_state);
            mqtt_state = MqttState::OK_TO_DISCONNECT;
          }
        } else if (type == 1) {  // DISCONNECT
          // This could be stale; trigger a poll
          if (mqtt_state >= MqttState::CONNACK_WAIT) do_poll_mqtt = true;
        } else if (type == 3) {  // PUBACK
          if (mqtt_state != MqttState::PUBACK_WAIT) {
            OK_ERROR("Unexpected MQTT PUBACK: %s", abbr(in_buf).c_str());
          } else if (err != 0) {
            OK_ERROR("MQTT publish failed: %s", abbr(in_buf).c_str());
            OK_DETAIL("💬 PUBACK_WAIT -> OK_TO_DISCONNECT (!PUBACK)");
            mqtt_state = MqttState::OK_TO_DISCONNECT;  // reconnect, retry?
          } else {
            OK_DETAIL("💬 PUBACK_WAIT -> CONNECTED (PUBACK)");
            mqtt_state = MqttState::CONNECTED;
            mqtt_outgoing = {};  // Message confirmed
          }
        } else if (type == 7) {  // SUBACK
          if (mqtt_state != MqttState::SUBACK_WAIT) {
            OK_ERROR("Unexpected MQTT SUBACK: %s", abbr(in_buf).c_str());
          } else if (err != 0) {
            OK_ERROR("MQTT subscribe failed: %s", abbr(in_buf).c_str());
            OK_DETAIL("💬 SUBACK_WAIT -> OK_TO_DISCONNECT (!SUBACK)");
            mqtt_state = MqttState::OK_TO_DISCONNECT;  // reconnect, retry
          } else {
            ++mqtt_subscribed;
            OK_DETAIL("💬 SUBACK_WAIT -> CONNECTED (SUBACK)");
            mqtt_state = MqttState::CONNECTED;
          }
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "#XMQTTMSG:")) {
      int topic_size, payload_size;
      if (eat_int(&rest, &topic_size) && eat(&rest, ",") &&
          eat_int(&rest, &payload_size)) {
        if (topic_size < 1 || payload_size < 1) {
          OK_ERROR("Bad sizes: %s", abbr(in_buf).c_str());
        } else {
          // (line parser takes the CR) LF + topic + CR LF + payload + CR LF
          mqtt_in_topic_size = topic_size;
          mqtt_in_payload_size = payload_size;
          in_counted_remain = 1 + topic_size + 2 + payload_size + 2;
          if (in_counted_remain >= 65536) {
            OK_ERROR("Resetting to escape drowning: %s", abbr(in_buf).c_str());
            next_hard_reset = {};
          }
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "#XSMVER:")) {
      etl::string_view v1, v2, v3;
      if (eat_quoted(&rest, &v1) &&
          eat(&rest, ",") && eat_quoted(&rest, &v2) &&
          eat(&rest, ",") && eat_quoted(&rest, &v3)) {
        status.versions[1] = v1.empty() ? "-" : v1;
        status.versions[2] = v2.empty() ? "-" : v2;
        status.versions[3] = v3.empty() ? "-" : v3;
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
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
      if (state == State::PROBE_WAIT) {
        state = State::PROBE_DRAIN;
      } else if (state == State::AT_XMQTTPUB_WAIT) {
        out_bufs.push(mqtt_outgoing.payload);
        OK_DETAIL("⏩️ %s", abbr(out_bufs).c_str());
        state = State::AT_XMQTTPUB_DATA;
      } else if (state == State::AT_CGPADDR_WAIT) {
        status.ip_attached = false;  // +CGPADDR completed, no IPs found
        status.ip_addr = 0;
        state = State::IDLE;
      } else if (state == State::OK_WAIT || state == State::AT_XMQTTCON_WAIT) {
        state = State::IDLE;
      } else {
        OK_ERROR("Unexpected OK (state=%d): %s", state, abbr(in_buf).c_str());
        state = State::IDLE;
      }
      return;
    }

    //
    // Other responses interpreted by .state
    //

    if (state == State::AT_CGMM_WAIT) {
      status.hardware = etl::trim_view_whitespace(rest);
      state = State::OK_WAIT;
      return;
    } else if (state == State::AT_CGMR_WAIT) {
      status.versions[0] = etl::trim_view_whitespace(rest);
      state = State::OK_WAIT;
      return;
    }

    OK_ERROR("Unexpected data (state=%d): %s", state, abbr(in_buf).c_str());
  }

  void handle_counted_input() {
    if (mqtt_in_topic_size <= 0 || mqtt_in_payload_size < 0) {
      OK_ERROR("Unexpected data block: %s", abbr(in_buf).c_str());
    } else {
      etl::string_view const in_view(in_buf);
      int const t_begin = 1, t_end = t_begin + mqtt_in_topic_size;
      int const p_begin = t_end + 2, p_end = p_begin + mqtt_in_payload_size;
      auto const header_lf = in_view.substr(0, t_begin);
      auto const topic_crlf = in_view.substr(t_end, 2);
      auto const payload_crlf = in_view.substr(p_end, 2);
      if (!etl::string_view("\n").starts_with(header_lf) ||
          !etl::string_view("\r\n").starts_with(topic_crlf) ||
          !etl::string_view("\r\n").starts_with(payload_crlf)) {
        OK_ERROR(
          "Bad MQTT framing: [%s] [%s] [%s]",
          abbr(header_lf).c_str(), abbr(topic_crlf).c_str(),
          abbr(payload_crlf).c_str()
        );
      } else {
        mqtt_incoming.topic = in_view.substr(t_begin, mqtt_in_topic_size);
        mqtt_incoming.payload = in_view.substr(p_begin, mqtt_in_payload_size);
        OK_DETAIL(
          "💬 Incoming (%d/%db): %s",
          mqtt_incoming.payload.size(), mqtt_in_payload_size,
          abbr(mqtt_incoming.topic).c_str()
        );
      }
    }
  }

  static etl::string<40> abbr(etl::string_view str) {
    return abbr(etl::circular_buffer<etl::string_view, 1>{str});
  }

  static etl::string<40> abbr(
    etl::icircular_buffer<etl::string_view> const& bufs
  ) {
    etl::string<40> out;
    for (auto const buf : bufs) {
      for (auto const ch : buf) {
        if (out.size() > out.max_size() - 10) {
          out.append("[...]");
          return out;
        } else if (ch == 10) {
          out.append("\\n");
        } else if (ch == 13) {
          out.append("\\r");
        } else if (ch < 32 || ch > 126) {
          out.append("\\x");
          auto const format = etl::format_spec().hex().width(2).fill('0');
          etl::to_string(ch, out, format, true);
        } else {
          out.push_back(ch);
        }
      }
    }
    return out;
  }

  static bool eat(etl::string_view* str, etl::string_view literal) {
    auto v = etl::trim_view_whitespace_left(*str);
    if (literal.empty()) return v.empty();  // special case for EOL
    if (!v.starts_with(literal)) return false;
    *str = v.substr(literal.size());
    return true;
  }

  static bool eat_int(etl::string_view* str, int* out) {
    auto v = etl::trim_view_whitespace_left(*str);
    auto const end = v.find_first_not_of("0123456789", v.starts_with("-"));
    auto const result = etl::to_arithmetic<int>(v.substr(0, end));
    if (!result) return false;
    *out = result.value();
    *str = (end == etl::string_view::npos) ? "" : v.substr(end);
    return true;
  }

  static bool eat_quoted(etl::string_view* str, etl::string_view* out) {
    auto v = *str;
    if (!eat(&v, "\"")) return false;
    auto const end = v.find('"');
    if (end == etl::string_view::npos) return false;
    *out = v.substr(0, end);
    *str = v.substr(end + 1);
    return true;
  }
};

etl::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial, int en_pin,
  MqttServerConfig const& server,
  etl::span<etl::string_view const> subs
) {
  return etl::unique_ptr(new CellModemClientDef(serial, en_pin, server, subs));
}
