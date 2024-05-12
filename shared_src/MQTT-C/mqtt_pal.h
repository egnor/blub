// *C-compatible* header file for the MQTT-C Platform Abstraction Layer (PAL)
// provided by XBeeMqttAdapter (xbee_mqtt_adapter.cpp).
// See: https://liambindle.ca/MQTT-C/group__pal.html

#ifndef MQTT_PAL_H  // #pragma once breaks with macro-based includes?
#define MQTT_PAL_H

// MQTT-C requires some definitions which are present in these headers
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef void *mqtt_pal_socket_handle;
typedef int ssize_t;

ssize_t mqtt_pal_sendall(mqtt_pal_socket_handle, void const*, size_t, int fl);
ssize_t mqtt_pal_recvall(mqtt_pal_socket_handle, void*, size_t, int fl);

typedef struct {} mqtt_pal_mutex_t;  // Dummy (single threaded)
static void MQTT_PAL_MUTEX_INIT(mqtt_pal_mutex_t*) {}
static void MQTT_PAL_MUTEX_LOCK(mqtt_pal_mutex_t*) {}
static void MQTT_PAL_MUTEX_UNLOCK(mqtt_pal_mutex_t*) {}

typedef long mqtt_pal_time_t;
mqtt_pal_time_t MQTT_PAL_TIME(void);

static uint16_t MQTT_PAL_HTONS(uint16_t x) { return (x >> 8) | (x << 8); }
static uint16_t MQTT_PAL_NTOHS(uint16_t x) { return MQTT_PAL_HTONS(x); }
static uint32_t MQTT_PAL_HTONL(uint32_t x) {
  return ((x >> 24) & 0xff) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) |
         ((x << 24) & 0xff000000);
}
static uint32_t MQTT_PAL_NTOHL(uint32_t x) { return MQTT_PAL_HTONL(x); }

#if defined(__cplusplus)
}
#endif

#endif  // XBEE_MQTT_PAL_H
