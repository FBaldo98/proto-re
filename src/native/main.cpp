// Native host entry point: replaces prototypef.exe. Loads prototypeenginef.dll into guest memory, runs its CRT
// startup (DllMain) and drives the six Engine* exports the way prototypef.exe does:
//   EngineInitialize(&{command line, hwnd, hwnd}) -> bool
//   loop: EngineKeyboardService(msg, wParam, lParam, caps, num) / EngineMouseService(msg, wParam, lParam) per
//         window message, then EngineService(quit_requested, inactive) -> bool (false: leave the loop),
//         EngineWindowFocus(focused)
//   EngineTerminate()
#include <SDL2/SDL.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

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
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
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
void InitD3D9Bridge();  // d3d9.cpp
void InitInput(uint32_t keyboard_service, uint32_t mouse_service);  // input.cpp
void ForwardInput(const SDL_Event& e);
extern SDL_Window* g_sdl_window;

}  // namespace native

using namespace native;

int main(int argc, char** argv) {
    g_game_dir = argc > 1 ? argv[1] : getenv("PROTORE_GAME");
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
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_sdl_window) Fatal("SDL_CreateWindow: %s", SDL_GetError());

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
    for (uint64_t frame = 0;; ++frame) {
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
