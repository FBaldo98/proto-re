# Builds the hook host as a 32-bit Windows binkw32.dll proxy.
CXX := i686-w64-mingw32-g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti
LDFLAGS := -shared -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup

OUT := ../../build/host

# Each module directory under src/ (relative to this Makefile) and its sources.
RECOMP_SRCS := bridge_x86.cpp
HOST_SRCS := dllmain.cpp hooks.cpp registry.cpp difftest.cpp probes.cpp memwatch.cpp world_stop.cpp
CORE_SRCS := report.cpp name_hash.cpp cement_library.cpp cement_file_range.cpp memory.cpp file_system.cpp mutex.cpp event.cpp file.cpp request.cpp
MATH_SRCS := vector_trace.cpp
TEST_SRCS := tests.cpp report_capture.cpp report_test.cpp vector_trace_test.cpp name_hash_test.cpp cement_library_test.cpp cement_file_range_test.cpp cement_state_test.cpp memory_test.cpp cement_load_test.cpp cement_lifetime_test.cpp file_test.cpp

OBJS := $(HOST_SRCS:%.cpp=$(OUT)/host/%.o) \
        $(CORE_SRCS:%.cpp=$(OUT)/core/%.o) \
        $(MATH_SRCS:%.cpp=$(OUT)/math/%.o) \
        $(TEST_SRCS:%.cpp=$(OUT)/tests/%.o) \
        $(RECOMP_SRCS:%.cpp=$(OUT)/recomp/%.o) \
        $(OUT)/recomp/recomp_gen.o

HEADERS := $(wildcard *.h ../core/*.h ../math/*.h ../tests/*.h)

$(OUT)/binkw32.dll: $(OBJS) binkw32.def
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) binkw32.def

$(OUT)/host/%.o: %.cpp $(HEADERS) | $(OUT)/host
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/%.o: ../%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/host:
	mkdir -p $@

# Statically recompiled engine code (tools/recomp/recomp.py). SSE math keeps the host x87 stack free for the
# engine's float return values.
CC := i686-w64-mingw32-gcc
RECOMP_CFLAGS := -std=c11 -O2 -msse2 -mfpmath=sse -fno-strict-aliasing -Wno-unused-label -I../recomp
$(OUT)/recomp/recomp_gen.o: ../../build/recomp/recomp_gen.c ../recomp/runtime.h ../recomp/runtime_fp.h
	@mkdir -p $(dir $@)
	$(CC) $(RECOMP_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: clean
