// CAN capture build (PlatformIO env "logger"). Not part of the speedometer firmware.
//
// Prints every frame on the bus over USB serial as GVRET-style CSV, which SavvyCAN
// can import (File -> Load Frames -> "CAN Bus Logger / GVRET CSV"). Always
// listen-only: never ACKs or transmits, so it cannot disturb the car.
//
// Wiring is the same as the speedometer: SN65HVD230 D -> GPIO 25, R -> GPIO 26.
// The display is not needed.

#include <Arduino.h>
#include "driver/twai.h"

#define CAN_TX_PIN GPIO_NUM_25
#define CAN_RX_PIN GPIO_NUM_26
#define LOG_BAUD   921600

static uint32_t lastMissed = 0;
static uint32_t lastStatusMs = 0;

void setup() {
  Serial.begin(LOG_BAUD);
  delay(300);

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 256;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &t, &f) != ESP_OK || twai_start() != ESP_OK) {
    Serial.println("# TWAI start FAILED");
    return;
  }
  Serial.println("# TWAI started, 500 kbps, listen-only");
  Serial.println("Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8");
}

void loop() {
  twai_message_t m;
  char line[96];

  // Drain everything that is waiting, so the 256-frame queue doesn't overflow.
  while (twai_receive(&m, 0) == ESP_OK) {
    if (m.rtr) continue;
    int n = snprintf(line, sizeof(line), "%lu,%08lX,%s,Rx,0,%u",
                     (unsigned long)micros(), (unsigned long)m.identifier,
                     m.extd ? "true" : "false", (unsigned)m.data_length_code);
    for (int i = 0; i < 8; ++i) {
      if (i < m.data_length_code) n += snprintf(line + n, sizeof(line) - n, ",%02X", m.data[i]);
      else                        n += snprintf(line + n, sizeof(line) - n, ",");
    }
    line[n++] = '\n';
    Serial.write((const uint8_t*)line, n);
  }

  // About once a second, note if frames were dropped (queue overflow). These
  // '#' lines are not CSV rows: strip them before importing (grep -v '^#').
  uint32_t now = millis();
  if (now - lastStatusMs >= 1000) {
    lastStatusMs = now;
    twai_status_info_t s;
    if (twai_get_status_info(&s) == ESP_OK && s.rx_missed_count != lastMissed) {
      lastMissed = s.rx_missed_count;
      Serial.printf("# missed frames so far: %lu\n", (unsigned long)s.rx_missed_count);
    }
  }
}
