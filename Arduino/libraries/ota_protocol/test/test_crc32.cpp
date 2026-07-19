#include "check.h"
#include "ota_protocol.h"
#include <cstring>

static void test_known_vectors() {
  const char* s = "123456789";
  CHECK_EQ(otap::crc32((const uint8_t*)s, 9), 0xCBF43926u); // canonical CRC-32/ISO-HDLC
  CHECK_EQ(otap::crc32(nullptr, 0), 0x00000000u);
  const char* a = "a";
  CHECK_EQ(otap::crc32((const uint8_t*)a, 1), 0xE8B7BE43u);
}

static void test_incremental_matches_oneshot() {
  const char* s = "The quick brown fox";
  size_t n = std::strlen(s);
  uint32_t c = otap::crc32_init();
  c = otap::crc32_update(c, (const uint8_t*)s, 5);
  c = otap::crc32_update(c, (const uint8_t*)s + 5, n - 5);
  CHECK_EQ(otap::crc32_final(c), otap::crc32((const uint8_t*)s, n));
}

int main() {
  RUN(test_known_vectors);
  RUN(test_incremental_matches_oneshot);
  REPORT();
}
