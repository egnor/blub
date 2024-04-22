#include "xbee_api.h"

namespace XBeeAPI {

char const* ATCommandResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(ERROR);
    S(BAD_COMMAND);
    S(BAD_PARAM);
#undef S
  }
  return "UNKNOWN_COMMAND_STATUS";
}

char const* TransmitStatus::status_text() const {
  // There's no enum for this one (too many to be worth it)
  switch (status) {
    case 0x0: return "OK";
    case 0x20: return "CONNECTION_NOT_FOUND";
    case 0x21: return "NETWORK_FAILURE";
    case 0x22: return "NETWORK_DISCONNECTED";
    case 0x2C: return "BAD_FRAME";
    case 0x31: return "INTERNAL_ERROR";
    case 0x32: return "RESOURCE_ERROR";
    case 0x74: return "MESSAGE_TOO_LONG";
    case 0x76: return "SOCKET_CLOSED_UNEXPECTEDLY";
    case 0x78: return "BAD_UDP_PORT";
    case 0x79: return "BAD_TCP_PORT";
    case 0x7A: return "BAD_IP_ADDRESS";
    case 0x7B: return "BAD_DATA_MODE";
    case 0x7C: return "BAD_INTERFACE";
    case 0x7D: return "INTERFACE_CLOSED";
    case 0x7E: return "MODEM_UPDATE";
    case 0x80: return "CONNECTION_REFUSED";
    case 0x81: return "CONNECTION_LOST";
    case 0x82: return "NO_SERVER";
    case 0x83: return "SOCKET_CLOSED";
    case 0x84: return "UNKNOWN_SERVER";
    case 0x85: return "UNKNOWN_ERROR";
    case 0x86: return "BAD_TLS_CONFIG";
    case 0x87: return "SOCKET_DISCONNECTED";
    case 0x88: return "SOCKET_UNBOUND";
    case 0x89: return "SOCKET_INACTIVITY_TIMEOUT";
    case 0x8A: return "NETWORK_PDP_DEACTIVATED";
    case 0x8B: return "TLS_AUTHENTICATION_ERROR";
  }
  return "UNKNOWN_TRANSMIT_STATUS";
}

char const *ModemStatus::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(POWER_UP);
    S(WATCHDOG_RESET);
    S(REGISTERED);
    S(UNREGISTERED);
    S(MANAGER_CONNECTED);
    S(MANAGER_DISCONNECTED);
    S(UPDATE_STARTED);
    S(UPDATE_FAILED);
    S(UPDATE_APPLYING);
#undef S
  }
  return "UNKNOWN_MODEM_STATUS";
}

char const* FirmwareUpdateResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(IN_PROGRESS);
    S(NOT_STARTED);
    S(SEQUENCE_ERROR);
    S(INTERNAL_ERROR);
    S(RESOURCE_ERROR);
#undef S
  }
  return "UNKNOWN_FIRMWARE_STATUS";
}

char const* SocketCreateResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(NOT_REGISTERED);
    S(INTERNAL_ERROR);
    S(RESOURCE_ERROR);
    S(BAD_PROTOCOL);
    S(MODEM_UPDATE);
    S(UNKNOWN_ERROR);
    S(BAD_TLS_PROFILE);
#undef S
  }
  return "UNKNOWN_CREATE_STATUS";
}

char const* SocketOptionResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(BAD_PARAM);
    S(FAILED);
    S(BAD_SOCKET);
#undef S
  }
  return "UNKNOWN_OPTION_STATUS";
}

char const* SocketConnectResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(STARTED);
    S(BAD_ADDRESS_TYPE);
    S(BAD_PARAM);
    S(ALREADY_IN_PROGRESS);
    S(ALREADY_CONNECTED);
    S(UNKNOWN_ERROR);
    S(BAD_SOCKET);
#undef S
  }
  return "UNKNOWN_CONNECT_STATUS";
}

char const* SocketCloseResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(BAD_SOCKET);
#undef S
  }
  return "UNKNOWN_CLOSE_STATUS";
}

char const* SocketBindListenResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(BAD_PORT);
    S(ERROR);
    S(ALREADY_BOUND);
    S(BAD_SOCKET);
#undef S
  }
  return "UNKNOWN_BIND_STATUS";
}

char const* SocketStatus::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(CONNECTED);
    S(FAILED_DNS);
    S(CONNECTION_REFUSED);
    S(TRANSPORT_CLOSED);
    S(TIMED_OUT);
    S(INTERNAL_ERROR);
    S(HOST_UNREACHABLE);
    S(CONNECTION_LOST);
    S(UNKNOWN_ERROR);
    S(UNKNOWN_SERVER);
    S(RESOURCE_ERROR);
    S(RESET_BY_PEER);
    S(INACTIVITY_CLOSED);
    S(PDP_DEACTIVATED);
#undef S
  }
  return "UNKNOWN_SOCKET_STATUS";
}

char const* GnssOneShot::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(INVALID);
    S(TIMEOUT);
    S(CANCELLED);
#undef S
  }
  return "UNKNOWN_GNSS_STATUS";
}

}  // namespace XBeeAPI
