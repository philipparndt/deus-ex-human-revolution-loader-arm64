# i386 guest libstdc++ (Apple GCC 4.2.1 ABI)

A static i386 Mach-O archive providing what the game imports from
`/usr/lib/libstdc++.6.dylib`: the old COW `std::string`, iostreams, locale,
RB-tree/list helpers, typeinfo/vtable data, `__cxa_*`, `__gxx_personality_v0`.
Our loader links it into the guest at runtime; nothing here is linked with
`ld` (Apple ld cannot link i386 anymore).

```
make            # -> build/libstdc++-i386.a, UNDEFINED.txt, build/libstdc++-i386.exp
make check      # -> ./check-exports.sh
./fetch-sources.sh   # re-create third_party/apple-libstdcxx from GitHub
```

Output is bit-reproducible (`ZERO_AR_DATE=1`, fixed member order).

## Sources

* **libstdc++ proper: Apple `libstdcxx-60`** (the OS X 10.9 library) — all 65
  files of `src/` that Apple's Xcode project compiles, except the 12 files that
  only duplicate C++-ABI runtime (`eh_exception.cc eh_terminate.cc
  new_handler.cc tinfo.cc new_op*.cc del_op*.cc`). Headers are the tree's
  pre-configured `include/c++/4.2.1` (its `c++config.h`, `gthr-default.h` =
  gthr-posix, `os_defines.h`, `c++locale.h` are used as-is; no configure step).
  Locale model is the one Apple shipped: *generic* with the *darwin*
  `ctype_members.cc`.
* **libsupc++: Apple `libstdcxx-39`** (GCC 4.2.1 as patched by Apple, the
  OS X 10.6 runtime) — the 27 files of `libsupc++/Makefile.am` (minus the ARM
  EHABI-only `eh_arm.cc`) plus libiberty `cp-demangle.c` (`__cxa_demangle`),
  built with `-DIN_GLIBCPP_V3` exactly like GCC's Makefile.

### Why libsupc++ and not libc++abi (ABI-relevant decision)

Since 10.7 Apple built `libstdc++.6.dylib` with `_LIBCPPABI_VERSION` defined:
`__cxa_*`, the personality routine, `std::exception`/`bad_alloc`/`type_info`,
`operator new/delete`, `std::logic_error::~logic_error` etc. were taken from
**libc++abi**, and the `__cxxabiv1::*_type_info` classes had libc++abi's
internal vtable layout. We build in the self-contained mode instead
(`_LIBCPPABI_VERSION` undefined, `cxxabi.h` from libsupc++), i.e. the same
symbols are implemented by GCC 4.2.1's libsupc++ as in 10.6.

The *public* ABI is identical: same mangled names, same object layouts the game
can see (vptr + `__name`; `__class_type_info` etc. objects emitted by the
compiler only reference our vtable symbols), Itanium C++ EH ABI, same 64-bit
guard object with "first byte != 0 means initialised". Things that differ but are
private to the runtime: layout of `__cxa_exception`/`__cxa_eh_globals`, the
order/number of internal virtuals in the `type_info` vtables (GCC 4.2:
`__is_pointer_p, __is_function_p, __do_catch, __do_upcast`, see
`libsupc++/typeinfo`), the default terminate handler
(`__gnu_cxx::__verbose_terminate_handler` messages instead of libc++abi's). The
game never touches those directly. libc++abi's `stdexcept.cpp` would also need
`dlopen("/usr/lib/libstdc++.6.dylib")` for its COW-string interop, which this
avoids.

Coverage check: every one of the 3267 symbols in Apple's own 10.9 i386 export
list (`libstdcxx-60/exports/libstdcxx-i386.exp`) is defined by the archive.

## Flags

```
clang++ -target i386-apple-macosx10.8 -isysroot <SDK> -O2 -g0 -std=gnu++98 -nostdinc++
        -fexceptions -frtti -fno-stack-protector -fno-common -fno-omit-frame-pointer
        -femit-dwarf-unwind=always -DBUILDING_LIBSTDCXX=1
libsupc++ additionally: -fno-implicit-templates -D_DTRACE_CXA_RUNTIME_H=1
concept-inst.cc: -D_GLIBCXX_CONCEPT_CHECKS (as in Apple's Xcode project)
```

* `size_t` = `unsigned long` (`m`), `ptrdiff_t`/`streamsize` = `int` (`i`),
  `wchar_t` 4 bytes, `long double` 80-bit x87 in 16 bytes — the i386 Darwin ABI,
  matching the `i` mangling in Apple's i386 export list.
* `-fno-stack-protector`: GCC 4.2 did not use it; avoids `___stack_chk_*`.
* `-femit-dwarf-unwind=always`: every function gets a DWARF FDE in `__eh_frame`
  in addition to `__LD,__compact_unwind`. The loader has no ld to turn compact
  unwind into `__unwind_info`, so it can simply register the `__eh_frame`
  sections (e.g. `__register_frame`) with the guest unwinder.
* `-O2` instead of Apple's `-Os`: no ABI effect.
* No `-fvisibility-inlines-hidden` (Apple: `GCC_INLINES_ARE_PRIVATE_EXTERN=NO`).

## Build fixes (no ABI effect)

* `patches/eh_call-clang-prototype.patch`: `__cxa_call_terminate` is declared
  `(void*)` in `unwind-cxx.h` but defined `(_Unwind_Exception*)`; GCC accepted
  it, clang does not.
* libsupc++ uses GCC's own `unwind.h` (`gcc/unwind-generic.h`, copied to
  `build/include-supc/unwind.h`) instead of the SDK's libunwind header.
* `bits/cxxabi_tweaks.h` (generic CPU) and `cxxabi.h` are copied into
  `build/include/` as GCC's configure would install them.
* dtrace USDT probes in `__cxa_throw`/`__cxa_rethrow` (`os_defines.h`) are
  compiled out: they are pseudo-symbols that only Apple's ld resolves.

## Notes for the loader

* `UNDEFINED.txt` (95 symbols): what libc/libSystem must provide —
  stdio/string/wchar/locale/ctype (`___maskrune`, `__DefaultRuneLocale`,
  `___mb_cur_max`, `$UNIX2003`/`$INODE64` variants as for a 10.8 i386 target),
  malloc/free/realloc, pthread key/once/mutex lock+unlock, `_Unwind_*`,
  `___cxa_atexit`, `___udivdi3` (compiler-rt), `___assert_rtn`, `___error`,
  and **`___dso_handle`**, which ld normally synthesises (address of the image's
  mach header; the loader must provide a per-image value).
* Mutexes are statically initialised (`PTHREAD_MUTEX_INITIALIZER` /
  `PTHREAD_RECURSIVE_MUTEX_INITIALIZER`, i386 `pthread_mutex_t` = 44 bytes with
  `__sig` 0x32AAABA7 / 0x32AAABA2, `pthread_once_t` 0x30B1BCBA); the host
  pthread layer must accept them.
  eh_globals uses `pthread_key_create`/`pthread_getspecific`.
* The archive exports more globals than the dylib did (~480 internal helpers and
  explicit instantiations). `build/libstdc++-i386.exp` is the real dylib export
  list; resolve the game's `libstdc++` imports only against that set if you want
  dylib-exact scoping. `weak private external` symbols (e.g.
  `___clang_call_terminate`) are per-object and must not be exported.
* Weak bits: the real dylib forced `__ZTV* __ZTT* __ZTC* __ZS* __ZN*` non-weak
  and the std exception `typeinfo`s weak (`exports/notweak.exp`, `weak.exp`).
  Our objects carry clang's natural weak bits (template instantiations weak,
  e.g. `__ZNSs4_Rep20_S_empty_rep_storageE`). Only matters if the loader
  implements weak-definition coalescing against the game's own weak symbols.
* Static constructors exist in `bitmap_allocator.o`, `locale-inst.o`,
  `wlocale-inst.o`, `mt_allocator.o` and `eh_globals.o`
  (`__DATA,__mod_init_func`); the loader must run them. `ios_base::Init` is
  constructed by the game's own TUs (it imports `Init::Init`).
