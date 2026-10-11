// C interface of the Rust dxcpu engine (src/cpu).
#pragma once
#include <stdint.h>

typedef struct dxcpu_engine dxcpu_engine;
typedef struct dxcpu_state dxcpu_state;

enum { DXCPU_STOP = 0, DXCPU_STUB = 1, DXCPU_FAULT = 2, DXCPU_UNIMPLEMENTED = 3, DXCPU_HALT = 4 };
enum { DXR_EAX, DXR_ECX, DXR_EDX, DXR_EBX, DXR_ESP, DXR_EBP, DXR_ESI, DXR_EDI, DXR_EIP, DXR_EFLAGS, DXR_FAULT_EIP,
       DXR_FAULT_ADDR };

void dxcpu_set_guest_base(uint8_t *base);
dxcpu_engine *dxcpu_engine_new(uint32_t stub_lo, uint32_t stub_hi);
void dxcpu_engine_flush(dxcpu_engine *e);
dxcpu_state *dxcpu_state_new(void);
uint32_t dxcpu_run(dxcpu_engine *e, dxcpu_state *s, uint32_t stop);
uint32_t dxcpu_get_reg(const dxcpu_state *s, uint32_t idx);
void dxcpu_set_reg(dxcpu_state *s, uint32_t idx, uint32_t v);
void dxcpu_x87_push(dxcpu_state *s, double v);
double dxcpu_x87_pop(dxcpu_state *s);
uint64_t dxcpu_icount(const dxcpu_state *s);
uint32_t dxcpu_disasm(uint32_t addr, char *buf, uint32_t len);
void dxcpu_print_stats(void);
