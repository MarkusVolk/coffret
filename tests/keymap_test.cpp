#include "check.h"
#include "input/linux_to_qnum.h"

#include <linux/input-event-codes.h>

int main() {
  const auto qnum = [](unsigned key) { return key < coffret_linux_to_qnum_len ? coffret_linux_to_qnum[key] : 0; };
  // The main block is AT set 1 unchanged.
  CHECK(qnum(KEY_ESC) == 0x01);
  CHECK(qnum(KEY_A) == 0x1e);
  CHECK(qnum(KEY_ENTER) == 0x1c);
  CHECK(qnum(KEY_LEFTCTRL) == 0x1d);
  CHECK(qnum(KEY_LEFTALT) == 0x38);
  CHECK(qnum(KEY_SPACE) == 0x39);
  CHECK(qnum(KEY_F12) == 0x58);
  // Keys with an E0 prefix carry it as bit 7.
  CHECK(qnum(KEY_RIGHTCTRL) == 0x9d);
  CHECK(qnum(KEY_RIGHTALT) == 0xb8);
  CHECK(qnum(KEY_UP) == 0xc8);
  CHECK(qnum(KEY_DELETE) == 0xd3);
  CHECK(qnum(KEY_LEFTMETA) == 0xdb);
  CHECK(qnum(KEY_KPENTER) == 0x9c);
  // The odd ones.
  CHECK(qnum(KEY_SYSRQ) == 0x54);
  CHECK(qnum(KEY_PAUSE) == 0xc6);
  CHECK(qnum(KEY_102ND) == 0x56);
  // No key, and codes beyond the table.
  CHECK(qnum(KEY_RESERVED) == 0);
  CHECK(qnum(100000) == 0);
  return 0;
}
