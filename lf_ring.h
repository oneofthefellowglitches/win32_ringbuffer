#pragma once

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- CRT-Free Assertions ---
#ifdef _DEBUG
#define LF_ASSERT(expr) if (!(expr)) { __debugbreak(); }
#else
#define LF_ASSERT(expr)
#endif

// --- Configuration ---
#define CACHE_LINE_SIZE 64
#define LF_QUEUE_MAX_CAPACITY (1024 * 1024 * 64) // 64M items max safety

// --- Power of 2 Helper (Compile time if possible, runtime here) ---
static inline bool is_power_of_2(uint64_t v) {
    return (v & (v - 1)) == 0;
}

// --- The Queue Structure ---
// Padding is CRITICAL to prevent false sharing between cores
typedef struct {
    // Consumer side (Cache Line 0)
    char pad0[CACHE_LINE_SIZE];
    volatile uint64_t tail; 
    char pad1[CACHE_LINE_SIZE - sizeof(uint64_t)];

    // Producer side (Cache Line 1)
    char pad2[CACHE_LINE_SIZE];
    volatile uint64_t head; 
    char pad3[CACHE_LINE_SIZE - sizeof(uint64_t)];

    uint64_t capacity;
    void*  buffer;     // Pointer to the actual data array
} LF_SPSC_Queue;


// --- API ---

/**
 * @brief Creates a Lock-free Queue.
 * @param capacity Number of elements. MUST be a power of 2.
 * @param item_size Size of a single element (e.g., sizeof(MyStruct)).
 * @return Pointer to the queue, or NULL on failure.
 */
LF_SPSC_Queue* lf_queue_create(uint64_t capacity, size_t item_size);

/**
 * @brief Destroys the queue and frees memory.
 */
void lf_queue_destroy(LF_SPSC_Queue* q);

/**
 * @brief Enqueues an item. Single Producer ONLY.
 * @return true if success, false if full.
 */
bool lf_queue_enq(LF_SPSC_Queue* q, const void* item);

/**
 * @brief Dequeues an item. Single Consumer ONLY.
 * @return true if success, false if empty.
 */
bool lf_queue_deq(LF_SPSC_Queue* q, void* out_item);

/**
 * @brief Checks if queue is empty (snapshot, race-prone, for debug only).
 */
bool lf_queue_is_empty(LF_SPSC_Queue* q);