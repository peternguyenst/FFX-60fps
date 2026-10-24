// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-AUTOBLINK (diagnostic, on while the mod is in development): finds blinking effects by watching the image.
//
// X2-DIAG-BLINK counts draw calls, which misses blinks that draw the same things on both kinds of frame (Ultima's
// glow). This row watches the picture: every present, the back buffer is shrunk on the GPU to a grid of at most 16 x 9
// cells and read back two presents later (no stall). For each frame it takes the mean brightness of each cell and of
// the whole frame.
// The test: a blink is a zigzag (up, down, up ...) in step with logic / render-only frames, with one kind brighter every
// time; motion and fades are not (a 30 fps animation shown at 60 fps is a staircase). For frame n it measures how much
// n sticks out from both neighbours, signs it by the frame's kind, and averages that over 60 frames.
// Verdict per 60-frame window: the whole frame's average >= X2-DIAG-AUTOBLINK-WHOLE (15 = 1.5 brightness levels) or
// any cell's >= X2-DIAG-AUTOBLINK-CELL (40), with >= 60% of the frames agreeing: a "[BLINK]" log line with the loaded
// spell DLLs, the cell and which kind is brighter; a summary per DLL at unload.
// X2-DIAG-AUTOBLINK-DUMP=1: the first blink of a load also saves 4 frames as BMP (<game>\fps60x2-blink-*.bmp).
// History: re/ledger/ULTIMA-0106.md.
#include "../x2.h"
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

namespace x2 {

extern Row row_DIAGAUTOBLINK;
extern void (*volatile g_present_tap3)(IDXGISwapChain* sc);
bool present_hook();

enum { RING = 3, GW = 16, GH = 9, NC = GW * GH, WIN = 60 };

static ID3D11Texture2D* g_mip = nullptr;         // back-buffer copy with a full mip chain
static ID3D11ShaderResourceView* g_srv = nullptr;
static ID3D11Texture2D* g_stage[RING] = {};
static ID3D11Texture2D* g_dump_stage = nullptr;
static UINT g_w = 0, g_h = 0, g_level = 0, g_gw = 0, g_gh = 0, g_dw = 0, g_dh = 0;
static DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
static bool g_bgra = false, g_failed = false;
struct Slot { bool used; int kind; uint32_t frame; };
static Slot g_slot[RING];
static uint32_t g_present = 0;

// analysis
struct Sample { bool ok; int kind; uint32_t frame; float whole; float cell[NC]; };
static Sample g_s[3];                             // n-1, n, n+1 (g_s[2] newest)
static double g_sum_w = 0, g_sum_c[NC];
static int g_pos_w = 0, g_neg_w = 0, g_pos_c[NC], g_neg_c[NC], g_judged = 0;
static int g_thr_whole = 15, g_thr_cell = 40, g_dump = 0, g_dumping = 0, g_dump_n = 0;
static char g_dump_tag[32];

// loaded magic DLLs (names) and per-load verdicts
static CRITICAL_SECTION g_cs;
enum { MAXDLL = 8 };
struct Dll { uint8_t* base; char name[24]; float max_whole, max_cell; int blinks, windows; bool dumped; };
static Dll g_dll[MAXDLL];
static float g_nomagic_max = 0;
static int g_nomagic_blinks = 0;

static void on_load(uint8_t* base, uint32_t, const char* name) {
    EnterCriticalSection(&g_cs);
    for (Dll& d : g_dll)
        if (!d.base) {
            d = Dll{};
            d.base = base;
            const char* p = strrchr(name, '\\');
            strncpy_s(d.name, p ? p + 1 : name, _TRUNCATE);
            break;
        }
    LeaveCriticalSection(&g_cs);
}
static void on_unload(uint8_t* base) {
    EnterCriticalSection(&g_cs);
    for (Dll& d : g_dll)
        if (d.base == base) {
            if (d.windows)
                log_line("X2-DIAG-AUTOBLINK: %s unloaded: %d of %d windows blinked, max whole %.1f, max cell %.1f%s", d.name,
                         d.blinks, d.windows, d.max_whole / 10.0f, d.max_cell / 10.0f, d.blinks ? "  [BLINK]" : "");
            d = Dll{};
        }
    LeaveCriticalSection(&g_cs);
}

static void release_all() {
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_mip) { g_mip->Release(); g_mip = nullptr; }
    for (auto& s : g_stage) if (s) { s->Release(); s = nullptr; }
    if (g_dump_stage) { g_dump_stage->Release(); g_dump_stage = nullptr; }
    memset(g_slot, 0, sizeof(g_slot));
}

static bool create(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& bb) {
    release_all();
    DXGI_FORMAT f = bb.Format;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) f = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if ((!g_bgra && !rgba) || bb.SampleDesc.Count != 1) return false;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = bb.Width; d.Height = bb.Height; d.MipLevels = 0; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_mip))) { g_mip = nullptr; return false; }
    if (FAILED(dev->CreateShaderResourceView(g_mip, nullptr, &g_srv))) { g_srv = nullptr; release_all(); return false; }
    UINT lv = 0, w = bb.Width, h = bb.Height;
    while (w > GW || h > GH) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; lv++; }
    g_level = lv; g_gw = w; g_gh = h;
    g_dw = bb.Width >> 2; g_dh = bb.Height >> 2;   // mip level 2 for dumps
    for (int i = 0; i < RING; i++) {
        D3D11_TEXTURE2D_DESC s = {};
        s.Width = w; s.Height = h; s.MipLevels = 1; s.ArraySize = 1; s.Format = f; s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&s, nullptr, &g_stage[i]))) { g_stage[i] = nullptr; release_all(); return false; }
    }
    if (g_dump && g_dw && g_dh) {
        D3D11_TEXTURE2D_DESC s = {};
        s.Width = g_dw; s.Height = g_dh; s.MipLevels = 1; s.ArraySize = 1; s.Format = f; s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&s, nullptr, &g_dump_stage))) g_dump_stage = nullptr;
    }
    g_w = bb.Width; g_h = bb.Height; g_fmt = bb.Format;
    log_line("X2-DIAG-AUTOBLINK: %ux%u back buffer, grid %ux%u (mip %u)", g_w, g_h, g_gw, g_gh, g_level);
    return true;
}

static float luma(const uint8_t* p) {
    float r = g_bgra ? p[2] : p[0], g = p[1], b = g_bgra ? p[0] : p[2];
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

static void game_path(char* out, const char* file) {
    DWORD n = GetModuleFileNameA(nullptr, out, MAX_PATH);
    while (n && out[n - 1] != '\\' && out[n - 1] != '/') n--;
    _snprintf_s(out + n, MAX_PATH - n, _TRUNCATE, "%s", file);
}

static void dump_frame(ID3D11DeviceContext* c, int kind) {
    if (!g_dump_stage) return;
    c->CopySubresourceRegion(g_dump_stage, 0, 0, 0, 0, g_mip, 2, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(c->Map(g_dump_stage, 0, D3D11_MAP_READ, 0, &m))) return;
    char name[64], path[MAX_PATH];
    _snprintf_s(name, _TRUNCATE, "fps60x2-blink-%s-%d%s.bmp", g_dump_tag, g_dump_n, kind == INV_LOGIC ? "L" : "R");
    game_path(path, name);
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (f) {
        UINT row = (g_dw * 3 + 3) & ~3u, size = 54 + row * g_dh;
        uint8_t hdr[54] = {'B', 'M'};
        *(uint32_t*)(hdr + 2) = size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
        *(int32_t*)(hdr + 18) = (int32_t)g_dw; *(int32_t*)(hdr + 22) = -(int32_t)g_dh;
        *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 24; *(uint32_t*)(hdr + 34) = row * g_dh;
        fwrite(hdr, 1, 54, f);
        uint8_t* line = (uint8_t*)calloc(row, 1);
        for (UINT y = 0; y < g_dh && line; y++) {
            const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (UINT x = 0; x < g_dw; x++) {
                const uint8_t* p = src + x * 4;
                line[x * 3] = g_bgra ? p[0] : p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = g_bgra ? p[2] : p[0];
            }
            fwrite(line, 1, row, f);
        }
        free(line);
        fclose(f);
    }
    c->Unmap(g_dump_stage, 0);
    if (++g_dump_n >= 4) { g_dumping = 0; log_line("X2-DIAG-AUTOBLINK: 4 frames saved as fps60x2-blink-%s-*.bmp", g_dump_tag); }
}

static void verdict() {
    double mw = g_sum_w / g_judged;
    int cw = mw >= 0 ? g_pos_w : g_neg_w;
    double best = 0; int bc = -1, bcons = 0;
    for (UINT i = 0; i < g_gw * g_gh; i++) {
        double m = g_sum_c[i] / g_judged;
        int cons = m >= 0 ? g_pos_c[i] : g_neg_c[i];
        if (fabs(m) > fabs(best) && cons * 10 >= g_judged * 6) { best = m; bc = (int)i; bcons = cons; }
    }
    bool whole = fabs(mw) * 10 >= g_thr_whole && cw * 10 >= g_judged * 6;
    bool cell = bc >= 0 && fabs(best) * 10 >= g_thr_cell;
    char dlls[200];
    int dn = 0;
    dlls[0] = 0;
    EnterCriticalSection(&g_cs);
    bool any = false;
    for (Dll& d : g_dll)
        if (d.base) {
            any = true;
            d.windows++;
            if (whole || cell) d.blinks++;
            // maxima over every window, flagged or not (U 23/10 21:25: a dressphere-change blink stayed below the
            // thresholds; the unload line shows how close each DLL came)
            if (fabs(mw) * 10 > d.max_whole) d.max_whole = (float)(fabs(mw) * 10);
            if (fabs(best) * 10 > d.max_cell) d.max_cell = (float)(fabs(best) * 10);
            if (dn < (int)sizeof(dlls) - 30) dn += _snprintf_s(dlls + dn, sizeof(dlls) - dn, _TRUNCATE, "%s ", d.name);
        }
    bool first_dump = false;
    if ((whole || cell) && g_dump && !g_dumping) {
        for (Dll& d : g_dll) if (d.base && !d.dumped) { d.dumped = true; first_dump = true; }
        if (!any && g_nomagic_blinks == 0) first_dump = true;
    }
    LeaveCriticalSection(&g_cs);
    if (!any && (whole || cell)) {
        g_nomagic_blinks++;
        if (fabs(mw) * 10 > g_nomagic_max) g_nomagic_max = (float)(fabs(mw) * 10);
    }
    if (whole || cell) {
        static DWORD last = 0;
        static int quiet = 0;
        DWORD now = GetTickCount();
        if (now - last >= 2000) {
            last = now;
            log_line("X2-DIAG-AUTOBLINK: [BLINK] %s: whole %+.1f (%d/%d frames), cell %d,%d %+.1f (%d/%d); brighter on %s; "
                     "magic: %s%s", whole ? "frame" : "region", mw, cw, g_judged, bc >= 0 ? bc % (int)g_gw : -1,
                     bc >= 0 ? bc / (int)g_gw : -1, best, bcons, g_judged,
                     (whole ? mw : best) > 0 ? "LOGIC" : "RENDER-ONLY", any ? dlls : "none", quiet ? " (more since last line)" : "");
            quiet = 0;
        } else quiet++;
        if (first_dump && g_dump_stage) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            _snprintf_s(g_dump_tag, _TRUNCATE, "%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
            g_dumping = 1; g_dump_n = 0;
        }
    }
}

static void analyse(const Sample& s) {
    g_s[0] = g_s[1]; g_s[1] = g_s[2]; g_s[2] = s;
    const Sample &p = g_s[0], &n = g_s[1], &q = g_s[2];
    if (!p.ok || !n.ok || !q.ok) return;
    if (n.frame != p.frame + 1 || q.frame != n.frame + 1) return;
    bool alt = (n.kind == INV_LOGIC || n.kind == INV_RENDER_ONLY) && p.kind == q.kind && p.kind != n.kind &&
               (p.kind == INV_LOGIC || p.kind == INV_RENDER_ONLY);
    if (!alt) return;
    double sg = n.kind == INV_LOGIC ? 1.0 : -1.0;
    auto zig = [](float a0, float a1, float a2) -> double {
        double a = a1 - a0, b = a2 - a1;
        if (a * b >= 0) return 0.0;
        double m = fabs(a) < fabs(b) ? fabs(a) : fabs(b);
        return a > 0 ? m : -m;
    };
    double ew = sg * zig(p.whole, n.whole, q.whole);
    g_sum_w += ew;
    if (ew > 0.5) g_pos_w++; else if (ew < -0.5) g_neg_w++;
    for (UINT i = 0; i < g_gw * g_gh; i++) {
        double e = sg * zig(p.cell[i], n.cell[i], q.cell[i]);
        g_sum_c[i] += e;
        if (e > 0.5) g_pos_c[i]++; else if (e < -0.5) g_neg_c[i]++;
    }
    if (++g_judged >= WIN) {
        verdict();
        g_sum_w = 0; g_pos_w = g_neg_w = 0; g_judged = 0;
        memset(g_sum_c, 0, sizeof(g_sum_c)); memset(g_pos_c, 0, sizeof(g_pos_c)); memset(g_neg_c, 0, sizeof(g_neg_c));
    }
}

static void present_tap(IDXGISwapChain* sc) {
    if (!row_DIAGAUTOBLINK.on || g_disabled || g_failed) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* c = nullptr;
    bb->GetDevice(&dev);
    dev->GetImmediateContext(&c);
    if (!g_mip || d.Width != g_w || d.Height != g_h || d.Format != g_fmt) {
        if (!create(dev, d)) {
            g_failed = true;
            log_line("X2-DIAG-AUTOBLINK: back buffer %ux%u format %d samples %u not supported - off", d.Width, d.Height,
                     (int)d.Format, d.SampleDesc.Count);
        }
    }
    if (g_mip) {
        int kind = pacing_kind() & 3;
        // read the slot written RING-1 presents ago (no wait)
        int rs = (int)((g_present + 1) % RING);
        if (g_slot[rs].used) {
            D3D11_MAPPED_SUBRESOURCE m;
            HRESULT hr = c->Map(g_stage[rs], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            Sample s = {};
            s.kind = g_slot[rs].kind; s.frame = g_slot[rs].frame;
            if (SUCCEEDED(hr)) {
                double sum = 0;
                for (UINT y = 0; y < g_gh; y++) {
                    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (UINT x = 0; x < g_gw; x++) { float l = luma(row + x * 4); s.cell[y * g_gw + x] = l; sum += l; }
                }
                c->Unmap(g_stage[rs], 0);
                s.whole = (float)(sum / (g_gw * g_gh));
                s.ok = true;
            }
            analyse(s);                          // a skipped frame breaks the triple (frame numbers)
            g_slot[rs].used = false;
        }
        int ws = (int)(g_present % RING);
        c->CopySubresourceRegion(g_mip, 0, 0, 0, 0, bb, 0, nullptr);
        c->GenerateMips(g_srv);
        c->CopySubresourceRegion(g_stage[ws], 0, 0, 0, 0, g_mip, g_level, nullptr);
        g_slot[ws] = Slot{true, kind, g_present};
        if (g_dumping) dump_frame(c, kind);
        g_present++;
    }
    c->Release(); dev->Release(); bb->Release();
}

static void post() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (g_nomagic_blinks && now - last >= 60000) {
        last = now;
        log_line("X2-DIAG-AUTOBLINK: no magic loaded: %d windows blinked in the last minute, max whole %.1f", g_nomagic_blinks,
                 g_nomagic_max / 10.0f);
        g_nomagic_blinks = 0; g_nomagic_max = 0;
    }
}

static bool install() {
    if (!present_hook()) return false;
    InitializeCriticalSection(&g_cs);
    g_thr_whole = config_row("X2-DIAG-AUTOBLINK-WHOLE", 15);
    g_thr_cell = config_row("X2-DIAG-AUTOBLINK-CELL", 40);
    g_dump = config_row("X2-DIAG-AUTOBLINK-DUMP", 0);
    if (!magic_mods_subscribe("X2-DIAG-AUTOBLINK", on_load, on_unload)) return false;
    g_present_tap3 = present_tap;
    log_line("  X2-DIAG-AUTOBLINK: image zigzag test per present (thresholds whole %.1f, cell %.1f luma; dump %s)",
             g_thr_whole / 10.0, g_thr_cell / 10.0, g_dump ? "on" : "off");
    return true;
}

Row row_DIAGAUTOBLINK = {
    "X2-DIAG-AUTOBLINK",
    "diagnostic: logs [BLINK] when the image zigzags between logic and render-only frames (back buffer grid, no stall)",
    1, install, nullptr, post, 0
};

} // namespace x2
