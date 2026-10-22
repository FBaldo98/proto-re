// Native host entry point: replaces prototypef.exe. Loads prototypeenginef.dll into guest memory, runs its CRT
// startup (DllMain) and drives the six Engine* exports the way prototypef.exe does:
//   EngineInitialize(&{command line, hwnd, hwnd}) -> bool
//   loop: EngineKeyboardService(msg, wParam, lParam, caps, num) / EngineMouseService(msg, wParam, lParam) per
//         window message, then EngineService(quit_requested, inactive) -> bool (false: leave the loop),
//         EngineWindowFocus(focused)
//   EngineTerminate()
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>  // Android: main() becomes SDL_main, called by SDLActivity
#include <sched.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <jni.h>
#endif

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"
#include "win.h"

namespace native {

const char* g_game_dir = nullptr;

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}  // namespace

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "protore", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

void Fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("FATAL: %s", buf);
    _exit(70);
}

uint32_t HostWindow();  // user32.cpp
void InitClientSize(int window_w, int window_h);
void StartWatchdog(std::atomic<uint64_t>* frames);  // watchdog.cpp
void InitD3D9Bridge();  // d3d9.cpp
void SelectVulkanDriver();  // vulkan_driver.cpp
void InitInput(uint32_t keyboard_service, uint32_t mouse_service);  // input.cpp
void ForwardInput(const SDL_Event& e);
void InjectKey(SDL_Scancode sc, bool down);
extern SDL_Window* g_sdl_window;

}  // namespace native


// ---- performance counters and frame limiter (read by the on-screen overlay through JNI) ----
namespace native {
std::atomic<float> g_fps{0}, g_frame_ms{0};
std::atomic<int> g_fps_limit{0};  // 0 = no limit (the swap interval still applies)

static long ResidentMiB() {
    long pages = 0, resident = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}
}  // namespace native

#ifdef __ANDROID__
extern "C" JNIEXPORT jfloat JNICALL Java_com_protore_game_TouchControls_nativeFps(JNIEnv*, jclass) {
    return native::g_fps.load();
}
extern "C" JNIEXPORT jfloat JNICALL Java_com_protore_game_TouchControls_nativeFrameMs(JNIEnv*, jclass) {
    return native::g_frame_ms.load();
}
extern "C" JNIEXPORT jint JNICALL Java_com_protore_game_TouchControls_nativeMemoryMiB(JNIEnv*, jclass) {
    return jint(native::ResidentMiB());
}
extern "C" JNIEXPORT void JNICALL Java_com_protore_game_TouchControls_nativeSetFpsLimit(JNIEnv*, jclass, jint fps) {
    native::g_fps_limit.store(fps);
    native::PerfHintTarget(fps);
}
#endif

// big.LITTLE: keep the engine's main thread on the fast cores (cpu_capacity >= half of the largest). The scheduler
// usually gets there by itself, but a frame-bound thread that waits on the GPU looks idle to it now and then and
// lands on a small core for a while. Setting big_cores (Android default 1).
static void PinToBigCores() {
#ifdef __ANDROID__
    // battery_saver: the scheduler picks the cores (the biggest ones draw several times the power per clock)
    if (!native::SettingInt("big_cores", 1) || native::SettingInt("battery_saver", 0)) return;
    int caps[64] = {}, max_cap = 0, n = 0;
    for (; n < 64; ++n) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", n);
        FILE* f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%d", &caps[n]) != 1) caps[n] = 0;
        fclose(f);
        if (caps[n] > max_cap) max_cap = caps[n];
    }
    if (n == 0 || max_cap == 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    int big = 0;
    for (int i = 0; i < n; ++i) {
        if (caps[i] * 2 >= max_cap) {
            CPU_SET(i, &set);
            ++big;
        }
    }
    if (big == 0 || big == n) return;  // not big.LITTLE
    int r = sched_setaffinity(0, sizeof(set), &set);
    native::Log("main thread pinned to %d of %d cores%s", big, n, r == 0 ? "" : " (failed)");
#endif
}

using namespace native;

#ifdef __ANDROID__
// DXVK and FAudio report errors on stderr, which goes nowhere in an app: forward it to logcat (tag protore-err).
static void RedirectStderrToLogcat() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], 2);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([] {
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) > 0;) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_WARN, "protore-err", line.c_str());
                    line.clear();
                } else {
                    line += buf[i];
                }
            }
        }
    }).detach();
}
#endif

int main(int argc, char** argv) {
#ifdef __ANDROID__
    RedirectStderrToLogcat();
    setenv("DXVK_LOG_LEVEL", "info", 0);
#endif
    g_game_dir = argc > 1 ? argv[1] : getenv("PROTORE_GAME");
#ifdef __ANDROID__
    // The user copies the game into <app external files>/game (/sdcard/Android/data/<package>/files/game); logs,
    // saves (native_c) and the DXVK caches go next to it.
    static std::string android_game;
    if (!g_game_dir) {
        const char* base = SDL_AndroidGetExternalStoragePath();
        Log("external storage: %s", base ? base : SDL_GetError());
        if (!base) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Prototype", SDL_GetError(), nullptr);
            return 1;
        }
        android_game = std::string(base) + "/game";
        g_game_dir = android_game.c_str();
        if (chdir(base) != 0) return 1;
        setenv("DXVK_STATE_CACHE_PATH", base, 0);
        setenv("DXVK_LOG_PATH", base, 0);
        std::string dll = android_game + "/prototypeenginef.dll";
        if (access(dll.c_str(), R_OK) != 0) {
            Log("cannot read %s: %s", dll.c_str(), strerror(errno));
            std::string msg = "Copy the game files (prototypeenginef.dll, *.rcf, ...) to\n" + android_game + "\n(" +
                              strerror(errno) + ")";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Prototype", msg.c_str(), nullptr);
            return 1;
        }
    }
#endif
    if (!g_game_dir) {
        fprintf(stderr, "usage: %s <game directory>\n", argv[0]);
        return 2;
    }
    g_log = fopen("proto-native.log", "w");
    InitGuestMemory();
    AttachThread(8u << 20);
    InitD3D9Bridge();
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);  // DXVK native: the HWNDs we pass are SDL windows
    SelectVulkanDriver();  // Adreno: the bundled Turnip driver
    if (SettingInt("present_stats", 0)) setenv("DXVK_PRESENT_STATS", "1", 0);  // Present timing in the DXVK log
    // dxvk_config = d3d9.presentInterval = 1, d3d9.numBackBuffers = 3  (DXVK options; ',' as ';' starts an ini
    // comment)
    if (std::string dc = Setting("dxvk_config", ""); !dc.empty()) {
        for (char& ch : dc) ch = ch == ',' ? ';' : ch;
        setenv("DXVK_CONFIG", dc.c_str(), 0);
    }

    // The game window: DXVK's native WSI takes the SDL window as HWND.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) Fatal("SDL_Init: %s", SDL_GetError());
    g_sdl_window = SDL_CreateWindow("Prototype", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
#ifdef __ANDROID__
                                        | SDL_WINDOW_FULLSCREEN
#endif
    );
    if (!g_sdl_window) Fatal("SDL_CreateWindow: %s", SDL_GetError());
    int window_w, window_h;
    SDL_GetWindowSize(g_sdl_window, &window_w, &window_h);
#ifdef __ANDROID__
    SDL_DisplayMode mode;  // the window becomes the whole screen; its size is not known yet
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        window_w = mode.w > mode.h ? mode.w : mode.h;  // landscape
        window_h = mode.w > mode.h ? mode.h : mode.w;
    }
#endif
    InitClientSize(window_w, window_h);

    std::string dll = std::string(g_game_dir) + "/prototypeenginef.dll";
    if (!LoadImage(dll.c_str(), &g_engine, kEngineBase)) return 1;
    g_engine_lo = g_engine.base;
    g_engine_hi = g_engine.base + g_engine.size;

    uint32_t init = FindExport(g_engine, "EngineInitialize");
    uint32_t service = FindExport(g_engine, "EngineService");
    uint32_t terminate = FindExport(g_engine, "EngineTerminate");
    uint32_t focus = FindExport(g_engine, "EngineWindowFocus");
    if (!init || !service || !terminate || !focus) Fatal("engine exports missing");

    Log("DllMain(PROCESS_ATTACH)");
    uint32_t ok = GuestCall(g_engine.entry, {g_engine.base, 1, 0});
    Log("DllMain returned %u", ok);
    if (!ok) return 1;

    uint32_t hwnd = HostWindow();
    uint32_t params = HeapAlloc(16);
    W32(params + 0, GuestStrdup(""));  // command line
    W32(params + 4, hwnd);
    W32(params + 8, hwnd);
    Log("EngineInitialize");
    uint32_t r = GuestCall(init, {params}) & 0xff;
    Log("EngineInitialize returned %u", r);
    if (!r) return 1;

    InitInput(FindExport(g_engine, "EngineKeyboardService"), FindExport(g_engine, "EngineMouseService"));
    bool focused = true;
    const bool ignore_focus = SettingInt("ignore_focus", 0) != 0;  // tests: an unfocused window keeps full speed
    // PROTORE_INPUT="<frame>:<SDL key name>,...": taps a key at that frame (held for 5 frames), for scripted tests.
    struct Tap {
        uint64_t frame;
        SDL_Scancode key;
    };
    std::vector<Tap> taps;
    if (const char* spec = getenv("PROTORE_INPUT")) {
        std::string all(spec);
        for (size_t pos = 0; pos < all.size();) {
            size_t end = all.find(',', pos);
            std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            size_t colon = item.find(':');
            if (colon != std::string::npos) {
                taps.push_back({strtoull(item.c_str(), nullptr, 10), SDL_GetScancodeFromName(item.c_str() + colon + 1)});
            }
            pos = end == std::string::npos ? all.size() : end + 1;
        }
    }
    static std::atomic<uint64_t> frames{0};
    StartWatchdog(&frames);
    PinToBigCores();
    int fps_limit = SettingInt("fps_limit", 0);
    if (SettingInt("battery_saver", 0) && (fps_limit == 0 || fps_limit > 30)) fps_limit = 30;
    g_fps_limit.store(fps_limit);
    PerfHintStart(fps_limit);
    using Clock = std::chrono::steady_clock;
    Clock::time_point stats_start = Clock::now(), frame_start = stats_start, next_log = stats_start;
    uint64_t stats_frames = 0;
    for (uint64_t frame = 0;; ++frame) {
        frames.store(frame, std::memory_order_relaxed);
        // Frame time, FPS over the last second, a perf line in the log every 30 s, and the optional limiter.
        Clock::time_point now = Clock::now();
        PerfHintFrame(std::chrono::duration_cast<std::chrono::nanoseconds>(now - frame_start).count());
        if (int limit = g_fps_limit.load(); limit > 0) {
            Clock::time_point due = frame_start + std::chrono::microseconds(1000000 / limit);
            if (now < due) {
                std::this_thread::sleep_until(due);
                now = Clock::now();
            }
        }
        g_frame_ms.store(std::chrono::duration<float, std::milli>(now - frame_start).count());
        frame_start = now;
        if (++stats_frames, now - stats_start >= std::chrono::seconds(1)) {
            g_fps.store(float(stats_frames) / std::chrono::duration<float>(now - stats_start).count());
            stats_frames = 0;
            stats_start = now;
        }
        if (now >= next_log) {
            if (frame) Log("perf: %.1f fps, %.1f ms, %ld MiB resident", g_fps.load(), g_frame_ms.load(), ResidentMiB());
            next_log = now + std::chrono::seconds(30);
        }
        for (const Tap& t : taps) {
            if (t.key == SDL_SCANCODE_UNKNOWN) continue;
            if (frame == t.frame) InjectKey(t.key, true);
            if (frame == t.frame + 5) InjectKey(t.key, false);
        }
        if ((frame & (frame - 1)) == 0) Log("frame %llu", (unsigned long long)frame);
        bool quit = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
            } else if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !ignore_focus) focused = false;
            } else {
                ForwardInput(e);
            }
        }
        bool keep = (GuestCall(service, {quit ? 1u : 0u, focused ? 0u : 1u}) & 0xff) != 0;
        GuestCall(focus, {focused ? 1u : 0u});
        if (!keep) break;
    }
    GuestCall(terminate, {});
    Log("EngineTerminate done");
    return 0;
}
