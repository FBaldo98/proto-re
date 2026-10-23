// Vulkan driver selection on Android. DXVK needs Vulkan features that Qualcomm's own Adreno drivers often lack
// (robustness2 null descriptors, extended dynamic state, ...), so on Adreno the port loads Mesa's Turnip driver
// shipped in the APK, through libadrenotools (which gets a driver past Android's linker namespaces), and hands its
// vkGetInstanceProcAddr to DXVK (DXVK_VK_GIPA, android/patches/dxvk-android.patch). Turnip is used only if it
// finds the GPU; otherwise, and on other GPUs (Mali, PowerVR, Xclipse), the system driver stays.
// Setting vulkan_driver: auto (default) | system | turnip.
#include "guest.h"

#if defined(__ANDROID__) && defined(PROTORE_ADRENOTOOLS)
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// libadrenotools is built for API 29, where libc exports android_get_device_api_level; libmain targets API 26,
// whose headers only have an inline version, so the Makefile points the symbol here (--defsym).
extern "C" int protore_device_api_level() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("ro.build.version.sdk", value);
    return atoi(value);
}

namespace native {

namespace {

// Creates a throwaway instance to see which GPU the driver drives; returns the device name ("" if none) and puts a
// line for the log in `report` (the probe may run in a forked child, which must not touch the log).
std::string ProbeDevice(PFN_vkGetInstanceProcAddr gipa, const char* driver, std::string& report) {
    char line[256];
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) {
        snprintf(line, sizeof(line), "vulkan: %s has no vkCreateInstance", driver);
        report = line;
        return "";
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (VkResult r = create(&ci, nullptr, &instance); r != VK_SUCCESS) {
        snprintf(line, sizeof(line), "vulkan: %s vkCreateInstance -> %d", driver, r);
        report = line;
        return "";
    }
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    std::string name;
    uint32_t count = 1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkResult r = enumerate ? enumerate(instance, &count, &device) : VK_ERROR_INITIALIZATION_FAILED;
    if (props && r >= 0 && count && device) {
        VkPhysicalDeviceProperties p;
        props(device, &p);
        name = p.deviceName;
        snprintf(line, sizeof(line), "vulkan: %s sees %s (Vulkan %u.%u.%u, driver %08x, vendor %04x device %08x)",
                 driver, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                 VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID);
    } else {
        snprintf(line, sizeof(line), "vulkan: %s vkEnumeratePhysicalDevices -> %d, %u device(s)", driver, r, count);
    }
    report = line;
    if (destroy) destroy(instance, nullptr);
    return name;
}

// Logs the system driver's GPU, for reports from phones where Turnip is not used.
void LogSystemDevice() {
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
    if (!gipa) return;
    std::string report;
    ProbeDevice(gipa, "system driver", report);
    Log("%s", report.c_str());
}

// Loads Turnip through adrenotools (in the libvulkan.so loader it returns, Turnip replaces the system driver) and
// checks that the device really is Turnip: if Turnip itself fails to load, the adrenotools hook quietly falls back
// to the system driver, and a second copy of that driver in the process must not be used.
PFN_vkGetInstanceProcAddr LoadTurnip(const std::string& dir, std::string& device, std::string& report) {
    // adrenotools joins the directory and the file name as is, so the directories need the trailing slash.
    void* lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, dir.c_str(), dir.c_str(),
                                           "libvulkan_freedreno.so", nullptr, nullptr);
    if (!lib) {
        const char* err = dlerror();
        report = std::string("vulkan: adrenotools could not load Turnip (") + (err ? err : "no dlerror") + ")";
        return nullptr;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) {
        report = "vulkan: the adrenotools loader has no vkGetInstanceProcAddr";
        return nullptr;
    }
    device = ProbeDevice(gipa, "Turnip", report);
    if (device.find("Turnip") == std::string::npos) {
        report += device.empty() ? "" : " (not Turnip: the driver did not load)";
        device.clear();
        return nullptr;
    }
    return gipa;
}

// Runs LoadTurnip in a forked child first, so that a driver that crashes or hangs on this phone costs a second of
// startup instead of the game. Returns true if the child found Turnip; its log line comes back through a pipe.
bool TurnipWorksInChild(const std::string& dir) {
    int fds[2];
    if (pipe(fds) != 0) return true;  // cannot test: try in-process
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return true;
    }
    if (pid == 0) {
        close(fds[0]);
        alarm(10);
        std::string device, report;
        bool ok = LoadTurnip(dir, device, report) != nullptr;
        (void)!write(fds[1], report.data(), report.size());
        _exit(ok ? 0 : 3);
    }
    close(fds[1]);
    std::string report;
    char buf[256];
    for (int waited = 0; waited < 8000;) {
        pollfd p = {fds[0], POLLIN, 0};
        int n = poll(&p, 1, 100);
        waited += 100;
        if (n > 0) {
            ssize_t got = read(fds[0], buf, sizeof(buf));
            if (got <= 0) break;  // child closed the pipe: done
            report.append(buf, size_t(got));
        }
    }
    close(fds[0]);
    int status = 0;
    pid_t done = 0;
    for (int i = 0; i < 20 && (done = waitpid(pid, &status, WNOHANG)) == 0; ++i) usleep(50000);
    if (done == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        Log("vulkan: Turnip test hung, using the system driver");
        return false;
    }
    if (!report.empty()) Log("%s (test process)", report.c_str());
    if (WIFSIGNALED(status)) {
        Log("vulkan: Turnip test crashed (signal %d), using the system driver", WTERMSIG(status));
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Crash guard (working directory = the app's files folder): turnip_pending exists while a Turnip run has not yet
// reached VulkanDriverConfirm. If it is still there at the next start, that run crashed or hung during startup (its
// log is proto-native.prev.log): turnip_failed is written and auto mode stays on the system driver for this build;
// vulkan_driver = turnip still forces Turnip. A failed test in the child process is not recorded: it costs a second
// and is retried (and logged) at every start.
constexpr const char* kPending = "turnip_pending";
constexpr const char* kFailed = "turnip_failed";
constexpr const char* kBuild = __DATE__ " " __TIME__;  // a failure recorded by another build is retried
bool g_turnip_pending = false;

void MarkFailed(const char* reason) {
    if (FILE* f = fopen(kFailed, "w")) {
        fprintf(f, "%s\n%s\n", kBuild, reason);
        fclose(f);
    }
}

// The recorded reason if Turnip failed with this build, "" otherwise.
std::string FailedBefore() {
    FILE* f = fopen(kFailed, "r");
    if (!f) return "";
    char build[64] = {}, reason[256] = {};
    fgets(build, sizeof(build), f);
    fgets(reason, sizeof(reason), f);
    fclose(f);
    build[strcspn(build, "\n")] = 0;
    reason[strcspn(reason, "\n")] = 0;
    if (strcmp(build, kBuild) != 0) return "";
    return reason[0] ? reason : "unknown";
}

}  // namespace

void VulkanDriverConfirm() {
    if (!g_turnip_pending) return;
    g_turnip_pending = false;
    unlink(kPending);
    Log("vulkan: Turnip startup ok");
}

void SelectVulkanDriver() {
    const std::string mode = Setting("vulkan_driver", "auto");
    const bool adreno = access("/dev/kgsl-3d0", F_OK) == 0;  // Qualcomm's GPU kernel driver
    if (mode == "system" || (mode == "auto" && !adreno)) {
        LogSystemDevice();
        return;
    }
    if (access(kPending, F_OK) == 0) {
        unlink(kPending);
        MarkFailed("the game crashed or hung during startup on Turnip (see proto-native.prev.log)");
        Log("vulkan: the last Turnip run ended during startup");
    }
    if (mode == "turnip") {
        unlink(kFailed);
    } else if (std::string why = FailedBefore(); !why.empty()) {
        Log("vulkan: Turnip failed here before: %s; using the system driver (vulkan_driver = turnip forces Turnip)",
            why.c_str());
        LogSystemDevice();
        return;
    }
    const char* libdir = getenv("PROTORE_NATIVE_LIB_DIR");  // set by ProtoActivity: the APK's extracted libraries
    if (!libdir || access((std::string(libdir) + "/libvulkan_freedreno.so").c_str(), F_OK) != 0) {
        Log("vulkan: no bundled Turnip driver, using the system driver");
        LogSystemDevice();
        return;
    }
    const std::string dir = std::string(libdir) + "/";
    if (!TurnipWorksInChild(dir)) {
        Log("vulkan: Turnip not usable, using the system driver");
        LogSystemDevice();
        return;
    }
    std::string device, report;
    PFN_vkGetInstanceProcAddr gipa = LoadTurnip(dir, device, report);
    Log("%s", report.c_str());
    if (!gipa) {
        Log("vulkan: Turnip not usable, using the system driver");
        LogSystemDevice();
        return;
    }
    char hex[32];
    snprintf(hex, sizeof(hex), "%llx", (unsigned long long)reinterpret_cast<uintptr_t>(gipa));
    setenv("DXVK_VK_GIPA", hex, 1);
    Log("vulkan: using Turnip (Mesa) on %s", device.c_str());
    if (FILE* f = fopen(kPending, "w")) fclose(f);
    g_turnip_pending = true;
}

}  // namespace native

#else

namespace native {

void SelectVulkanDriver() {}
void VulkanDriverConfirm() {}

}  // namespace native

#endif
