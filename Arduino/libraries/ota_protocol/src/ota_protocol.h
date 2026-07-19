#pragma once
#include <stdint.h>
#include <stddef.h>

namespace otap {

// ---------- CRC32 (IEEE 802.3) ----------
uint32_t crc32_init();
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);
uint32_t crc32_final(uint32_t crc);
uint32_t crc32(const uint8_t* data, size_t len);

// ---------- Frame format ----------
// Wire: MAGIC0 MAGIC1 TYPE LEN_LO LEN_HI PAYLOAD[LEN] CRC32(4, LE over TYPE..PAYLOAD)
static const uint8_t MAGIC0 = 0xA5;
static const uint8_t MAGIC1 = 0x5A;
static const size_t  FRAME_OVERHEAD = 2 /*magic*/ + 1 /*type*/ + 2 /*len*/ + 4 /*crc*/;
static const size_t  MAX_PAYLOAD = 64;

// Data-phase block size. The sender transmits one block of raw image bytes, then
// waits for an ACK before the next — flow control so the receiver's UART RX FIFO
// is never overrun while it pauses to write a block to flash.
static const uint32_t OTA_BLOCK = 4096;

enum class MsgType : uint8_t {
  ENTER_UPDATE = 0x01,
  ACK          = 0x02,
  HEADER       = 0x03,  // payload: u32 img_len (LE) + u32 img_crc32 (LE)
  RESULT       = 0x04,  // payload: 1 byte ResultCode
  PING         = 0x05,
  VERSION      = 0x06,  // payload: version string (no NUL)
  NAK          = 0x07,
};

enum class ResultCode : uint8_t {
  OK           = 0x00,
  CRC_MISMATCH = 0x01,
  FLASH_ERROR  = 0x02,
  TIMEOUT      = 0x03,
  BAD_LENGTH   = 0x04,
};

// Encode a control frame. Returns total bytes written, or 0 if payload too big
// or out_cap too small.
size_t frame_encode(MsgType type, const uint8_t* payload, size_t payload_len,
                    uint8_t* out, size_t out_cap);

enum class ParseStatus : uint8_t { NEED_MORE, FRAME_READY, BAD_CRC };

struct ParsedFrame {
  MsgType type;
  uint8_t payload[MAX_PAYLOAD];
  size_t  payload_len;
};

struct FrameParser {
  uint8_t buf[FRAME_OVERHEAD + MAX_PAYLOAD];
  size_t  have;
};

void        frameparser_reset(FrameParser* p);
// Feed one received byte. Returns FRAME_READY (fills out) once a valid frame is
// complete, BAD_CRC if a fully-received frame fails its CRC (parser auto-resets),
// else NEED_MORE. Resynchronizes on bad magic.
ParseStatus frameparser_push(FrameParser* p, uint8_t byte, ParsedFrame* out);

// Convenience payload (de)serialization for HEADER.
void header_encode(uint32_t img_len, uint32_t img_crc32, uint8_t out[8]);
void header_decode(const uint8_t in[8], uint32_t* img_len, uint32_t* img_crc32);

// ---------- Update state machine (pure) ----------
enum class State  : uint8_t { IDLE, WAIT_HEADER, RECEIVING, VERIFYING, DONE, ABORTED };
enum class Event  : uint8_t { RX_ENTER, RX_HEADER, DATA_COMPLETE, CRC_OK, CRC_FAIL,
                              TIMEOUT, FLASH_FAIL, RESET };
enum class Action : uint8_t { NONE, SEND_ACK, BEGIN_FLASH, COMMIT, ABORT_RESUME };

struct Transition { State next; Action action; };
Transition sm_step(State s, Event e);

} // namespace otap
