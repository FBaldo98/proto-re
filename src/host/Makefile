# Builds the hook host as a 32-bit Windows binkw32.dll proxy.
CXX := i686-w64-mingw32-g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti
LDFLAGS := -shared -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup

OUT := ../../build/host
SRCS := dllmain.cpp hooks.cpp registry.cpp
CORE_SRCS := report.cpp
MATH_SRCS := vector_trace.cpp
OBJS := $(SRCS:%.cpp=$(OUT)/%.o) $(CORE_SRCS:%.cpp=$(OUT)/core/%.o) $(MATH_SRCS:%.cpp=$(OUT)/math/%.o)

$(OUT)/binkw32.dll: $(OBJS) binkw32.def
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) binkw32.def

$(OUT)/%.o: %.cpp hooks.h registry.h | $(OUT)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/core/%.o: ../core/%.cpp ../core/%.h hooks.h | $(OUT)/core
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/math/%.o: ../math/%.cpp ../math/%.h hooks.h | $(OUT)/math
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT) $(OUT)/core $(OUT)/math:
	mkdir -p $@

clean:
	rm -rf $(OUT)

.PHONY: clean
