# Writing API thunks

The game is a 32-bit (i386) macOS program. It runs as guest code inside the
64-bit `dxhost` process. Every function it imports from a system library is
implemented in `src/host/api/*.c` (or `.m`) as a *thunk*: a host function that
reads the guest's arguments, calls the real 64-bit API and returns the result in
guest terms.

## Ground rules

- **Guest memory is not host memory.** A guest pointer is a 32-bit `gaddr_t`.
  Convert it with `g2h()` before dereferencing on the host, and convert host
  pointers into guest memory back with `h2g()`. With the Rosetta backend the
  two are the same, but natively on arm64 there is an offset. Code that skips
  the conversion will break later.
- **Host pointers never reach the guest.** If an API returns a pointer to host
  memory:
  - copy the data into guest memory (`gheap_alloc`, `gheap_strdup`,
    `guest_static_string`), or
  - for opaque objects (CF/ObjC objects, dispatch queues, …), hand out a
    handle with `hobj_wrap()` and turn it back with `hobj_unwrap()`.
- **The ABI is Darwin i386:**
  - `int`, `long`, `size_t` and pointers are 4 bytes; `long long` and `off_t`
    are 8 bytes and take two argument slots (`ARG64(i)`).
  - `double` arguments also take two slots (`ARGD(i)`), and `float` arguments
    take one (`ARGF(i)`).
  - Integer results go in `RET(x)`, or `RET64(x)` for 64-bit results (edx:eax).
  - `float` and `double` results go through `RETD(x)` (they are returned in
    st(0)).
  - Structs larger than 8 bytes are returned through a hidden first argument:
    write the result there, `RET(hidden_ptr)`, and set `r->pop = 4` (the
    callee pops the hidden pointer).
  - Structs of 8 bytes or less are returned in edx:eax (`RET64`).
  - 64-bit fields inside structs only need 4-byte alignment.
- **Struct layouts differ.** Anything containing `long`, pointers, `size_t` or
  `time_t` has a different layout. Convert explicitly; `common.h` has helpers
  for `timeval`, `timespec` and `tm`.
- **errno:** use `RET_ERRNO(expr)` for POSIX calls, or `set_gerrno(e)`.
- **Callbacks into the guest** use `cpu_call()` / `gcall1..3()`.
- **Varargs:** `GVA_FROM_ARG(i)` / `gva_at(va_list_ptr)` with
  `gfmt_vsnprintf` / `gfmt_vsscanf`.

## Defining thunks

```c
#include "common.h"

THUNK(strlen) { RET(strlen(ARGS(0))); }                 // symbol _strlen
THUNK_AS(fopen_unix, "_fopen$UNIX2003") { ... }         // explicit symbol name
static void init_x(gaddr_t a) { gwrite32(a, 42); }
GDATA(some_global, 4, init_x);                          // data symbol _some_global
```

Registration is automatic (linker section `__DATA,__dxsyms`). Duplicate names are
reported at startup: `grep -rn '"_name"\|THUNK(name)' src/host/api` before adding.

## Testing

Write a small C test in `tests/guest/<area>_<topic>.c`, compile it for i386 and run
it under the harness:

```sh
make
clang -target i386-apple-macosx10.8 -O1 -c tests/guest/foo.c -o build/guest-tests/foo.o
build/x86_64/dxhost --test build/guest-tests/foo.o      # DXH_LOG=3 traces every thunk call
```

Calling a function nobody implements aborts with
`guest called unimplemented function _name`.
