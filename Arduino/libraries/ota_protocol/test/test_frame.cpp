#include "check.h"
#include "ota_protocol.h"
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

int main() {
  RUN(test_roundtrip_no_payload);
  RUN(test_roundtrip_with_payload);
  RUN(test_bad_crc_rejected);
  RUN(test_resync_on_leading_garbage);
  RUN(test_encode_rejects_oversize);
  REPORT();
}
