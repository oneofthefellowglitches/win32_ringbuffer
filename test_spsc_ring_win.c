/* test_spsc_ring_win.c
   CRT-free test for spsc_ring_win.h (MSVC + WinAPI only).
*/
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include "spsc_ring_win.h"

static HANDLE g_stdout;

static __forceinline uint32_t cstr_len_a(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void write_all(const void* p, DWORD n) {
    DWORD wrote = 0;
    if (!g_stdout || g_stdout == INVALID_HANDLE_VALUE) return;
    (void)WriteFile(g_stdout, p, n, &wrote, NULL);
}

static void write_cstr(const char* s) {
    write_all(s, (DWORD)cstr_len_a(s));
}

static void write_crlf(void) {
    write_all("\r\n", 2);
}

static void write_u32(uint32_t v) {
    char buf[16];
    uint32_t i = 0;

    if (v == 0) {
        write_all("0", 1);
        return;
    }

    while (v && i < (uint32_t)sizeof(buf)) {
        uint32_t d = v % 10u;
        buf[i++] = (char)('0' + d);
        v /= 10u;
    }
    while (i) write_all(&buf[--i], 1);
}

/* ---------------- test ---------------- */

typedef struct TestCtx {
    SPSC_RingShared* ring;

    uint32_t total;             /* requested messages */
    volatile LONG stop;         /* set to 1 to stop both threads */

    uint32_t produced;
    uint32_t consumed;

    volatile LONG error;        /* 1 if mismatch */
    uint32_t bad_expected;
    uint32_t bad_got;

    HANDLE start_evt;
} TestCtx;

static DWORD WINAPI producer_thread(LPVOID param) {
    TestCtx* ctx = (TestCtx*)param;
    (void)WaitForSingleObject(ctx->start_evt, INFINITE);

    uint32_t i = 0;
    for (; i < ctx->total; ) {
        if (InterlockedCompareExchange(&ctx->stop, 0, 0) != 0) break;

        uint32_t v = i;
        if (spsc_ring_try_enqueue(ctx->ring, &v)) {
            i++;
        } else {
            spsc_cpu_relax();
        }
    }

    ctx->produced = i;
    return 0;
}

static DWORD WINAPI consumer_thread(LPVOID param) {
    TestCtx* ctx = (TestCtx*)param;
    (void)WaitForSingleObject(ctx->start_evt, INFINITE);

    uint32_t expected = 0;
    uint32_t got = 0;

    while (expected < ctx->total) {
        if (InterlockedCompareExchange(&ctx->stop, 0, 0) != 0) break;

        if (spsc_ring_try_dequeue(ctx->ring, &got)) {
            if (got != expected) {
                ctx->bad_expected = expected;
                ctx->bad_got = got;
                InterlockedExchange(&ctx->error, 1);
                InterlockedExchange(&ctx->stop, 1);
                break;
            }
            expected++;
        } else {
            spsc_cpu_relax();
        }
    }

    ctx->consumed = expected;
    return 0;
}

static void print_results(TestCtx* ctx, DWORD elapsed_ms) {
    write_cstr("Ring capacity (raw): ");
    write_u32(ctx->ring->capacity);
    write_cstr("  usable: ");
    write_u32(spsc_ring_usable_capacity(ctx->ring));
    write_cstr("  elem_size: ");
    write_u32(ctx->ring->elem_size);
    write_crlf();

    write_cstr("Requested messages: ");
    write_u32(ctx->total);
    write_crlf();

    write_cstr("Produced: ");
    write_u32(ctx->produced);
    write_cstr("  Consumed: ");
    write_u32(ctx->consumed);
    write_crlf();

    write_cstr("Elapsed ms: ");
    write_u32(elapsed_ms);
    write_crlf();

    if (elapsed_ms == 0) elapsed_ms = 1;

    /* Keep arithmetic 32-bit friendly; MulDiv is WinAPI and uses 64-bit internally. */
    {
        int msgs_per_sec = MulDiv((int)ctx->consumed, 1000, (int)elapsed_ms);
        uint32_t bytes_total = ctx->consumed * ctx->ring->elem_size;
        int bytes_per_sec = MulDiv((int)bytes_total, 1000, (int)elapsed_ms);

        write_cstr("Throughput: ");
        write_u32((uint32_t)msgs_per_sec);
        write_cstr(" msg/s, ");
        write_u32((uint32_t)bytes_per_sec);
        write_cstr(" bytes/s");
        write_crlf();
    }

    if (InterlockedCompareExchange(&ctx->error, 0, 0) != 0) {
        write_cstr("ERROR: sequence mismatch. expected=");
        write_u32(ctx->bad_expected);
        write_cstr(" got=");
        write_u32(ctx->bad_got);
        write_crlf();
    } else if (ctx->consumed != ctx->total) {
        write_cstr("ERROR: did not complete (stopped early).");
        write_crlf();
    } else {
        write_cstr("OK: sequence verified.");
        write_crlf();
    }
}

/* CRT-free entry point */
void __cdecl mainCRTStartup(void) {
    /* Ensure we have a console (ignore failure if already attached). */
    (void)AllocConsole();
    g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    write_cstr("SPSC ring test (WinAPI-only, CRT-free)");
    write_crlf();

    const uint32_t capacity = 1u << 16;   /* raw slots (usable = capacity-1) */
    const uint32_t elem_size = 4u;        /* uint32_t */
    const uint32_t total_msgs = 5000000u; /* adjust as desired */

    SPSC_RingShared* ring = spsc_ring_create_local(capacity, elem_size);
    if (!ring) {
        write_cstr("Failed to create ring.");
        write_crlf();
        ExitProcess(2);
    }

    TestCtx ctx;
    RtlZeroMemory(&ctx, sizeof(ctx));
    ctx.ring = ring;
    ctx.total = total_msgs;
    ctx.start_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ctx.start_evt) {
        write_cstr("CreateEvent failed.");
        write_crlf();
        spsc_ring_destroy_local(ring);
        ExitProcess(3);
    }

    HANDLE th_prod = CreateThread(NULL, 0, producer_thread, &ctx, 0, NULL);
    HANDLE th_cons = CreateThread(NULL, 0, consumer_thread, &ctx, 0, NULL);
    if (!th_prod || !th_cons) {
        write_cstr("CreateThread failed.");
        write_crlf();
        if (th_prod) CloseHandle(th_prod);
        if (th_cons) CloseHandle(th_cons);
        CloseHandle(ctx.start_evt);
        spsc_ring_destroy_local(ring);
        ExitProcess(4);
    }

    DWORD t0 = GetTickCount();
    (void)SetEvent(ctx.start_evt);

    (void)WaitForSingleObject(th_prod, INFINITE);
    (void)WaitForSingleObject(th_cons, INFINITE);
    DWORD t1 = GetTickCount();

    CloseHandle(th_prod);
    CloseHandle(th_cons);
    CloseHandle(ctx.start_evt);

    print_results(&ctx, (DWORD)(t1 - t0));

    spsc_ring_destroy_local(ring);

    /* exit code: 0 ok, 1 error */
    ExitProcess((InterlockedCompareExchange(&ctx.error, 0, 0) != 0) ? 1u : 0u);
}
