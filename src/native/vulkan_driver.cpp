// Vulkan driver selection on Android. DXVK needs Vulkan features that Qualcomm's own Adreno drivers often lack
// (robustness2 null descriptors, extended dynamic state, ...), so on Adreno the port loads Mesa's Turnip driver
// shipped in the APK, through libadrenotools (which gets a driver past Android's linker namespaces), and hands its
// vkGetInstanceProcAddr to DXVK (DXVK_VK_GIPA, android/patches/dxvk-android.patch). Turnip is used only if it
// finds the GPU; otherwise, and on other GPUs (Mali, PowerVR, Xclipse), the system driver stays.
// Setting vulkan_driver: auto (default) | system | turnip.
#include "guest.h"

#if defined(__ANDROID__) && defined(PROTORE_ADRENOTOOLS)
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
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

// Creates a throwaway instance to see whether the driver drives this GPU; returns its name, or "" if not (the reason
// goes to the log).
std::string ProbeDevice(PFN_vkGetInstanceProcAddr gipa, const char* driver) {
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) {
        Log("vulkan: %s has no vkCreateInstance", driver);
        return "";
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (VkResult r = create(&ci, nullptr, &instance); r != VK_SUCCESS) {
        Log("vulkan: %s vkCreateInstance -> %d", driver, r);
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
        Log("vulkan: %s sees %s (Vulkan %u.%u.%u, driver %08x, vendor %04x device %08x)", driver, p.deviceName,
            VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
            p.driverVersion, p.vendorID, p.deviceID);
    } else {
        Log("vulkan: %s vkEnumeratePhysicalDevices -> %d, %u device(s)", driver, r, count);
    }
    if (destroy) destroy(instance, nullptr);
    return name;
}

// Mesa built for Android is a Vulkan HAL module: it exports only HMI (hardware/hwvulkan.h), and the entry points
// come from the device it opens. The structs below are the parts of hardware.h/hwvulkan.h needed for that (LP64).
struct HwModule;
struct HwModuleMethods {
    int (*open)(const HwModule* module, const char* id, struct HwDevice** device);
};
struct HwModule {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    HwModuleMethods* methods;
    void* dso;
    uint64_t reserved[32 - 7];
};
struct HwDevice {
    uint32_t tag, version;
    HwModule* module;
    uint64_t reserved[12];
    int (*close)(HwDevice* device);
};
struct HwVulkanDevice {
    HwDevice common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

PFN_vkGetInstanceProcAddr DriverGipa(void* lib) {
    if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))) return gipa;
    auto hmi = static_cast<const HwModule*>(dlsym(lib, "HMI"));
    if (!hmi || !hmi->methods || !hmi->methods->open) {
        Log("vulkan: Turnip exports neither vkGetInstanceProcAddr nor HMI");
        return nullptr;
    }
    HwDevice* device = nullptr;
    if (int r = hmi->methods->open(hmi, "vk0", &device); r != 0 || !device) {
        Log("vulkan: Turnip HMI open -> %d", r);
        return nullptr;
    }
    return reinterpret_cast<HwVulkanDevice*>(device)->GetInstanceProcAddr;  // the device stays open
}

// Logs the system driver's GPU, for reports from phones where Turnip is not used.
void LogSystemDevice() {
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
    if (gipa) ProbeDevice(gipa, "system driver");
}

// Crash guard (working directory = the app's files folder): turnip_pending holds how many runs in a row ended
// before VulkanDriverConfirm, i.e. crashed or hung during startup on Turnip. After two of them turnip_failed is
// written and auto mode stays on the system driver; vulkan_driver = turnip still forces Turnip.
constexpr const char* kPending = "turnip_pending";
constexpr const char* kFailed = "turnip_failed";
bool g_turnip_pending = false;

int PendingRuns() {
    FILE* f = fopen(kPending, "r");
    if (!f) return 0;
    int n = 0;
    if (fscanf(f, "%d", &n) != 1) n = 1;
    fclose(f);
    return n;
}

void SetPendingRuns(int n) {
    if (FILE* f = fopen(kPending, "w")) {
        fprintf(f, "%d\n", n);
        fclose(f);
    }
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
    const int pending = PendingRuns();
    if (mode == "auto" && pending >= 2) {
        unlink(kPending);
        if (FILE* f = fopen(kFailed, "w")) fclose(f);
    }
    if (mode == "auto" && access(kFailed, F_OK) == 0) {
        Log("vulkan: Turnip crashed during startup before (%s), using the system driver; vulkan_driver = turnip "
            "forces it", kFailed);
        LogSystemDevice();
        return;
    }
    if (mode == "turnip") unlink(kFailed);
    const char* libdir = getenv("PROTORE_NATIVE_LIB_DIR");  // set by ProtoActivity: the APK's extracted libraries
    if (!libdir || access((std::string(libdir) + "/libvulkan_freedreno.so").c_str(), F_OK) != 0) {
        Log("vulkan: no bundled Turnip driver, using the system driver");
        LogSystemDevice();
        return;
    }
    // adrenotools joins the directory and the file name as is, so the directories need the trailing slash.
    const std::string dir = std::string(libdir) + "/";
    void* lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, dir.c_str(), dir.c_str(),
                                           "libvulkan_freedreno.so", nullptr, nullptr);
    if (!lib) {
        const char* err = dlerror();
        Log("vulkan: adrenotools could not load Turnip (%s)", err ? err : "no dlerror");
    }
    auto gipa = lib ? DriverGipa(lib) : nullptr;
    const std::string device = gipa ? ProbeDevice(gipa, "Turnip") : "";
    if (device.empty()) {
        Log("vulkan: Turnip did not load or found no GPU, using the system driver");
        LogSystemDevice();
        return;
    }
    char hex[32];
    snprintf(hex, sizeof(hex), "%llx", (unsigned long long)reinterpret_cast<uintptr_t>(gipa));
    setenv("DXVK_VK_GIPA", hex, 1);
    Log("vulkan: Turnip (Mesa) on %s%s", device.c_str(), pending ? " (previous run ended during startup)" : "");
    SetPendingRuns(pending + 1);
    g_turnip_pending = true;
}

}  // namespace native

#else

namespace native {

void SelectVulkanDriver() {}
void VulkanDriverConfirm() {}

}  // namespace native

#endif
