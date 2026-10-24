// SPDX-License-Identifier: Apache-2.0
// Rollback engine: rows register memory spans (fixed, or found again each frame by a resolver); at the start of each
// render-only invocation every span is copied out, and at its end copied back, so whatever the game changed in them
// during that invocation is undone (used by X2-LOOP-004 for the camera blocks). A span that faults is disabled.
// Same model as the FFX mod's rollback.cpp, own state. Tested by tests/x2/test_rollback.cpp.
#include "x2.h"
#include <string.h>

namespace x2 {

struct Span {
    const char*     row_id;
    uint8_t*        rt;          // static base, or null for dynamic
    span_resolver_t fn;
    void*           ctx;
    uint32_t        max_len;
    uint8_t*        buf;
    uint8_t*        saved_at;    // base used for the current snapshot (null = nothing saved)
    uint32_t        saved_len;
    int             dead;
};

static const int MAX_SPANS = 64;
static Span g_spans[MAX_SPANS];
static int  g_nspans = 0;

static Span* add_span(const char* row_id, uint32_t max_len) {
    if (g_nspans >= MAX_SPANS || max_len == 0 || max_len > (16u << 20)) {
        log_line("rollback: %s span rejected (count %d, len 0x%X)", row_id, g_nspans, max_len);
        return nullptr;
    }
    uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, max_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { log_line("rollback: %s no memory for 0x%X bytes", row_id, max_len); return nullptr; }
    Span& s = g_spans[g_nspans++];
    memset(&s, 0, sizeof(s));
    s.row_id = row_id;
    s.max_len = max_len;
    s.buf = buf;
    return &s;
}

bool rollback_add_static(const char* row_id, uint8_t* rt, uint32_t len) {
    if (!rt) { log_line("rollback: %s static span unresolved - not added", row_id); return false; }
    Span* s = add_span(row_id, len);
    if (!s) return false;
    s->rt = rt;
    log_line("rollback: %s span gva=0x%08X len=0x%X (static)", row_id, rt_to_gva(rt), len);
    return true;
}

bool rollback_add_dynamic(const char* row_id, span_resolver_t fn, void* ctx, uint32_t max_len) {
    if (!fn) return false;
    Span* s = add_span(row_id, max_len);
    if (!s) return false;
    s->fn = fn;
    s->ctx = ctx;
    log_line("rollback: %s span (dynamic, max len 0x%X)", row_id, max_len);
    return true;
}

void rollback_pre() {
    for (int i = 0; i < g_nspans; i++) g_spans[i].saved_at = nullptr;
    if (pacing_kind() != INV_RENDER_ONLY) return;
    for (int i = 0; i < g_nspans; i++) {
        Span& s = g_spans[i];
        if (s.dead || !row_on(s.row_id)) continue;
        uint8_t* base = s.rt;
        uint32_t len = s.max_len;
        if (s.fn) {
            bool ok = false;
            __try { ok = s.fn(s.ctx, &base, &len); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            if (!ok || !base || len == 0) continue;  // block not present this frame
            if (len > s.max_len) len = s.max_len;
        }
        if (!safe_copy(s.buf, base, len)) {
            s.dead = 1;
            log_line("rollback: %s span %p len 0x%X faulted on snapshot - disabled", s.row_id, base, len);
            continue;
        }
        s.saved_at = base;
        s.saved_len = len;
    }
}

void rollback_post() {
    for (int i = 0; i < g_nspans; i++) {
        Span& s = g_spans[i];
        if (!s.saved_at) continue;
        if (!safe_copy(s.saved_at, s.buf, s.saved_len)) {
            s.dead = 1;
            log_line("rollback: %s span %p faulted on restore - disabled", s.row_id, s.saved_at);
        }
        s.saved_at = nullptr;
    }
}

} // namespace x2
