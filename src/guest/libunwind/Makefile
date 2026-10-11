# LLVM libunwind for the i386 guest (provides _Unwind_* for C++ exceptions).
# Built as objects + static archive; dxhost links it at runtime.
SRC := ../../../third_party/libunwind-18.1.8.src
OUT := build
CC := clang
TARGET := -target i386-apple-macosx10.8
FLAGS := $(TARGET) -O2 -fno-stack-protector -funwind-tables -fvisibility=hidden \
	-I$(SRC)/include -D_LIBUNWIND_IS_NATIVE_ONLY -DNDEBUG -D_LIBUNWIND_DISABLE_VISIBILITY_ANNOTATIONS=0 \
	-femit-dwarf-unwind=always
CXXFLAGS := $(FLAGS) -std=c++17 -fno-exceptions -fno-rtti -nostdinc++ -I$(SRC)/src
CFLAGS := $(FLAGS) -std=c99 -I$(SRC)/src

OBJS := $(OUT)/libunwind.o $(OUT)/UnwindLevel1.o $(OUT)/UnwindLevel1-gcc-ext.o \
	$(OUT)/UnwindRegistersSave.o $(OUT)/UnwindRegistersRestore.o

all: $(OUT)/libunwind-i386.a

$(OUT)/libunwind-i386.a: $(OBJS)
	rm -f $@
	ZERO_AR_DATE=1 ar rcs $@ $^

$(OUT)/%.o: $(SRC)/src/%.cpp
	@mkdir -p $(OUT)
	$(CC) $(CXXFLAGS) -c $< -o $@

$(OUT)/%.o: $(SRC)/src/%.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: $(SRC)/src/%.S
	@mkdir -p $(OUT)
	$(CC) $(FLAGS) -c $< -o $@

clean:
	rm -rf $(OUT)
.PHONY: all clean
