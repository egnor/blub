// Defined frames for communication with Digi XBee radios in API mode.

#pragma once

#include <stdint.h>

#include <new>

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
    enum Status : uint8_t { OK = 0, ERROR = 1, BAD_COMMAND = 2, BAD_PARAM = 3 };

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
    enum Protocol : uint8_t { UDP = 0, TCP = 1, TLS = 4 };
    enum Option : uint8_t { NO_OPTIONS = 0, CLOSE_SOCKET = 2 };

    static constexpr int TYPE = 0x20;
    uint8_t frame_id = 1;
    uint8_t dest_ip[4] = {};
    uint16_be dest_port = 0;
    uint16_be source_port = 0;
    Protocol protocol = Protocol::UDP;
    Option options = Option::NO_OPTIONS;
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
    Protocol protocol;
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
    Interface interface = Interface::SERIAL_PORT;
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
    uint8_t flags = Flags::NO_FLAGS;
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

  struct __attribute__((packed)) SocketCreate {
    using Protocol = TransmitIP::Protocol;

    static constexpr int TYPE = 0x40;
    uint8_t frame_id = 1;
    Protocol protocol = Protocol::UDP;
  };

  struct __attribute__((packed)) SocketCreateResponse {
    enum Status : uint8_t {
      OK = 0, NOT_REGISTERED = 0x22, INTERNAL_ERROR = 0x31,
      RESOURCE_ERROR = 0x32, BAD_PROTOCOL = 0x7B, MODEM_UPDATE = 0x7E,
      UNKNOWN_ERROR = 0x85, BAD_TLS_PROFILE = 0x86,
    };

    static constexpr int TYPE = 0xC0;
    uint8_t frame_id;
    uint8_t socket;
    Status status;

    char const* status_text() const;
  };

  struct __attribute__((packed)) SocketOptionRequest {
    enum Option : uint8_t { TLS_PROFILE = 0 };

    static constexpr int TYPE = 0x41;
    uint8_t frame_id = 1;
    uint8_t socket = 0;
    Option option = Option::TLS_PROFILE;
    uint8_t data[0];  // Packet-length determined if present
  };

  struct __attribute__((packed)) SocketOptionResponse {
    enum Status : uint8_t {
      OK = 0, BAD_PARAM = 0x01, FAILED = 0x02, BAD_SOCKET = 0x20,
    };

    static constexpr int TYPE = 0xC1;
    uint8_t frame_id;
    uint8_t socket;
    uint8_t status;
    uint8_t data[0];  // Packet-length determined if present

    char const* status_text() const;
  };

  struct __attribute__((packed)) SocketConnect {
    enum AddressType : uint8_t { IPV4 = 0, TEXT = 1 };

    static constexpr int TYPE = 0x42;
    uint8_t frame_id = 1;
    uint8_t socket = 0;
    uint16_be dest_port = 0;
    AddressType address_type = AddressType::IPV4;
    uint8_t address[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) SocketConnectResponse {
    enum Status : uint8_t {
      STARTED = 0, BAD_ADDRESS_TYPE = 0x01, BAD_PARAM = 0x02,
      ALREADY_IN_PROGRESS = 0x03, ALREADY_CONNECTED = 0x04,
      UNKNOWN_ERROR = 0x05, BAD_SOCKET = 0x20,
    };

    static constexpr int TYPE = 0xC2;
    uint8_t frame_id;
    uint8_t socket;
    Status status;

    char const* status_text() const;
  };

  struct __attribute__((packed)) SocketClose {
    static constexpr uint8_t ALL_SOCKETS = 0xFF;

    static constexpr int TYPE = 0x43;
    uint8_t frame_id = 1;
    uint8_t socket = 0;
  };

  struct __attribute__((packed)) SocketCloseResponse {
    enum Status : uint8_t { OK = 0, BAD_SOCKET = 0x20 };

    static constexpr int TYPE = 0xC3;
    uint8_t frame_id;
    uint8_t socket;
    Status status;

    char const* status_text() const;
  };

  struct __attribute__((packed)) SocketSend {
    static constexpr int TYPE = 0x44;
    uint8_t frame_id = 1;
    uint8_t socket = 0;
    uint8_t reserved = 0;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) SocketSendTo {
    static constexpr int TYPE = 0x45;
    uint8_t frame_id = 1;
    uint8_t socket = 0;
    uint8_t dest_ip[4] = {};
    uint16_be dest_port = 0;
    uint8_t reserved = 0;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) SocketBindListen {
    static constexpr int TYPE = 0x46;
    uint8_t frame_id = 1;
    uint8_t socket = 0;
    uint16_be port = 0;
  };

  struct __attribute__((packed)) SocketBindListenResponse {
    enum Status : uint8_t {
      OK = 0, BAD_PORT = 0x01, ERROR = 0x02, ALREADY_BOUND = 0x03,
      BAD_SOCKET = 0x20,
    };

    static constexpr int TYPE = 0xC6;
    uint8_t frame_id;
    uint8_t socket;
    Status status;

    char const* status_text() const;
  };

  struct __attribute__((packed)) SocketNewClient {
    static constexpr int TYPE = 0xCC;
    uint8_t listener_socket;
    uint8_t new_socket;
    uint8_t remote_ip[4];
    uint16_be remote_port;
  };

  struct __attribute__((packed)) SocketReceive {
    static constexpr int TYPE = 0xCD;
    uint8_t frame_id;
    uint8_t socket;
    uint8_t reserved;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) SocketReceiveFrom {
    static constexpr int TYPE = 0xCE;
    uint8_t frame_id;
    uint8_t socket;
    uint8_t source_ip[4];
    uint16_be source_port;
    uint8_t reserved;
    uint8_t data[0];  // Packet-length terminated
  };

  struct __attribute__((packed)) SocketStatus {
    enum Status : uint8_t {
      CONNECTED = 0x00, FAILED_DNS = 0x01, CONNECTION_REFUSED = 0x02,
      TRANSPORT_CLOSED = 0x03, TIMED_OUT = 0x04, INTERNAL_ERROR = 0x05,
      HOST_UNREACHABLE = 0x06, CONNECTION_LOST = 0x07, UNKNOWN_ERROR = 0x08,
      UNKNOWN_SERVER = 0x09, RESOURCE_ERROR = 0x0A,
      RESET_BY_PEER = 0x0C, INACTIVITY_CLOSED = 0x0D, PDP_DEACTIVATED = 0x0E,
    };

    static constexpr int TYPE = 0xCF;
    uint8_t socket;
    Status status;

    char const* status_text() const;
  };

  struct __attribute__((packed)) GnssRequest {
    enum Command : uint8_t {
      START_ONESHOT = 0, STOP_ONESHOT = 4, START_NMEA = 5, STOP_NMEA = 6
    };

    static constexpr int TYPE = 0x3D;
    uint8_t frame_id = 1;
    Command command = Command::START_ONESHOT;
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
    enum Status : uint8_t { OK = 0, INVALID = 1, TIMEOUT = 2, CANCELLED = 3 };

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
