# Native host (no Wine): the recompiled engine plus the Win32 emulation layer, built for the host (Linux x86-64
# for now; the same sources target Android arm64 through the NDK).
CXX ?= g++
CC ?= gcc
OUT ?= ../../build/native
RECOMP_DIR ?= ../../build/recomp/all

# DXVK native (D3D9 on Vulkan): headers from its source tree, libdxvk_d3d9.so from its build.
DXVK ?= $(HOME)/proto-re-data/deps/dxvk
DXVK_LIB ?= $(DXVK)/build-native/src/d3d9
# FAudio (XAudio2 reimplementation, zlib license) for the engine's XAudio2 2.3 renderer.
FAUDIO ?= $(HOME)/proto-re-data/deps/FAudio
FAUDIO_LIB ?= $(FAUDIO)/build-native
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -DRECOMP_FLAT_MEMORY -I../recomp \
            -I$(DXVK)/include/native/directx -I$(DXVK)/include/native/windows -I$(DXVK)/include/native/wsi \
            -I$(FAUDIO)/include
# Generated code: -O1 keeps the 220k-line parts quick to build; flat memory turns G() into plain pointers.
RECOMP_CFLAGS := -std=c11 -O1 -g0 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -w \
                 -DRECOMP_FLAT_MEMORY -I../recomp
LDFLAGS := -pthread -L$(DXVK_LIB) -Wl,-rpath,$(DXVK_LIB) -ldxvk_d3d9 -L$(FAUDIO_LIB) -Wl,-rpath,$(FAUDIO_LIB) -lFAudio -lSDL2

SRCS := main.cpp memory.cpp loader.cpp imports.cpp runtime.cpp thread.cpp kernel32.cpp msvcr80.cpp user32.cpp \
        advapi32.cpp misc.cpp d3d9.cpp input.cpp xaudio2.cpp bink.cpp
GEN := $(wildcard $(RECOMP_DIR)/*.c)
OBJS = $(SRCS:%.cpp=$(OUT)/%.o) $(GEN:$(RECOMP_DIR)/%.c=$(OUT)/gen/%.o)

$(OUT)/proto-native: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: %.cpp guest.h win.h ../recomp/runtime.h d3d9_gen.inc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/gen/%.o: $(RECOMP_DIR)/%.c ../recomp/runtime.h ../recomp/runtime_fp.h
	@mkdir -p $(dir $@)
	$(CC) $(RECOMP_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: clean
