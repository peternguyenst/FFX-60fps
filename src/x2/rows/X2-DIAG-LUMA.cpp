// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-LUMA (diagnostic, off, observe only): measures a blink from the pixels instead of by eye.
//
// While an X2-DIAG-FTBLINK-DLL spell DLL is loaded (X2-DIAG-FTBLINK on), every present copies three 16 x 16 patches of
// the back buffer (left edge, right edge, upper middle: background areas) to the CPU and logs each patch's mean
// brightness (0-255) with the invocation kind (at most 1200 lines per load). The copy stalls the GPU every frame: for
// tests only. 8-bit RGBA / BGRA back buffers without MSAA only.
// X2-DIAG-LUMA-DUMP=N: the first time a patch reaches brightness N after the DLL loads, the next 4 frames are saved as
// half-size BMPs (<game>\fps60x2-dump-<load>-<k>-<LOGIC|RENDER>.bmp; once per load, at most 4 loads).
// X2-DIAG-AUTOBLINK does this automatically and without the stall. History: re/ledger/ULTIMA-0106.md.
#include "../x2.h"
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

extern Row row_DIAGLUMA;
extern void (*volatile g_present_tap)(IDXGISwapChain* sc);
bool present_hook();
bool ftblink_watching();                         // X2-DIAG-FTBLINK: a watched DLL is loaded

enum { PATCH = 16, NP = 3 };
static const float PX[NP] = {0.06f, 0.94f, 0.30f}, PY[NP] = {0.40f, 0.40f, 0.25f};
static ID3D11Texture2D* g_stage = nullptr;
static DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
static int g_lines = 0, g_refused = 0;
static bool g_was = false;
static int g_dump_at = 0, g_dump_left = 0, g_dump_loads = 0;
static bool g_dump_armed = false;
static ID3D11Texture2D* g_full = nullptr;

static void dump_frame(ID3D11DeviceContext* ctx, ID3D11Device* dev, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& d,
                       bool bgra, const char* kind) {
    if (!g_full) {
        D3D11_TEXTURE2D_DESC s = d;
        s.MipLevels = 1; s.ArraySize = 1; s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.MiscFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&s, nullptr, &g_full))) { g_full = nullptr; return; }
    }
    ctx->CopyResource(g_full, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_full, 0, D3D11_MAP_READ, 0, &m))) return;
    UINT w = d.Width / 2, h = d.Height / 2, stride = (w * 3 + 3) & ~3u;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    _snprintf_s(path + n, MAX_PATH - n, _TRUNCATE, "fps60x2-dump-%d-%d-%s.bmp", g_dump_loads, 4 - g_dump_left, kind);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        uint8_t hdr[54] = {'B', 'M'};
        *(uint32_t*)(hdr + 2) = 54 + stride * h; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
        *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = (int32_t)h; *(uint16_t*)(hdr + 26) = 1;
        *(uint16_t*)(hdr + 28) = 24; *(uint32_t*)(hdr + 34) = stride * h;
        fwrite(hdr, 1, 54, f);
        uint8_t* line = (uint8_t*)malloc(stride);
        if (line) {
            for (UINT y = 0; y < h; y++) {
                const uint8_t* src = (const uint8_t*)m.pData + (size_t)(d.Height - 1 - 2 * y) * m.RowPitch;
                memset(line, 0, stride);
                for (UINT x = 0; x < w; x++) {
                    const uint8_t* px = src + (size_t)x * 8;
                    line[x * 3 + 0] = bgra ? px[0] : px[2];
                    line[x * 3 + 1] = px[1];
                    line[x * 3 + 2] = bgra ? px[2] : px[0];
                }
                fwrite(line, 1, stride, f);
            }
            free(line);
        }
        fclose(f);
        log_line("X2-DIAG-LUMA-DUMP: %s", path);
    }
    ctx->Unmap(g_full, 0);
}

static void tap(IDXGISwapChain* sc) {
    if (!row_DIAGLUMA.on || g_disabled) return;
    bool w = ftblink_watching();
    if (w && !g_was) { g_lines = 0; g_dump_armed = g_dump_at > 0 && g_dump_loads < 4; g_dump_left = 0; }
    g_was = w;
    if (!w || g_lines >= 1200) return;
    ID3D11Texture2D* bb = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if ((!bgra && !rgba) || d.SampleDesc.Count != 1 || d.Width < 64 || d.Height < 64) {
        if (!g_refused++) log_line("X2-DIAG-LUMA: back buffer format %u samples %u %ux%u not supported - nothing measured",
                                   (unsigned)d.Format, d.SampleDesc.Count, d.Width, d.Height);
        bb->Release();
        return;
    }
    bb->GetDevice(&dev);
    dev->GetImmediateContext(&ctx);
    if (!g_stage || g_fmt != d.Format) {
        if (g_stage) { g_stage->Release(); g_stage = nullptr; }
        D3D11_TEXTURE2D_DESC s = {};
        s.Width = PATCH * NP; s.Height = PATCH; s.MipLevels = 1; s.ArraySize = 1; s.Format = d.Format;
        s.SampleDesc.Count = 1; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&s, nullptr, &g_stage))) g_stage = nullptr;
        g_fmt = d.Format;
    }
    double luma[NP] = {0, 0, 0};
    if (g_stage) {
        for (int i = 0; i < NP; i++) {
            D3D11_BOX b;
            b.left = (UINT)(PX[i] * d.Width) - PATCH / 2; b.top = (UINT)(PY[i] * d.Height) - PATCH / 2;
            b.right = b.left + PATCH; b.bottom = b.top + PATCH; b.front = 0; b.back = 1;
            ctx->CopySubresourceRegion(g_stage, 0, i * PATCH, 0, 0, bb, 0, &b);
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &m))) {
            for (int i = 0; i < NP; i++) {
                double sum = 0;
                for (int y = 0; y < PATCH; y++) {
                    const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch + i * PATCH * 4;
                    for (int x = 0; x < PATCH; x++) {
                        const uint8_t* px = row + x * 4;
                        double r = bgra ? px[2] : px[0], g = px[1], bl = bgra ? px[0] : px[2];
                        sum += 0.299 * r + 0.587 * g + 0.114 * bl;
                    }
                }
                luma[i] = sum / (PATCH * PATCH);
            }
            ctx->Unmap(g_stage, 0);
        }
    }
    static const char* K[] = {"LOGIC", "RENDER", "BACKLOG", "STOCK"};
    if (g_dump_armed && !g_dump_left && luma[2] >= g_dump_at) { g_dump_armed = false; g_dump_left = 4; g_dump_loads++; }
    if (g_dump_left > 0) { dump_frame(ctx, dev, bb, d, bgra, K[pacing_kind() & 3]); g_dump_left--; }
    ctx->Release();
    dev->Release();
    bb->Release();
    log_line("LUMA #%d %s fc=%u L %.1f R %.1f T %.1f", g_lines, K[pacing_kind() & 3], g_frame_counter ? *g_frame_counter : 0,
             luma[0], luma[1], luma[2]);
    g_lines++;
}

static bool install() {
    if (!present_hook()) return false;
    g_present_tap = tap;
    g_dump_at = config_row("X2-DIAG-LUMA-DUMP", 0);
    if (g_dump_at) log_line("  X2-DIAG-LUMA-DUMP: 4 frames saved when patch T first reaches luma %d after a watched load", g_dump_at);
    log_line("  X2-DIAG-LUMA: back-buffer patches measured at Present while an X2-DIAG-FTBLINK-DLL magic DLL is loaded");
    return true;
}

Row row_DIAGLUMA = {
    "X2-DIAG-LUMA",
    "diagnostic: back-buffer luma of 3 background patches per Present while a watched magic DLL is loaded (blink measure)",
    0, install, nullptr, nullptr, 0
};

} // namespace x2
