#include <stdio.h> // Only for printf in this demo main()
#include "lf_spsc.h"

// A typical market data tick
typedef struct {
    uint64_t timestamp;
    double   price;
    uint32_t volume;
} MarketTick;

// Global queue pointer (in real app, pass via context)
LF_Queue* g_queue = NULL;

// Producer Thread (Market Feed Handler)
DWORD WINAPI producer_thread(LPVOID param) {
    MarketTick tick = { 0 };
    
    for (int i = 0; i < 1000000; ++i) {
        tick.timestamp = GetTickCount64();
        tick.price = 100.0 + (rand() % 100);
        tick.volume = 1000;

        // Busy-wait if full (low latency strategy)
        // In real app, maybe yield or sleep(0)
        while (!lf_enq(g_queue, &tick)) {
            // Spin or backoff
        }
    }
    return 0;
}

// Consumer Thread (Audit Logger / Strategy)
DWORD WINAPI consumer_thread(LPVOID param) {
    MarketTick tick;
    int processed = 0;
    
    while (processed < 1000000) {
        if (lf_deq(g_queue, &tick)) {
            // Process tick
            // e.g. write_to_audit_log(tick);
            processed++;
        } else {
            // Queue empty, spin or sleep
            // SwitchToThread(); // Yields CPU
        }
    }
    return 0;
}

int main() {
    // 1. Create Queue: 65536 items, size of MarketTick
    g_queue = lf_create(65536, sizeof(MarketTick));
    if (!g_queue) {
        printf("Failed to create queue\n");
        return 1;
    }

    printf("Queue created. Head at offset %d, Tail at %d\n", 
        (int)offsetof(LF_Queue, head), (int)offsetof(LF_Queue, tail));

    // 2. Start Threads
    HANDLE hProducer = CreateThread(NULL, 0, producer_thread, NULL, 0, NULL);
    HANDLE hConsumer = CreateThread(NULL, 0, consumer_thread, NULL, 0, NULL);

    // 3. Wait
    WaitForSingleObject(hProducer, INFINITE);
    WaitForSingleObject(hConsumer, INFINITE);

    printf("Done.\n");

    // 4. Cleanup
    CloseHandle(hProducer);
    CloseHandle(hConsumer);
    lf_destroy(g_queue);

    return 0;
}
