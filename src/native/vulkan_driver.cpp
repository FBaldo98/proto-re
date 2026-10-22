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

// Creates a throwaway instance to see whether the driver drives this GPU; returns its name, or "" if not.
std::string ProbeDevice(PFN_vkGetInstanceProcAddr gipa) {
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) return "";
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&ci, nullptr, &instance) != VK_SUCCESS) return "";
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    std::string name;
    uint32_t count = 1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (enumerate && props && enumerate(instance, &count, &device) >= 0 && count && device) {
        VkPhysicalDeviceProperties p;
        props(device, &p);
        name = p.deviceName;
    }
    if (destroy) destroy(instance, nullptr);
    return name;
}

}  // namespace

void SelectVulkanDriver() {
    const std::string mode = Setting("vulkan_driver", "auto");
    if (mode == "system") return;
    const bool adreno = access("/dev/kgsl-3d0", F_OK) == 0;  // Qualcomm's GPU kernel driver
    if (mode == "auto" && !adreno) return;
    const char* libdir = getenv("PROTORE_NATIVE_LIB_DIR");  // set by ProtoActivity: the APK's extracted libraries
    if (!libdir || access((std::string(libdir) + "/libvulkan_freedreno.so").c_str(), F_OK) != 0) {
        Log("vulkan: no bundled Turnip driver, using the system driver");
        return;
    }
    void* lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, libdir, libdir,
                                           "libvulkan_freedreno.so", nullptr, nullptr);
    auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
    const std::string device = gipa ? ProbeDevice(gipa) : "";
    if (device.empty()) {
        Log("vulkan: Turnip did not load or found no GPU, using the system driver");
        return;
    }
    char hex[32];
    snprintf(hex, sizeof(hex), "%llx", (unsigned long long)reinterpret_cast<uintptr_t>(gipa));
    setenv("DXVK_VK_GIPA", hex, 1);
    Log("vulkan: Turnip (Mesa) on %s", device.c_str());
}

}  // namespace native

#else

namespace native {

void SelectVulkanDriver() {}

}  // namespace native

#endif
