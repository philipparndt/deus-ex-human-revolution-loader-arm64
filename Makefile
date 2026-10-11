# dxhost build.
#
#   make            -> build/x86_64/dxhost (Rosetta backend, feasibility check)
#   make run        -> run Human Revolution with the Rosetta backend

HR_APP ?= games/hr/Deus Ex Human Revolution - Directors Cut.app

CC := clang
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Isrc/host
# EXCLUDE: space-separated source paths to leave out (e.g. files under construction)
EXCLUDE ?=
HOST_SRCS := $(filter-out $(EXCLUDE),$(wildcard src/host/*.c) $(wildcard src/host/api/*.c) $(wildcard src/host/api/*.m))

ROSETTA_DIR ?= build/x86_64
ROSETTA_SRCS := $(filter-out src/host/cpu_unicorn.c src/host/cpu_dxcpu.c,$(HOST_SRCS))
ROSETTA_OBJS := $(patsubst src/%,$(ROSETTA_DIR)/%.o,$(ROSETTA_SRCS)) $(ROSETTA_DIR)/host/cpu_rosetta_x86_64.S.o
ROSETTA_FLAGS := -arch x86_64 -DCPU_BACKEND_ROSETTA
# Small __PAGEZERO and a high image base keep the low 4 GB free for the guest.
ROSETTA_LDFLAGS := -arch x86_64 -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000 \
	-Wl,-segaddr,__GUEST,0x4000 -Wl,-segprot,__GUEST,rwx,rwx \
	-framework CoreFoundation -framework Foundation -framework AppKit -framework OpenGL \
	-framework Carbon -framework IOKit -framework CoreAudio -framework AudioToolbox -framework OpenAL \
	-framework GameKit -framework StoreKit -framework AVFoundation -framework ScriptingBridge -lffi

GUEST_LIBS := $(ROSETTA_DIR)/guest/libstdc++-i386.a $(ROSETTA_DIR)/guest/libunwind-i386.a

all: $(ROSETTA_DIR)/dxhost $(GUEST_LIBS)

src/guest/libstdcxx/build/libstdc++-i386.a:
	$(MAKE) -C src/guest/libstdcxx

src/guest/libunwind/build/libunwind-i386.a:
	$(MAKE) -C src/guest/libunwind

$(ROSETTA_DIR)/guest/%.a: src/guest/libstdcxx/build/%.a
	@mkdir -p $(dir $@)
	cp $< $@

$(ROSETTA_DIR)/guest/libunwind-i386.a: src/guest/libunwind/build/libunwind-i386.a
	@mkdir -p $(dir $@)
	cp $< $@

$(ROSETTA_DIR)/dxhost: $(ROSETTA_OBJS)
	$(CC) $(ROSETTA_LDFLAGS) $^ -o $@

$(ROSETTA_DIR)/%.c.o: src/%.c $(wildcard src/host/*.h) $(wildcard src/host/api/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(ROSETTA_FLAGS) $(CFLAGS) -c $< -o $@

$(ROSETTA_DIR)/%.m.o: src/%.m $(wildcard src/host/*.h) $(wildcard src/host/api/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(ROSETTA_FLAGS) $(CFLAGS) -fno-objc-arc -Wno-deprecated-declarations -c $< -o $@

$(ROSETTA_DIR)/%.S.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ROSETTA_FLAGS) -c $< -o $@

# --- native arm64 (Unicorn backend) -----------------------------------------
NATIVE_DIR ?= build/arm64
UNICORN ?= /opt/homebrew/opt/unicorn
NATIVE_SRCS := $(filter-out src/host/cpu_rosetta.c src/host/cpu_dxcpu.c,$(HOST_SRCS))
NATIVE_OBJS := $(patsubst src/%,$(NATIVE_DIR)/%.o,$(NATIVE_SRCS))
NATIVE_FLAGS := -arch arm64 -DCPU_BACKEND_UNICORN -I$(UNICORN)/include
NATIVE_LDFLAGS := -arch arm64 -L$(UNICORN)/lib -lunicorn -Wl,-rpath,$(UNICORN)/lib \
	-framework CoreFoundation -framework Foundation -framework AppKit -framework OpenGL \
	-framework Carbon -framework IOKit -framework CoreAudio -framework AudioToolbox -framework OpenAL \
	-framework GameKit -framework StoreKit -framework AVFoundation -framework ScriptingBridge -lffi
NATIVE_GUEST_LIBS := $(NATIVE_DIR)/guest/libstdc++-i386.a $(NATIVE_DIR)/guest/libunwind-i386.a

native: $(NATIVE_DIR)/dxhost $(NATIVE_GUEST_LIBS)

$(NATIVE_DIR)/dxhost: $(NATIVE_OBJS)
	$(CC) $(NATIVE_LDFLAGS) $^ -o $@

$(NATIVE_DIR)/%.c.o: src/%.c $(wildcard src/host/*.h) $(wildcard src/host/api/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(NATIVE_FLAGS) $(CFLAGS) -c $< -o $@

$(NATIVE_DIR)/%.m.o: src/%.m $(wildcard src/host/*.h) $(wildcard src/host/api/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(NATIVE_FLAGS) $(CFLAGS) -fno-objc-arc -Wno-deprecated-declarations -c $< -o $@

$(NATIVE_DIR)/guest/libstdc++-i386.a: src/guest/libstdcxx/build/libstdc++-i386.a
	@mkdir -p $(dir $@)
	cp $< $@

$(NATIVE_DIR)/guest/libunwind-i386.a: src/guest/libunwind/build/libunwind-i386.a
	@mkdir -p $(dir $@)
	cp $< $@

# --- native arm64 (dxcpu backend: our own interpreter/JIT) -------------------
DXCPU_DIR ?= build/arm64-dxcpu
DXCPU_LIB := src/cpu/target/release/libdxcpu.a
DXCPU_SRCS := $(filter-out src/host/cpu_rosetta.c src/host/cpu_unicorn.c,$(HOST_SRCS))
DXCPU_OBJS := $(patsubst src/%,$(DXCPU_DIR)/%.o,$(DXCPU_SRCS))
DXCPU_FLAGS := -arch arm64 -DCPU_BACKEND_DXCPU

native-dxcpu: $(DXCPU_DIR)/dxhost $(DXCPU_DIR)/guest/libstdc++-i386.a $(DXCPU_DIR)/guest/libunwind-i386.a

$(DXCPU_LIB): $(wildcard src/cpu/src/*.rs) src/cpu/Cargo.toml
	cd src/cpu && cargo build --release

$(DXCPU_DIR)/dxhost: $(DXCPU_OBJS) $(DXCPU_LIB)
	$(CC) $(filter-out -lunicorn -Wl$(comma)-rpath$(comma)$(UNICORN)/lib -L$(UNICORN)/lib,$(NATIVE_LDFLAGS)) $^ -o $@

$(DXCPU_DIR)/%.c.o: src/%.c $(wildcard src/host/*.h) $(wildcard src/host/api/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(DXCPU_FLAGS) $(CFLAGS) -c $< -o $@

$(DXCPU_DIR)/%.m.o: src/%.m $(wildcard src/host/*.h) $(wildcard src/host/api/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(DXCPU_FLAGS) $(CFLAGS) -fno-objc-arc -Wno-deprecated-declarations -c $< -o $@

$(DXCPU_DIR)/guest/%.a: src/guest/libstdcxx/build/%.a
	@mkdir -p $(dir $@)
	cp $< $@

$(DXCPU_DIR)/guest/libunwind-i386.a: src/guest/libunwind/build/libunwind-i386.a
	@mkdir -p $(dir $@)
	cp $< $@

comma := ,

run: $(ROSETTA_DIR)/dxhost
	$(ROSETTA_DIR)/dxhost "$(HR_APP)"

clean:
	rm -rf build

.PHONY: all native native-dxcpu run clean
