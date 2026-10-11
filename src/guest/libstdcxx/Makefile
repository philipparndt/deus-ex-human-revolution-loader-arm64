# Builds an i386 static archive of Apple's GCC-4.2.1-based libstdc++
# (macOS 10.9 / libstdcxx-60) plus GCC 4.2.1 libsupc++ (Apple libstdcxx-39).
#
# Nothing is linked (Apple ld cannot link i386); the archive is meant to be
# linked into the guest at runtime by our own loader.
#
#   make            -> build/libstdc++-i386.a, build/UNDEFINED.txt (+ copy)
#   make check      -> verify all game imports are defined
#   make clean

HERE      := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT      := $(abspath $(HERE)/../../..)
TP        := $(ROOT)/third_party/apple-libstdcxx
STDCXX    := $(TP)/libstdcxx-60
SUPCXX_TP := $(TP)/libstdcxx-39-libsupc++
SUPCXX    := $(SUPCXX_TP)/libstdc++-v3/libsupc++
BUILD     := $(HERE)/build

CC        := xcrun clang
CXX       := xcrun clang++
AR        := xcrun ar
RANLIB    := xcrun ranlib
NM        := xcrun nm
SDKROOT   ?= $(shell xcrun --sdk macosx --show-sdk-path)

# Code generation: match Apple's 2013 build of libstdc++.6.dylib for i386 as
# far as it is ABI relevant (i386 SysV/Darwin ABI, 16-byte stack alignment,
# SSE3 baseline = clang's default for i386-apple-macosx, 80-bit long double).
TARGET    := -target i386-apple-macosx10.8 -isysroot $(SDKROOT)
CGFLAGS   := $(TARGET) -O2 -g0 \
             -fno-stack-protector \
             -fno-common \
             -femit-dwarf-unwind=always \
             -fno-omit-frame-pointer
WARN      := -w

# libstdc++ proper (10.9 sources). BUILDING_LIBSTDCXX is set by Apple's Xcode
# project.  _LIBCPPABI_VERSION stays undefined (we provide libsupc++ ourselves
# rather than libc++abi), see README.md.
STDCXX_CXXFLAGS := $(CGFLAGS) $(WARN) -std=gnu++98 -nostdinc++ \
             -fexceptions -frtti \
             -DBUILDING_LIBSTDCXX=1 \
             -I$(BUILD)/include \
             -I$(STDCXX)/include/c++/4.2.1 \
             -I$(STDCXX)/include/c++/4.2.1/backward

# libsupc++ (GCC 4.2.1 as patched by Apple). Its own headers (typeinfo,
# exception, new, cxxabi.h, tinfo.h) come first, exactly like the GCC build.
# _DTRACE_CXA_RUNTIME_H pre-empts the inline dtrace probe header from
# bits/os_defines.h: the probes are ld-resolved pseudo symbols
# (___dtrace_probe$...) that only Apple's static linker understands; with the
# guard set, eh_throw.cc compiles them out via its #ifdef CXA_RUNTIME_*.
SUPCXX_CXXFLAGS := $(CGFLAGS) $(WARN) -std=gnu++98 -nostdinc++ \
             -fexceptions -frtti -fno-implicit-templates \
             -DBUILDING_LIBSTDCXX=1 \
             -D_DTRACE_CXA_RUNTIME_H=1 \
             -I$(SUPCXX) \
             -I$(BUILD)/include-supc \
             -I$(BUILD)/include \
             -I$(STDCXX)/include/c++/4.2.1 \
             -I$(SUPCXX_TP)/gcc

DEMANGLE_CFLAGS := $(CGFLAGS) $(WARN) -std=gnu89 \
             -DIN_GLIBCPP_V3 -DHAVE_STDLIB_H -DHAVE_STRING_H \
             -I$(SUPCXX_TP)/libiberty -I$(SUPCXX_TP)/include

# Files of libstdcxx-60/src that are only libc++abi-era duplicates of
# libsupc++ functionality; the libsupc++ originals are used instead.
STDCXX_EXCLUDE := eh_exception.cc eh_terminate.cc new_handler.cc tinfo.cc \
             new_op.cc new_opnt.cc new_opv.cc new_opvnt.cc \
             del_op.cc del_opnt.cc del_opv.cc del_opvnt.cc

STDCXX_SRCS := allocator-inst.cc atomicity.cc basic_file.cc bitmap_allocator.cc \
	c++locale.cc codecvt.cc codecvt_members.cc collate_members.cc \
	compatibility-ldbl.cc compatibility.cc complex_io.cc concept-inst.cc \
	ctype.cc ctype_members.cc debug.cc debug_list.cc ext-inst.cc \
	fstream-inst.cc functexcept.cc globals_io.cc ios-inst.cc ios.cc \
	ios_failure.cc ios_init.cc ios_locale.cc iostream-inst.cc istream-inst.cc \
	istream.cc limits.cc list.cc locale-inst.cc locale.cc locale_facets.cc \
	locale_init.cc localename.cc messages_members.cc misc-inst.cc \
	monetary_members.cc mt_allocator.cc numeric_members.cc ostream-inst.cc \
	pool_allocator.cc sstream-inst.cc stdexcept.cc streambuf-inst.cc \
	streambuf.cc string-inst.cc strstream.cc time_members.cc tree.cc \
	valarray-inst.cc wlocale-inst.cc wstring-inst.cc

SUPCXX_SRCS := del_op.cc del_opnt.cc del_opv.cc del_opvnt.cc eh_alloc.cc \
	eh_aux_runtime.cc eh_call.cc eh_catch.cc eh_exception.cc \
	eh_globals.cc eh_personality.cc eh_term_handler.cc eh_terminate.cc \
	eh_throw.cc eh_type.cc eh_unex_handler.cc guard.cc new_handler.cc \
	new_op.cc new_opnt.cc new_opv.cc new_opvnt.cc pure.cc tinfo.cc \
	tinfo2.cc vec.cc vterminate.cc

STDCXX_OBJS := $(addprefix $(BUILD)/obj/stdcxx/,$(STDCXX_SRCS:.cc=.o))
SUPCXX_OBJS := $(addprefix $(BUILD)/obj/supcxx/,$(SUPCXX_SRCS:.cc=.o))
DEMANGLE_OBJ := $(BUILD)/obj/supcxx/cp-demangle.o
OBJS := $(STDCXX_OBJS) $(SUPCXX_OBJS) $(DEMANGLE_OBJ)

LIB := $(BUILD)/libstdc++-i386.a
GEN_HEADERS := $(BUILD)/include/cxxabi.h $(BUILD)/include/bits/cxxabi_tweaks.h \
               $(BUILD)/include-supc/unwind.h

.PHONY: all check clean
all: $(LIB) $(HERE)/UNDEFINED.txt $(BUILD)/libstdc++-i386.exp

# The configure step of GCC would install the generic cxxabi_tweaks.h as
# bits/cxxabi_tweaks.h and expose libsupc++'s cxxabi.h; mirror that.
$(BUILD)/include/cxxabi.h: $(SUPCXX)/cxxabi.h
	@mkdir -p $(dir $@)
	cp $< $@
$(BUILD)/include/bits/cxxabi_tweaks.h: $(SUPCXX_TP)/libstdc++-v3/config/cpu/generic/cxxabi_tweaks.h
	@mkdir -p $(dir $@)
	cp $< $@

# GCC's own unwind.h (generated from gcc/unwind-generic.h), used by libsupc++
# only; the SDK's libunwind header lacks _Unwind_Ptr & co.
$(BUILD)/include-supc/unwind.h: $(SUPCXX_TP)/gcc/unwind-generic.h
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/obj/stdcxx/concept-inst.o: EXTRA := -D_GLIBCXX_CONCEPT_CHECKS
$(BUILD)/obj/stdcxx/%.o: $(STDCXX)/src/%.cc $(GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(STDCXX_CXXFLAGS) $(EXTRA) -c $< -o $@

$(BUILD)/obj/supcxx/%.o: $(SUPCXX)/%.cc $(GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(SUPCXX_CXXFLAGS) -c $< -o $@

# Sources needing a (non-ABI) build fix for clang: copied + patched in build/.
$(BUILD)/patched/eh_call.cc: $(SUPCXX)/eh_call.cc $(HERE)/patches/eh_call-clang-prototype.patch
	@mkdir -p $(dir $@)
	cp $< $@
	patch -s -d $(dir $@) -p1 < $(HERE)/patches/eh_call-clang-prototype.patch

$(BUILD)/obj/supcxx/eh_call.o: $(BUILD)/patched/eh_call.cc $(GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(SUPCXX_CXXFLAGS) -c $< -o $@

$(DEMANGLE_OBJ): $(SUPCXX_TP)/libiberty/cp-demangle.c
	@mkdir -p $(dir $@)
	$(CC) $(DEMANGLE_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	@rm -f $@
	ZERO_AR_DATE=1 $(AR) rcs $@ $(OBJS)
	ZERO_AR_DATE=1 $(RANLIB) $@

# Exported-symbol list of the real 10.9 i386 dylib (for loader scoping).
$(BUILD)/libstdc++-i386.exp: $(STDCXX)/exports/libstdcxx-i386.exp
	grep -v '^#' $< | grep -v '^[[:space:]]*$$' | sort -u > $@

# Undefined symbols that are not satisfied inside the archive.
$(HERE)/UNDEFINED.txt: $(LIB)
	$(NM) -g -j -U $(LIB) 2>/dev/null | grep -v ':$$' | grep -v '^$$' | sort -u > $(BUILD)/defined.txt
	$(NM) -g -j -u $(LIB) 2>/dev/null | grep -v ':$$' | grep -v '^$$' | sort -u > $(BUILD)/undefined-all.txt
	comm -23 $(BUILD)/undefined-all.txt $(BUILD)/defined.txt > $@

check: all
	$(HERE)/check-exports.sh

clean:
	rm -rf $(BUILD)
