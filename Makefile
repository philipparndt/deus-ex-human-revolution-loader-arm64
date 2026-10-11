# dxhost build.
#
#   make            -> build/x86_64/dxhost (Rosetta backend, feasibility check)
#   make run        -> run Human Revolution with the Rosetta backend

HR_APP ?= games/hr/Deus Ex Human Revolution - Directors Cut.app

CC := clang
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Isrc/host
HOST_SRCS := $(wildcard src/host/*.c) $(wildcard src/host/api/*.c) $(wildcard src/host/api/*.m)

ROSETTA_DIR := build/x86_64
ROSETTA_OBJS := $(patsubst src/%,$(ROSETTA_DIR)/%.o,$(HOST_SRCS)) $(ROSETTA_DIR)/host/cpu_rosetta_x86_64.S.o
ROSETTA_FLAGS := -arch x86_64 -DCPU_BACKEND_ROSETTA
# Small __PAGEZERO and a high image base keep the low 4 GB free for the guest.
ROSETTA_LDFLAGS := -arch x86_64 -Wl,-no_pie -Wl,-pagezero_size,0x1000 -Wl,-image_base,0x200000000 \
	-Wl,-segaddr,__GUEST,0x4000 -Wl,-segprot,__GUEST,rwx,rwx \
	-framework CoreFoundation -framework Foundation -framework AppKit -framework OpenGL \
	-framework Carbon -framework IOKit -framework CoreAudio -framework AudioToolbox -framework OpenAL

all: $(ROSETTA_DIR)/dxhost

$(ROSETTA_DIR)/dxhost: $(ROSETTA_OBJS)
	$(CC) $(ROSETTA_LDFLAGS) $^ -o $@

$(ROSETTA_DIR)/%.c.o: src/%.c $(wildcard src/host/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(ROSETTA_FLAGS) $(CFLAGS) -c $< -o $@

$(ROSETTA_DIR)/%.m.o: src/%.m $(wildcard src/host/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(ROSETTA_FLAGS) $(CFLAGS) -fobjc-arc -c $< -o $@

$(ROSETTA_DIR)/%.S.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ROSETTA_FLAGS) -c $< -o $@

run: $(ROSETTA_DIR)/dxhost
	$(ROSETTA_DIR)/dxhost "$(HR_APP)"

clean:
	rm -rf build

.PHONY: all run clean
