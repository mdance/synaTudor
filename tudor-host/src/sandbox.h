#ifndef TUDOR_HOST_SANDBOX_H
#define TUDOR_HOST_SANDBOX_H

#include <stdint.h>

#define SANDBOX_UID 3333
#define SANDBOX_GID 3333

#define SANDBOX_DATA_LIMIT (128 * 1024 * 1024)
#define SANDBOX_STACK_LIMIT (64 * 1024)
#define SANDBOX_MAX_FDS 64
// The DLL spawns worker threads per capture and is slow to close their handles,
// so a single enroll (esp. with retries) can transiently hold many threads. The
// host is short-lived (one fprintd operation), so allow generous headroom;
// seccomp still blocks fork/clone except CLONE_THREAD.
#define SANDBOX_MAX_THREADS 512

void activate_sandbox();
void setup_usb_sbox(int usb_fd, uint8_t usb_bus, uint8_t usb_addr);

#endif