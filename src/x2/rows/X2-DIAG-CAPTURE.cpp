// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-CAPTURE (diagnostic, off, observe only): records everything about one spell's frames in one go.
//
// While an X2-DIAG-FTBLINK-DLL spell DLL is loaded (its first load only), for up to X2-DIAG-CAPTURE-FRAMES presents
// (900), it writes:
//   <game>\fps60x2-capture-draws.csv  one "F" line per present (frame, invocation kind, frame counter) and one "D" line
//     per D3D11 draw: call and arguments, topology, shaders, blend / depth state, render target, texture 0, blend
//     factor, and the first values of the vertex and pixel constant buffers 0-1
//   "S" lines per frame: how often each HD submit function (sprites, VFX instances) was called and by whom, and by
//     which particle program; "B" lines: each blend state, once
//   <game>\fps60x2-capture-frames.bin  every frame's picture at 1/4 size (24-bit RGB, 16-byte header per frame)
// Comparing a logic frame with the render-only frame after it, draw by draw, shows which draw differs. Everything is
// read back on the CPU, so the game runs slowly while capturing. 8-bit RGBA / BGRA back buffers only.
// Signatures tools/sigs_x2_capture.txt. History: re/ledger/ULTIMA-0106.md.
#include "../x2.h"
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include <math.h>

namespace x2 {

extern Row row_DIAGCAPTURE, row_DIAGBLINK;
extern void (*volatile g_present_tap2)(IDXGISwapChain* sc);
extern bool (*volatile g_draw_tap)(ID3D11DeviceContext* c, int type, UINT a, UINT b, UINT d, INT e);
bool present_hook();
bool ftblink_watching();
uint8_t* ftblink_base();

typedef HRESULT(STDMETHODCALLTYPE* map_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* unmap_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* update_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
static map_t g_map = nullptr;
static unmap_t g_unmap = nullptr;
static update_t g_update = nullptr;

static CRITICAL_SECTION g_cs;
static FILE* g_csv = nullptr;
static FILE* g_bin = nullptr;
static volatile bool g_on = false;              // capturing now
static bool g_was = false, g_done = false;
static int g_max = 900, g_min = 300, g_frame = 0, g_draw = 0;
// X2-DIAG-CAPTURE-DRAWS=0: no per-draw D lines (frames and F / S / C / K / Q lines only). X2-DIAG-CAPTURE-LOADS=<n>
// (default 1): n watched loads appended to the same files (frame numbers continue), for X2-DEV-BISECT-AUTO runs over
// several casts; the F line carries the bisect step (g_bisect_step).
static int g_draws_on = 1, g_loads = 1, g_loads_done = 0;
// X2-DIAG-CAPTURE-IMAGES=0: no frame images, only each frame's mean luma (1/4 size) on its F line.
static int g_images = 1;
static double g_last_luma = -1;
// HD frame object (g_dwHdFrame 0xDCDA04, from svc +0xEBC FUN_0044d260 `MOV ECX,[..]`; sig tools/sigs_x2_capture.txt):
// its cycling buffer pointers +0x44 / +0x64 / +0x68 / +0xFE4 and toggle byte +0xE920 go on each F line (auto-bisect
// 04:16: the blink's sign flips mid-cast at a timing hitch, as if tied to a per-present rotation, not to L / R).
static uint8_t** g_hdframe = nullptr;
extern volatile int g_bisect_step;
static ID3D11Texture2D* g_stage = nullptr;

// Constant buffer contents by buffer identity: hash and first 8 floats of the last upload.
enum { HB = 4096 };
struct CB { void* buf; uint32_t hash; float f[8]; };
static CB g_cb[HB];
static void* g_map_res = nullptr;
static void* g_map_data = nullptr;
static UINT g_map_size = 0;

static uint32_t hmix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
static CB* cb_slot(void* b, bool add) {
    uint32_t h = hmix((uint32_t)(uintptr_t)b);
    for (int k = 0; k < 64; k++) {
        CB& e = g_cb[(h + k) & (HB - 1)];
        if (e.buf == b) return &e;
        if (!e.buf) { if (!add) return nullptr; e.buf = b; return &e; }
    }
    return nullptr;
}
static void cb_store(void* b, const void* data, UINT size) {
    CB* e = cb_slot(b, true);
    if (!e || !data) return;
    uint32_t h = 2166136261u;
    __try {
        const uint8_t* p = (const uint8_t*)data;
        for (UINT i = 0; i < size; i++) h = (h ^ p[i]) * 16777619u;
        memset(e->f, 0, sizeof(e->f));
        memcpy(e->f, data, size < sizeof(e->f) ? size : sizeof(e->f));
    } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
    e->hash = h;
}
static UINT const_buffer_size(ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC d;
    ((ID3D11Buffer*)r)->GetDesc(&d);
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? d.ByteWidth : 0;
}

static HRESULT STDMETHODCALLTYPE h_map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT fl,
                                       D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = g_map(c, r, sub, t, fl, m);
    if (g_on && SUCCEEDED(hr) && m && r && t != D3D11_MAP_READ) {
        UINT sz = const_buffer_size(r);
        if (sz) { g_map_res = r; g_map_data = m->pData; g_map_size = sz; }
    }
    return hr;
}
static void STDMETHODCALLTYPE h_unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (g_on && r && r == g_map_res) {
        EnterCriticalSection(&g_cs);
        cb_store(r, g_map_data, g_map_size);
        LeaveCriticalSection(&g_cs);
        g_map_res = nullptr;
    }
    g_unmap(c, r, sub);
}
static void STDMETHODCALLTYPE h_update(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                       const void* src, UINT rp, UINT dp) {
    if (g_on && r && !box) {
        UINT sz = const_buffer_size(r);
        if (sz) { EnterCriticalSection(&g_cs); cb_store(r, src, sz); LeaveCriticalSection(&g_cs); }
    }
    g_update(c, r, sub, box, src, rp, dp);
}

// ---- HD submit counting per caller
typedef int(__cdecl* sub_t)(int, int, int, int, int, int);
// Sprite submits, VFX create and VFX update FUN_0080a9e0 take a colour struct as argument 4 (+4 R, +5 G, +6 B, +7 A
// bytes, +0x10 matrix pointer; S FUN_00808940 lines 158-164, FUN_0080a9e0 line 535 / 537): its sums per caller and
// frame show whose sprites change colour, alpha or size between alternate presents (capture 04:20: same sprite count,
// logic frames brighter).
enum { NSUB = 5, HS = 2048 };
static const char* SUB_NAME[NSUB] = {"spriteA_808940", "spriteB_8099D0", "vfxSubmit_456A00", "vfxCreate_7FDDE0",
                                     "vfxUpdate_80A9E0"};
static const bool SUB_COLOUR[NSUB] = {true, true, false, true, true};
static sub_t g_sub[NSUB];
static volatile int32_t* g_skip = nullptr;
struct SubC { uint32_t fn_caller; void* prog; int cls; uint32_t n, skip; double a, rgb, scale; };
static SubC g_sc[HS];
static void sub_note(int fn, void* ret, const uint8_t* col) {
    if (!g_on) return;
    uint32_t key = ((uint32_t)fn << 28) | (rt_to_gva(ret) & 0x0FFFFFFF);
    bool sk = g_skip && *g_skip != 0;
    int cls = 0;
    void* prog = mag006_cur_step(&cls);
    if (!prog) cls = 0;
    double a = 0, rgb = 0, sc = 0;
    if (col) {
        __try {
            rgb = (double)col[4] + col[5] + col[6];
            a = col[7];
            const float* m = *(const float* const*)(col + 0x10);
            if (m) sc = sqrt((double)m[0] * m[0] + (double)m[1] * m[1] + (double)m[2] * m[2]);
        } __except (EXCEPTION_EXECUTE_HANDLER) { a = rgb = sc = 0; }
    }
    EnterCriticalSection(&g_cs);
    uint32_t h = hmix(key ^ (uint32_t)(uintptr_t)prog);
    for (int k = 0; k < 64; k++) {
        SubC& e = g_sc[(h + k) & (HS - 1)];
        if ((e.fn_caller == key && e.prog == prog) || !e.n) {
            e.fn_caller = key; e.prog = prog; e.cls = cls; e.n++; if (sk) e.skip++;
            e.a += a; e.rgb += rgb; e.scale += sc;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
}
template <int I> static int __cdecl h_sub(int a, int b, int c, int d, int e, int f) {
    sub_note(I, _ReturnAddress(), SUB_COLOUR[I] ? (const uint8_t*)(uintptr_t)d : nullptr);
    return g_sub[I](a, b, c, d, e, f);
}
static void* const SUB_DET[NSUB] = {(void*)&h_sub<0>, (void*)&h_sub<1>, (void*)&h_sub<2>, (void*)&h_sub<3>, (void*)&h_sub<4>};
static void sub_flush() {                       // under g_cs
    for (SubC& e : g_sc)
        if (e.n) {
            fprintf(g_csv, "S,%d,%s,%08X,%u,%u,%p,%d,%.1f,%.1f,%.3f\n", g_frame, SUB_NAME[e.fn_caller >> 28],
                    e.fn_caller & 0x0FFFFFFF, e.n, e.skip, e.prog, e.cls, e.a, e.rgb, e.scale);
            e = SubC{};
        }
}

// ---- copies, clears and query reads ("C" / "K" / "Q" lines; capture 04:03: the post chain is identical on both
// kinds, so look for an image or query result that one frame takes from an earlier one)
typedef void(STDMETHODCALLTYPE* csr_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* cr_t)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* rs_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
typedef void(STDMETHODCALLTYPE* crtv_t)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef HRESULT(STDMETHODCALLTYPE* gd_t)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
static csr_t g_csr = nullptr; static cr_t g_cr = nullptr; static rs_t g_rs = nullptr;
static crtv_t g_crtv = nullptr; static gd_t g_gd = nullptr;
static volatile bool g_self = false;             // our own frame copy in save_frame

// ---- frame-to-frame feedback (capture 04:25 / 04:09: a full-screen draw copies the resolved scene into a texture
// at the end of each frame; the next frame's early draws, 8-24 sphere meshes of 768 indices during Ultima's sphere,
// sample it before it is rewritten). A resource read by a draw while its last write (render target, clear, copy or
// resolve destination) was in an earlier frame is a feedback read; counted per frame on the F line. With
// X2-DIAG-CAPTURE-FBSKIP=1 those draws are skipped, =2 skipped in odd GetTickCount seconds only (A / B in one cast),
// to test whether the loop sets the present-parity blink.
enum { HR = 1024 };
struct ResW { void* res; int frame; };
static ResW g_rw[HR];
static int g_fbskip = 0, g_fb_reads = 0, g_fb_skipped = 0, g_phaseflip = 0, g_backlog_inj = 0;
static ResW* rw_slot(void* r, bool add) {
    uint32_t h = hmix((uint32_t)(uintptr_t)r);
    for (int k = 0; k < 64; k++) {
        ResW& e = g_rw[(h + k) & (HR - 1)];
        if (e.res == r) return &e;
        if (!e.res) { if (!add) return nullptr; e.res = r; e.frame = -1; return &e; }
    }
    return nullptr;
}
static void note_write(void* r) {                // under g_cs
    if (ResW* e = r ? rw_slot(r, true) : nullptr) e->frame = g_frame;
}
// "R" lines: each texture's description once (bound resources only, so the pointer is live)
enum { HRS = 1024 };
static void* g_res_seen[HRS];
static void note_res(ID3D11Resource* r) {        // under g_cs
    if (!r || !g_csv) return;
    uint32_t h = hmix((uint32_t)(uintptr_t)r);
    for (int k = 0; k < 64; k++) {
        void*& e = g_res_seen[(h + k) & (HRS - 1)];
        if (e == r) return;
        if (!e) {
            e = r;
            D3D11_RESOURCE_DIMENSION dim;
            r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d;
                ((ID3D11Texture2D*)r)->GetDesc(&d);
                fprintf(g_csv, "R,%d,%p,%u,%u,%d,%u,%u,%u,%u\n", g_frame, (void*)r, d.Width, d.Height, (int)d.Format,
                        d.BindFlags, d.MiscFlags, d.SampleDesc.Count, d.MipLevels);
            } else fprintf(g_csv, "R,%d,%p,dim%d\n", g_frame, (void*)r, (int)dim);
            return;
        }
    }
}
static bool fbskip_now() {
    return g_fbskip == 1 || (g_fbskip == 2 && ((GetTickCount() / 1000) & 1));
}

// ---- blend states: one "B" line per new state (render target 0 description), to tell an additive, subtractive or
// inverting pass from a plain alpha blend
// Each state also gets a category for X2-DIAG-CAPTURE-CATSKIP: 0 opaque (blend off; also no blend state bound),
// 1 additive (ADD, dest ONE), 2 alpha (ADD, dest INV_SRC_ALPHA), 3 SUBTRACT, 4 REV_SUBTRACT, 5 other.
enum { HBS = 512, NCAT = 6 };
static void* g_bs_seen[HBS];
static int8_t g_bs_cat[HBS];
static int blend_cat(const D3D11_RENDER_TARGET_BLEND_DESC& t) {
    if (!t.BlendEnable) return 0;
    if (t.BlendOp == D3D11_BLEND_OP_ADD && t.DestBlend == D3D11_BLEND_ONE) return 1;
    if (t.BlendOp == D3D11_BLEND_OP_ADD && t.DestBlend == D3D11_BLEND_INV_SRC_ALPHA) return 2;
    if (t.BlendOp == D3D11_BLEND_OP_SUBTRACT) return 3;
    if (t.BlendOp == D3D11_BLEND_OP_REV_SUBTRACT) return 4;
    return 5;
}
static int note_blend(ID3D11BlendState* bs) {    // under g_cs; returns the category
    if (!bs) return 0;
    uint32_t h = hmix((uint32_t)(uintptr_t)bs);
    for (int k = 0; k < 64; k++) {
        int slot = (h + k) & (HBS - 1);
        void*& e = g_bs_seen[slot];
        if (e == bs) return g_bs_cat[slot];
        if (!e) {
            e = bs;
            D3D11_BLEND_DESC d;
            bs->GetDesc(&d);
            const D3D11_RENDER_TARGET_BLEND_DESC& t = d.RenderTarget[0];
            g_bs_cat[slot] = (int8_t)blend_cat(t);
            fprintf(g_csv, "B,%d,%p,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", g_frame, (void*)bs, d.AlphaToCoverageEnable,
                    d.IndependentBlendEnable, t.BlendEnable, t.SrcBlend, t.DestBlend, t.BlendOp, t.SrcBlendAlpha,
                    t.DestBlendAlpha, t.BlendOpAlpha, t.RenderTargetWriteMask, g_bs_cat[slot]);
            return g_bs_cat[slot];
        }
    }
    return 5;
}

// X2-DIAG-CAPTURE-CATSKIP=1 (capture 20:0x: the blink survives every row and loop test; same draw list on L and R,
// different brightness): draws of one blend category are skipped in odd GetTickCount seconds, the category stepping
// 0..5 every two seconds (12 s cycle; even seconds = baseline). The category whose skip removes the L / R gap holds
// the draw that differs. Draws only while capturing, i.e. while the watched DLL is loaded.
static int g_catskip = 0, g_cat_now = -1;
// Capture 20:24: with 1 s steps a 12 s cycle missed alpha / subtract / opaque in both sphere phases, and no blend
// state falls in "other": now 0.5 s steps over categories 0..4 (5 s cycle).
// X2-DIAG-CAPTURE-CATSKIP=10+c: only category c, in odd seconds (capture 20:3x: only skipping additive draws took the
// gap away, 1.1 vs 4 / 0.9 around it, and -0.4 at 20:24).
// X2-DIAG-CAPTURE-CATSKIP=20 (capture 20:30: additive skipped -> gap 0.07 vs 3.3, confirmed): additive draws split
// by shape, 0.5 s steps, 3 s cycle: sub 0 = non-indexed full-screen Draw (glow composites, 3 vertices), 1 = indexed
// <= 24 indices (particle sprite batches), 2 = indexed > 24 (sphere / trail meshes). The F line carries 10 + sub.
static int catskip_now() {
    if (!g_catskip) return -1;
    if (g_catskip == 20) {
        DWORD s = (GetTickCount() / 500) % 6;
        return (s & 1) ? 10 + (int)(s / 2) : -1;
    }
    if (g_catskip >= 10) return ((GetTickCount() / 1000) & 1) ? g_catskip - 10 : -1;
    DWORD s = (GetTickCount() / 500) % (2 * (NCAT - 1));
    return (s & 1) ? (int)(s / 2) : -1;
}

static void STDMETHODCALLTYPE h_csr(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z,
                                    ID3D11Resource* s, UINT ss, const D3D11_BOX* b) {
    if (g_on && g_csv && !g_self) {
        EnterCriticalSection(&g_cs);
        fprintf(g_csv, "C,%d,%d,region,%p,%p,%u,%u,%u,%u\n", g_frame, g_draw, (void*)d, (void*)s, x, y,
                b ? b->right - b->left : 0, b ? b->bottom - b->top : 0);
        note_write(d);
        LeaveCriticalSection(&g_cs);
    }
    g_csr(c, d, ds, x, y, z, s, ss, b);
}
static void STDMETHODCALLTYPE h_cr(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) {
    if (g_on && g_csv && !g_self) {
        EnterCriticalSection(&g_cs);
        fprintf(g_csv, "C,%d,%d,full,%p,%p,0,0,0,0\n", g_frame, g_draw, (void*)d, (void*)s);
        note_write(d);
        LeaveCriticalSection(&g_cs);
    }
    g_cr(c, d, s);
}
static void STDMETHODCALLTYPE h_rs(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss, DXGI_FORMAT f) {
    if (g_on && g_csv) {
        EnterCriticalSection(&g_cs);
        fprintf(g_csv, "C,%d,%d,resolve,%p,%p,0,0,0,0\n", g_frame, g_draw, (void*)d, (void*)s);
        note_write(d);
        LeaveCriticalSection(&g_cs);
    }
    g_rs(c, d, ds, s, ss, f);
}
static void STDMETHODCALLTYPE h_crtv(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
    if (g_on && g_csv && v) {
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        EnterCriticalSection(&g_cs);
        fprintf(g_csv, "K,%d,%d,%p,%g,%g,%g,%g\n", g_frame, g_draw, (void*)r, col[0], col[1], col[2], col[3]);
        note_write(r);
        LeaveCriticalSection(&g_cs);
        if (r) r->Release();
    }
    g_crtv(c, v, col);
}
static HRESULT STDMETHODCALLTYPE h_gd(ID3D11DeviceContext* c, ID3D11Asynchronous* q, void* data, UINT size, UINT fl) {
    HRESULT hr = g_gd(c, q, data, size, fl);
    if (g_on && g_csv) {
        unsigned long long v = 0;
        if (hr == S_OK && data && size >= 4) v = size >= 8 ? *(unsigned long long*)data : *(UINT*)data;
        EnterCriticalSection(&g_cs);
        fprintf(g_csv, "Q,%d,%d,%p,%u,%08lX,%llu\n", g_frame, g_draw, (void*)q, size, (unsigned long)hr, v);
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

// X2-DIAG-CAPTURE-CBREAD=1 (capture 20:40: the full-screen chain whose composites carry the blink reads this frame's
// depth and scene, so its parameters must differ L / R; its constant uploads never reached the Map / Update hooks,
// all hashes 0): for every non-indexed full-screen draw, VS cb0 / PS cb0 / PS cb1 are copied to a staging buffer and
// read back on the CPU. "P" line: frame, draw, slot (0 vs0, 1 ps0, 2 ps1), size, hash of up to 512 bytes, 16 floats.
static int g_cbread = 0;
enum { NSTG = 16 };
static struct { UINT size; ID3D11Buffer* buf; } g_stg[NSTG];
static void cb_readback(ID3D11DeviceContext* c, ID3D11Buffer* b, int slot, int draw) {   // under g_cs
    if (!b || !g_csv) return;
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    ID3D11Buffer* s = nullptr;
    for (auto& e : g_stg) if (e.buf && e.size == d.ByteWidth) { s = e.buf; break; }
    if (!s) {
        ID3D11Device* dev = nullptr;
        c->GetDevice(&dev);
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (dev && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &s))) {
            for (auto& e : g_stg) if (!e.buf) { e.buf = s; e.size = d.ByteWidth; break; }
        } else s = nullptr;
        if (dev) dev->Release();
        if (!s) return;
    }
    g_self = true;
    c->CopyResource(s, b);
    g_self = false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_map(c, s, 0, D3D11_MAP_READ, 0, &m))) return;
    const uint8_t* p = (const uint8_t*)m.pData;
    UINT n = d.ByteWidth < 512 ? d.ByteWidth : 512;
    uint32_t h = 2166136261u;
    for (UINT i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    float f[16] = {};
    memcpy(f, p, n < sizeof(f) ? n : sizeof(f));
    g_unmap(c, s, 0);
    fprintf(g_csv, "P,%d,%d,%d,%u,%08X", g_frame, draw, slot, d.ByteWidth, h);
    for (float x : f) fprintf(g_csv, ",%g", x);
    fprintf(g_csv, "\n");
}

// X2-DIAG-CAPTURE-TEXMEAN=1 (capture 20:44: every full-screen pass has the same constants L / R in the sphere, so the
// glow chain's inputs must differ): for each full-screen draw, the mean R / G / B of every single-sample 8-bit RGBA /
// BGRA texture it reads ("M" line: frame, draw, slot 0-7, texture, means). Staging copy + read of every 8th pixel in
// both directions; the game runs slowly with it.
static int g_texmean = 0;
enum { NSTT = 24 };
static struct { UINT w, h; DXGI_FORMAT f; ID3D11Texture2D* t; } g_stt[NSTT];
static void tex_mean(ID3D11DeviceContext* c, ID3D11Resource* r, int slot, int draw) {   // under g_cs
    if (!r || !g_csv) return;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D*)r)->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if ((!bgra && !rgba) || d.SampleDesc.Count != 1 || d.ArraySize != 1) return;
    ID3D11Texture2D* s = nullptr;
    for (auto& e : g_stt) if (e.t && e.w == d.Width && e.h == d.Height && e.f == d.Format) { s = e.t; break; }
    if (!s) {
        ID3D11Device* dev = nullptr;
        c->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &s))) {
            for (auto& e : g_stt) if (!e.t) { e.t = s; e.w = d.Width; e.h = d.Height; e.f = d.Format; break; }
        } else s = nullptr;
        if (dev) dev->Release();
        if (!s) return;
    }
    g_self = true;
    c->CopySubresourceRegion(s, 0, 0, 0, 0, r, 0, nullptr);
    g_self = false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_map(c, s, 0, D3D11_MAP_READ, 0, &m))) return;
    double sum[3] = {}; uint32_t n = 0;
    for (UINT y = 0; y < d.Height; y += 8) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        for (UINT x = 0; x < d.Width; x += 8) { sum[0] += row[x * 4]; sum[1] += row[x * 4 + 1]; sum[2] += row[x * 4 + 2]; n++; }
    }
    g_unmap(c, s, 0);
    if (!n) return;
    double R = (bgra ? sum[2] : sum[0]) / n, G = sum[1] / n, B = (bgra ? sum[0] : sum[2]) / n;
    fprintf(g_csv, "M,%d,%d,%d,%p,%.3f,%.3f,%.3f\n", g_frame, draw, slot, (void*)r, R, G, B);
}

static void put_cb(char* out, size_t n, ID3D11Buffer* b) {
    CB* e = b ? cb_slot(b, false) : nullptr;
    if (!e) { _snprintf_s(out, n, _TRUNCATE, "%p,0,,,,,,,,", (void*)b); return; }
    _snprintf_s(out, n, _TRUNCATE, "%p,%08X,%g,%g,%g,%g,%g,%g,%g,%g", (void*)b, e->hash, e->f[0], e->f[1], e->f[2],
                e->f[3], e->f[4], e->f[5], e->f[6], e->f[7]);
}

static bool draw_tap(ID3D11DeviceContext* c, int type, UINT a, UINT b, UINT d, INT e) {
    if (!g_on || !g_csv) return false;
    // resources behind the render target and the 8 PS texture slots (a view differs from its resource; reads of an
    // earlier frame's target are matched on resources)
    void* rres = nullptr;
    void* sres[8] = {};
    // depth target written by this draw (capture 20:36: the start-of-frame glow chain reads 43FF01A4, which no tracked
    // draw, copy or clear writes; depth writes were not tracked, so a depth-fed loop was invisible to FBSKIP)
    void* dres = nullptr;
    {
        ID3D11RenderTargetView* v = nullptr;
        ID3D11DepthStencilView* dv = nullptr;
        c->OMGetRenderTargets(1, &v, &dv);
        if (v) {
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            rres = r;
            if (r) r->Release();
            v->Release();
        }
        if (dv) {
            ID3D11DepthStencilState* dss = nullptr; UINT sr = 0;
            c->OMGetDepthStencilState(&dss, &sr);
            D3D11_DEPTH_STENCIL_DESC dd = {};
            if (dss) { dss->GetDesc(&dd); dss->Release(); }
            if (dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) {
                ID3D11Resource* r = nullptr;
                dv->GetResource(&r);
                dres = r;
                if (r) r->Release();
            }
            dv->Release();
        }
        ID3D11ShaderResourceView* srvs[8] = {};
        c->PSGetShaderResources(0, 8, srvs);
        for (int i = 0; i < 8; i++)
            if (srvs[i]) {
                ID3D11Resource* r = nullptr;
                srvs[i]->GetResource(&r);
                sres[i] = r;
                if (r) r->Release();
                srvs[i]->Release();
            }
    }
    ID3D11BlendState* cbs = nullptr;
    if (g_catskip) { float f4[4]; UINT m; c->OMGetBlendState(&cbs, f4, &m); }
    EnterCriticalSection(&g_cs);
    bool fb = false;
    for (void* r : sres)
        if (ResW* w = r ? rw_slot(r, false) : nullptr)
            if (w->frame >= 0 && w->frame < g_frame) fb = true;
    bool skip = fb && fbskip_now();
    if (g_catskip && g_cat_now >= 10) {
        int sub = type == 13 ? 0 : a <= 24 ? 1 : 2;   // a = vertex / index count
        if (note_blend(cbs) == 1 && sub == g_cat_now - 10) skip = true;
    } else if (g_catskip && g_cat_now >= 0 && note_blend(cbs) == g_cat_now) skip = true;
    if (cbs) cbs->Release();
    if (fb) g_fb_reads++;
    if (skip && fb) g_fb_skipped++;
    if (!skip) { note_write(rres); note_write(dres); }
    note_res((ID3D11Resource*)rres); note_res((ID3D11Resource*)dres);
    for (void* r : sres) note_res((ID3D11Resource*)r);
    LeaveCriticalSection(&g_cs);
    if (!g_draws_on) return skip;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* bs = nullptr; ID3D11DepthStencilState* ds = nullptr;
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Buffer* vcb[2] = {}; ID3D11Buffer* pcb[2] = {};
    float bf[4] = {}; UINT mask = 0, sref = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->OMGetBlendState(&bs, bf, &mask);
    c->OMGetDepthStencilState(&ds, &sref);
    c->OMGetRenderTargets(1, &rtv, &dsv);
    c->PSGetShaderResources(0, 1, &srv);
    c->VSGetConstantBuffers(0, 2, vcb);
    c->PSGetConstantBuffers(0, 2, pcb);
    c->IAGetPrimitiveTopology(&topo);
    char res[200];
    int rn = 0;
    rn += _snprintf_s(res + rn, sizeof(res) - rn, _TRUNCATE, "%p,", rres);
    for (void* sr : sres)
        if (rn >= 0 && rn < (int)sizeof(res)) rn += _snprintf_s(res + rn, sizeof(res) - rn, _TRUNCATE, "%p|", sr);
    // the first 4 return addresses inside FFX-2.exe (Ghidra VAs): which HD code issued the draw (capture 04:20: logic
    // frames carry extra sprite batches that none of the counted submit functions explains)
    void* bt[24] = {};
    USHORT nbt = RtlCaptureStackBackTrace(1, 24, bt, nullptr);
    char stk[64];
    int sn = 0, found = 0;
    stk[0] = 0;
    for (USHORT i = 0; i < nbt && found < 4; i++)
        if (in_image(bt[i])) {
            sn += _snprintf_s(stk + sn, sizeof(stk) - sn, _TRUNCATE, "%08X|", rt_to_gva(bt[i]));
            found++;
        }
    char v0[160], v1[160], p0[160], p1[160];
    EnterCriticalSection(&g_cs);
    put_cb(v0, sizeof(v0), vcb[0]); put_cb(v1, sizeof(v1), vcb[1]);
    put_cb(p0, sizeof(p0), pcb[0]); put_cb(p1, sizeof(p1), pcb[1]);
    note_blend(bs);
    if (g_cbread && type == 13) {
        ID3D11Buffer* const which[3] = {vcb[0], pcb[0], pcb[1]};
        for (int i = 0; i < 3; i++) cb_readback(c, which[i], i, g_draw);
    }
    if (g_texmean && type == 13)
        for (int i = 0; i < 8; i++) tex_mean(c, (ID3D11Resource*)sres[i], i, g_draw);
    fprintf(g_csv, "D,%d,%d,%d,%u,%u,%u,%d,%d,%p,%p,%p,%g,%g,%g,%g,%p,%p,%p,%s,%s,%s,%s,%s,%s,%d\n", g_frame, g_draw++, type, a,
            b, d, e, (int)topo, (void*)vs, (void*)ps, (void*)bs, bf[0], bf[1], bf[2], bf[3], (void*)ds, (void*)rtv,
            (void*)srv, v0, v1, p0, p1, res, stk, fb ? (skip ? 2 : 1) : 0);
    LeaveCriticalSection(&g_cs);
    if (vs) vs->Release(); if (ps) ps->Release(); if (bs) bs->Release(); if (ds) ds->Release();
    if (rtv) rtv->Release(); if (dsv) dsv->Release(); if (srv) srv->Release();
    for (ID3D11Buffer* x : vcb) if (x) x->Release();
    for (ID3D11Buffer* x : pcb) if (x) x->Release();
    return skip;
}

static void game_path(char* out, const char* file) {
    DWORD n = GetModuleFileNameA(nullptr, out, MAX_PATH);
    while (n && out[n - 1] != '\\' && out[n - 1] != '/') n--;
    _snprintf_s(out + n, MAX_PATH - n, _TRUNCATE, "%s", file);
}

static int g_toggles_on = 0;
static void toggles_report();
static void stop(const char* why) {
    g_on = false;
    EnterCriticalSection(&g_cs);
    if (g_csv && g_toggles_on) toggles_report();
    if (g_csv) { fclose(g_csv); g_csv = nullptr; }
    if (g_bin) { fclose(g_bin); g_bin = nullptr; }
    LeaveCriticalSection(&g_cs);
    // U 23/10/2026 04:01: Ultima loaded once for 168 frames before the real cast; a load shorter than
    // X2-DIAG-CAPTURE-MIN presents (default 300) does not count, the next load is captured over it.
    g_done = g_loads > 1 || g_frame >= g_min;
    log_line("X2-DIAG-CAPTURE: stopped (%s) after %d frames%s", why, g_frame, g_done ? "" : " - too short, next load captured");
}

static void save_frame(IDXGISwapChain* sc, int kind) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if ((!bgra && !rgba) || d.SampleDesc.Count != 1) { bb->Release(); return; }
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    bb->GetDevice(&dev);
    dev->GetImmediateContext(&ctx);
    if (!g_stage) {
        D3D11_TEXTURE2D_DESC s = d;
        s.MipLevels = 1; s.ArraySize = 1; s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.MiscFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&s, nullptr, &g_stage))) g_stage = nullptr;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (g_stage) {
        g_self = true;
        ctx->CopyResource(g_stage, bb);
        g_self = false;
        if (SUCCEEDED(g_map(ctx, g_stage, 0, D3D11_MAP_READ, 0, &m))) {
            uint32_t w = d.Width / 4, h = d.Height / 4;
            uint8_t* buf = (uint8_t*)malloc((size_t)w * h * 3);
            if (buf) {
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++) {
                        uint32_t s[3] = {};
                        for (int yy = 0; yy < 4; yy++) {
                            const uint8_t* row = (const uint8_t*)m.pData + (size_t)(y * 4 + yy) * m.RowPitch + x * 16;
                            for (int xx = 0; xx < 4; xx++) { s[0] += row[xx * 4]; s[1] += row[xx * 4 + 1]; s[2] += row[xx * 4 + 2]; }
                        }
                        uint8_t* o = buf + ((size_t)y * w + x) * 3;
                        o[0] = (uint8_t)((bgra ? s[2] : s[0]) / 16); o[1] = (uint8_t)(s[1] / 16); o[2] = (uint8_t)((bgra ? s[0] : s[2]) / 16);
                    }
                double sum = 0;
                for (size_t i = 0; i < (size_t)w * h * 3; i += 3) sum += 0.299 * buf[i] + 0.587 * buf[i + 1] + 0.114 * buf[i + 2];
                g_last_luma = sum / ((double)w * h);
                if (g_images) {
                    uint32_t hdr[4] = {(uint32_t)g_frame, (uint32_t)kind, w, h};
                    fwrite(hdr, 4, 4, g_bin);
                    fwrite(buf, 1, (size_t)w * h * 3, g_bin);
                }
                free(buf);
            }
            g_unmap(ctx, g_stage, 0);
        }
    }
    ctx->Release(); dev->Release(); bb->Release();
}

// ---- toggle scan (X2-DIAG-CAPTURE-TOGGLES=1; capture 19:01: skipping the scene feedback draws keeps the blink, and
// L and R frames issue the same draws, so the difference is in data). Every present, each dword of FFX-2.exe .data,
// the watched DLL's .data and the HD frame object (64 KB) is compared with the two presents before: a dword back at
// its value of two presents ago but not the last one counts as an alternation; when the frame's luma also moved by
// more than 2 from the previous present, "agree" adds +1 if the dword rose with the luma and -1 if it fell. A dword
// that drives the blink scores |agree| near its alternation count with the same sign in every cast. "T" lines at the
// end of each load: the top dwords by |agree|, then by alternations.
struct TReg { const char* name; uint8_t* base; uint32_t n; uint32_t *p1, *p2; uint16_t* alt; int16_t* agr; };
enum { NREG = 3 };
static TReg g_treg[NREG];
static int g_toggles = 0, g_tframes = 0;
static double g_tprev_luma = -1;
static bool pe_data(uint8_t* base, uint8_t** out, uint32_t* size) {
    __try {
        IMAGE_DOS_HEADER* dh = (IMAGE_DOS_HEADER*)base;
        if (dh->e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dh->e_lfanew);
        IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (!memcmp(s[i].Name, ".data", 6)) { *out = base + s[i].VirtualAddress; *size = s[i].Misc.VirtualSize; return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static void treg_free(TReg& r) {
    free(r.p1); free(r.p2); free(r.alt); free(r.agr);
    r = TReg{};
}
static void treg_set(int i, const char* name, uint8_t* base, uint32_t bytes) {
    TReg& r = g_treg[i];
    treg_free(r);
    if (!base || bytes < 4) return;
    r.name = name; r.base = base; r.n = bytes / 4;
    r.p1 = (uint32_t*)calloc(r.n, 4); r.p2 = (uint32_t*)calloc(r.n, 4);
    r.alt = (uint16_t*)calloc(r.n, 2); r.agr = (int16_t*)calloc(r.n, 2);
    if (!r.p1 || !r.p2 || !r.alt || !r.agr) { log_line("X2-DIAG-CAPTURE: toggle scan %s: out of memory", name); treg_free(r); }
}
static void toggles_start() {
    uint8_t* b = nullptr; uint32_t sz = 0;
    if (pe_data((uint8_t*)GetModuleHandleA(nullptr), &b, &sz)) treg_set(0, "exe", b, sz);
    b = nullptr; sz = 0;
    if (uint8_t* m = ftblink_base()) if (pe_data(m, &b, &sz)) treg_set(1, "dll", b, sz);
    uint8_t* h = nullptr;
    __try { h = g_hdframe ? *g_hdframe : nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) { h = nullptr; }
    treg_set(2, "hdframe", h, 0x10000);
    g_tframes = 0; g_tprev_luma = -1;
    log_line("X2-DIAG-CAPTURE: toggle scan exe %u / dll %u / hdframe %u dwords", g_treg[0].n, g_treg[1].n, g_treg[2].n);
}
static void toggles_step(double luma) {
    double dl = (g_tprev_luma >= 0 && luma >= 0) ? luma - g_tprev_luma : 0;
    g_tprev_luma = luma;
    int s = dl > 2 ? 1 : dl < -2 ? -1 : 0;
    for (TReg& r : g_treg) {
        if (!r.n) continue;
        __try {
            const uint32_t* cur = (const uint32_t*)r.base;
            for (uint32_t i = 0; i < r.n; i++) {
                uint32_t v = cur[i];
                if (g_tframes >= 2 && v == r.p2[i] && v != r.p1[i]) {
                    if (r.alt[i] < 0xFFFF) r.alt[i]++;
                    if (s) {
                        int a = r.agr[i] + (v > r.p1[i] ? s : -s);
                        if (a > -32768 && a < 32767) r.agr[i] = (int16_t)a;
                    }
                }
                r.p2[i] = r.p1[i]; r.p1[i] = v;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-DIAG-CAPTURE: toggle scan %s unreadable - dropped", r.name); treg_free(r); }
    }
    g_tframes++;
}
static void toggles_report() {                   // under g_cs, files open
    enum { TOP = 60 };
    for (int pass = 0; pass < 2; pass++) {
        struct Top { int reg; uint32_t i; int score; } top[TOP] = {};
        int nt = 0;
        for (int k = 0; k < NREG; k++) {
            TReg& r = g_treg[k];
            for (uint32_t i = 0; r.n && i < r.n; i++) {
                int sc = pass == 0 ? abs((int)r.agr[i]) : (int)r.alt[i];
                if (sc < 4) continue;
                if (nt < TOP) top[nt++] = {k, i, sc};
                else {
                    int lo = 0;
                    for (int j = 1; j < TOP; j++) if (top[j].score < top[lo].score) lo = j;
                    if (sc > top[lo].score) top[lo] = {k, i, sc};
                }
            }
        }
        for (int a = 0; a < nt; a++)
            for (int b = a + 1; b < nt; b++) if (top[b].score > top[a].score) { Top t = top[a]; top[a] = top[b]; top[b] = t; }
        for (int a = 0; a < nt; a++) {
            TReg& r = g_treg[top[a].reg];
            uint8_t* p = r.base + top[a].i * 4;
            fprintf(g_csv, "T,%d,%s,%s,+0x%X,%08X,%u,%d,%u,%08X,%08X\n", g_frame, pass == 0 ? "agree" : "alt", r.name,
                    top[a].i * 4, top[a].reg == 0 ? rt_to_gva(p) : 0, r.alt[top[a].i], r.agr[top[a].i], g_tframes,
                    r.p1[top[a].i], r.p2[top[a].i]);
        }
    }
    for (TReg& r : g_treg) treg_free(r);
}

static void present_tap(IDXGISwapChain* sc) {
    if (!row_DIAGCAPTURE.on || g_disabled || g_done) return;
    bool w = ftblink_watching();
    if (w && !g_was && !g_on && g_csv && g_bin) {          // next load of a multi-load capture
        g_on = true;
        if (g_toggles) toggles_start();
        log_line("X2-DIAG-CAPTURE: load %d of %d, frames continue at %d", g_loads_done + 1, g_loads, g_frame);
    } else if (w && !g_was && !g_on) {
        char p[MAX_PATH];
        game_path(p, "fps60x2-capture-draws.csv");
        fopen_s(&g_csv, p, "w");
        game_path(p, "fps60x2-capture-frames.bin");
        fopen_s(&g_bin, p, "wb");
        if (g_csv && g_bin) {
            setvbuf(g_csv, nullptr, _IOFBF, 1 << 20);
            fprintf(g_csv, "C,frame,before_draw,kind,dst_res,src_res,x,y,w,h\nK,frame,before_draw,rt_res,r,g,b,a\nQ,frame,before_draw,query,size,hr,value\n"
                           "S,frame (submits before this frame's F line),fn,caller_gva,calls,skipflag_set,program,class,sum_alpha,sum_rgb,sum_scale\nF,frame,kind,fc,bisect_step,luma,hd44,hd64,hd68,hdFE4,hdE920,present_dt_ms,fb_reads,fb_skipped,frame_byte_0106,cam_r2x,cam_r2y,cam_r2z,cam_r2w,cam_px,cam_py,cam_pz,cam_pw,catskip\n"
                           "T,frame,list,region,offset,exe_gva,alternations,agree,frames,last,before_last\n"
                           "R,frame,texture,width,height,dxgi_format,bind,misc,samples,mips\n"
                           "B,frame,blend,alpha_to_cov,independent,enable,src,dst,op,src_a,dst_a,op_a,mask,category\nD,frame,draw,type,a,b,d,e,topo,vs,ps,blend,bf0,bf1,bf2,bf3,depth,rtv,srv,"
                           "vcb0,vh0,v0f0..7,,,,,,,,vcb1,vh1,v1f0..7,,,,,,,,pcb0,ph0,p0f0..7,,,,,,,,pcb1,ph1,p1f0..7,,,,,,,,rt_res,srv_res0|..|7,exe_stack,fb(1 read 2 skipped)\n");
            memset(g_cb, 0, sizeof(g_cb));
            memset(g_rw, 0, sizeof(g_rw));
            memset(g_bs_seen, 0, sizeof(g_bs_seen));
            memset(g_res_seen, 0, sizeof(g_res_seen));
            memset(g_sc, 0, sizeof(g_sc));
            g_frame = 0; g_draw = 0;
            g_on = true;
            if (g_toggles) toggles_start();
            log_line("X2-DIAG-CAPTURE: started (up to %d frames)", g_max);
        } else stop("files not opened");
    }
    g_was = w;
    if (!g_on) return;
    if (!w) {
        if (g_loads > 1 && ++g_loads_done < g_loads && g_frame < g_max) {
            g_on = false;                        // files stay open for the next load
            if (g_toggles) { EnterCriticalSection(&g_cs); toggles_report(); LeaveCriticalSection(&g_cs); }
            log_line("X2-DIAG-CAPTURE: load %d of %d done at frame %d", g_loads_done, g_loads, g_frame);
        } else stop("DLL unloaded");
        return;
    }
    int kind = pacing_kind() & 3;
    EnterCriticalSection(&g_cs);
    sub_flush();
    g_last_luma = -1;
    save_frame(sc, kind);
    // X2-DIAG-CAPTURE-TOGGLES=2 (U 23/10/2026 21:3x: Paine's dressphere card, magic_0673, alternates between two states
    // on every game tick, render-only frames repeat the logic frame: a 15 Hz flicker at 60 fps, 30 Hz at stock): the
    // scan samples on logic frames only, so a per-tick toggle shows as the same alternation, "agree" against the luma
    // change between consecutive logic frames.
    if (g_toggles == 1 || (g_toggles == 2 && kind == INV_LOGIC)) toggles_step(g_last_luma);
    uint32_t hd[5] = {};
    __try {
        uint8_t* h = g_hdframe ? *g_hdframe : nullptr;
        if (h) { hd[0] = *(uint32_t*)(h + 0x44); hd[1] = *(uint32_t*)(h + 0x64); hd[2] = *(uint32_t*)(h + 0x68);
                 hd[3] = *(uint32_t*)(h + 0xFE4); hd[4] = h[0xE920]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // magic_0106 frame byte DAT_101c282c (++ per Mag_Draw 0x10002730 on every invocation): its parity at
    // load would set a per-cast blink phase that follows presents
    int fbyte = -1;
    __try { if (uint8_t* m = ftblink_base()) fbyte = m[0x1C282C]; } __except (EXCEPTION_EXECUTE_HANDLER) { fbyte = -1; }
    // HD camera actually used: slot 0 world matrix copied by HdCam_Set 0x482320 to hdframe+0xE78C (outer repo
    // re/ledger/HDNODE.md §0: first commit per HD frame wins; on logic frames the game's raw view commits first). Rows
    // 2 and 3 (forward, position) show whether the view zigzags between logic and render-only frames.
    float cam[8] = {};
    __try {
        uint8_t* h = g_hdframe ? *g_hdframe : nullptr;
        if (h) { const float* m = (const float*)(h + 0xE78C); for (int i = 0; i < 4; i++) { cam[i] = m[8 + i]; cam[4 + i] = m[12 + i]; } }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // time since the previous present (bisect 04:16: the blink follows present parity; 59.94 Hz with ms timers gives
    // alternating 16 / 17 ms frames)
    static LARGE_INTEGER qf = {}, last = {};
    LARGE_INTEGER now;
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&now);
    double dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)qf.QuadPart : 0.0;
    last = now;
    fprintf(g_csv, "F,%d,%d,%u,%d,%.3f,%08X,%08X,%08X,%08X,%u,%.3f,%d,%d,%d,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%d\n", g_frame, kind, g_frame_counter ? *g_frame_counter : 0,
            g_bisect_step, g_last_luma, hd[0], hd[1], hd[2], hd[3], hd[4], dt, g_fb_reads, g_fb_skipped, fbyte, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6], cam[7], g_cat_now);
    g_cat_now = catskip_now();                   // one category for the whole next frame
    g_fb_reads = g_fb_skipped = 0;
    // X2-DIAG-CAPTURE-PHASEFLIP=<s>: every s seconds one repeated invocation kind (capture 19:05: logic frames, the
    // 0106 frame byte's parity and the present count stayed locked through both loads, so the blink could follow any)
    // X2-DIAG-CAPTURE-BACKLOG=<s>: instead, one injected no-draw logic invocation every s seconds (capture 19:10: the
    // sign changed only at a hitch with a BACKLOG invocation; H: it is set by the parity of game logic steps against
    // 0106's own steps, which a no-draw step shifts and a repeated kind does not)
    static DWORD next_flip = 0;
    int every = g_backlog_inj > 0 ? g_backlog_inj : g_phaseflip;
    if (every > 0) {
        DWORD t = GetTickCount();
        if (!next_flip || (int)(t - next_flip) >= 0) {
            if (next_flip) {
                if (g_backlog_inj > 0) pacing_inject_backlog();
                else pacing_flip_phase();
            }
            next_flip = t + (DWORD)every * 1000;
        }
    }
    g_frame++;
    g_draw = 0;
    LeaveCriticalSection(&g_cs);
    if (g_frame >= g_max) stop("frame limit");
}

static bool install() {
    if (!row_DIAGBLINK.on) { log_line("  X2-DIAG-CAPTURE: needs X2-DIAG-BLINK (draw hooks) - off"); return false; }
    if (!present_hook()) return false;
    InitializeCriticalSection(&g_cs);
    g_max = config_row("X2-DIAG-CAPTURE-FRAMES", 900);
    g_min = config_row("X2-DIAG-CAPTURE-MIN", 300);
    g_draws_on = config_row("X2-DIAG-CAPTURE-DRAWS", 1);
    g_images = config_row("X2-DIAG-CAPTURE-IMAGES", 1);
    g_fbskip = config_row("X2-DIAG-CAPTURE-FBSKIP", 0);
    g_toggles = g_toggles_on = config_row("X2-DIAG-CAPTURE-TOGGLES", 0);
    g_phaseflip = config_row("X2-DIAG-CAPTURE-PHASEFLIP", 0);
    if (g_phaseflip) log_line("  X2-DIAG-CAPTURE: logic / render-only phase flipped every %d s while capturing", g_phaseflip);
    g_cbread = config_row("X2-DIAG-CAPTURE-CBREAD", 0);
    g_texmean = config_row("X2-DIAG-CAPTURE-TEXMEAN", 0);
    g_catskip = config_row("X2-DIAG-CAPTURE-CATSKIP", 0);
    if (g_catskip) log_line("  X2-DIAG-CAPTURE: one blend category of draws skipped in odd seconds (0..5, 12 s cycle)");
    g_backlog_inj = config_row("X2-DIAG-CAPTURE-BACKLOG", 0);
    if (g_backlog_inj) log_line("  X2-DIAG-CAPTURE: one no-draw logic invocation injected every %d s while capturing", g_backlog_inj);
    if (g_fbskip) log_line("  X2-DIAG-CAPTURE: feedback-read draws skipped %s", g_fbskip == 2 ? "in odd seconds" : "always");
    if (uint8_t* s = sig_find("X2-DIAG-CAPTURE svc 0xEBC FUN_0044d260", "8B 0D ?? ?? ?? ?? E9 E5 D9 01 00", 0x0044D260)) {
        uint8_t* p = abs_operand("g_dwHdFrame", s + 2);
        if (p && rt_to_gva(p) == 0x00DCDA04) g_hdframe = (uint8_t**)p;
    }
    log_line("  X2-DIAG-CAPTURE: HD frame pointer %s", g_hdframe ? "gva=0x00DCDA04" : "not found - hd fields 0");
    g_loads = config_row("X2-DIAG-CAPTURE-LOADS", 1);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    void* fm = nullptr; void* fu = nullptr; void* fs = nullptr;
    void* fx[5] = {};       // CopySubresourceRegion 46, CopyResource 47, ResolveSubresource 57, ClearRenderTargetView 50, GetData 29
    if (SUCCEEDED(hr) && ctx) {
        void** vt = *(void***)ctx; fm = vt[14]; fu = vt[15]; fs = vt[48];
        fx[0] = vt[46]; fx[1] = vt[47]; fx[2] = vt[57]; fx[3] = vt[50]; fx[4] = vt[29];
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (!fm) { log_line("  X2-DIAG-CAPTURE: throwaway device failed (0x%08lX) - off", (unsigned long)hr); return false; }
    if (!hook_entry("X2-DIAG-CAPTURE Map", fm, (void*)&h_map, (void**)&g_map)) return false;
    if (!hook_entry("X2-DIAG-CAPTURE Unmap", fu, (void*)&h_unmap, (void**)&g_unmap)) { hook_remove(fm); return false; }
    if (!hook_entry("X2-DIAG-CAPTURE UpdateSubresource", fs, (void*)&h_update, (void**)&g_update)) { hook_remove(fm); hook_remove(fu); return false; }
    static const char* XN[5] = {"X2-DIAG-CAPTURE CopySubresourceRegion", "X2-DIAG-CAPTURE CopyResource",
                                "X2-DIAG-CAPTURE ResolveSubresource", "X2-DIAG-CAPTURE ClearRenderTargetView", "X2-DIAG-CAPTURE GetData"};
    void* XD[5] = {(void*)&h_csr, (void*)&h_cr, (void*)&h_rs, (void*)&h_crtv, (void*)&h_gd};
    void** XO[5] = {(void**)&g_csr, (void**)&g_cr, (void**)&g_rs, (void**)&g_crtv, (void**)&g_gd};
    int nx = 0;
    for (int i = 0; i < 5; i++) if (fx[i] && hook_entry(XN[i], fx[i], XD[i], XO[i])) nx++;
    log_line("  X2-DIAG-CAPTURE: %d of 5 copy / clear / query hooks", nx);
    static const struct { const char* label; const char* pat; uint32_t gva; } SIGS[NSUB] = {
        {"X2-DIAG-CAPTURE sprite submit A FUN_00808940", "55 8B EC 81 EC 18 01 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 45 0C", 0x00808940},
        {"X2-DIAG-CAPTURE sprite submit B FUN_008099d0", "55 8B EC 81 EC 24 01 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 83 3D ?? ?? ?? ?? 00", 0x008099D0},
        {"X2-DIAG-CAPTURE Hd_VfxSubmit", "55 8B EC 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 8B 4D 08 8B 55 0C 8B C1 0B C2 0F 84 ?? ?? ?? ?? 56 52 51 E8", 0x00456A00},
        {"X2-DIAG-CAPTURE Hd_VfxCreate", "55 8B EC 83 EC 08 53 8B 5D 0C 57 FF 75 1C", 0x007FDDE0},
        {"X2-DIAG-CAPTURE HD VFX update FUN_0080a9e0", "55 8B EC 81 EC AC 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 4D 14 8B 45 0C 53", 0x0080A9E0},
    };
    int nsub = 0;
    for (int i = 0; i < NSUB; i++) {
        uint8_t* t = sig_find(SIGS[i].label, SIGS[i].pat, SIGS[i].gva);
        if (i == 2 && t) {
            g_skip = (volatile int32_t*)abs_operand("render skip flag", t + 5);
            if (g_skip && rt_to_gva((const void*)g_skip) != 0x00DF6438) g_skip = nullptr;
        }
        if (t && hook_entry(SIGS[i].label, t, SUB_DET[i], (void**)&g_sub[i])) nsub++;
    }
    log_line("  X2-DIAG-CAPTURE: %d of %d HD submit functions counted per caller; skip flag %s", nsub, NSUB,
             g_skip ? "gva=0x00DF6438" : "not found");
    g_draw_tap = draw_tap;
    g_present_tap2 = present_tap;
    log_line("  X2-DIAG-CAPTURE: draws, constant buffers and 1/4-size frames captured on the first watched DLL load (max %d frames)", g_max);
    return true;
}

Row row_DIAGCAPTURE = {
    "X2-DIAG-CAPTURE",
    "diagnostic: every D3D11 draw (state, constants) and every frame (1/4 size) while a watched magic DLL is loaded, to files",
    0, install, nullptr, nullptr, 0
};

} // namespace x2
