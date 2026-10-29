// Defaults tuned per device for the Android port. The engine is CPU bound, but phones differ in how much GPU work
// they can take for free: a flagship can render at 1080p with full-resolution textures while a 2019 mid-ranger
// needs 540-720p and dropped mip levels. These are only defaults: every value is overridden by protore.ini or a
// PROTORE_* environment variable. `device_profile = off` restores the old fixed defaults (720p, one mip level
// skipped, no frame cap).
//
// The profile is read once, from /sys/class/kgsl/kgsl-3d0/gpu_model (Adreno), the system properties
// (ro.soc.model / ro.board.platform / ro.product.model) and /proc/meminfo.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "guest.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <unistd.h>

namespace native {

namespace {

struct Preset {
    const char* tier;
    int render_height;
    int texture_skip_mips;
    int texture_skip_min_size;
    int fps_limit;
};

// Built-in profiles (ROADMAP "Performance"): resolution is the internal render height, the frame is scaled to the
// screen by DXVK. A 60 fps cap keeps a 120 Hz panel from running the CPU flat out; `fps_limit = 0` removes it.
constexpr Preset kLow{"low", 540, 1, 512, 0};
constexpr Preset kMedium{"medium", 720, 1, 512, 0};
constexpr Preset kHigh{"high", 900, 1, 512, 60};
constexpr Preset kFlagship{"flagship", 1080, 0, 512, 60};
constexpr Preset kDefault{"off", 720, 1, 512, 0};

#ifdef __ANDROID__
std::string ReadLine(const char* path) {
    char buf[256] = {};
    if (FILE* f = fopen(path, "r")) {
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
    }
    buf[strcspn(buf, "\r\n")] = 0;
    return buf;
}

std::string Property(const char* key) {
    char buf[PROP_VALUE_MAX] = {};
    if (__system_property_get(key, buf) <= 0) return "";
    return buf;
}

long MemoryMiB() {
    long kb = 0;
    if (FILE* f = fopen("/proc/meminfo", "r")) {
        if (fscanf(f, "MemTotal: %ld kB", &kb) != 1) kb = 0;
        fclose(f);
    }
    return kb / 1024;
}

// "Adreno840v2" -> 840, "Adreno 740" -> 740; 0 when the model is not an Adreno one.
int AdrenoNumber(const std::string& model) {
    size_t i = 0;
    while (i < model.size() && !isdigit((unsigned char)model[i])) ++i;
    int n = 0;
    while (i < model.size() && isdigit((unsigned char)model[i]) && n < 10000) n = n * 10 + (model[i++] - '0');
    return n;
}

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(tolower((unsigned char)ch));
    return s;
}

const Preset& Pick(const std::string& gpu, const std::string& soc_in, long ram_mib) {
    const std::string soc = Lower(soc_in);
    const bool adreno = gpu.find("Adreno") != std::string::npos || access("/dev/kgsl-3d0", F_OK) == 0;
    const int number = AdrenoNumber(gpu);
    // Find N6 (CPH2765): Snapdragon 8 Elite Gen 5 (SM8850), Adreno 840. Other Adreno 8xx parts get the same path.
    // The SoC name is a fallback for phones where /sys/class/kgsl is not readable.
    if (soc == "sm8850" || soc == "sm8750") return ram_mib >= 8 * 1024 ? kFlagship : kHigh;
    if (adreno && number >= 800 && ram_mib >= 8 * 1024) return kFlagship;
    // Adreno 740 and up are the flagships of their generation; 710-735 are the mid-range (they already struggle
    // with 720p on the CPU) and stay at Medium.
    if (soc == "sm8650" || soc == "sm8550") return ram_mib >= 8 * 1024 ? kHigh : kMedium;
    if (adreno && number >= 740 && ram_mib >= 8 * 1024) return kHigh;
    if (adreno && number >= 600) return kMedium;
    // Mali and other GPUs: the board name is all the port can see before the driver is up.
    if (gpu.find("Mali-G8") != std::string::npos || gpu.find("Xclipse") != std::string::npos) {
        return ram_mib >= 8 * 1024 ? kHigh : kMedium;
    }
    if (soc.find("sm8") == 0 || soc.find("mt69") == 0) return ram_mib >= 8 * 1024 ? kHigh : kMedium;
    if (ram_mib && ram_mib < 4 * 1024) return kLow;
    if (ram_mib >= 6 * 1024) return kMedium;
    return kDefault;
}
#endif

Preset Named(const std::string& name) {
    if (name == "low") return kLow;
    if (name == "medium") return kMedium;
    if (name == "high") return kHigh;
    if (name == "flagship") return kFlagship;
    return kDefault;
}

DeviceTuning Tuning(const Preset& p) {
    return DeviceTuning{p.tier, p.render_height, p.texture_skip_mips, p.texture_skip_min_size, p.fps_limit};
}

}  // namespace

const DeviceTuning& Device() {
    static DeviceTuning tuning;
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string mode = Setting("device_profile", "auto");
        if (mode != "auto" && !mode.empty()) {
            tuning = Tuning(Named(mode));
            Log("device: profile %s (device_profile = %s): render height %d, mip skip %d, fps limit %d", tuning.tier,
                mode.c_str(), tuning.render_height, tuning.texture_skip_mips, tuning.fps_limit);
            return;
        }
#ifdef __ANDROID__
        std::string gpu = ReadLine("/sys/class/kgsl/kgsl-3d0/gpu_model");
        if (gpu.empty()) gpu = ReadLine("/sys/class/misc/mali0/device/gpuinfo");
        const std::string soc = Property("ro.soc.model").empty() ? Property("ro.board.platform")
                                                                 : Property("ro.soc.model");
        const std::string model = Property("ro.product.model");
        const long ram = MemoryMiB();
        const Preset& p = Pick(gpu, soc, ram);
        tuning = Tuning(p);
        std::string spec = model.empty() ? "" : model + " ";
        if (!soc.empty()) spec += soc + " ";
        spec += gpu.empty() ? "unknown GPU" : gpu;
        Log("device: %s, %ld MiB RAM -> profile %s: render height %d, mip skip %d, fps limit %d", spec.c_str(), ram,
            tuning.tier, tuning.render_height, tuning.texture_skip_mips, tuning.fps_limit);
#else
        tuning = Tuning(kDefault);
#endif
    });
    return tuning;
}

}  // namespace native
