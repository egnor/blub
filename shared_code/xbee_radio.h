// Put a Digi XBee into API mode and communicate with it

#pragma once

#include <stdint.h>

namespace arduino { class HardwareSerial; }

class XBeeRadio {
 public:
  //
  // API frame types (both for sending and receiving)
  //

  static constexpr int MAX_API_PAYLOAD = 1600;  // Can hold max packet size

  struct __attribute__((packed)) ATCommand {
    static constexpr int TYPE = 0x08;
    uint8_t frame_id;
    char command[2];
    uint8_t value[0];  // Packet-length determined if present
  };

  struct __attribute__((packed)) ATCommandQueue {
    static constexpr int TYPE = 0x09;
    uint8_t frame_id;
    char command[2];
    uint8_t value[0];  // Packet-length determined if present
  };

  struct __attribute__((packed)) ATCommandResponse {
    static constexpr int TYPE = 0x88;
    enum Status : uint8_t { OK = 0, ERROR, BAD_COMMAND = 2, BAD_PARAM = 3 };

    uint8_t frame_id;
    char command[2];
    Status status;
    uint8_t value[0];  // Packet-length determined if present
    char const* status_text() const;
  };

  struct __attribute__((packed)) TransmitSMS {
    static constexpr int TYPE = 0x1F;
    uint8_t frame_id;
    uint8_t reserved;
    char phone_number[20];  // NUL-terminated, numbers and + only
    char message[0];        // Packet-length terminated
  };

  struct __attribute__((packed)) ReceiveSMS {
    static constexpr int TYPE = 0x9F;
    char phone_number[20];  // NUL-padded, numbers and + only
    char message[0];        // Packet-length terminated
  };

  struct __attribute__((packed)) TransmitIP {
    static constexpr int TYPE = 0x20;
    enum Protocol : uint8_t { UDP = 0, TCP, TLS = 4 };
    enum Option : uint8_t { NO_OPTIONS = 0, CLOSE_SOCKET = 2 };

    uint8_t frame_id;
    uint32_t dest_ip;
    uint16_t dest_port;
    uint16_t source_port;
    Protocol protocol = UDP;
    Option options = NO_OPTIONS;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) TransmitTLS {
    static constexpr int TYPE = 0x23;
    using Option = TransmitIP::Option;

    uint8_t frame_id;
    uint32_t dest_ip;
    uint16_t dest_port;
    uint16_t source_port;
    uint8_t tls_profile;
    uint8_t options = Option::NO_OPTIONS;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) ReceiveIP {
    static constexpr int TYPE = 0xB0;
    using Protocol = TransmitIP::Protocol;

    uint32_t source_ip;
    uint16_t dest_port;
    uint16_t source_port;
    uint8_t protocol;
    uint8_t reserved;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) TransmitStatus {
    static constexpr int TYPE = 0x89;
    uint8_t frame_id;
    uint8_t status;
    char const* status_text() const;
  };

  struct __attribute__((packed)) ModemStatus {
    static constexpr int TYPE = 0x8A;
    enum Status : uint8_t {
      POWER_UP = 0, WATCHDOG_RESET, REGISTERED = 2, UNREGISTERED = 3,
      MANAGER_CONNECTED = 0x0E, MANAGER_DISCONNECTED = 0x0F,
      UPDATE_STARTED = 0x38, UPDATE_FAILED = 0x39, UPDATE_APPLYING = 0x3A,
    };

    Status status;
    char const* status_text() const;
  };

  struct __attribute__((packed)) RelayToInterface {
    static constexpr int TYPE = 0x2D;
    enum Interface : uint8_t { SERIAL_PORT = 0, BLE, PYTHON = 2 };

    uint8_t frame_id;
    Interface interface = SERIAL_PORT;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) RelayFromInterface {
    static constexpr int TYPE = 0xAD;
    using Interface = RelayToInterface::Interface;

    Interface interface = Interface::SERIAL_PORT;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) FirmwareUpdate {
    static constexpr int TYPE = 0x2B;
    enum Flags : uint8_t { NO_FLAGS = 0, INITIAL, FINAL = 2, CANCEL = 4 };

    uint8_t id;
    uint8_t component;
    uint8_t flags = NO_FLAGS;
    uint8_t payload[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) FirmwareUpdateResponse {
    static constexpr int TYPE = 0xAB;
    enum Status : uint8_t {
      OK = 0, IN_PROGRESS = 2, NOT_STARTED = 3,
      SEQUENCE_ERROR = 4, INTERNAL_ERROR = 5, RESOURCE_ERROR = 6,
    };

    uint8_t id;
    uint8_t status;
    char const* status_text() const;
  };

  struct __attribute__((packed)) GnssRequest {
    static constexpr int TYPE = 0x3D;
    enum Command : uint8_t {
      START_ONESHOT = 0, STOP_ONESHOT = 4, START_NMEA = 5, STOP_NMEA = 6
    };

    uint8_t frame_id;
    Command command = START_ONESHOT;
    uint16_t oneshot_timeout;
  };

  struct __attribute__((packed)) GnssResponse {
    static constexpr int TYPE = 0xBD;
    using Command = GnssRequest::Command;
    enum Status : uint8_t { OK = 0, ERROR };

    uint8_t frame_id;
    Command command;
    Status status;
  };

  struct __attribute__((packed)) GnssNmea {
    static constexpr int TYPE = 0xBE;
    uint8_t nmea[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) GnssOneShot {
    static constexpr int TYPE = 0xBF;
    enum Status : uint8_t { OK = 0, INVALID, TIMEOUT = 2, CANCELLED = 3 };

    Status status;
    uint32_t lock_time;
    uint32_t longitude;
    uint32_t altitude;
    uint8_t satellites;
    char const* status_text() const;
  };

  // TODO: BLE Unlock
  // TODO: Socket commands
  // TODO: Filesystem commands

  struct OutgoingFrame {
    int type;
    union {
      ATCommand at_command;
      ATCommandQueue at_command_queue;
      TransmitSMS transmit_sms;
      TransmitIP transmit_ip;
      TransmitTLS transmit_tls;
      RelayToInterface relay_to_interface;
      FirmwareUpdate firmware_update;
      GnssRequest gnss_request;
      uint8_t raw_bytes[MAX_API_PAYLOAD];
    } payload;
    int extra_size;  // Size (in bytes) any array at the tail of the payload
  };

  struct IncomingFrame {
    int type;
    union {
      ATCommandResponse at_command_response;
      ReceiveSMS receive_sms;
      ReceiveIP receive_ip;
      TransmitStatus transmit_status;
      ModemStatus modem_status;
      RelayFromInterface relay_from_interface;
      FirmwareUpdateResponse firmware_update_response;
      GnssResponse gnss_response;
      GnssNmea gnss_nmea;
      GnssOneShot gnss_one_shot;
      uint8_t raw_bytes[MAX_API_PAYLOAD];
    } payload;
    int extra_size;  // Size (in bytes) any array at the tail of the payload
  };

  virtual ~XBeeRadio() = default;
  virtual bool poll_api(IncomingFrame* out) = 0;
  virtual void send_api_frame(OutgoingFrame const&) = 0;
  virtual int send_space_available() const = 0;
  virtual arduino::HardwareSerial* raw_serial() const = 0;
};

XBeeRadio* make_xbee_radio(arduino::HardwareSerial*);
