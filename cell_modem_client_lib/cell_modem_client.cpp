#pragma GCC diagnostic error "-Wimplicit-fallthrough" 
#pragma GCC diagnostic error "-Wswitch" 

#include "cell_modem_client.h"

#include <Arduino.h>
#include <etl/array.h>
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

static const OkLoggingContext
  OK_CONTEXT("cell_modem_client"),
  OK_CONTEXT_hard_resets("cell_modem_client.hard_reset"),
  OK_CONTEXT_radio_resets("cell_modem_client.radio_reset"),
  OK_CONTEXT_mqtt_resets("cell_modem_client.mqtt_reset"),
  OK_CONTEXT_usage_errors("cell_modem_client.app"),
  OK_CONTEXT_serial_errors("cell_modem_client.serial"),
  OK_CONTEXT_timeout_errors("cell_modem_client.timeout"),
  OK_CONTEXT_modem_errors("cell_modem_client.modem"),
  OK_CONTEXT_mqtt_errors("cell_modem_client.mqtt");

#define COUNT_ERROR(counter, ...) ({  \
  OK_LOG(OK_CONTEXT_##counter, OK_ERROR_LEVEL, __VA_ARGS__);  \
  ++status.counter;  \
})

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

    if (server.cert.empty()) cert_state = CertState::DONE;  // no cert
  }

  CellModemStatus const* poll() override {
    auto const last_poll = poll_time;
    poll_time = steady_clock::now();
    auto const elapsed = poll_time - last_poll;
    if (elapsed > 50_ms && last_poll > steady_clock::time_point{}) {
      COUNT_ERROR(usage_errors, "Slow poll (%.3fs)", raw_count<secd>(elapsed));
    }

    if (!mqtt_incoming.topic.empty()) {
      COUNT_ERROR(usage_errors, "Unhandled MQTT message: %s",
                  abbr(mqtt_incoming.topic).c_str());
      mqtt_incoming = {};  // Depends on counted buffer which will be lost
    }

    // Hard modem reset at startup, on request, or if MQTT fails to thrive
    if (poll_time >= next_hard_reset && enable_pin >= 0) {
      next_hard_reset = poll_time + 1800_s;
      reset_session_state();
      if (last_poll > steady_clock::time_point{}) {
        COUNT_ERROR(hard_resets, "Starting reset (p%d LOW)", enable_pin);
      }
      pinMode(enable_pin, OUTPUT);
      digitalWrite(enable_pin, LOW);
      last_step = poll_time;  // start reset timer now
      state = State::RESET_TIME;
    }

    //
    // Read serial input
    //

    if (in_counted_todo == 0) {
      in_buf.clear();  // Counted buffer only remains for one poll cycle
      in_counted_todo = -1;
    }

    for (int av = 0; av || ((av = serial->available()) > 0); --av) {
      int const ch = serial->read();
      if (ch < 0) {
        COUNT_ERROR(serial_errors, "Read error: avail=%d ch=%d", av, ch);
        break;
      }

      if (in_counted_todo > 0) {
        if (!in_buf.full()) in_buf.push_back(ch);
        if (--in_counted_todo == 0) {
          handle_counted_input();
          break;  // buffer stays valid until the next poll() (see above)
        }
      } else if (ch == '\r' || ch == '\n') {
        if (!in_buf.empty()) {
          handle_input_line();
          in_buf.clear();
        }
      } else if (ch < 32 || ch > 255) {
        COUNT_ERROR(serial_errors, "Bad char (st=%d): 0x%02x", state, ch);
        in_buf.clear();
      } else {
        if (in_buf.full()) {
          COUNT_ERROR(serial_errors, "Long input: %s", abbr(in_buf).c_str());
          in_buf.clear();
        }
        in_buf.push_back(ch);
      }
    }

    //
    // State timeouts
    //

    if (state != State::READY) {
      auto const elapsed = poll_time - last_step;
      auto const allowed =
        (state == State::RESET_TIME || state == State::PROBE_DRAIN) ? 100_ms :
        (state == State::AT_XMQTTCON_WAIT) ? 60000_ms : 10000_ms;
      if (elapsed > allowed) {
        out_todo.clear();  // cancel remainder of sequence
        switch (state) {
          case State::READY:
          case State::AT_CGMM_WAIT:
          case State::AT_CGMR_WAIT:
          case State::AT_CGPADDR_WAIT:
          case State::OK_WAIT:
          case State::PROBE_WAIT:
            COUNT_ERROR(timeout_errors, "AT timeout (st=%d)", state);
            state = State::READY;
            do_probe = true;  // check on the modem and poll everything
            break;
          case State::AT_XMQTT_WAIT:
          case State::AT_XMQTTCON_WAIT:
          case State::AT_XMQTTPUB_DATA_WAIT:
            COUNT_ERROR(timeout_errors, "AT#XMQTT timeout (st=%d)", state);
            state = State::READY;
            mqtt_state = MqttState::NEEDS_DISCONNECT;
            do_probe = true;  // check on the modem and poll everything
            break;
          case State::PROBE_DRAIN:
            OK_DETAIL("✅️ Modem probe complete");
            next_radio_reset = poll_time + 300_s;  // give the radio a chance
            next_periodic = {};  // poll everything right away
            state = State::READY;
            do_get_version = do_setup_radio = true;  // setup all the things
            break;
          case State::RESET_TIME:
            OK_DETAIL("🔛 Ending modem reset (p%d PULLUP)", enable_pin);
            pinMode(enable_pin, INPUT_PULLUP);
            digitalWrite(enable_pin, HIGH);
            state = State::READY;
            break;
        }
      }
    }

    //
    // Command sending (transitions from idle)
    //

    if (state == State::READY && in_counted_todo < 0 && out_todo.empty()) {
      // command probe (at startup and whenever we lose track of the modem)
      if (do_probe) {
        do_probe = false;
        out_todo.push({"\r+++\"\rAT\r", State::PROBE_WAIT});

      // hardware ID (after every probe)
      } else if (do_get_version) {
        do_get_version = false;
        out_todo.push({"AT+CGMM\r", State::AT_CGMM_WAIT});  // modem model
        out_todo.push({"AT+CGMR\r", State::AT_CGMR_WAIT});  // modem revision
        out_todo.push({"AT#XSMVER\r", State::OK_WAIT});     // modem versions
        out_todo.push({"AT+CGSN=2\r", State::OK_WAIT});     // IMEI

      // cert state (once at startup)
      } else if (cert_state == CertState::UNKNOWN) {
        out_todo.push({"AT%CMNG=1,0,0\r", State::OK_WAIT});  // check cert
        cert_state = CertState::INVALID;  // unless set by %CMNG handler
      } else if (cert_state == CertState::INVALID) {
        out_todo.push({"AT+CFUN=4\r", State::OK_WAIT});      // radio off
        out_todo.push({"AT%CMNG=3,0,0\r", State::OK_WAIT});  // erase slot 0
        out_todo.push({"AT%CMNG=0,0,0,\"", State::READY});   // write slot 0
        out_todo.push({mqtt_server.cert, State::READY});
        out_todo.push({"\"\r", State::OK_WAIT});
        cert_state = CertState::DONE;  // proceed, even if it fails
        do_setup_radio = true;  // turn radio back on after

      // radio setup (after every probe, after cert setup)
      } else if (do_setup_radio) {
        do_setup_radio = false;
        out_todo.push({"AT%XPDNCFG=1\r", State::OK_WAIT});  // always-on IP
        out_todo.push({"AT+CFUN=1\r", State::OK_WAIT});     // enable radio
        out_todo.push({"AT+CEREG=1\r", State::OK_WAIT});    // radio events on
        out_todo.push({"AT+CGEREP=1\r", State::OK_WAIT});   // data events on

      // periodic status polls
      } else if (poll_time >= next_periodic) {
        OK_DETAIL("⏱️ Periodic status check");
        next_periodic = poll_time + 10_s;
        do_poll_radio = do_poll_ip = do_poll_mqtt = true;
      } else if (do_poll_radio) {
        do_poll_radio = false;
        out_todo.push({"AT%XMONITOR\r", State::OK_WAIT});  // net/radio status
      } else if (do_poll_ip) {
        do_poll_ip = false;
        out_todo.push({"AT+CGPADDR\r", State::AT_CGPADDR_WAIT});  // IP status
      } else if (do_poll_mqtt) {
        do_poll_mqtt = false;
        out_todo.push({"AT#XMQTTCON?\r", State::OK_WAIT});  // MQTT status

      // soft radio restart if it doesn't seem to be running
      } else if (poll_time >= next_radio_reset) {
        COUNT_ERROR(radio_resets, "Restarting radio (not registered)");
        out_todo.push({"AT+CFUN=4\r", State::OK_WAIT});  // turn radio off
        next_radio_reset = poll_time + 300_s;  // time for it to work
        do_setup_radio = true;

      // MQTT connection management
      // Note, IP attach can bridge cell registration outage.
      // - require BOTH registered AND ip_attached for connect (AT#XMQTTCON=1)
      // - disconnect if !ip_attached, NOT for !registered by itself
      } else if (mqtt_state == MqttState::NEEDS_DISCONNECT || (
                   mqtt_state >= MqttState::CONNACK_WAIT && (
                     !status.ip_attached || poll_time >= next_mqtt_reset))) {
        if (mqtt_state >= MqttState::CONNACK_WAIT) ++status.mqtt_resets;
        out_todo.push({"AT#XMQTTCON=0\r", State::AT_XMQTT_WAIT});
        mqtt_state = MqttState::NEEDS_CONNECT;
      } else if (mqtt_state == MqttState::NEEDS_CONNECT &&
                 !status.imeisv.empty() && !mqtt_server.host.empty() &&
                 status.registered && status.ip_attached &&
                 poll_time >= mqtt_backoff) {
        out_todo.push({"AT#XMQTTCFG=\"", State::READY});
        out_todo.push({status.imeisv, State::READY});
        out_todo.push({"\",60,1\r", State::AT_XMQTT_WAIT});
        out_todo.push({"AT#XMQTTCON=1,\"", State::READY});
        out_todo.push({mqtt_server.user, State::READY});
        out_todo.push({"\",\"", State::READY});
        out_todo.push({mqtt_server.password, State::READY});
        out_todo.push({"\",\"", State::READY});
        out_todo.push({mqtt_server.host, State::READY});
        out_todo.push({"\",", State::READY});
        etl::to_string(mqtt_server.port, out_scratch);
        out_todo.push({out_scratch, State::READY});
        auto const& end = mqtt_server.cert.empty() ? "\r" : ",0\r";
        out_todo.push({end, State::AT_XMQTTCON_WAIT});
        mqtt_state = MqttState::CONNACK_WAIT;
        mqtt_backoff = poll_time + 30_s;  // rate-limit reconnection attempts
        next_mqtt_reset = poll_time + 120_s;  // time for connection process
      } else if (mqtt_state == MqttState::READY && !mqtt_subs_todo.empty()) {
        out_todo.push({"AT#XMQTTSUB=\"", State::READY});
        out_todo.push({mqtt_subs_todo.front(), State::READY});
        out_todo.push({"\",0\r", State::AT_XMQTT_WAIT});
        mqtt_state = MqttState::SUBACK_WAIT;
      } else if (mqtt_state == MqttState::READY &&
                 !mqtt_outgoing.topic.empty()) {
        out_todo.push({"AT#XMQTTPUB=\"", State::READY});
        out_todo.push({mqtt_outgoing.topic, State::READY});
        out_todo.push({"\",\"\",1,0,", State::READY});
        etl::to_string(mqtt_outgoing.payload.size(), out_scratch);
        out_todo.push({out_scratch, State::READY});
        out_todo.push({"\r", State::AT_XMQTT_WAIT});
        out_todo.push({mqtt_outgoing.payload, State::AT_XMQTTPUB_DATA_WAIT});
        mqtt_state = MqttState::PUBACK_WAIT;
      }
    }

    while (state == State::READY && !out_todo.empty()) {
      auto* const step = &out_todo.front();
      if (!step->logged) {
        OK_DETAIL(
          "▶️ (%db) %s ▶%d",
          step->send.size(), abbr(step->send).c_str(), step->next_state
        );
        step->logged = true;
      }
      for (int av = 0; av || ((av = serial->availableForWrite()) > 0); --av) {
        if (step->send.empty()) break;
        serial->write(step->send.front());
        step->send.remove_prefix(1);
      }
      if (!step->send.empty()) break;
      state = step->next_state;
      last_step = poll_time;
      out_todo.pop();
    }

    if (mqtt_state < MqttState::READY && !mqtt_outgoing.topic.empty()) {
      COUNT_ERROR(mqtt_errors, "Lost: %s", abbr(mqtt_outgoing.topic).c_str());
      mqtt_outgoing = {};
    }

    status.mqtt_ready = (
      mqtt_state >= MqttState::READY &&
      mqtt_subs_todo.empty()
    );
    status.mqtt_publish_busy = !mqtt_outgoing.topic.empty();
    status.mqtt_receive_ready = !mqtt_incoming.topic.empty();
    return &status;
  }

  void publish(MqttMessage pub) override {
    if (!mqtt_outgoing.topic.empty()) {
      COUNT_ERROR(usage_errors, "Pub but busy: %s", abbr(pub.topic).c_str());
    } else if (mqtt_state != MqttState::READY) {
      COUNT_ERROR(usage_errors, "Pub but !ready: %s", abbr(pub.topic).c_str());
    } else if (pub.topic.empty() || pub.topic.size() > 128) {
      COUNT_ERROR(usage_errors, "Bad topic size (%s): %db",
                  abbr(pub.topic).c_str(), pub.topic.size());
    } else if (pub.payload.empty() || pub.payload.size() > 8192) {
      COUNT_ERROR(usage_errors, "Bad payload size (%s): %db",
                  abbr(pub.topic).c_str(), pub.payload.size());
    } else {
      OK_DETAIL("💬 Pub request: %s", abbr(pub.topic).c_str());
      mqtt_outgoing = pub;
      status.mqtt_publish_busy = true;
    }
  }

  MqttMessage receive() override {
    if (mqtt_incoming.topic.empty()) {
      COUNT_ERROR(usage_errors, "Receive while empty");
    }
    status.mqtt_receive_ready = false;
    auto const temp = mqtt_incoming;
    mqtt_incoming = {};
    return temp;
  }

 private:
  // What we're waiting for from the modem (which command is outstanding)
  enum class State {
    READY,
    AT_CGMM_WAIT,
    AT_CGMR_WAIT,
    AT_CGPADDR_WAIT,
    AT_XMQTT_WAIT,
    AT_XMQTTCON_WAIT,
    AT_XMQTTPUB_DATA_WAIT,
    OK_WAIT,
    PROBE_DRAIN,
    PROBE_WAIT,
    RESET_TIME,
  };

  enum class CertState {
    UNKNOWN,
    INVALID,
    DONE
  };

  enum class MqttState {
    NEEDS_DISCONNECT,
    NEEDS_CONNECT,
    CONNACK_WAIT,
    READY,
    SUBACK_WAIT,
    PUBACK_WAIT,
  };

  struct OutputStep {
    etl::string_view send;
    State next_state;
    bool logged = false;
  };

  HardwareSerial* const serial;
  int const enable_pin;
  MqttServerConfig const mqtt_server;
  etl::span<etl::string_view const> const mqtt_subs;
  CellModemStatus status;

  State state = State::READY;
  steady_clock::time_point poll_time = {};
  steady_clock::time_point last_step = {};
  steady_clock::time_point next_periodic = {};
  steady_clock::time_point next_mqtt_reset = {};
  steady_clock::time_point next_radio_reset = {};
  steady_clock::time_point next_hard_reset = {};
  bool do_probe = true;
  bool do_get_version = true;
  bool do_setup_radio = true;
  bool do_poll_radio = true;
  bool do_poll_ip = true;
  bool do_poll_mqtt = true;

  etl::string<2048> in_buf;
  int in_counted_todo = -1;  // -1=linemode, 0=buffered, >0=buffering

  etl::circular_buffer<OutputStep, 16> out_todo;
  etl::string<10> out_scratch;

  CertState cert_state = CertState::UNKNOWN;
  MqttState mqtt_state = MqttState::NEEDS_DISCONNECT;
  steady_clock::time_point mqtt_backoff = {};
  etl::span<etl::string_view const> mqtt_subs_todo;
  MqttMessage mqtt_outgoing = {};
  MqttMessage mqtt_incoming = {};
  int mqtt_in_topic_size = -1;
  int mqtt_in_payload_size = -1;

  // The modem has restarted (or is about to): forget everything we knew
  void reset_session_state() {
    state = State::READY;
    cert_state = CertState::UNKNOWN;
    mqtt_state = MqttState::NEEDS_DISCONNECT;
    status.running = status.registered = status.ip_attached = false;
    in_counted_todo = -1;
    in_buf.clear();
    out_todo.clear();
    do_probe = true;  // implies radio setup, etc.
  }

  void handle_input_line() {
    OK_DETAIL("⬅️ %s", abbr(in_buf).c_str());
    etl::string_view rest(in_buf);

    //
    // Generic fault/reset messages
    //

    if (state == State::RESET_TIME) return;  // Ignore noise in RESET

    // "Ready" is often prefixed with \xFF because... UART init or something?
    if (eat(&rest, "Ready") || eat(&rest, "\xffReady")) {
      if (!status.running) {
        OK_NOTE("📡 Modem init: %s", abbr(in_buf).c_str());
      } else {
        COUNT_ERROR(hard_resets, "Modem restart: %s", abbr(in_buf).c_str());
        status.failed = true;
      }
      reset_session_state();
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      // Modem library fault/shutdown/recovery; MQTT state is unreliable
      // after this, so use the reset ladder rather than trying to be clever
      COUNT_ERROR(modem_errors, "(st=%d) %s", state, abbr(in_buf).c_str());
      status.failed = true;
      reset_session_state();
      next_hard_reset = {};  // reset ASAP
      return;
    }

    if (eat(&rest, "ERROR") || eat(&rest, "+CME") || eat(&rest, "+CMS")) {
      switch (state) {
        case State::READY:
        case State::RESET_TIME:
        case State::PROBE_WAIT:
        case State::PROBE_DRAIN:
          COUNT_ERROR(modem_errors, "(st=%d) %s", state, abbr(in_buf).c_str());
          break;  // eat stale responses, keep going in state
        case State::AT_CGMM_WAIT:
        case State::AT_CGMR_WAIT:
        case State::AT_CGPADDR_WAIT:
        case State::OK_WAIT:
          COUNT_ERROR(modem_errors, "(st=%d) %s", state, abbr(in_buf).c_str());
          out_todo.clear();  // cancel remainder of sequence
          state = State::READY;
          break;
        case State::AT_XMQTT_WAIT:
        case State::AT_XMQTTCON_WAIT:
        case State::AT_XMQTTPUB_DATA_WAIT:  // (shouldn't happen in DATA, but)
          // AT#XMQTTxxx => ERROR means
          // - the session failed (and we hadn't noticed yet)
          // - we were never actually connected (oops?)
          // - we're in the CONNECT<>CONNACK dead zone
          // ...poll MQTT state and attempt a reconnect cycle.
          COUNT_ERROR(mqtt_errors, "(st=%d) %s", state, abbr(in_buf).c_str());
          state = State::READY;
          mqtt_state = MqttState::NEEDS_DISCONNECT;
          out_todo.clear();  // cancel remainder of sequence
          do_poll_mqtt = true;
          break;
      }
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
      int slot;
      etl::string_view a1, a2;
      if (eat_int(&rest, &slot)) {
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
            COUNT_ERROR(serial_errors, "Bad IPv4: %s", abbr(in_buf).c_str());
          }
        }
      }
      return;
    }

    if (eat(&rest, "+CGSN:")) {
      etl::string_view v;
      if (eat_quoted(&rest, &v)) status.imeisv = v.empty() ? "-" : v;
      return;
    }

    if (eat(&rest, "%CMNG:")) {
      int tag, type;
      etl::string_view sha;
      if (eat_int(&rest, &tag) && eat(&rest, ",") &&
          eat_int(&rest, &type) && eat(&rest, ",") &&
          eat_quoted(&rest, &sha) && tag == 0 && type == 0) {
        if (sha == mqtt_server.cert_sha256) {
          cert_state = CertState::DONE;
        } else {
          cert_state = CertState::INVALID;
          COUNT_ERROR(
            mqtt_errors, "Cert mismatch:\n  expect: %.*s\n  actual: %.*s",
            mqtt_server.cert_sha256.size(), mqtt_server.cert_sha256.data(),
            sha.size(), sha.data()
          );
        }
      }
      return;
    }

    if (eat(&rest, "#XDATAMODE:")) {
      int err;
      if (eat_int(&rest, &err)) {
        if (state == State::AT_XMQTTPUB_DATA_WAIT) {
          if (err != 0) {
            COUNT_ERROR(mqtt_errors, "Pub fail: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::NEEDS_DISCONNECT;
          }
          state = State::READY;
        } else {
          COUNT_ERROR(serial_errors, "Unexp #XDATAMODE (st=%d): %s",
                      state, abbr(in_buf).c_str());
        }
      }
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

        // restart the radio (AT+CFUN=4) if 5min elapse without searching
        if (status.running) next_radio_reset = poll_time + 300_s;

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
      return;
    }

    if (eat(&rest, "#XMQTTCON:")) {
      if (eat(&rest, "0")) {
        switch (mqtt_state) {
          case MqttState::NEEDS_DISCONNECT:
            mqtt_state = MqttState::NEEDS_CONNECT;  // already disconnected
            break;
          case MqttState::NEEDS_CONNECT:
            break;  // expected status, carry on
          case MqttState::CONNACK_WAIT:
          case MqttState::READY:
          case MqttState::SUBACK_WAIT:
          case MqttState::PUBACK_WAIT:
            COUNT_ERROR(mqtt_errors, "Reconnecting");
            mqtt_state = MqttState::NEEDS_CONNECT;
            break;
        }
      } else if (eat(&rest, "1")) {
        switch (mqtt_state) {
          case MqttState::NEEDS_DISCONNECT:
            break;  // expected until we disconnect
          case MqttState::NEEDS_CONNECT:
            COUNT_ERROR(mqtt_errors, "Unexp session: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::NEEDS_DISCONNECT;
            break;
          case MqttState::CONNACK_WAIT:
          case MqttState::READY:
          case MqttState::SUBACK_WAIT:
          case MqttState::PUBACK_WAIT:
            etl::string_view id, host;
            int port, sec_tag = -1;
            if (eat(&rest, ",") && eat_quoted(&rest, &id) &&
                eat(&rest, ",") && eat_quoted(&rest, &host) &&
                eat(&rest, ",") && eat_int(&rest, &port) &&
                ((eat(&rest, ",") && eat_int(&rest, &sec_tag)) || true)) {
              int const config_sec = mqtt_server.cert.empty() ? -1 : 0;
              if (host != mqtt_server.host || port != mqtt_server.port ||
                  id != status.imeisv || sec_tag != config_sec) {
                COUNT_ERROR(
                  mqtt_errors,
                  "Bad conn:\n  %.*s:%d[%d] (%.*s) !=\n  %.*s:%d[%d] (%.*s)",
                  host.size(), host.data(), port, sec_tag, id.size(), id.data(),
                  mqtt_server.host.size(), mqtt_server.host.data(),
                  mqtt_server.port, config_sec,
                  status.imeisv.size(), status.imeisv.data()
                );
                mqtt_state = MqttState::NEEDS_DISCONNECT;
              }
            }
            break;
        }
      } else {
        COUNT_ERROR(serial_errors, "Bad status: %s", abbr(in_buf).c_str());
      }
      return;
    }

    if (eat(&rest, "#XMQTTEVT:")) {
      int type, err;
      if (eat_int(&rest, &type) && eat(&rest, ",") && eat_int(&rest, &err)) {
        if (type == 0) {  // CONNACK (or connection failed)
          if (err != 0) {
            // Connection failed - this could be stale; trigger a poll
            COUNT_ERROR(mqtt_errors, "Connect fail: %s", abbr(in_buf).c_str());
            if (mqtt_state >= MqttState::CONNACK_WAIT) do_poll_mqtt = true;
          } else if (mqtt_state == MqttState::CONNACK_WAIT) {
            OK_DETAIL("💬 MQTT connected");
            mqtt_state = MqttState::READY;
            mqtt_subs_todo = mqtt_subs;
          } else {
            COUNT_ERROR(mqtt_errors, "Unexp CONNACK: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::NEEDS_DISCONNECT;
          }
        } else if (type == 1) {  // DISCONNECT
          if (mqtt_state >= MqttState::CONNACK_WAIT) {
            // This could be stale; trigger a poll
            COUNT_ERROR(mqtt_errors, "Disconnected: %s", abbr(in_buf).c_str());
            do_poll_mqtt = true;
          }
        } else if (type == 3) {  // PUBACK
          if (mqtt_state != MqttState::PUBACK_WAIT) {
            COUNT_ERROR(mqtt_errors, "Unexp PUBACK: %s", abbr(in_buf).c_str());
          } else if (err != 0) {
            COUNT_ERROR(mqtt_errors, "Pub fail: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::NEEDS_DISCONNECT;  // reconnect
          } else {
            mqtt_state = MqttState::READY;
            mqtt_outgoing = {};  // Message confirmed
          }
        } else if (type == 7) {  // SUBACK
          if (mqtt_state != MqttState::SUBACK_WAIT) {
            COUNT_ERROR(mqtt_errors, "Unexp SUBACK: %s", abbr(in_buf).c_str());
            ++status.mqtt_errors;
          } else if (err != 0) {
            COUNT_ERROR(mqtt_errors, "Sub fail: %s", abbr(in_buf).c_str());
            ++status.mqtt_errors;
            mqtt_state = MqttState::NEEDS_DISCONNECT;  // reconnect, retry
          } else {
            mqtt_state = MqttState::READY;
            mqtt_subs_todo = mqtt_subs_todo.subspan(1);
            if (mqtt_subs_todo.empty()) {
              OK_DETAIL("💬 MQTT ready (%d subs)", mqtt_subs.size());
            }
          }
        }

        // MQTT ACKs (incl. PINGRESP) postpone reset *if* MQTT returns to READY
        if (type != 1 && err == 0 && mqtt_state == MqttState::READY) {
          next_hard_reset = poll_time + 1800_s;
          next_mqtt_reset = poll_time + 90_s;  // Keepalive is 60s
        }
      }
      return;
    }

    if (eat(&rest, "#XMQTTMSG:")) {
      int topic_size, payload_size;
      if (eat_int(&rest, &topic_size) && eat(&rest, ",") &&
          eat_int(&rest, &payload_size)) {
        if (topic_size < 1 || payload_size < 1) {
          COUNT_ERROR(mqtt_errors, "Bad sizes: %s", abbr(in_buf).c_str());
        } else {
          // (line parser takes the CR) LF + topic + CR LF + payload + CR LF
          mqtt_in_topic_size = topic_size;
          mqtt_in_payload_size = payload_size;
          in_counted_todo = 1 + topic_size + 2 + payload_size + 2;
          if (in_counted_todo >= 65536) {
            COUNT_ERROR(mqtt_errors, "Resetting to escape drowning: %s",
                        abbr(in_buf).c_str());
            next_hard_reset = {};  // reset ASAP
          }
        }
      }
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
      return;
    }

    if (rest.starts_with("+") || rest.starts_with("#")) {
      COUNT_ERROR(serial_errors, "Unexp notify (st=%d): %s",
                  state, abbr(in_buf).c_str());
      return;
    }

    //
    // "OK" success interpreted by .state
    //

    if (eat(&rest, "OK")) {
      if (!eat(&rest, "")) {
        COUNT_ERROR(serial_errors, "Bad OK: %s", abbr(in_buf).c_str());
      }
      switch (state) {
        case State::READY:
        case State::RESET_TIME:
        case State::PROBE_DRAIN:
        case State::AT_XMQTTPUB_DATA_WAIT:
          COUNT_ERROR(serial_errors, "Unexp OK (st=%d)", state);
          break;
        case State::AT_CGPADDR_WAIT:
          status.ip_attached = false;  // +CGPADDR completed, no IPs found
          status.ip_addr = 0;
          state = State::READY;
          break;
        case State::AT_CGMM_WAIT:
        case State::AT_CGMR_WAIT:
        case State::AT_XMQTT_WAIT:
        case State::AT_XMQTTCON_WAIT:
        case State::OK_WAIT:
          state = State::READY;
          break;
        case State::PROBE_WAIT:
          last_step = poll_time;  // start the PROBE_DRAIN timer now
          state = State::PROBE_DRAIN;
          break;
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

    COUNT_ERROR(serial_errors, "Unexp data (st=%d): %s",
                state, abbr(in_buf).c_str());
  }

  void handle_counted_input() {
    int const t_begin = 1, t_end = t_begin + mqtt_in_topic_size;
    int const p_begin = t_end + 2, p_end = p_begin + mqtt_in_payload_size;
    if (p_end + 2 > in_buf.max_size()) {
      COUNT_ERROR(mqtt_errors, "MQTT message dropped (%db > %db max): %s",
                  p_end + 2, in_buf.max_size(), abbr(in_buf).c_str());
      ++status.mqtt_errors;
      return;
    }

    etl::string_view const in_view(in_buf);
    auto const header_lf = in_view.substr(0, t_begin);
    auto const topic_crlf = in_view.substr(t_end, 2);
    auto const payload_crlf = in_view.substr(p_end, 2);
    if (header_lf != "\n" || topic_crlf != "\r\n" || payload_crlf != "\r\n") {
      COUNT_ERROR(serial_errors, "Bad MQTT framing: [%s] [%s] [%s]",
                  abbr(header_lf).c_str(), abbr(topic_crlf).c_str(),
                  abbr(payload_crlf).c_str());
      return;
    }

    mqtt_incoming.topic = in_view.substr(t_begin, mqtt_in_topic_size);
    mqtt_incoming.payload = in_view.substr(p_begin, mqtt_in_payload_size);
    OK_DETAIL(
      "💬 Incoming (%db): %s",
      mqtt_incoming.payload.size(), abbr(mqtt_incoming.topic).c_str()
    );
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
