// Native host entry point: replaces prototypef.exe. Loads prototypeenginef.dll into guest memory, runs its CRT
// startup (DllMain) and drives the six Engine* exports the way prototypef.exe does:
//   EngineInitialize(&{command line, hwnd, hwnd}) -> bool
//   loop: EngineKeyboardService(msg, wParam, lParam, caps, num) / EngineMouseService(msg, wParam, lParam) per
//         window message, then EngineService(quit_requested, inactive) -> bool (false: leave the loop),
//         EngineWindowFocus(focused)
//   EngineTerminate()
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>  // Android: main() becomes SDL_main, called by SDLActivity
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
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
void InitD3D9Bridge();  // d3d9.cpp
void InitInput(uint32_t keyboard_service, uint32_t mouse_service);  // input.cpp
void ForwardInput(const SDL_Event& e);
void InjectKey(SDL_Scancode sc, bool down);
extern SDL_Window* g_sdl_window;

}  // namespace native

using namespace native;

int main(int argc, char** argv) {
    g_game_dir = argc > 1 ? argv[1] : getenv("PROTORE_GAME");
#ifdef __ANDROID__
    // The user copies the game into <app external files>/game (/sdcard/Android/data/<package>/files/game); logs,
    // saves (native_c) and the DXVK caches go next to it.
    static std::string android_game;
    if (!g_game_dir) {
        const char* base = SDL_AndroidGetExternalStoragePath();
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
            std::string msg = "Copy the game files (prototypeenginef.dll, *.rcf, ...) to\n" + android_game;
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
    if (!LoadImage(dll.c_str(), &g_engine)) return 1;
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
    for (uint64_t frame = 0;; ++frame) {
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
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = false;
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
