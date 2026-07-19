#include "check.h"
#include "ota_protocol.h"
using namespace otap;

static void test_happy_path() {
  Transition t;
  t = sm_step(State::IDLE, Event::RX_ENTER);
  CHECK_EQ((int)t.next, (int)State::WAIT_HEADER); CHECK_EQ((int)t.action, (int)Action::SEND_ACK);
  t = sm_step(State::WAIT_HEADER, Event::RX_HEADER);
  CHECK_EQ((int)t.next, (int)State::RECEIVING);   CHECK_EQ((int)t.action, (int)Action::BEGIN_FLASH);
  t = sm_step(State::RECEIVING, Event::DATA_COMPLETE);
  CHECK_EQ((int)t.next, (int)State::VERIFYING);   CHECK_EQ((int)t.action, (int)Action::NONE);
  t = sm_step(State::VERIFYING, Event::CRC_OK);
  CHECK_EQ((int)t.next, (int)State::DONE);        CHECK_EQ((int)t.action, (int)Action::COMMIT);
}

static void test_crc_fail_aborts() {
  Transition t = sm_step(State::VERIFYING, Event::CRC_FAIL);
  CHECK_EQ((int)t.next, (int)State::ABORTED);     CHECK_EQ((int)t.action, (int)Action::ABORT_RESUME);
}

static void test_timeout_and_flashfail_abort_from_any_active_state() {
  State actives[] = { State::WAIT_HEADER, State::RECEIVING, State::VERIFYING };
  for (State s : actives) {
    Transition t1 = sm_step(s, Event::TIMEOUT);
    CHECK_EQ((int)t1.next, (int)State::ABORTED); CHECK_EQ((int)t1.action, (int)Action::ABORT_RESUME);
    Transition t2 = sm_step(s, Event::FLASH_FAIL);
    CHECK_EQ((int)t2.next, (int)State::ABORTED); CHECK_EQ((int)t2.action, (int)Action::ABORT_RESUME);
  }
}

static void test_unexpected_event_aborts() {
  Transition t = sm_step(State::IDLE, Event::RX_HEADER); // HEADER before ENTER
  CHECK_EQ((int)t.next, (int)State::ABORTED); CHECK_EQ((int)t.action, (int)Action::ABORT_RESUME);
}

static void test_reset_returns_to_idle() {
  CHECK_EQ((int)sm_step(State::ABORTED, Event::RESET).next, (int)State::IDLE);
  CHECK_EQ((int)sm_step(State::DONE, Event::RESET).next, (int)State::IDLE);
  CHECK_EQ((int)sm_step(State::RECEIVING, Event::RESET).next, (int)State::IDLE);
}

int main() {
  RUN(test_happy_path);
  RUN(test_crc_fail_aborts);
  RUN(test_timeout_and_flashfail_abort_from_any_active_state);
  RUN(test_unexpected_event_aborts);
  RUN(test_reset_returns_to_idle);
  REPORT();
}
