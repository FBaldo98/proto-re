// D3D9 for the native host: the engine's COM calls are forwarded to DXVK (D3D9 on Vulkan, built natively).
//
// Guest objects are 16-byte blocks in guest memory: [0] guest vtable (thunk addresses), [4] index of the host
// object. Method handlers are generated from d3d9.h (tools/gen_d3d9_bridge.py -> d3d9_gen.inc); this file holds the
// object table and the methods whose arguments need conversion: structures with window handles (present
// parameters) and Lock/Unlock, where the host's mapped pointer lies outside guest memory and a guest shadow buffer
// is copied in and out.
#include <SDL2/SDL.h>
#include <d3d9.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "win.h"

namespace native {

SDL_Window* g_sdl_window = nullptr;
uint32_t HostWindow();

namespace {

struct MethodDef {
    const char* name;
    ImportFn fn;
};
struct InterfaceDef {
    const char* name;
    const MethodDef* methods;
    size_t count;
};

std::recursive_mutex g_mutex;
std::vector<void*> g_hosts;                      // index -> host object
std::unordered_map<void*, uint32_t> g_guest_of;  // host object -> guest object
std::map<std::string, uint32_t> g_vtables;       // interface name -> guest vtable

template <typename T>
T* Host(uint32_t guest) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return static_cast<T*>(g_hosts[R32(guest + 4)]);
}

template <typename T>
T* Unwrap(uint32_t guest) {
    return guest ? Host<T>(guest) : nullptr;
}

uint32_t WrapAs(void* host, const char* iface) {
    if (!host) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_guest_of.find(host);
    if (it != g_guest_of.end()) return it->second;
    auto vt = g_vtables.find(iface);
    if (vt == g_vtables.end()) Fatal("d3d9: no vtable for %s", iface);
    uint32_t g = HeapAlloc(16);
    W32(g, vt->second);
    W32(g + 4, uint32_t(g_hosts.size()));
    g_hosts.push_back(host);
    g_guest_of[host] = g;
    return g;
}

void Forget(uint32_t guest) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t index = R32(guest + 4);
    g_guest_of.erase(g_hosts[index]);
    g_hosts[index] = nullptr;
    W32(guest, 0);
    // The guest block is not reused: a stale guest pointer then faults on a null vtable instead of aliasing.
}

uint32_t Wrap(IDirect3D9* p) { return WrapAs(p, "IDirect3D9"); }
uint32_t Wrap(IDirect3DDevice9* p) { return WrapAs(p, "IDirect3DDevice9"); }
uint32_t Wrap(IDirect3DSwapChain9* p) { return WrapAs(p, "IDirect3DSwapChain9"); }
uint32_t Wrap(IDirect3DSurface9* p) { return WrapAs(p, "IDirect3DSurface9"); }
uint32_t Wrap(IDirect3DVolume9* p) { return WrapAs(p, "IDirect3DVolume9"); }
uint32_t Wrap(IDirect3DVertexBuffer9* p) { return WrapAs(p, "IDirect3DVertexBuffer9"); }
uint32_t Wrap(IDirect3DIndexBuffer9* p) { return WrapAs(p, "IDirect3DIndexBuffer9"); }
uint32_t Wrap(IDirect3DTexture9* p) { return WrapAs(p, "IDirect3DTexture9"); }
uint32_t Wrap(IDirect3DCubeTexture9* p) { return WrapAs(p, "IDirect3DCubeTexture9"); }
uint32_t Wrap(IDirect3DVolumeTexture9* p) { return WrapAs(p, "IDirect3DVolumeTexture9"); }
uint32_t Wrap(IDirect3DVertexDeclaration9* p) { return WrapAs(p, "IDirect3DVertexDeclaration9"); }
uint32_t Wrap(IDirect3DVertexShader9* p) { return WrapAs(p, "IDirect3DVertexShader9"); }
uint32_t Wrap(IDirect3DPixelShader9* p) { return WrapAs(p, "IDirect3DPixelShader9"); }
uint32_t Wrap(IDirect3DStateBlock9* p) { return WrapAs(p, "IDirect3DStateBlock9"); }
uint32_t Wrap(IDirect3DQuery9* p) { return WrapAs(p, "IDirect3DQuery9"); }
uint32_t Wrap(IDirect3DBaseTexture9* p) {
    if (!p) return 0;
    switch (p->GetType()) {
        case D3DRTYPE_CUBETEXTURE: return WrapAs(p, "IDirect3DCubeTexture9");
        case D3DRTYPE_VOLUMETEXTURE: return WrapAs(p, "IDirect3DVolumeTexture9");
        default: return WrapAs(p, "IDirect3DTexture9");
    }
}
uint32_t Wrap(IDirect3DResource9* p) {
    if (!p) return 0;
    switch (p->GetType()) {
        case D3DRTYPE_SURFACE: return WrapAs(p, "IDirect3DSurface9");
        case D3DRTYPE_VOLUME: return WrapAs(p, "IDirect3DVolume9");
        case D3DRTYPE_VERTEXBUFFER: return WrapAs(p, "IDirect3DVertexBuffer9");
        case D3DRTYPE_INDEXBUFFER: return WrapAs(p, "IDirect3DIndexBuffer9");
        default: return Wrap(static_cast<IDirect3DBaseTexture9*>(p));
    }
}

HWND HostHwnd(uint32_t guest_hwnd) { return guest_hwnd ? reinterpret_cast<HWND>(g_sdl_window) : nullptr; }

void ReturnFStd(Ctx* c, float v, int args) {
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    c->ret_target = Pop32(c);
    c->esp += 4 * args;
}

template <typename T>
void ReleaseImpl(Ctx* c) {
    uint32_t g = Arg(c, 0);
    ULONG r = Host<T>(g)->Release();
    if (r == 0) Forget(g);
    Return(c, r, 1);
}

template <typename T>
void QueryInterfaceImpl(Ctx* c) {
    uint32_t g = Arg(c, 0), out = Arg(c, 2);
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    if (riid == __uuidof(IUnknown) || riid == __uuidof(T)) {
        Host<T>(g)->AddRef();
        W32(out, g);
        Return(c, S_OK, 3);
        return;
    }
    Log("d3d9: QueryInterface for an unsupported interface");
    W32(out, 0);
    Return(c, E_NOINTERFACE, 3);
}

// ---- present parameters: 56 bytes in guest memory, hDeviceWindow at +28 ----
D3DPRESENT_PARAMETERS ToHost(uint32_t g) {
    D3DPRESENT_PARAMETERS p = {};
    p.BackBufferWidth = R32(g + 0);
    p.BackBufferHeight = R32(g + 4);
    p.BackBufferFormat = D3DFORMAT(R32(g + 8));
    p.BackBufferCount = R32(g + 12);
    p.MultiSampleType = D3DMULTISAMPLE_TYPE(R32(g + 16));
    p.MultiSampleQuality = R32(g + 20);
    p.SwapEffect = D3DSWAPEFFECT(R32(g + 24));
    p.hDeviceWindow = HostHwnd(R32(g + 28) ? R32(g + 28) : HostWindow());
    p.Windowed = R32(g + 32);
    p.EnableAutoDepthStencil = R32(g + 36);
    p.AutoDepthStencilFormat = D3DFORMAT(R32(g + 40));
    p.Flags = R32(g + 44);
    p.FullScreen_RefreshRateInHz = R32(g + 48);
    p.PresentationInterval = R32(g + 52);
    if (getenv("PROTORE_WINDOWED")) {
        p.Windowed = TRUE;
        p.FullScreen_RefreshRateInHz = 0;
    }
    return p;
}

void ToGuest(const D3DPRESENT_PARAMETERS& p, uint32_t g) {
    W32(g + 0, p.BackBufferWidth);
    W32(g + 4, p.BackBufferHeight);
    W32(g + 8, p.BackBufferFormat);
    W32(g + 12, p.BackBufferCount);
    W32(g + 16, p.MultiSampleType);
    W32(g + 20, p.MultiSampleQuality);
    W32(g + 24, p.SwapEffect);
    W32(g + 28, HostWindow());
    W32(g + 36, p.EnableAutoDepthStencil);
    W32(g + 40, p.AutoDepthStencilFormat);
    W32(g + 44, p.Flags);
    W32(g + 52, p.PresentationInterval);
}

// ---- lock shadows ----
struct Shadow {
    uint32_t guest = 0, capacity = 0, size = 0;
    void* host = nullptr;
    bool write_back = false;
};
std::map<std::pair<void*, uint32_t>, Shadow> g_shadows;  // (object, subresource) -> shadow

bool IsBlockCompressed(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 ||
           f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '1')) || f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2'));
}

// Maps the host's locked memory into a guest buffer: copies it in unless the lock discards or never overwrites
// (the guest then only writes), and remembers to copy it back on unlock unless read-only.
uint32_t ShadowLock(void* object, uint32_t sub, void* host, uint32_t size, DWORD flags) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Shadow& s = g_shadows[{object, sub}];
    if (s.capacity < size) {
        if (s.guest) HeapFree(s.guest);
        s.guest = HeapAlloc(size);
        s.capacity = size;
    }
    s.size = size;
    s.host = host;
    s.write_back = !(flags & D3DLOCK_READONLY);
    if (!(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) memcpy(Ptr<void>(s.guest), host, size);
    return s.guest;
}

void ShadowUnlock(void* object, uint32_t sub) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_shadows.find({object, sub});
    if (it == g_shadows.end() || !it->second.host) return;
    Shadow& s = it->second;
    if (s.write_back) memcpy(s.host, Ptr<void>(s.guest), s.size);
    s.host = nullptr;
}

void ForgetShadows(void* object) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto it = g_shadows.lower_bound({object, 0}); it != g_shadows.end() && it->first.first == object;) {
        if (it->second.guest) HeapFree(it->second.guest);
        it = g_shadows.erase(it);
    }
}

uint32_t RectRows(const D3DSURFACE_DESC& d, const RECT* r) {
    uint32_t h = r ? uint32_t(r->bottom - r->top) : d.Height;
    return IsBlockCompressed(d.Format) ? (h + 3) / 4 : h;
}

// LockRect common part: D3DLOCKED_RECT in guest memory is {INT Pitch; void* pBits} (8 bytes).
void LockRectCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_RECT& lr,
                    const D3DSURFACE_DESC& desc, const RECT* rect, DWORD flags, uint32_t out, int args) {
    if (SUCCEEDED(hr)) {
        uint32_t size = uint32_t(lr.Pitch) * RectRows(desc, rect);
        W32(out, uint32_t(lr.Pitch));
        W32(out + 4, ShadowLock(object, sub, lr.pBits, size, flags));
    }
    Return(c, uint32_t(hr), args);
}

// ---- Direct3DCreate9 ----
void x_Direct3DCreate9(Ctx* c) {
    IDirect3D9* d3d = Direct3DCreate9(Arg(c, 0));
    Log("Direct3DCreate9(%u) -> %p", Arg(c, 0), static_cast<void*>(d3d));
    Return(c, Wrap(d3d), 1);
}

}  // namespace

// ---------------- manual methods ----------------
#define M(iface, method) void Manual_##iface##_##method(Ctx* c)

M(IDirect3D9, GetAdapterMonitor) { Return(c, 1, 2); }
M(IDirect3D9, CreateDevice) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 5));
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = Host<IDirect3D9>(Arg(c, 0))->CreateDevice(Arg(c, 1), D3DDEVTYPE(Arg(c, 2)), HostHwnd(Arg(c, 3)),
                                                         Arg(c, 4), &pp, &dev);
    Log("CreateDevice %ux%u fmt %u windowed %u -> %08x", pp.BackBufferWidth, pp.BackBufferHeight,
        pp.BackBufferFormat, pp.Windowed, uint32_t(hr));
    ToGuest(pp, Arg(c, 5));
    W32(Arg(c, 6), SUCCEEDED(hr) ? Wrap(dev) : 0);
    Return(c, uint32_t(hr), 7);
}
M(IDirect3DDevice9, GetCreationParameters) {
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->GetCreationParameters(&p);
    uint32_t g = Arg(c, 1);
    W32(g, p.AdapterOrdinal);
    W32(g + 4, p.DeviceType);
    W32(g + 8, HostWindow());
    W32(g + 12, p.BehaviorFlags);
    Return(c, uint32_t(hr), 2);
}
M(IDirect3DDevice9, SetCursorProperties) {
    Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->SetCursorProperties(
                  Arg(c, 1), Arg(c, 2), Unwrap<IDirect3DSurface9>(Arg(c, 3)))),
           4);
}
M(IDirect3DDevice9, CreateAdditionalSwapChain) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    IDirect3DSwapChain9* sc = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateAdditionalSwapChain(&pp, &sc);
    ToGuest(pp, Arg(c, 1));
    W32(Arg(c, 2), SUCCEEDED(hr) ? Wrap(sc) : 0);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DDevice9, Reset) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->Reset(&pp);
    Log("Reset %ux%u -> %08x", pp.BackBufferWidth, pp.BackBufferHeight, uint32_t(hr));
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}
M(IDirect3DDevice9, Present) {
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->Present(Ptr<RECT>(Arg(c, 1)), Ptr<RECT>(Arg(c, 2)), nullptr,
                                                            nullptr);
    Return(c, uint32_t(hr), 5);
}
M(IDirect3DSwapChain9, Present) {
    HRESULT hr = Host<IDirect3DSwapChain9>(Arg(c, 0))->Present(Ptr<RECT>(Arg(c, 1)), Ptr<RECT>(Arg(c, 2)), nullptr,
                                                               nullptr, Arg(c, 5));
    Return(c, uint32_t(hr), 6);
}
M(IDirect3DSwapChain9, GetPresentParameters) {
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = Host<IDirect3DSwapChain9>(Arg(c, 0))->GetPresentParameters(&pp);
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}

// GetContainer(riid, void**): the container is a texture or the device.
template <typename T>
void GetContainerImpl(Ctx* c) {
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    void* p = nullptr;
    HRESULT hr = Host<T>(Arg(c, 0))->GetContainer(riid, &p);
    uint32_t g = 0;
    if (SUCCEEDED(hr)) {
        if (riid == __uuidof(IDirect3DTexture9)) g = Wrap(static_cast<IDirect3DTexture9*>(p));
        else if (riid == __uuidof(IDirect3DCubeTexture9)) g = Wrap(static_cast<IDirect3DCubeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DVolumeTexture9)) g = Wrap(static_cast<IDirect3DVolumeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DBaseTexture9)) g = Wrap(static_cast<IDirect3DBaseTexture9*>(p));
        else if (riid == __uuidof(IDirect3DDevice9)) g = Wrap(static_cast<IDirect3DDevice9*>(p));
        else Log("d3d9: GetContainer for an unsupported interface");
    }
    W32(Arg(c, 2), g);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DSurface9, GetContainer) { GetContainerImpl<IDirect3DSurface9>(c); }
M(IDirect3DVolume9, GetContainer) { GetContainerImpl<IDirect3DVolume9>(c); }

M(IDirect3DSurface9, LockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    const RECT* rect = Ptr<RECT>(Arg(c, 2));
    D3DLOCKED_RECT lr;
    HRESULT hr = s->LockRect(&lr, rect, Arg(c, 3));
    D3DSURFACE_DESC desc;
    s->GetDesc(&desc);
    LockRectCommon(c, s, 0, hr, lr, desc, rect, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DSurface9, UnlockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    ShadowUnlock(s, 0);
    Return(c, uint32_t(s->UnlockRect()), 1);
}
M(IDirect3DSurface9, GetDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }
M(IDirect3DSurface9, ReleaseDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }

M(IDirect3DTexture9, LockRect) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const RECT* rect = Ptr<RECT>(Arg(c, 3));
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(level, &lr, rect, Arg(c, 4));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, level, hr, lr, desc, rect, Arg(c, 4), Arg(c, 2), 5);
}
M(IDirect3DTexture9, UnlockRect) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    ShadowUnlock(t, Arg(c, 1));
    Return(c, uint32_t(t->UnlockRect(Arg(c, 1))), 2);
}
M(IDirect3DCubeTexture9, LockRect) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    uint32_t face = Arg(c, 1), level = Arg(c, 2);
    const RECT* rect = Ptr<RECT>(Arg(c, 4));
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(D3DCUBEMAP_FACES(face), level, &lr, rect, Arg(c, 5));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, face * 64 + level, hr, lr, desc, rect, Arg(c, 5), Arg(c, 3), 6);
}
M(IDirect3DCubeTexture9, UnlockRect) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    ShadowUnlock(t, Arg(c, 1) * 64 + Arg(c, 2));
    Return(c, uint32_t(t->UnlockRect(D3DCUBEMAP_FACES(Arg(c, 1)), Arg(c, 2))), 3);
}

// D3DLOCKED_BOX in guest memory: {INT RowPitch; INT SlicePitch; void* pBits} (12 bytes).
void LockBoxCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_BOX& lb, uint32_t depth,
                   DWORD flags, uint32_t out, int args) {
    if (SUCCEEDED(hr)) {
        W32(out, uint32_t(lb.RowPitch));
        W32(out + 4, uint32_t(lb.SlicePitch));
        W32(out + 8, ShadowLock(object, sub, lb.pBits, uint32_t(lb.SlicePitch) * depth, flags));
    }
    Return(c, uint32_t(hr), args);
}
M(IDirect3DVolumeTexture9, LockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 3));
    D3DLOCKED_BOX lb;
    HRESULT hr = t->LockBox(level, &lb, box, Arg(c, 4));
    D3DVOLUME_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockBoxCommon(c, t, level, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 4), Arg(c, 2), 5);
}
M(IDirect3DVolumeTexture9, UnlockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    ShadowUnlock(t, Arg(c, 1));
    Return(c, uint32_t(t->UnlockBox(Arg(c, 1))), 2);
}
M(IDirect3DVolume9, LockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 2));
    D3DLOCKED_BOX lb;
    HRESULT hr = v->LockBox(&lb, box, Arg(c, 3));
    D3DVOLUME_DESC desc;
    v->GetDesc(&desc);
    LockBoxCommon(c, v, 0, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DVolume9, UnlockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    ShadowUnlock(v, 0);
    Return(c, uint32_t(v->UnlockBox()), 1);
}

template <typename T, typename Desc>
void BufferLock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    uint32_t offset = Arg(c, 1), size = Arg(c, 2), out = Arg(c, 3);
    DWORD flags = Arg(c, 4);
    void* p = nullptr;
    HRESULT hr = b->Lock(offset, size, &p, flags);
    if (SUCCEEDED(hr)) {
        if (!size) {
            Desc d;
            b->GetDesc(&d);
            size = d.Size - offset;
        }
        W32(out, ShadowLock(b, 0, p, size, flags));
    }
    Return(c, uint32_t(hr), 5);
}
template <typename T>
void BufferUnlock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    ShadowUnlock(b, 0);
    Return(c, uint32_t(b->Unlock()), 1);
}
M(IDirect3DVertexBuffer9, Lock) { BufferLock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(c); }
M(IDirect3DVertexBuffer9, Unlock) { BufferUnlock<IDirect3DVertexBuffer9>(c); }
M(IDirect3DIndexBuffer9, Lock) { BufferLock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(c); }
M(IDirect3DIndexBuffer9, Unlock) { BufferUnlock<IDirect3DIndexBuffer9>(c); }

#undef M

#include "d3d9_gen.inc"

namespace {

// Builds the guest vtables (one thunk per method) and registers Direct3DCreate9.
struct Registrar {
    Registrar() {
        static const ImportDef create[] = {{"d3d9.dll", "Direct3DCreate9", x_Direct3DCreate9}};
        RegisterImports(create, 1);
        for (const InterfaceDef& i : kInterfaces) {
            std::vector<ImportDef> defs;
            for (size_t k = 0; k < i.count; ++k) {
                std::string n = std::string(i.name) + "::" + i.methods[k].name;
                defs.push_back({"d3d9.com", strdup(n.c_str()), i.methods[k].fn});
            }
            RegisterImports(defs.data(), defs.size());
        }
    }
} g_registrar;

}  // namespace

// Called once guest memory exists (the vtables live there).
void InitD3D9Bridge() {
    for (const InterfaceDef& i : kInterfaces) {
        uint32_t vt = HeapAlloc(uint32_t(4 * i.count));
        for (size_t k = 0; k < i.count; ++k) {
            std::string n = std::string(i.name) + "::" + i.methods[k].name;
            W32(vt + uint32_t(4 * k), ImportThunk("d3d9.com", n.c_str()));
        }
        g_vtables[i.name] = vt;
    }
}

}  // namespace native
