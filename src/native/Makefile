# Native host (no Wine): the recompiled engine plus the Win32 emulation layer, built for the host (Linux x86-64
# for now; the same sources target Android arm64 through the NDK).
CXX ?= g++
CC ?= gcc
OUT ?= ../../build/native
RECOMP_DIR ?= ../../build/recomp/all

CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -DRECOMP_FLAT_MEMORY -I../recomp
# Generated code: -O1 keeps the 220k-line parts quick to build; flat memory turns G() into plain pointers.
RECOMP_CFLAGS := -std=c11 -O1 -g0 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -w \
                 -DRECOMP_FLAT_MEMORY -I../recomp
LDFLAGS := -pthread

SRCS := main.cpp memory.cpp loader.cpp imports.cpp runtime.cpp thread.cpp kernel32.cpp msvcr80.cpp user32.cpp \
        advapi32.cpp misc.cpp
GEN := $(wildcard $(RECOMP_DIR)/*.c)
OBJS = $(SRCS:%.cpp=$(OUT)/%.o) $(GEN:$(RECOMP_DIR)/%.c=$(OUT)/gen/%.o)

$(OUT)/proto-native: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: %.cpp guest.h win.h ../recomp/runtime.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/gen/%.o: $(RECOMP_DIR)/%.c ../recomp/runtime.h ../recomp/runtime_fp.h
	@mkdir -p $(dir $@)
	$(CC) $(RECOMP_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: clean
