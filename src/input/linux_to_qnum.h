#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Indexed by the Linux evdev key code; 0 where QEMU has no key for it. */
extern const unsigned short coffret_linux_to_qnum[];
extern const unsigned int coffret_linux_to_qnum_len;

#ifdef __cplusplus
}
#endif
