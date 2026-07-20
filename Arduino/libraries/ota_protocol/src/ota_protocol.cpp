#include "ota_protocol.h"

namespace otap {

// ---------- CRC32 (IEEE 802.3, poly 0xEDB88320) ----------
uint32_t crc32_init() { return 0xFFFFFFFFu; }

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1u) - 1u)));
  }
  return crc;
}

uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

uint32_t crc32(const uint8_t* d, size_t n) {
  return crc32_final(crc32_update(crc32_init(), d, n));
}

// ---------- little-endian helpers ----------
static void put_u32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint32_t get_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void header_encode(uint32_t img_len, uint32_t img_crc32, uint8_t out[8]) {
  put_u32(out, img_len); put_u32(out + 4, img_crc32);
}
void header_decode(const uint8_t in[8], uint32_t* img_len, uint32_t* img_crc32) {
  *img_len = get_u32(in); *img_crc32 = get_u32(in + 4);
}

size_t list_entry_encode(bool isDir, uint32_t size, const char* name, uint8_t* out, size_t out_cap) {
  size_t nlen = 0; while (name[nlen]) nlen++;
  if (nlen > OTA_MAX_NAME) return 0;
  size_t total = 1 + 4 + nlen;
  if (out_cap < total || total > MAX_PAYLOAD) return 0;
  out[0] = isDir ? 1 : 0;
  put_u32(out + 1, size);
  for (size_t i = 0; i < nlen; ++i) out[5 + i] = (uint8_t)name[i];
  return total;
}
bool list_entry_decode(const uint8_t* payload, size_t plen, bool* isDir, uint32_t* size,
                       char* name, size_t name_cap) {
  if (plen < 5) return false;
  *isDir = (payload[0] & 1) != 0;
  *size = get_u32(payload + 1);
  size_t nlen = plen - 5;
  if (nlen >= name_cap) return false;
  for (size_t i = 0; i < nlen; ++i) name[i] = (char)payload[5 + i];
  name[nlen] = 0;
  return true;
}
size_t writehdr_encode(uint32_t len, uint32_t crc, const char* path, uint8_t* out, size_t out_cap) {
  size_t plen = 0; while (path[plen]) plen++;
  if (plen > OTA_MAX_NAME) return 0;
  size_t total = 8 + plen;
  if (out_cap < total || total > MAX_PAYLOAD) return 0;
  put_u32(out, len); put_u32(out + 4, crc);
  for (size_t i = 0; i < plen; ++i) out[8 + i] = (uint8_t)path[i];
  return total;
}
bool writehdr_decode(const uint8_t* payload, size_t plen, uint32_t* len, uint32_t* crc,
                     char* path, size_t path_cap) {
  if (plen < 8) return false;
  *len = get_u32(payload); *crc = get_u32(payload + 4);
  size_t n = plen - 8;
  if (n >= path_cap) return false;
  for (size_t i = 0; i < n; ++i) path[i] = (char)payload[8 + i];
  path[n] = 0;
  return true;
}

// ---------- frame codec ----------
size_t frame_encode(MsgType type, const uint8_t* payload, size_t payload_len,
                    uint8_t* out, size_t out_cap) {
  if (payload_len > MAX_PAYLOAD) return 0;
  size_t total = FRAME_OVERHEAD + payload_len;
  if (out_cap < total) return 0;
  out[0] = MAGIC0; out[1] = MAGIC1;
  out[2] = (uint8_t)type;
  out[3] = (uint8_t)(payload_len & 0xFF);
  out[4] = (uint8_t)((payload_len >> 8) & 0xFF);
  for (size_t i = 0; i < payload_len; ++i) out[5 + i] = payload[i];
  // CRC over TYPE..PAYLOAD (bytes [2 .. 5+payload_len))
  uint32_t c = crc32(out + 2, 3 + payload_len);
  put_u32(out + 5 + payload_len, c);
  return total;
}

void frameparser_reset(FrameParser* p) { p->have = 0; }

ParseStatus frameparser_push(FrameParser* p, uint8_t byte, ParsedFrame* out) {
  // Assemble into p->buf with resync on magic.
  if (p->have == 0) {
    if (byte != MAGIC0) return ParseStatus::NEED_MORE;
  } else if (p->have == 1) {
    if (byte != MAGIC1) {
      // Restart; the stray byte may itself be a fresh MAGIC0.
      p->have = (byte == MAGIC0) ? 1 : 0;
      if (p->have == 1) p->buf[0] = MAGIC0;
      return ParseStatus::NEED_MORE;
    }
  }
  if (p->have >= sizeof(p->buf)) { frameparser_reset(p); return ParseStatus::NEED_MORE; }
  p->buf[p->have++] = byte;

  if (p->have < 5) return ParseStatus::NEED_MORE;           // need len field
  size_t payload_len = (size_t)p->buf[3] | ((size_t)p->buf[4] << 8);
  if (payload_len > MAX_PAYLOAD) { frameparser_reset(p); return ParseStatus::NEED_MORE; }
  size_t total = FRAME_OVERHEAD + payload_len;
  if (p->have < total) return ParseStatus::NEED_MORE;

  uint32_t want = get_u32(p->buf + 5 + payload_len);
  uint32_t got  = crc32(p->buf + 2, 3 + payload_len);
  if (want != got) { frameparser_reset(p); return ParseStatus::BAD_CRC; }

  out->type = (MsgType)p->buf[2];
  out->payload_len = payload_len;
  for (size_t i = 0; i < payload_len; ++i) out->payload[i] = p->buf[5 + i];
  frameparser_reset(p);
  return ParseStatus::FRAME_READY;
}

// ---------- update state machine ----------
Transition sm_step(State s, Event e) {
  if (e == Event::RESET)      return { State::IDLE,    Action::NONE };
  if (e == Event::TIMEOUT ||
      e == Event::FLASH_FAIL) return { State::ABORTED, Action::ABORT_RESUME };

  switch (s) {
    case State::IDLE:
      if (e == Event::RX_ENTER)      return { State::WAIT_HEADER, Action::SEND_ACK };
      break;
    case State::WAIT_HEADER:
      if (e == Event::RX_HEADER)     return { State::RECEIVING,   Action::BEGIN_FLASH };
      break;
    case State::RECEIVING:
      if (e == Event::DATA_COMPLETE) return { State::VERIFYING,   Action::NONE };
      break;
    case State::VERIFYING:
      if (e == Event::CRC_OK)        return { State::DONE,        Action::COMMIT };
      if (e == Event::CRC_FAIL)      return { State::ABORTED,     Action::ABORT_RESUME };
      break;
    default: break;
  }
  return { State::ABORTED, Action::ABORT_RESUME }; // any unexpected event
}

} // namespace otap
