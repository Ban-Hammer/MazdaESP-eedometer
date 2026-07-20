#include "check.h"
#include "ota_protocol.h"
#include <string>
using namespace otap;

// Push every byte of `buf` and return the status of the last byte.
static ParseStatus feed(FrameParser* p, const uint8_t* buf, size_t n, ParsedFrame* out) {
  ParseStatus st = ParseStatus::NEED_MORE;
  for (size_t i = 0; i < n; ++i) st = frameparser_push(p, buf[i], out);
  return st;
}

static void test_roundtrip_no_payload() {
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::PING, nullptr, 0, wire, sizeof(wire));
  CHECK_EQ(n, FRAME_OVERHEAD);
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::FRAME_READY);
  CHECK_EQ((int)f.type, (int)MsgType::PING);
  CHECK_EQ(f.payload_len, 0u);
}

static void test_roundtrip_with_payload() {
  uint8_t hdr[8]; header_encode(0x00100000u, 0xCBF43926u, hdr);
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::HEADER, hdr, 8, wire, sizeof(wire));
  CHECK_EQ(n, FRAME_OVERHEAD + 8);
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::FRAME_READY);
  CHECK_EQ((int)f.type, (int)MsgType::HEADER);
  CHECK_EQ(f.payload_len, 8u);
  uint32_t len, crc; header_decode(f.payload, &len, &crc);
  CHECK_EQ(len, 0x00100000u);
  CHECK_EQ(crc, 0xCBF43926u);
}

static void test_bad_crc_rejected() {
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::ACK, nullptr, 0, wire, sizeof(wire));
  wire[2] = (uint8_t)MsgType::NAK; // corrupt the type after CRC computed
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::BAD_CRC);
}

static void test_resync_on_leading_garbage() {
  uint8_t wire[64];
  size_t n = frame_encode(MsgType::PING, nullptr, 0, wire, sizeof(wire));
  FrameParser p; frameparser_reset(&p);
  ParsedFrame f;
  frameparser_push(&p, 0x00, &f);
  frameparser_push(&p, 0xFF, &f);
  frameparser_push(&p, 0xA5, &f); // stray magic0 then non-magic1
  frameparser_push(&p, 0x11, &f);
  CHECK_EQ((int)feed(&p, wire, n, &f), (int)ParseStatus::FRAME_READY);
  CHECK_EQ((int)f.type, (int)MsgType::PING);
}

static void test_encode_rejects_oversize() {
  uint8_t big[MAX_PAYLOAD + 1] = {0};
  uint8_t wire[128];
  CHECK_EQ(frame_encode(MsgType::VERSION, big, sizeof(big), wire, sizeof(wire)), 0u);
}

static void test_list_entry_roundtrip() {
  uint8_t p[MAX_PAYLOAD];
  size_t n = list_entry_encode(true, 6321, "000.jpg", p, sizeof(p));
  CHECK(n > 0);
  bool isDir; uint32_t sz; char name[OTA_MAX_NAME];
  CHECK(list_entry_decode(p, n, &isDir, &sz, name, sizeof(name)));
  CHECK_EQ(isDir, true);
  CHECK_EQ(sz, 6321u);
  CHECK(std::string(name) == "000.jpg");
}

static void test_list_entry_rejects_long_name() {
  char big[OTA_MAX_NAME + 5]; for (auto& c : big) c = 'x'; big[OTA_MAX_NAME + 4] = 0;
  uint8_t p[MAX_PAYLOAD];
  CHECK_EQ(list_entry_encode(false, 0, big, p, sizeof(p)), 0u);
}

static void test_writehdr_roundtrip() {
  uint8_t p[MAX_PAYLOAD];
  size_t n = writehdr_encode(40000, 0xDEADBEEFu, "/splash/012.jpg", p, sizeof(p));
  CHECK(n > 0);
  uint32_t len, crc; char path[OTA_MAX_NAME];
  CHECK(writehdr_decode(p, n, &len, &crc, path, sizeof(path)));
  CHECK_EQ(len, 40000u);
  CHECK_EQ(crc, 0xDEADBEEFu);
  CHECK(std::string(path) == "/splash/012.jpg");
}

int main() {
  RUN(test_roundtrip_no_payload);
  RUN(test_roundtrip_with_payload);
  RUN(test_bad_crc_rejected);
  RUN(test_resync_on_leading_garbage);
  RUN(test_encode_rejects_oversize);
  RUN(test_list_entry_roundtrip);
  RUN(test_list_entry_rejects_long_name);
  RUN(test_writehdr_roundtrip);
  REPORT();
}
