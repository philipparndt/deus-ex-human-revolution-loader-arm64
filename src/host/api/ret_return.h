// Include after common.h / cf_bridge.h: makes RET/RET64/RETD also return from
// the thunk, so `if (x) RET(v);` is an early return. Every RET in the
// including file must therefore be the last action of its path.
#pragma once
#include "../thunk.h"
#undef RET
#undef RET64
#undef RETD
#define RET(v) do { r->ret = (uint32_t)(v); return; } while (0)
#define RET64(v) do { r->ret = (uint64_t)(v); return; } while (0)
#define RETD(v) do { r->fret = (double)(v); r->fp_ret = 1; return; } while (0)
