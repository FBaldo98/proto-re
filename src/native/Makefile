# Native host (no Wine): the recompiled engine plus the Win32 emulation layer.
#   make                         Linux x86-64 executable (build/native/proto-native)
#   make PLATFORM=android [ABI=arm64-v8a|x86_64]
#                                libmain.so for the APK (android/build-apk.sh), against the libraries from
#                                android/build-deps.sh
PLATFORM ?= linux
RECOMP_DIR ?= ../../build/recomp/all
DXVK ?= $(HOME)/proto-re-data/deps/dxvk
COMMON_FLAGS := -DRECOMP_FLAT_MEMORY -fsigned-char -I../recomp
# Generated code: -O1 keeps the large parts quick to build; flat memory turns G() into plain pointers.
RECOMP_CFLAGS = -std=c11 -O1 -g0 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -w $(COMMON_FLAGS) \
                $(TARGET_FLAGS)
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter $(COMMON_FLAGS) $(TARGET_FLAGS) \
           -I$(DXVK)/include/native/directx -I$(DXVK)/include/native/windows -I$(DXVK)/include/native/wsi \
           $(DEP_INCLUDES)

ifeq ($(PLATFORM),android)
ABI ?= arm64-v8a
NDK ?= $(HOME)/Android/Sdk/ndk/28.2.13676358
DEPS ?= $(HOME)/proto-re-data/android-deps/$(ABI)
TRIPLE := $(if $(filter x86_64,$(ABI)),x86_64,aarch64)-linux-android26
TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX := $(TOOLCHAIN)/$(TRIPLE)-clang++
CC := $(TOOLCHAIN)/$(TRIPLE)-clang
OUT ?= ../../build/android/$(ABI)
TARGET_FLAGS := -fPIC
DEP_INCLUDES := -I$(DEPS)/include
LDFLAGS := -shared -static-libstdc++ -Wl,--no-undefined -L$(DEPS)/lib -ldxvk_d3d9 -lFAudio -lSDL2 -llog -landroid
BINARY := libmain.so
else
CXX ?= g++
CC ?= gcc
OUT ?= ../../build/native
# DXVK native (D3D9 on Vulkan) and FAudio (XAudio2 reimplementation, zlib license), built in deps.
DXVK_LIB ?= $(DXVK)/build-native/src/d3d9
FAUDIO ?= $(HOME)/proto-re-data/deps/FAudio
FAUDIO_LIB ?= $(FAUDIO)/build-native
TARGET_FLAGS :=
DEP_INCLUDES := -I$(FAUDIO)/include
LDFLAGS := -pthread -L$(DXVK_LIB) -Wl,-rpath,$(DXVK_LIB) -ldxvk_d3d9 -L$(FAUDIO_LIB) -Wl,-rpath,$(FAUDIO_LIB) -lFAudio \
           -lSDL2
BINARY := proto-native
endif

SRCS := main.cpp memory.cpp loader.cpp imports.cpp runtime.cpp thread.cpp kernel32.cpp msvcr80.cpp user32.cpp \
        advapi32.cpp misc.cpp d3d9.cpp input.cpp xaudio2.cpp bink.cpp dinput.cpp
GEN := $(wildcard $(RECOMP_DIR)/*.c)
OBJS = $(SRCS:%.cpp=$(OUT)/%.o) $(GEN:$(RECOMP_DIR)/%.c=$(OUT)/gen/%.o)

$(OUT)/$(BINARY): $(OBJS)
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
