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
    HardwareSerial* serial,
    MqttServerConfig const& server,
    etl::span<etl::string_view const> subs
  ) : serial(serial), mqtt_server(server), mqtt_subs(subs) {
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

    auto const last_poll = poll_time;
    poll_time = steady_clock::now();
    if (last_poll > steady_clock::time_point()) {
      if (auto const elapsed = poll_time - last_poll; elapsed > 50_ms) {
        OK_ERROR("Slow poll (%.3fs since last)", raw_count<secd>(elapsed));
      }
    }

    if (state != State::IDLE && poll_time >= state_deadline) {
      OK_ERROR("Command timeout (state=%d)", state);
      out_bufs.push("\r+++\"\r");  // unstick modem parser state
      state = State::IDLE;
    }

    if (periodic_step < 0 && poll_time >= next_periodic) {
      OK_DETAIL(
        "⏱️ Periodic check (%.1f > %.1fs)",
        raw_count<secd>(poll_time), raw_count<secd>(next_periodic)
      );
      next_periodic = poll_time + 10_s;
      periodic_step = 0;
      do_poll_reg = do_poll_ip = do_poll_mqtt = true;
    }

    if (state == State::IDLE && out_bufs.empty()) {
      // hardware ID (once at startup)
      if (status.hardware.empty()) {
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
      } else if (periodic_step == 0) {
        out_bufs.push("AT+CMEE=1\r");  // enable extended errors
        state = State::OK_WAIT;
        ++periodic_step;
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

      } else if (do_poll_reg) {
        out_bufs.push("AT%XMONITOR\r");  // network and radio status
        state = State::OK_WAIT;
        do_poll_reg = false;
      } else if (do_poll_ip) {
        out_bufs.push("AT+CGPADDR\r");  // get packet (IP) addresses
        state = State::AT_CGPADDR_WAIT;
        do_poll_ip = false;
      } else if (do_poll_mqtt) {
        out_bufs.push("AT#XMQTTCON?\r");  // get MQTT connection status
        state = State::OK_WAIT;
        do_poll_mqtt = false;

        // MQTT connection management
      } else if (mqtt_state == MqttState::OK_TO_DISCONNECT) {
        out_bufs.push("AT#XMQTTCON=0\r");
        state = State::OK_WAIT;
        OK_DETAIL("💬 OK_TO_DISCONNECT -> OK_TO_CONFIG");
        mqtt_state = MqttState::OK_TO_CONFIG;
      } else if (mqtt_state == MqttState::OK_TO_CONFIG &&
                 !status.imeisv.empty()) {
        out_bufs.push("AT#XMQTTCFG=\"");
        out_bufs.push(status.imeisv);
        out_bufs.push("\",60,1\r");
        state = State::OK_WAIT;
        OK_DETAIL("💬 OK_TO_CONFIG -> OK_TO_CONNECT");
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
        state = State::AT_XMQTTCON_WAIT;
        OK_DETAIL("💬 OK_TO_CONNECT -> CONNECT_WAIT");
        mqtt_state = MqttState::CONNECT_WAIT;
        mqtt_backoff = poll_time + 30_s;  // limit reconnection attempts
      } else if (mqtt_state == MqttState::CONNECTED &&
                 mqtt_subscribed < mqtt_subs.size()) {
        auto const sub = mqtt_subs[mqtt_subscribed];
        out_bufs.push("AT#XMQTTSUB=\"");
        out_bufs.push(sub);
        out_bufs.push("\",0\r");
        state = State::OK_WAIT;
        OK_DETAIL("💬 CONNECTED -> SUBSCRIBE_WAIT");
        mqtt_state = MqttState::SUBSCRIBE_WAIT;
      } else if (mqtt_state == MqttState::PUBLISH_PENDING) {
        out_bufs.push("AT#XMQTTPUB=\"");
        out_bufs.push(mqtt_pub.topic);
        out_bufs.push("\",\"\",0,0,");
        etl::to_string(mqtt_pub.payload.size(), out_scratch);
        out_bufs.push(out_scratch);
        out_bufs.push("\r");
        state = State::AT_XMQTTPUB_WAIT;
        OK_DETAIL("💬 PUBLISH_PENDING -> CONNECTED");
        mqtt_state = MqttState::CONNECTED;
      }

      if (!out_bufs.empty()) OK_DETAIL("▶️ %s", abbr(out_bufs).c_str());
    }

    while (!out_bufs.empty() && serial->availableForWrite() > 0) {
      auto* buf = &out_bufs.front();
      while (!buf->empty() && serial->availableForWrite() > 0) {
        serial->write(buf->front());
        buf->remove_prefix(1);
      }
      if (buf->empty()) out_bufs.pop();
    }

    // Start the state timeout after output buffers are written
    if (!out_bufs.empty() || state == State::IDLE) {
      state_deadline = steady_clock::time_point::max();
    } else if (state == State::AT_XMQTTCON_WAIT) {
      state_deadline = min(state_deadline, poll_time + 60_s);
    } else {
      state_deadline = min(state_deadline, poll_time + 5_s);
    }

    if (mqtt_state != MqttState::PUBLISH_PENDING &&
        state != State::AT_XMQTTPUB_WAIT &&
        state != State::AT_XMQTTPUB_DATA) {
      mqtt_pub = {};  // Done with borrowed data
    }

    status.mqtt_ready = (
      mqtt_state >= MqttState::CONNECTED &&
      mqtt_subscribed >= mqtt_subs.size()
    );
    status.mqtt_publish_busy = !mqtt_pub.topic.empty();
    return status;
  }

  void publish(MqttMessage pub) override {
    if (!mqtt_pub.topic.empty()) {
      OK_ERROR("MQTT publish while busy: %s", abbr(pub.topic).c_str());
    } else if (mqtt_state != MqttState::CONNECTED ||
               mqtt_subscribed < mqtt_subs.size()) {
      OK_ERROR("MQTT publish while unready: %s", abbr(pub.topic).c_str());
    } else if (pub.topic.empty() || pub.topic.size() > 128) {
      OK_ERROR("Bad MQTT topic size (%s): %db", abbr(pub.topic).c_str(),
               pub.topic.size());
    } else if (pub.payload.empty() || pub.payload.size() > 8192) {
      OK_ERROR("Bad MQTT payload size (%s): %db",
               abbr(pub.topic).c_str(), pub.payload.size());
    } else {
      mqtt_pub = pub;
      OK_DETAIL("💬 CONNECTED -> PUBLISH_PENDING");
      mqtt_state = MqttState::PUBLISH_PENDING;
      status.mqtt_publish_busy = true;
    }
  }

  MqttMessage incoming() const override {
    return {};
  }

 private:
  enum class State {
    IDLE,
    AT_CGMM_WAIT,
    AT_CGMR_WAIT,
    AT_CGPADDR_WAIT,
    AT_XMQTTCON_WAIT,
    AT_XMQTTPUB_DATA,
    AT_XMQTTPUB_WAIT,
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
    OK_TO_DISCONNECT,
    OK_TO_CONFIG,
    OK_TO_CONNECT,
    CONNECT_WAIT,
    CONNECTED,
    SUBSCRIBE_WAIT,
    PUBLISH_PENDING,
  };

  HardwareSerial* const serial;
  MqttServerConfig const mqtt_server;
  etl::span<etl::string_view const> const mqtt_subs;
  CellModemStatus status;

  State state = State::IDLE;
  steady_clock::time_point poll_time = {};
  steady_clock::time_point state_deadline = {};
  steady_clock::time_point next_periodic = {};
  int periodic_step = -1;
  bool do_poll_reg = true;
  bool do_poll_ip = true;
  bool do_poll_mqtt = true;

  CertState cert_state = CertState::UNKNOWN;
  MqttState mqtt_state = MqttState::OK_TO_DISCONNECT;
  steady_clock::time_point mqtt_backoff = {};
  MqttMessage mqtt_pub = {};
  int mqtt_subscribed = 0;

  etl::string<256> in_buf;
  int in_expect = 0;

  etl::circular_buffer<etl::string_view, 16> out_bufs;
  etl::string<10> out_scratch;

  void handle_input_block() {
    OK_ERROR("Unexpected (state=%d): %s", state, abbr(in_buf).c_str());
  }

  void handle_input_line() {
    etl::string_view rest(in_buf);

    //
    // Generic fault/reset messages
    //

    if (eat(&rest, "Ready")) {
      if (!status.running) {
        OK_NOTE("Modem init: %s", abbr(in_buf).c_str());
      } else {
        OK_ERROR("Modem reset: %s", abbr(in_buf).c_str());
      }
      state = State::IDLE;
      OK_DETAIL("💬 %d -> OK_TO_DISCONNECT", mqtt_state);
      mqtt_state = MqttState::OK_TO_DISCONNECT;
      status.running = true;
      next_periodic = {};  // initialize immediately
      out_bufs = {};  // stop anything we were doing
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      OK_ERROR("Modem fault (state=%d): %s", state, abbr(in_buf).c_str());
      state = State::FAILED;
      status.running = false;
      status.failed = true;
      out_bufs = {};  // stop anything we were doing
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
      do_poll_reg = do_poll_ip = true;  // This could be stale; trigger a poll
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
            OK_ERROR("Bad +CGPADDR IPv4: %s", abbr(in_buf).c_str());
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
      } else {
        OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      }
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
            "Unexpected #XDATAMODE (state=%d): %s",
            state, abbr(in_buf).c_str()
          );
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      return;
    }

    if (eat(&rest, "%XMONITOR:")) {
      int reg;
      if (eat_int(&rest, &reg)) {
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
        if (mqtt_state >= MqttState::CONNECT_WAIT) {
          OK_ERROR("MQTT not connected, reconnecting");
          OK_DETAIL("💬 %d -> OK_TO_DISCONNECT", mqtt_state);
          mqtt_state = MqttState::OK_TO_DISCONNECT;  // Recommended by Nordic
        }
      } else if (eat(&rest, "1") &&
                 eat(&rest, ",") && eat_quoted(&rest, &cid) &&
                 eat(&rest, ",") && eat_quoted(&rest, &host) &&
                 eat(&rest, ",") && eat_int(&rest, &port) &&
                 ((eat(&rest, ",") && eat_int(&rest, &sec_tag)) || true)) {
        int const config_sec = mqtt_server.cert.empty() ? -1 : 0;
        if (mqtt_state > MqttState::OK_TO_DISCONNECT &&
            mqtt_state < MqttState::CONNECT_WAIT) {
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
        if (type == 0) {  // CONNACK (or connection failed)
          if (err != 0) {
            // Connection failed - this could be stale; trigger a poll
            OK_ERROR("MQTT connect failed: %s", abbr(in_buf).c_str());
            if (mqtt_state >= MqttState::CONNECT_WAIT) do_poll_mqtt = true;
          } else if (mqtt_state == MqttState::CONNECT_WAIT) {
            OK_DETAIL("💬 CONNECT_WAIT -> CONNECTED (CONNACK)");
            mqtt_state = MqttState::CONNECTED;  // CONNACK confirmed!
            mqtt_subscribed = 0;
          } else {
            OK_ERROR("Unexpected MQTT CONNACK: %s", abbr(in_buf).c_str());
            OK_DETAIL("💬 %d -> OK_TO_DISCONNECT", mqtt_state);
            mqtt_state = MqttState::OK_TO_DISCONNECT;
          }
        } else if (type == 1) {  // Disconnected
          // This could be stale; trigger a poll
          if (mqtt_state >= MqttState::CONNECT_WAIT) do_poll_mqtt = true;
        } else if (type == 2) {  // PUBLISH
          // no action; #XMQTTMSG carries the actual message
        } else if (type == 3) {  // PUBACK
          // no action; we onyl use QoS-0 fire and forget
        } else if (type == 7) {  // SUBACK
          if (mqtt_state != MqttState::SUBSCRIBE_WAIT) {
            OK_ERROR("Unexpected MQTT SUBACK: %s", abbr(in_buf).c_str());
          } else if (err != 0) {
            OK_ERROR("MQTT subscribe failed: %s", abbr(in_buf).c_str());
            OK_DETAIL("💬 SUBSCRIBE_WAIT -> OK_TO_DISCONNECT (!SUBACK)");
            mqtt_state = MqttState::OK_TO_DISCONNECT;  // reconnect, retry
          } else {
            ++mqtt_subscribed;
            OK_DETAIL("💬 SUBSCRIBE_WAIT -> CONNECTED (SUBACK)");
            mqtt_state = MqttState::CONNECTED;
          }
        } else if (type == 9) {  // PINGRESP
          // no action; the MQTT client tracks staleness
        } else {
          // remaining types are related to QoS-2 messages, etc.
          OK_ERROR("Unexpected #XMQTTEVT: %s", abbr(in_buf).c_str());
        }
      }
      if (!eat(&rest, "")) OK_ERROR("Bad #XMQTTEVT: %s", abbr(in_buf).c_str());
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
      auto const old_state = state;
      state = State::IDLE;
      if (old_state == State::AT_CGMM_WAIT) {
        status.hardware = "-";
      } else if (old_state == State::AT_CGMR_WAIT) {
        status.versions[0] = "-";
      } else if (old_state == State::AT_CGPADDR_WAIT) {
        status.ip_attached = false;  // +CGPADDR completed, no IPs found
        status.ip_addr = 0;
      } else if (old_state == State::AT_XMQTTCON_WAIT) {
        // (no further action)
      } else if (old_state == State::AT_XMQTTPUB_WAIT) {
        out_bufs.push(mqtt_pub.payload);
        OK_DETAIL("⏩️ %s", abbr(out_bufs).c_str());
        state = State::AT_XMQTTPUB_DATA;
      } else if (old_state != State::OK_WAIT) {
        OK_ERROR("Unexpected OK (state=%d): %s", state, abbr(in_buf).c_str());
      }
      if (!eat(&rest, "")) OK_ERROR("Bad OK: %s", abbr(in_buf).c_str());
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
  arduino::HardwareSerial* serial,
  MqttServerConfig const& server,
  etl::span<etl::string_view const> subs
) {
  return etl::unique_ptr(new CellModemClientDef(serial, server, subs));
}
