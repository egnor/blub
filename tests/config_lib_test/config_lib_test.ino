// Unit tests for config & utilities (config_lib/)

#include "blub_clock_util.h"
#include "blub_mqtt_config.h"

#include <Arduino.h>
#include <etl/chrono.h>
#include <etl/format.h>
#include <SHA256.h>

#include <verifiers.h>

char const* const ok_logging_config = "DETAIL";

static OkLoggingContext OK_CONTEXT("config_lib_test");

static void test_blub_clock_util() {
  OK_NOTE("\n#TEST# test_blub_clock_util");
  etl::chrono::duration<int, etl::ratio<3, 7>> dur_300(700);
  VERIFY_A_OP_B_INT(raw_count<millis64>(dur_300), ==, 300000);
  VERIFY_A_NEAR_B_FP(raw_count<secf>(dur_300), 300.0, 1e-6);
  VERIFY_A_NEAR_B_FP(raw_count<secd>(dur_300), 300.0, 1e-6);
}

static void test_etl_steady_clock() {
  OK_NOTE("\n#TEST# test_etl_steady_clock");
  auto const start = etl::chrono::steady_clock::now();
  delay(100);
  auto const end = etl::chrono::steady_clock::now();
  VERIFY_A_NEAR_B_FP(raw_count<secd>(end - start), 0.1, 1e-3);
}

static void test_mqtt_config_sha256() {
  OK_NOTE("\n#TEST# test_mqtt_config_sha256");
  SHA256 sha256;
  sha256.update(blub_mqtt_config.cert.data(), blub_mqtt_config.cert.size());
  uint8_t digest[32];
  sha256.finalize(digest, sizeof(digest));
  for (int i = 0; i < sizeof(digest); ++i) {
    etl::string<3> digest_byte;
    etl::format_to(digest_byte, "{:02X}", digest[i]);
    auto const stored_byte = blub_mqtt_config.cert_sha256.substr(i * 2, 2);
    if (!VERIFY_A_OP_B_STR(digest_byte, ==, stored_byte))
      OK_ERROR("Mismatch in blub_mqtt_cert_sha256[%d]", i);
  }
}

void setup() {
  Serial1.begin(115200);
  ok_logging_stream = &Serial1;
  OK_NOTE("#BEGIN-TESTS#");
  test_blub_clock_util();
  test_etl_steady_clock();
  test_mqtt_config_sha256();
  OK_NOTE("#END-TESTS#");
}

void loop() {}
