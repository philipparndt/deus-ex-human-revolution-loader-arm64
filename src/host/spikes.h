// DXH_SPIKES=<ms>: per-frame accounting of where the time went, logged for
// every presented frame slower than the threshold (stutter spikes). See spikes.c.
#pragma once
#include "cpu.h"

#include <stdbool.h>
#include <stdint.h>

extern bool spikes_on; // constant after thunks_init

void spikes_init(void);
// Host call boundaries (host_dispatch), around the thunk function only.
void spikes_thunk_enter(uint32_t id, const struct guest_regs *r);
void spikes_thunk_exit(void);
// Guest code run from the host (cpu_call, cpu_start): counts as guest time
// even inside a host call. Returns what spikes_guest_exit restores.
uint32_t spikes_guest_enter(void);
void spikes_guest_exit(uint32_t saved);
// Main-queue work (mainq.c); queued: when a waited-for item was queued, else 0.
uint64_t spikes_now(void);
uint64_t spikes_mainq_begin(uint64_t queued);
void spikes_mainq_end(uint64_t begin);
// A frame was presented (CGLFlushDrawable): the calling thread is the frame thread.
void spikes_frame(void);
