#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CACHE_LINE 64
#define LF_ASSERT(x) if(!(x)) __debugbreak()

typedef struct {
    // Padding to prevent false sharing
    char p1[CACHE_LINE];
    volatile uint64_t head; // Producer writes
    char p2[CACHE_LINE];
    volatile uint64_t tail; // Consumer writes
    char p3[CACHE_LINE];
    
    uint64_t capacity; // Must be power of 2
    size_t   item_size;
    void*    buffer;
} LF_Queue;

// Creates queue. Returns NULL on failure.
LF_Queue* lf_create(uint64_t capacity, size_t item_size);
void      lf_destroy(LF_Queue* q);

// Returns true if success
bool lf_enq(LF_Queue* q, const void* data);
bool lf_deq(LF_Queue* q, void* out_data);

// --- Atomic Helpers (x64 Win32) ---
// On x64, aligned loads/stores are acquire/release. 
// Interlocked ops are full barriers. We use Interlocked for simplicity/safety.
static inline uint64_t load_acquire(volatile uint64_t* a) {
    return InterlockedOr64(a, 0);
}
static inline void store_release(volatile uint64_t* a, uint64_t v) {
    InterlockedExchange64(a, v);
}
static inline uint64_t fetch_add(volatile uint64_t* a, uint64_t v) {
    return InterlockedExchangeAdd64(a, v);
}

LF_Queue* lf_create(uint64_t capacity, size_t item_size) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) return NULL;

    // Allocate control structure
    LF_Queue* q = (LF_Queue*)VirtualAlloc(NULL, sizeof(LF_Queue), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!q) return NULL;

    // Allocate data buffer
    // Check for overflow
    if (capacity > (SIZE_MAX / item_size)) {
        VirtualFree(q, 0, MEM_RELEASE);
        return NULL;
    }
    void* buf = VirtualAlloc(NULL, capacity * item_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        VirtualFree(q, 0, MEM_RELEASE);
        return NULL;
    }

    q->head = 0;
    q->tail = 0;
    q->capacity = capacity;
    q->item_size = item_size;
    q->buffer = buf;
    
    return q;
}

void lf_destroy(LF_Queue* q) {
    if (!q) return;
    VirtualFree(q->buffer, 0, MEM_RELEASE);
    VirtualFree(q, 0, MEM_RELEASE);
}

bool lf_enq(LF_Queue* q, const void* item) {
    uint64_t h = load_acquire(&q->head);
    uint64_t t = load_acquire(&q->tail);

    // Check Full: (head - tail) >= capacity
    if (h - t >= q->capacity) return false;

    // Copy data
    uint64_t idx = h & (q->capacity - 1);
    uint8_t* dest = (uint8_t*)q->buffer + (idx * q->item_size);
    
    // Manual memcpy
    const uint8_t* src = (const uint8_t*)item;
    for (size_t i = 0; i < q->item_size; ++i) {
        dest[i] = src[i];
    }

    // Release: Make data visible before updating head
    // InterlockedExchangeAdd acts as a full memory barrier on x86
    fetch_add(&q->head, 1);
    
    return true;
}

bool lf_deq(LF_Queue* q, void* out_item) {
    uint64_t t = load_acquire(&q->tail);
    uint64_t h = load_acquire(&q->head);

    // Check Empty
    if (h == t) return false;

    // Copy data
    uint64_t idx = t & (q->capacity - 1);
    const uint8_t* src = (const uint8_t*)q->buffer + (idx * q->item_size);
    uint8_t* dest = (uint8_t*)out_item;

    for (size_t i = 0; i < q->item_size; ++i) {
        dest[i] = src[i];
    }

    // Release: Make sure we are done reading before we update tail
    fetch_add(&q->tail, 1);
    
    return true;
}