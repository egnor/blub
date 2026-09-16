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

    if (server.cert.empty()) cert_state = CertState::DONE;  // no cert
  }

  CellModemStatus const& poll() override {
    auto const new_time = steady_clock::now();
    auto const elapsed = new_time - poll_time;
    if (elapsed > 50_ms && poll_time > steady_clock::time_point{}) {
      OK_ERROR("Slow poll (%.3fs)", raw_count<secd>(elapsed));
    }
    poll_time = new_time;

    if (!mqtt_incoming.topic.empty()) {
      OK_ERROR("Unhandled MQTT message: %s", abbr(mqtt_incoming.topic).c_str());
      mqtt_incoming = {};  // Depends on counted buffer which will be lost
    }

    // Hard modem reset at startup, on request, or if MQTT fails to thrive
    if (poll_time >= next_hard_reset && enable_pin >= 0) {
      next_hard_reset = poll_time + 1800_s;
      reset_session_state();
      OK_NOTE("📴 Resetting modem (p%d LOW)", enable_pin);
      pinMode(enable_pin, OUTPUT);
      digitalWrite(enable_pin, LOW);
      last_serial_traffic = poll_time;
      state = State::RESET_WAIT;
    }

    //
    // Read serial input
    //

    if (in_counted_todo == 0) {
      in_buf.clear();  // Counted buffer only remains for one poll cycle
      in_counted_todo = -1;
    }

    for (int av = 0; av || ((av = serial->available()) > 0); --av) {
      last_serial_traffic = poll_time;
      int const ch = serial->read();
      if (ch < 0) {
        OK_ERROR("Serial read error: avail=%d ch=%d", av, ch);
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

    auto const quiet = poll_time - last_serial_traffic;
    if (in_counted_todo > 0) {
      if (quiet > 60_s) {
        OK_ERROR("Data timeout: %.1fs > 60s", raw_count<secd>(quiet));
        in_counted_todo = -1;
        in_buf.clear();
        out_bufs.clear();
        do_probe = true;
        state = State::IDLE;  // any pending command was surely lost
      }
    } else if (state == State::RESET_WAIT) {
      if (quiet >= 100_ms) {
        OK_NOTE("🔛 Ending modem reset (p%d PULLUP)", enable_pin);
        pinMode(enable_pin, INPUT_PULLUP);
        digitalWrite(enable_pin, HIGH);
        last_serial_traffic = poll_time;
        state = State::IDLE;
      }
    } else if (state == State::PROBE_DRAIN) {
      if (quiet >= 100_ms) {
        OK_DETAIL("✅️ Modem probe complete");
        setup_step = 0;  // (re)send setup commands
        next_periodic = {};  // then poll status right away
        next_soft_reset = poll_time + 60_s;  // give the radio a chance
        state = State::IDLE;
      }
    } else if (state != State::IDLE) {
      auto const allow = (state == State::AT_XMQTTCON_WAIT) ? 60_s : 5_s;
      if (quiet >= allow) {
        OK_ERROR(
          "Command timeout (state=%d): %.1f > %.1fs", state,
          raw_count<secd>(quiet), raw_count<secd>(allow)
        );
        state = State::IDLE;
        out_bufs.clear();
        do_probe = true;
      }
    }

    //
    // Command sending (transitions from idle)
    //

    if (state == State::IDLE && in_counted_todo < 0 && out_bufs.empty()) {
      // command probe (at startup and whenever we lose track of the modem)
      if (do_probe) {
        out_bufs.push("\r+++\"\rAT\r");  // unstick modem parser, ask for OK
        state = State::PROBE_WAIT;  // wait for OK, then drain buffers
        do_probe = false;

      // hardware ID (after every probe)
      } else if (setup_step >= 0 && setup_step < SETUP_ID_STEPS) {
        send_setup_step();

      // cert state (once at startup)
      } else if (cert_state == CertState::UNKNOWN) {
        out_bufs.push("AT%CMNG=1,0,0\r");  // 1=check slot=0 type=0=root
        state = State::OK_WAIT;
        cert_state = CertState::INVALID;  // unless the response says otherwise
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
        cert_state = CertState::DONE;

      // radio setup (after every probe, after cert setup)
      } else if (setup_step >= 0) {
        send_setup_step();

      // periodic status polls
      } else if (poll_time >= next_periodic) {
        next_periodic = poll_time + 10_s;
        do_poll_radio = do_poll_ip = do_poll_mqtt = true;
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
        OK_NOTE("📡 Restarting radio (not registered)");
        out_bufs.push("AT+CFUN=4\r");  // turn radio off
        state = State::OK_WAIT;
        next_soft_reset = poll_time + 300_s;  // time for it to work
        setup_step = SETUP_ID_STEPS;  // redo radio setup (turns it back on)

      // MQTT connection management
      } else if (mqtt_state == MqttState::OK_TO_DISCONNECT || (
                   mqtt_state >= MqttState::CONNACK_WAIT && (
                     !status.ip_attached || poll_time >= next_mqtt_reset))) {
        out_bufs.push("AT#XMQTTCON=0\r");
        state = State::AT_XMQTT_WAIT;
        mqtt_state = MqttState::OK_TO_CONFIG;
      } else if (mqtt_state == MqttState::OK_TO_CONFIG &&
                 !status.imeisv.empty()) {
        out_bufs.push("AT#XMQTTCFG=\"");
        out_bufs.push(status.imeisv);
        out_bufs.push("\",60,1\r");
        state = State::AT_XMQTT_WAIT;
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
        out_secret = true;  // don't log the password
        state = State::AT_XMQTTCON_WAIT;
        mqtt_state = MqttState::CONNACK_WAIT;
        mqtt_backoff = poll_time + 30_s;  // rate-limit reconnection attempts
        next_mqtt_reset = poll_time + 120_s;  // time for connection process
      } else if (mqtt_state == MqttState::READY && !mqtt_subs_todo.empty()) {
        out_bufs.push("AT#XMQTTSUB=\"");
        out_bufs.push(mqtt_subs_todo.front());
        out_bufs.push("\",0\r");
        state = State::AT_XMQTT_WAIT;
        mqtt_state = MqttState::SUBACK_WAIT;
      } else if (mqtt_state == MqttState::READY &&
                 !mqtt_outgoing.topic.empty()) {
        out_bufs.push("AT#XMQTTPUB=\"");
        out_bufs.push(mqtt_outgoing.topic);
        out_bufs.push("\",\"\",1,0,");
        etl::to_string(mqtt_outgoing.payload.size(), out_scratch);
        out_bufs.push(out_scratch);
        out_bufs.push("\r");
        state = State::AT_XMQTTPUB_WAIT;
        mqtt_state = MqttState::PUBACK_WAIT;
      }

      if (!out_bufs.empty()) {
        if (out_secret) {
          OK_DETAIL("▶️ %s[...]", abbr(out_bufs.front()).c_str());
        } else {
          OK_DETAIL("▶️ %s", abbr(out_bufs).c_str());
        }
      }
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
    if (out_bufs.empty()) out_secret = false;

    if (mqtt_state < MqttState::READY && !mqtt_outgoing.topic.empty()) {
      OK_ERROR("MQTT publish lost: %s", abbr(mqtt_outgoing.topic).c_str());
      mqtt_outgoing = {};
    }

    status.mqtt_ready = (
      mqtt_state >= MqttState::READY &&
      mqtt_subs_todo.empty()
    );
    status.mqtt_publish_busy = !mqtt_outgoing.topic.empty();
    status.mqtt_receive_ready = !mqtt_incoming.topic.empty();
    return status;
  }

  void publish(MqttMessage pub) override {
    if (!mqtt_outgoing.topic.empty()) {
      OK_ERROR("MQTT publish while busy: %s", abbr(pub.topic).c_str());
    } else if (mqtt_state != MqttState::READY) {
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
  // What we're waiting for from the modem (which command is outstanding)
  enum class State {
    IDLE,
    RESET_WAIT,
    PROBE_WAIT,
    PROBE_DRAIN,
    AT_CGMM_WAIT,
    AT_CGMR_WAIT,
    AT_CGPADDR_WAIT,
    AT_XMQTT_WAIT,
    AT_XMQTTCON_WAIT,
    AT_XMQTTPUB_WAIT,
    AT_XMQTTPUB_DATA,
    OK_WAIT,
  };

  enum class CertState {
    UNKNOWN,
    INVALID,
    OK_TO_ERASE,
    OK_TO_WRITE,
    DONE
  };

  // What the MQTT session needs next. CONNACK_WAIT doubles as "session in
  // doubt": wait for the modem's events, an #XMQTTCON? poll, or a timeout
  enum class MqttState {
    OK_TO_DISCONNECT,
    OK_TO_CONFIG,
    OK_TO_CONNECT,
    CONNACK_WAIT,
    READY,
    SUBACK_WAIT,
    PUBACK_WAIT,
  };

  // Sent in order after every probe (cert setup happens after the ID steps)
  struct SetupStep { etl::string_view command; State wait; };
  static constexpr int SETUP_ID_STEPS = 4;
  static constexpr auto SETUP_STEPS = etl::make_array<SetupStep>(
    SetupStep{"AT+CGMM\r", State::AT_CGMM_WAIT},   // modem model
    SetupStep{"AT+CGMR\r", State::AT_CGMR_WAIT},   // modem revision
    SetupStep{"AT#XSMVER\r", State::OK_WAIT},      // modem versions
    SetupStep{"AT+CGSN=2\r", State::OK_WAIT},      // get IMEI
    SetupStep{"AT%XPDNCFG=1\r", State::OK_WAIT},   // always-on packet network
    SetupStep{"AT+CFUN=1\r", State::OK_WAIT},      // enable radio
    SetupStep{"AT+CEREG=1\r", State::OK_WAIT},     // radio events (after CFUN)
    SetupStep{"AT+CGEREP=1\r", State::OK_WAIT}     // data events (after CFUN)
  );

  HardwareSerial* const serial;
  int const enable_pin;
  MqttServerConfig const mqtt_server;
  etl::span<etl::string_view const> const mqtt_subs;
  CellModemStatus status;

  State state = State::IDLE;
  steady_clock::time_point poll_time = {};
  steady_clock::time_point last_serial_traffic = {};
  steady_clock::time_point next_periodic = {};
  steady_clock::time_point next_mqtt_reset = {};
  steady_clock::time_point next_soft_reset = {};
  steady_clock::time_point next_hard_reset = {};
  int setup_step = -1;  // index into SETUP_STEPS, or -1 if done
  bool do_probe = true;
  bool do_poll_radio = true;
  bool do_poll_ip = true;
  bool do_poll_mqtt = true;

  etl::string<2048> in_buf;
  int in_counted_todo = -1;  // -1=linemode, 0=buffered, >0=buffering

  etl::circular_buffer<etl::string_view, 16> out_bufs;
  etl::string<10> out_scratch;
  bool out_secret = false;

  CertState cert_state = CertState::UNKNOWN;
  MqttState mqtt_state = MqttState::OK_TO_DISCONNECT;
  steady_clock::time_point mqtt_backoff = {};
  etl::span<etl::string_view const> mqtt_subs_todo;
  MqttMessage mqtt_outgoing = {};
  MqttMessage mqtt_incoming = {};
  int mqtt_in_topic_size = -1;
  int mqtt_in_payload_size = -1;

  void send_setup_step() {
    auto const& step = SETUP_STEPS[setup_step];
    out_bufs.push(step.command);
    state = step.wait;
    if (++setup_step >= SETUP_STEPS.size()) setup_step = -1;
  }

  // The modem has restarted (or is about to): forget everything we knew
  void reset_session_state() {
    state = State::IDLE;
    status.running = status.registered = status.ip_attached = false;
    in_counted_todo = -1;
    in_buf.clear();
    out_bufs.clear();
    setup_step = -1;
    do_probe = true;  // probe for liveness, then redo setup
    do_poll_radio = do_poll_ip = do_poll_mqtt = true;
    mqtt_state = MqttState::OK_TO_DISCONNECT;
  }

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
      reset_session_state();
      return;
    }

    if (eat(&rest, "#XMODEM:") || eat(&rest, "INIT ERROR")) {
      // Modem library fault/shutdown/recovery; MQTT state is unreliable
      // after this, so use the reset ladder rather than trying to be clever
      OK_ERROR("Modem fault (state=%d): %s", state, abbr(in_buf).c_str());
      status.failed = true;
      reset_session_state();
      next_hard_reset = {};  // reset ASAP
      return;
    }

    if (eat(&rest, "ERROR") || eat(&rest, "+CME") || eat(&rest, "+CMS")) {
      OK_ERROR("Modem error (state=%d): %s", state, abbr(in_buf).c_str());
      switch (state) {
        case State::IDLE:
        case State::RESET_WAIT:
        case State::PROBE_WAIT:  // eat stale responses until OK
        case State::PROBE_DRAIN:  // eat stale responses until quiet
          break;
        case State::AT_CGMM_WAIT:
        case State::AT_CGMR_WAIT:
        case State::AT_CGPADDR_WAIT:
        case State::OK_WAIT:
          state = State::IDLE;  // failure value already set
          break;
        case State::AT_XMQTT_WAIT:
        case State::AT_XMQTTCON_WAIT:
        case State::AT_XMQTTPUB_WAIT:
        case State::AT_XMQTTPUB_DATA:  // (shouldn't happen in DATA, but ???)
          // AT#XMQTTxxx => ERROR means
          // - the session failed (and we hadn't noticed yet)
          // - we were never actually connected (oops?)
          // - we're in the CONNECT<>CONNACK dead zone
          // ...poll MQTT state and attempt a reconnect cycle.
          state = State::IDLE;
          mqtt_state = MqttState::OK_TO_DISCONNECT;
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
          cert_state = CertState::DONE;
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
            mqtt_state = MqttState::OK_TO_DISCONNECT;  // reconnect
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
        if (reg != 0) next_soft_reset = poll_time + 60_s;  // push forward
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
      if (eat(&rest, "0")) {
        switch (mqtt_state) {
          case MqttState::OK_TO_DISCONNECT:
            mqtt_state = MqttState::OK_TO_CONFIG;  // already disconnected
            break;
          case MqttState::OK_TO_CONFIG:
          case MqttState::OK_TO_CONNECT:
            break;  // expected status, carry on
          case MqttState::CONNACK_WAIT:
          case MqttState::READY:
          case MqttState::SUBACK_WAIT:
          case MqttState::PUBACK_WAIT:
            OK_NOTE("MQTT not connected, reconnecting");
            mqtt_state = MqttState::OK_TO_CONFIG;
            break;
        }
      } else if (eat(&rest, "1")) {
        switch (mqtt_state) {
          case MqttState::OK_TO_DISCONNECT:
            break;  // expected until we disconnect
          case MqttState::OK_TO_CONFIG:
          case MqttState::OK_TO_CONNECT:
            OK_ERROR("Unexpected MQTT session: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::OK_TO_DISCONNECT;
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
                OK_ERROR(
                  "Bad MQTT:\n  %.*s:%d[%d] (%.*s) !=\n  %.*s:%d[%d] (%.*s)",
                  host.size(), host.data(), port, sec_tag, id.size(), id.data(),
                  mqtt_server.host.size(), mqtt_server.host.data(),
                  mqtt_server.port, config_sec,
                  status.imeisv.size(), status.imeisv.data()
                );
                mqtt_state = MqttState::OK_TO_DISCONNECT;
              }
            }
            break;
        }
      } else {
        OK_ERROR("Bad input: %s", abbr(in_buf).c_str());
      }
      return;
    }

    if (eat(&rest, "#XMQTTEVT:")) {
      int type, err;
      if (eat_int(&rest, &type) && eat(&rest, ",") && eat_int(&rest, &err)) {
        // MQTT progress (incl. PINGRESP) postpones resets
        if (type != 1 && err == 0) {
          next_hard_reset = poll_time + 1800_s;
          next_mqtt_reset = poll_time + 90_s;
        }

        if (type == 0) {  // CONNACK (or connection failed)
          if (err != 0) {
            // Connection failed - this could be stale; trigger a poll
            OK_ERROR("MQTT connect failed: %s", abbr(in_buf).c_str());
            if (mqtt_state >= MqttState::CONNACK_WAIT) do_poll_mqtt = true;
          } else if (mqtt_state == MqttState::CONNACK_WAIT) {
            OK_DETAIL("💬 MQTT connected");
            mqtt_state = MqttState::READY;
            mqtt_subs_todo = mqtt_subs;
          } else {
            OK_ERROR("Unexpected MQTT CONNACK: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::OK_TO_DISCONNECT;
          }
        } else if (type == 1) {  // DISCONNECT
          if (mqtt_state >= MqttState::CONNACK_WAIT) {
            // This could be stale; trigger a poll
            OK_NOTE("MQTT disconnected: %s", abbr(in_buf).c_str());
            do_poll_mqtt = true;
          }
        } else if (type == 3) {  // PUBACK
          if (mqtt_state != MqttState::PUBACK_WAIT) {
            OK_ERROR("Unexpected MQTT PUBACK: %s", abbr(in_buf).c_str());
          } else if (err != 0) {
            OK_ERROR("MQTT publish failed: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::OK_TO_DISCONNECT;  // reconnect
          } else {
            mqtt_state = MqttState::READY;
            mqtt_outgoing = {};  // Message confirmed
          }
        } else if (type == 7) {  // SUBACK
          if (mqtt_state != MqttState::SUBACK_WAIT) {
            OK_ERROR("Unexpected MQTT SUBACK: %s", abbr(in_buf).c_str());
          } else if (err != 0) {
            OK_ERROR("MQTT subscribe failed: %s", abbr(in_buf).c_str());
            mqtt_state = MqttState::OK_TO_DISCONNECT;  // reconnect, retry
          } else {
            mqtt_state = MqttState::READY;
            mqtt_subs_todo = mqtt_subs_todo.subspan(1);
            if (mqtt_subs_todo.empty()) {
              OK_DETAIL("💬 MQTT ready (%d subs)", mqtt_subs.size());
            }
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
          in_counted_todo = 1 + topic_size + 2 + payload_size + 2;
          if (in_counted_todo >= 65536) {
            OK_ERROR("Resetting to escape drowning: %s", abbr(in_buf).c_str());
            next_hard_reset = {};  // reset ASAP
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
      switch (state) {
        case State::PROBE_WAIT:
          state = State::PROBE_DRAIN;
          break;
        case State::AT_XMQTTPUB_WAIT:
          out_bufs.push(mqtt_outgoing.payload);
          OK_DETAIL("⏩️ %s", abbr(out_bufs).c_str());
          state = State::AT_XMQTTPUB_DATA;
          break;
        case State::AT_CGPADDR_WAIT:
          status.ip_attached = false;  // +CGPADDR completed, no IPs found
          status.ip_addr = 0;
          state = State::IDLE;
          break;
        case State::AT_CGMM_WAIT:
        case State::AT_CGMR_WAIT:
        case State::AT_XMQTT_WAIT:
        case State::AT_XMQTTCON_WAIT:
        case State::OK_WAIT:
          state = State::IDLE;
          break;
        case State::RESET_WAIT:
        case State::PROBE_DRAIN:
        case State::IDLE:
        case State::AT_XMQTTPUB_DATA:
          OK_ERROR("Unexpected OK (state=%d): %s", state, abbr(in_buf).c_str());
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

    OK_ERROR("Unexpected data (state=%d): %s", state, abbr(in_buf).c_str());
  }

  void handle_counted_input() {
    int const t_begin = 1, t_end = t_begin + mqtt_in_topic_size;
    int const p_begin = t_end + 2, p_end = p_begin + mqtt_in_payload_size;
    if (p_end + 2 > in_buf.max_size()) {
      OK_ERROR(
        "MQTT message dropped (%db > %db max): %s", p_end + 2,
        in_buf.max_size(), abbr(in_buf).c_str()
      );
      return;
    }

    etl::string_view const in_view(in_buf);
    auto const header_lf = in_view.substr(0, t_begin);
    auto const topic_crlf = in_view.substr(t_end, 2);
    auto const payload_crlf = in_view.substr(p_end, 2);
    if (header_lf != "\n" || topic_crlf != "\r\n" || payload_crlf != "\r\n") {
      OK_ERROR(
        "Bad MQTT framing: [%s] [%s] [%s]",
        abbr(header_lf).c_str(), abbr(topic_crlf).c_str(),
        abbr(payload_crlf).c_str()
      );
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
