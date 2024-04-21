#include "xbee_api.h"

namespace XBeeApi {

char const* ATCommandResponse::status_text() const {
  switch (status) {
#define S(x) case x: return #x
    S(OK);
    S(ERROR);
    S(BAD_COMMAND);
    S(BAD_PARAM);
#undef S
  }
  return "UNKNOWN";
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
  return "UNKNOWN";
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
  return "UNKNOWN";
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
  return "UNKNOWN";
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
  return "UNKNOWN";
}

}  // namespace XBeeApi
