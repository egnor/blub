// Defined frames for communication with Digi XBee radios in API mode.

#pragma once

#include <stdint.h>

#include "endian_types.h"

namespace XBeeAPI {
  static constexpr int MAX_PAYLOAD = 1536;  // Big enough for 1500b packet

  // Generic, untyped frame with storage for the largest possible payload
  struct Frame {
    int type = -1;
    int payload_size = 0;
    uint8_t payload[MAX_PAYLOAD];

    template <typename PT>
    PT const* decode_as(int* extra_size = nullptr) const {
      if (type != PT::TYPE || payload_size < sizeof(PT)) return nullptr;
      if (extra_size) *extra_size = payload_size - sizeof(PT);
      return reinterpret_cast<PT const*>(payload);
    }

    template <typename PT>
    PT* setup_as(int extra_size = 0) {
      type = PT::TYPE;
      payload_size = sizeof(PT) + extra_size;
      auto* out = reinterpret_cast<PT*>(payload);
      return new (out) PT{};
    }

    int wire_size() const { return payload_size + 5; }
  };

  template <typename PT>
  static constexpr int wire_size_of(int extra_size = 0) {
    PT::TYPE;  // Ensure PT is a frame payload type
    return sizeof(PT) + extra_size + 5;
  }

  //
  // Specific frame payload types
  //

  struct __attribute__((packed)) ATCommand {
    static constexpr int TYPE = 0x08;
    uint8_t frame_id = 1;
    char command[2] = {};
    uint8_t data[0];  // Packet-length determined if present
  };

  struct __attribute__((packed)) ATCommandQueue {
    static constexpr int TYPE = 0x09;
    uint8_t frame_id = 1;
    char command[2] = {};
    uint8_t data[0];  // Packet-length determined if present
  };

  struct __attribute__((packed)) ATCommandResponse {
    enum Status : uint8_t { OK = 0, ERROR, BAD_COMMAND = 2, BAD_PARAM = 3 };

    static constexpr int TYPE = 0x88;
    uint8_t frame_id;
    char command[2];
    Status status;
    uint8_t data[0];  // Packet-length determined if present

    char const* status_text() const;
  };

  struct __attribute__((packed)) TransmitSMS {
    static constexpr int TYPE = 0x1F;
    uint8_t frame_id = 1;
    uint8_t reserved = 0;
    char phone_number[20] = {};  // NUL-terminated, numbers and + only
    char message[0];             // Packet-length terminated
  };

  struct __attribute__((packed)) ReceiveSMS {
    static constexpr int TYPE = 0x9F;
    char phone_number[20];  // NUL-padded, numbers and + only
    char message[0];        // Packet-length terminated
  };

  struct __attribute__((packed)) TransmitIP {
    enum Protocol : uint8_t { UDP = 0, TCP, TLS = 4 };
    enum Option : uint8_t { NO_OPTIONS = 0, CLOSE_SOCKET = 2 };

    static constexpr int TYPE = 0x20;
    uint8_t frame_id = 1;
    uint8_t dest_ip[4] = {};
    uint16_be dest_port = 0;
    uint16_be source_port = 0;
    Protocol protocol = UDP;
    Option options = NO_OPTIONS;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) TransmitIPWithTLSProfile {
    using Option = TransmitIP::Option;

    static constexpr int TYPE = 0x23;
    uint8_t frame_id = 1;
    uint8_t dest_ip[4] = {};
    uint16_be dest_port = 0;
    uint16_be source_port = 0;
    uint8_t tls_profile = 0;
    uint8_t options = Option::NO_OPTIONS;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) ReceiveIP {
    using Protocol = TransmitIP::Protocol;

    static constexpr int TYPE = 0xB0;
    uint8_t source_ip[4];
    uint16_be dest_port;
    uint16_be source_port;
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
    enum Status : uint8_t {
      POWER_UP = 0, WATCHDOG_RESET, REGISTERED = 2, UNREGISTERED = 3,
      MANAGER_CONNECTED = 0x0E, MANAGER_DISCONNECTED = 0x0F,
      UPDATE_STARTED = 0x38, UPDATE_FAILED = 0x39, UPDATE_APPLYING = 0x3A,
    };

    static constexpr int TYPE = 0x8A;
    Status status;

    char const* status_text() const;
  };

  struct __attribute__((packed)) RelayToInterface {
    enum Interface : uint8_t { SERIAL_PORT = 0, BLE, PYTHON = 2 };

    static constexpr int TYPE = 0x2D;
    uint8_t frame_id = 1;
    Interface interface = SERIAL_PORT;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) RelayFromInterface {
    using Interface = RelayToInterface::Interface;

    static constexpr int TYPE = 0xAD;
    Interface interface = RelayToInterface::SERIAL_PORT;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) FirmwareUpdate {
    enum Flags : uint8_t { NO_FLAGS = 0, INITIAL, FINAL = 2, CANCEL = 4 };

    static constexpr int TYPE = 0x2B;
    uint8_t id = 0;
    uint8_t component = 0;
    uint8_t flags = NO_FLAGS;
    uint8_t payload[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) FirmwareUpdateResponse {
    enum Status : uint8_t {
      OK = 0, IN_PROGRESS = 2, NOT_STARTED = 3,
      SEQUENCE_ERROR = 4, INTERNAL_ERROR = 5, RESOURCE_ERROR = 6,
    };

    static constexpr int TYPE = 0xAB;
    uint8_t id;
    uint8_t status;

    char const* status_text() const;
  };

  struct __attribute__((packed)) GnssRequest {
    enum Command : uint8_t {
      START_ONESHOT = 0, STOP_ONESHOT = 4, START_NMEA = 5, STOP_NMEA = 6
    };

    static constexpr int TYPE = 0x3D;
    uint8_t frame_id = 1;
    Command command = START_ONESHOT;
    uint16_be oneshot_timeout;
  };

  struct __attribute__((packed)) GnssResponse {
    using Command = GnssRequest::Command;
    enum Status : uint8_t { OK = 0, ERROR };

    static constexpr int TYPE = 0xBD;
    uint8_t frame_id;
    Command command;
    Status status;
  };

  struct __attribute__((packed)) GnssNmea {
    static constexpr int TYPE = 0xBE;
    uint8_t nmea[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) GnssOneShot {
    enum Status : uint8_t { OK = 0, INVALID, TIMEOUT = 2, CANCELLED = 3 };

    static constexpr int TYPE = 0xBF;
    Status status;
    uint32_be lock_time;
    int32_be latitude;
    int32_be longitude;
    int32_be altitude;
    uint8_t satellites;

    char const* status_text() const;
  };

  // TODO: BLE unlock, extended socket commands, filesystem commands
}
