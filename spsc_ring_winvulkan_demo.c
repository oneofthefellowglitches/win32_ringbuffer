/* spsc_ring_winvulkan_demo.c */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>

#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)
#pragma intrinsic(_mm_pause)

/* -------- CRT-free requirements when using floating point in MSVC -------- */
#if defined(_MSC_VER)
int _fltused = 0;
#endif

/* Force MSVC to generate actual calls (or at least allow resolving)
   instead of assuming CRT-provided versions. */
#if defined(_MSC_VER)
#pragma function(memcpy, memset)
#endif

void* __cdecl memcpy(void* dst, const void* src, size_t n) {
#if defined(_M_IX86) || defined(_M_X64)
    __movsb((unsigned char*)dst, (const unsigned char*)src, n);
    return dst;
#else
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) { *d++ = *s++; }
    return dst;
#endif
}

void* __cdecl memset(void* dst, int c, size_t n) {
#if defined(_M_IX86) || defined(_M_X64)
    __stosb((unsigned char*)dst, (unsigned char)c, n);
    return dst;
#else
    unsigned char* d = (unsigned char*)dst;
    while (n--) { *d++ = (unsigned char)c; }
    return dst;
#endif
}

/* -------------------- small CRT-free helpers -------------------- */
static __forceinline void cpu_relax(void) { _mm_pause(); }

#if defined(_M_ARM) || defined(_M_ARM64)
  #define ACQUIRE_FENCE() MemoryBarrier()
  #define RELEASE_FENCE() MemoryBarrier()
#else
  #define ACQUIRE_FENCE() _ReadWriteBarrier()
  #define RELEASE_FENCE() _ReadWriteBarrier()
#endif

static __forceinline void copy_bytes(void* dst, const void* src, size_t n) {
#if defined(_M_IX86) || defined(_M_X64)
    __movsb((unsigned char*)dst, (const unsigned char*)src, n);
#else
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
#endif
}

static void* mem_reserve_commit(size_t bytes) {
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static void mem_release(void* p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

__declspec(noreturn) static void fatalA(const char* msg) {
    MessageBoxA(NULL, msg, "Fatal", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static uint32_t next_pow2_u32(uint32_t x) {
    if (x < 2u) return 2u;
    x--;
    x |= x >> 1; x |= x >> 2; x |= x >> 4;
    x |= x >> 8; x |= x >> 16;
    return x + 1u;
}

/* Read file (WinAPI only). Returns buffer allocated with VirtualAlloc. */
typedef struct FileBlob {
    void*  data;
    DWORD  size;
} FileBlob;

static FileBlob read_entire_file_w(const wchar_t* path) {
    FileBlob b; b.data = NULL; b.size = 0;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return b;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 0x7fffffff) {
        CloseHandle(h);
        return b;
    }

    DWORD size = (DWORD)sz.QuadPart;
    void* buf = mem_reserve_commit((size_t)size);
    if (!buf) { CloseHandle(h); return b; }

    DWORD read = 0;
    if (!ReadFile(h, buf, size, &read, NULL) || read != size) {
        mem_release(buf);
        CloseHandle(h);
        return b;
    }

    CloseHandle(h);
    b.data = buf;
    b.size = size;
    return b;
}

/* Build "<exe-dir>\<file>" without CRT */
static int build_exe_dir_path(wchar_t* out, uint32_t out_cch, const wchar_t* file) {
    DWORD n = GetModuleFileNameW(NULL, out, out_cch);
    if (n == 0 || n >= out_cch) return 0;

    /* truncate at last backslash */
    uint32_t i = (uint32_t)n;
    while (i > 0) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') break;
        i--;
    }
    if (i == 0) return 0;

    /* append file */
    uint32_t j = 0;
    while (file[j]) j++;

    if (i + j + 1 >= out_cch) return 0;
    for (uint32_t k = 0; k < j; k++) out[i + k] = file[k];
    out[i + j] = 0;
    return 1;
}

/* -------------------- SPSC ring buffer (fixed-size elements) -------------------- */

typedef struct SPSC_Ring {
    uint32_t capacity; /* power-of-two */
    uint32_t mask;
    uint32_t elem_size;

    __declspec(align(64)) volatile uint32_t head; /* producer writes */
    __declspec(align(64)) volatile uint32_t tail; /* consumer writes */

    uint8_t* buffer;
} SPSC_Ring;

static uint32_t load_acquire_u32(volatile uint32_t* p) {
    uint32_t v = *p;
    ACQUIRE_FENCE();
    return v;
}
static void store_release_u32(volatile uint32_t* p, uint32_t v) {
    RELEASE_FENCE();
    *p = v;
}

static SPSC_Ring* spsc_create(uint32_t capacity, uint32_t elem_size) {
    if (!elem_size) return NULL;
    uint32_t cap = next_pow2_u32(capacity);

    size_t bytes = sizeof(SPSC_Ring) + (size_t)cap * (size_t)elem_size;
    uint8_t* mem = (uint8_t*)mem_reserve_commit(bytes);
    if (!mem) return NULL;

    SPSC_Ring* r = (SPSC_Ring*)mem;
    r->capacity = cap;
    r->mask = cap - 1u;
    r->elem_size = elem_size;
    r->head = 0;
    r->tail = 0;
    r->buffer = mem + sizeof(SPSC_Ring);
    return r;
}

static void spsc_destroy(SPSC_Ring* r) { mem_release(r); }

/* returns 1 ok, 0 full */
static int spsc_try_enqueue(SPSC_Ring* r, const void* elem) {
    uint32_t head = r->head;
    uint32_t next = (head + 1u) & r->mask;
    uint32_t tail = load_acquire_u32(&r->tail);
    if (next == tail) return 0;

    copy_bytes(r->buffer + (size_t)head * r->elem_size, elem, r->elem_size);
    store_release_u32(&r->head, next);
    return 1;
}

/* returns 1 ok, 0 empty */
static int spsc_try_dequeue(SPSC_Ring* r, void* out) {
    uint32_t tail = r->tail;
    uint32_t head = load_acquire_u32(&r->head);
    if (tail == head) return 0;

    copy_bytes(out, r->buffer + (size_t)tail * r->elem_size, r->elem_size);
    store_release_u32(&r->tail, (tail + 1u) & r->mask);
    return 1;
}

static uint32_t spsc_size(SPSC_Ring* r) {
    uint32_t head = load_acquire_u32(&r->head);
    uint32_t tail = load_acquire_u32(&r->tail);
    return (head - tail) & r->mask;
}
static uint32_t spsc_usable_capacity(SPSC_Ring* r) { return r->capacity - 1u; }

/* -------------------- Demo event + producer thread -------------------- */

typedef struct Event {
    uint32_t v;
} Event;

typedef struct ProducerCtx {
    SPSC_Ring* q;
    volatile LONG running;
} ProducerCtx;

static uint32_t xorshift32(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static DWORD WINAPI producer_thread(LPVOID param) {
    ProducerCtx* ctx = (ProducerCtx*)param;

    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    uint32_t rng = 0xC001D00Du;
    uint64_t last = (uint64_t)t0.QuadPart;

    while (InterlockedCompareExchange(&ctx->running, 1, 1) == 1) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        uint64_t t = (uint64_t)now.QuadPart;

        /* Produce bursts ~ every 1ms */
        uint64_t dt = t - last;
        if (dt < (uint64_t)freq.QuadPart / 1000u) {
            cpu_relax();
            continue;
        }
        last = t;

        uint32_t burst = (xorshift32(&rng) & 1023u) + 64u; /* 64..1087 events */
        for (uint32_t i = 0; i < burst; i++) {
            Event e; e.v = xorshift32(&rng);
            /* If full, drop (common for telemetry/market bursts) */
            (void)spsc_try_enqueue(ctx->q, &e);
        }
    }
    return 0;
}

/* -------------------- Vulkan dynamic loader -------------------- */
typedef struct VkDyn {
    HMODULE vulkan;

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   vkGetDeviceProcAddr;

    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkCreateDevice vkCreateDevice;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkDestroyInstance vkDestroyInstance;

    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;

    /* Instance/device functions loaded later */
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle;

    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR vkQueuePresentKHR;

    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;

    PFN_vkCreateRenderPass vkCreateRenderPass;
    PFN_vkDestroyRenderPass vkDestroyRenderPass;

    PFN_vkCreateFramebuffer vkCreateFramebuffer;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer;

    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;

    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;

    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;

    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkResetCommandBuffer vkResetCommandBuffer;

    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
    PFN_vkCmdSetViewport vkCmdSetViewport;
    PFN_vkCmdSetScissor vkCmdSetScissor;
    PFN_vkCmdDraw vkCmdDraw;

    PFN_vkCreateSemaphore vkCreateSemaphore;
    PFN_vkDestroySemaphore vkDestroySemaphore;
    PFN_vkCreateFence vkCreateFence;
    PFN_vkDestroyFence vkDestroyFence;
    PFN_vkWaitForFences vkWaitForFences;
    PFN_vkResetFences vkResetFences;

    PFN_vkQueueSubmit vkQueueSubmit;

    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;

    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
} VkDyn;

static void* vk_sym(HMODULE m, const char* name) {
    return (void*)GetProcAddress(m, name);
}

static void vk_load_global(VkDyn* d) {
    d->vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!d->vulkan) fatalA("Failed to LoadLibraryW(vulkan-1.dll). Install Vulkan runtime/driver.");

    d->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)vk_sym(d->vulkan, "vkGetInstanceProcAddr");
    if (!d->vkGetInstanceProcAddr) fatalA("vkGetInstanceProcAddr not found.");

    d->vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vk_sym(d->vulkan, "vkGetDeviceProcAddr");
    if (!d->vkGetDeviceProcAddr) fatalA("vkGetDeviceProcAddr not found.");

    d->vkCreateInstance =
        (PFN_vkCreateInstance)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!d->vkCreateInstance) fatalA("Failed to load vkCreateInstance.");
}

/*
static void vk_load_global(VkDyn* d) {
    d->vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!d->vulkan) fatalA("Failed to LoadLibraryW(vulkan-1.dll). Install Vulkan runtime/driver.");

    d->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)vk_sym(d->vulkan, "vkGetInstanceProcAddr");
    if (!d->vkGetInstanceProcAddr) fatalA("vkGetInstanceProcAddr not found.");

    d->vkCreateInstance = (PFN_vkCreateInstance)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    d->vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumeratePhysicalDevices");
    d->vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceQueueFamilyProperties");
    d->vkGetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceSupportKHR");
    d->vkGetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    d->vkGetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    d->vkGetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    d->vkCreateDevice = (PFN_vkCreateDevice)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
    d->vkGetDeviceQueue = (PFN_vkGetDeviceQueue)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetDeviceQueue");
    d->vkDestroyInstance = (PFN_vkDestroyInstance)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroyInstance");
    d->vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateWin32SurfaceKHR");
    d->vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)d->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroySurfaceKHR");

    if (!d->vkCreateInstance || !d->vkCreateWin32SurfaceKHR) fatalA("Failed to load Vulkan global funcs.");
}
*/

static void vk_load_instance_device(VkDyn* d, VkInstance inst, VkDevice dev) {
    /* instance-level */
    d->vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)d->vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceMemoryProperties");

    /* device-level (via instance proc addr is ok on loader) */
    d->vkDestroyDevice = (PFN_vkDestroyDevice)d->vkGetInstanceProcAddr(inst, "vkDestroyDevice");
    d->vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)d->vkGetInstanceProcAddr(inst, "vkDeviceWaitIdle");

    d->vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)d->vkGetInstanceProcAddr(inst, "vkCreateSwapchainKHR");
    d->vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)d->vkGetInstanceProcAddr(inst, "vkDestroySwapchainKHR");
    d->vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)d->vkGetInstanceProcAddr(inst, "vkGetSwapchainImagesKHR");
    d->vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)d->vkGetInstanceProcAddr(inst, "vkAcquireNextImageKHR");
    d->vkQueuePresentKHR = (PFN_vkQueuePresentKHR)d->vkGetInstanceProcAddr(inst, "vkQueuePresentKHR");

    d->vkCreateImageView = (PFN_vkCreateImageView)d->vkGetInstanceProcAddr(inst, "vkCreateImageView");
    d->vkDestroyImageView = (PFN_vkDestroyImageView)d->vkGetInstanceProcAddr(inst, "vkDestroyImageView");

    d->vkCreateRenderPass = (PFN_vkCreateRenderPass)d->vkGetInstanceProcAddr(inst, "vkCreateRenderPass");
    d->vkDestroyRenderPass = (PFN_vkDestroyRenderPass)d->vkGetInstanceProcAddr(inst, "vkDestroyRenderPass");

    d->vkCreateFramebuffer = (PFN_vkCreateFramebuffer)d->vkGetInstanceProcAddr(inst, "vkCreateFramebuffer");
    d->vkDestroyFramebuffer = (PFN_vkDestroyFramebuffer)d->vkGetInstanceProcAddr(inst, "vkDestroyFramebuffer");

    d->vkCreateShaderModule = (PFN_vkCreateShaderModule)d->vkGetInstanceProcAddr(inst, "vkCreateShaderModule");
    d->vkDestroyShaderModule = (PFN_vkDestroyShaderModule)d->vkGetInstanceProcAddr(inst, "vkDestroyShaderModule");

    d->vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout)d->vkGetInstanceProcAddr(inst, "vkCreatePipelineLayout");
    d->vkDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)d->vkGetInstanceProcAddr(inst, "vkDestroyPipelineLayout");

    d->vkCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)d->vkGetInstanceProcAddr(inst, "vkCreateGraphicsPipelines");
    d->vkDestroyPipeline = (PFN_vkDestroyPipeline)d->vkGetInstanceProcAddr(inst, "vkDestroyPipeline");

    d->vkCreateCommandPool = (PFN_vkCreateCommandPool)d->vkGetInstanceProcAddr(inst, "vkCreateCommandPool");
    d->vkDestroyCommandPool = (PFN_vkDestroyCommandPool)d->vkGetInstanceProcAddr(inst, "vkDestroyCommandPool");
    d->vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)d->vkGetInstanceProcAddr(inst, "vkAllocateCommandBuffers");
    d->vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)d->vkGetInstanceProcAddr(inst, "vkBeginCommandBuffer");
    d->vkEndCommandBuffer = (PFN_vkEndCommandBuffer)d->vkGetInstanceProcAddr(inst, "vkEndCommandBuffer");
    d->vkResetCommandBuffer = (PFN_vkResetCommandBuffer)d->vkGetInstanceProcAddr(inst, "vkResetCommandBuffer");

    d->vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)d->vkGetInstanceProcAddr(inst, "vkCmdBeginRenderPass");
    d->vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass)d->vkGetInstanceProcAddr(inst, "vkCmdEndRenderPass");
    d->vkCmdBindPipeline = (PFN_vkCmdBindPipeline)d->vkGetInstanceProcAddr(inst, "vkCmdBindPipeline");
    d->vkCmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers)d->vkGetInstanceProcAddr(inst, "vkCmdBindVertexBuffers");
    d->vkCmdSetViewport = (PFN_vkCmdSetViewport)d->vkGetInstanceProcAddr(inst, "vkCmdSetViewport");
    d->vkCmdSetScissor = (PFN_vkCmdSetScissor)d->vkGetInstanceProcAddr(inst, "vkCmdSetScissor");
    d->vkCmdDraw = (PFN_vkCmdDraw)d->vkGetInstanceProcAddr(inst, "vkCmdDraw");

    d->vkCreateSemaphore = (PFN_vkCreateSemaphore)d->vkGetInstanceProcAddr(inst, "vkCreateSemaphore");
    d->vkDestroySemaphore = (PFN_vkDestroySemaphore)d->vkGetInstanceProcAddr(inst, "vkDestroySemaphore");
    d->vkCreateFence = (PFN_vkCreateFence)d->vkGetInstanceProcAddr(inst, "vkCreateFence");
    d->vkDestroyFence = (PFN_vkDestroyFence)d->vkGetInstanceProcAddr(inst, "vkDestroyFence");
    d->vkWaitForFences = (PFN_vkWaitForFences)d->vkGetInstanceProcAddr(inst, "vkWaitForFences");
    d->vkResetFences = (PFN_vkResetFences)d->vkGetInstanceProcAddr(inst, "vkResetFences");

    d->vkQueueSubmit = (PFN_vkQueueSubmit)d->vkGetInstanceProcAddr(inst, "vkQueueSubmit");

    d->vkCreateBuffer = (PFN_vkCreateBuffer)d->vkGetInstanceProcAddr(inst, "vkCreateBuffer");
    d->vkDestroyBuffer = (PFN_vkDestroyBuffer)d->vkGetInstanceProcAddr(inst, "vkDestroyBuffer");
    d->vkGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)d->vkGetInstanceProcAddr(inst, "vkGetBufferMemoryRequirements");
    d->vkAllocateMemory = (PFN_vkAllocateMemory)d->vkGetInstanceProcAddr(inst, "vkAllocateMemory");
    d->vkFreeMemory = (PFN_vkFreeMemory)d->vkGetInstanceProcAddr(inst, "vkFreeMemory");
    d->vkBindBufferMemory = (PFN_vkBindBufferMemory)d->vkGetInstanceProcAddr(inst, "vkBindBufferMemory");
    d->vkMapMemory = (PFN_vkMapMemory)d->vkGetInstanceProcAddr(inst, "vkMapMemory");
    d->vkUnmapMemory = (PFN_vkUnmapMemory)d->vkGetInstanceProcAddr(inst, "vkUnmapMemory");

    (void)dev;
}

/* -------------------- Vulkan demo renderer -------------------- */

typedef struct Vertex {
    float x, y;
    float r, g, b;
} Vertex;

typedef struct VkDemo {
    VkDyn d;

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t qfam;

    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D extent;

    VkImage images[8];
    VkImageView views[8];
    VkFramebuffer fbs[8];
    uint32_t image_count;

    VkRenderPass rp;
    VkPipelineLayout pl;
    VkPipeline pipe;

    VkCommandPool cmdpool;
    VkCommandBuffer cmdbufs[8];

    VkSemaphore sem_acquire;
    VkSemaphore sem_render;
    VkFence fence;

    VkBuffer vbuf;
    VkDeviceMemory vmem;
    void* vmap;

    uint32_t max_vertices;
} VkDemo;

static uint32_t find_mem_type(VkDyn* d, VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    d->vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && ((mp.memoryTypes[i].propertyFlags & want) == want)) return i;
    }
    return 0xFFFFFFFFu;
}

static VkShaderModule make_shader(VkDemo* vk, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci;
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.pNext = NULL;
    ci.flags = 0;
    ci.codeSize = bytes;
    ci.pCode = code;

    VkShaderModule m = VK_NULL_HANDLE;
    if (vk->d.vkCreateShaderModule(vk->device, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static void vk_init(VkDemo* vk, HWND hwnd) {
    vk_load_global(&vk->d);

    const char* exts[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };

    VkApplicationInfo ai;
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pNext = NULL;
    ai.pApplicationName = "SPSC Ring Vulkan Demo";
    ai.applicationVersion = 1;
    ai.pEngineName = "none";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici;
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pNext = NULL;
    ici.flags = 0;
    ici.pApplicationInfo = &ai;
    ici.enabledLayerCount = 0;
    ici.ppEnabledLayerNames = NULL;
    ici.enabledExtensionCount = (uint32_t)(sizeof(exts) / sizeof(exts[0]));
    ici.ppEnabledExtensionNames = exts;

    if (vk->d.vkCreateInstance(&ici, NULL, &vk->instance) != VK_SUCCESS)
        fatalA("vkCreateInstance failed.");

    /* Surface */
    VkWin32SurfaceCreateInfoKHR sci;
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.pNext = NULL;
    sci.flags = 0;
    sci.hinstance = GetModuleHandleW(NULL);
    sci.hwnd = hwnd;

    if (vk->d.vkCreateWin32SurfaceKHR(vk->instance, &sci, NULL, &vk->surface) != VK_SUCCESS)
        fatalA("vkCreateWin32SurfaceKHR failed.");

    /* Pick device + queue family */
    uint32_t pd_count = 0;
    if (vk->d.vkEnumeratePhysicalDevices(vk->instance, &pd_count, NULL) != VK_SUCCESS || pd_count == 0)
        fatalA("No Vulkan physical devices.");

    VkPhysicalDevice pds[8];
    if (pd_count > 8) pd_count = 8;
    if (vk->d.vkEnumeratePhysicalDevices(vk->instance, &pd_count, pds) != VK_SUCCESS)
        fatalA("vkEnumeratePhysicalDevices failed.");

    vk->phys = pds[0];

    uint32_t qf_count = 0;
    vk->d.vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &qf_count, NULL);
    if (qf_count == 0) fatalA("No queue families.");

    VkQueueFamilyProperties qfp[16];
    if (qf_count > 16) qf_count = 16;
    vk->d.vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &qf_count, qfp);

    vk->qfam = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (!(qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_FALSE;
        vk->d.vkGetPhysicalDeviceSurfaceSupportKHR(vk->phys, i, vk->surface, &present);
        if (present) { vk->qfam = i; break; }
    }
    if (vk->qfam == 0xFFFFFFFFu) fatalA("No graphics+present queue family.");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.pNext = NULL;
    qci.flags = 0;
    qci.queueFamilyIndex = vk->qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* dev_exts[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci;
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = NULL;
    dci.flags = 0;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledLayerCount = 0;
    dci.ppEnabledLayerNames = NULL;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    dci.pEnabledFeatures = NULL;

    if (vk->d.vkCreateDevice(vk->phys, &dci, NULL, &vk->device) != VK_SUCCESS)
        fatalA("vkCreateDevice failed.");

    vk_load_instance_device(&vk->d, vk->instance, vk->device);

    vk->d.vkGetDeviceQueue(vk->device, vk->qfam, 0, &vk->queue);

    /* Swapchain */
    VkSurfaceCapabilitiesKHR caps;
    vk->d.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->phys, vk->surface, &caps);

    uint32_t fmt_count = 0;
    vk->d.vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &fmt_count, NULL);
    if (fmt_count == 0) fatalA("No surface formats.");
    VkSurfaceFormatKHR fmts[16];
    if (fmt_count > 16) fmt_count = 16;
    vk->d.vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &fmt_count, fmts);

    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = fmts[i]; break; }
    }
    vk->swap_format = chosen.format;

    /* Fixed extent (disable resizing in window style for simplicity) */
    vk->extent = caps.currentExtent;
    if (vk->extent.width == 0xFFFFFFFFu) {
        vk->extent.width = 1280;
        vk->extent.height = 720;
    }

    uint32_t pm_count = 0;
    vk->d.vkGetPhysicalDeviceSurfacePresentModesKHR(vk->phys, vk->surface, &pm_count, NULL);
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR; /* always supported */

    uint32_t min_images = 2;
    if (caps.maxImageCount && min_images > caps.maxImageCount) min_images = caps.maxImageCount;
    if (min_images < caps.minImageCount) min_images = caps.minImageCount;

    VkSwapchainCreateInfoKHR sci2;
    sci2.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci2.pNext = NULL;
    sci2.flags = 0;
    sci2.surface = vk->surface;
    sci2.minImageCount = min_images;
    sci2.imageFormat = chosen.format;
    sci2.imageColorSpace = chosen.colorSpace;
    sci2.imageExtent = vk->extent;
    sci2.imageArrayLayers = 1;
    sci2.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci2.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci2.queueFamilyIndexCount = 0;
    sci2.pQueueFamilyIndices = NULL;
    sci2.preTransform = caps.currentTransform;
    sci2.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci2.presentMode = present;
    sci2.clipped = VK_TRUE;
    sci2.oldSwapchain = VK_NULL_HANDLE;

    if (vk->d.vkCreateSwapchainKHR(vk->device, &sci2, NULL, &vk->swapchain) != VK_SUCCESS)
        fatalA("vkCreateSwapchainKHR failed.");

    vk->image_count = 0;
    vk->d.vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->image_count, NULL);
    if (vk->image_count == 0 || vk->image_count > 8) fatalA("Unexpected swapchain image count.");
    vk->d.vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->image_count, vk->images);

    for (uint32_t i = 0; i < vk->image_count; i++) {
        VkImageViewCreateInfo iv;
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.pNext = NULL;
        iv.flags = 0;
        iv.image = vk->images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = vk->swap_format;
        iv.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.baseMipLevel = 0;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.baseArrayLayer = 0;
        iv.subresourceRange.layerCount = 1;

        if (vk->d.vkCreateImageView(vk->device, &iv, NULL, &vk->views[i]) != VK_SUCCESS)
            fatalA("vkCreateImageView failed.");
    }

    /* Render pass */
    VkAttachmentDescription ad;
    ad.flags = 0;
    ad.format = vk->swap_format;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ad.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference aref;
    aref.attachment = 0;
    aref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sp;
    sp.flags = 0;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.inputAttachmentCount = 0;
    sp.pInputAttachments = NULL;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &aref;
    sp.pResolveAttachments = NULL;
    sp.pDepthStencilAttachment = NULL;
    sp.preserveAttachmentCount = 0;
    sp.pPreserveAttachments = NULL;

    VkRenderPassCreateInfo rpci;
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.pNext = NULL;
    rpci.flags = 0;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &ad;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;
    rpci.dependencyCount = 0;
    rpci.pDependencies = NULL;

    if (vk->d.vkCreateRenderPass(vk->device, &rpci, NULL, &vk->rp) != VK_SUCCESS)
        fatalA("vkCreateRenderPass failed.");

    /* Pipeline: load SPIR-V from exe dir */
    wchar_t path_vs[MAX_PATH];
    wchar_t path_fs[MAX_PATH];
    if (!build_exe_dir_path(path_vs, MAX_PATH, L"graph.vert.spv")) fatalA("Path build failed (VS).");
    if (!build_exe_dir_path(path_fs, MAX_PATH, L"graph.frag.spv")) fatalA("Path build failed (FS).");

    FileBlob vsb = read_entire_file_w(path_vs);
    FileBlob fsb = read_entire_file_w(path_fs);
    if (!vsb.data || !fsb.data) fatalA("Failed to read graph.vert.spv / graph.frag.spv (place next to EXE).");

    VkShaderModule vs = make_shader(vk, (const uint32_t*)vsb.data, (size_t)vsb.size);
    VkShaderModule fs = make_shader(vk, (const uint32_t*)fsb.data, (size_t)fsb.size);
    mem_release(vsb.data);
    mem_release(fsb.data);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) fatalA("Shader module creation failed.");

    VkPipelineShaderStageCreateInfo stages[2];
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].pNext = NULL;
    stages[0].flags = 0;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = NULL;

    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;

    VkVertexInputBindingDescription bind;
    bind.binding = 0;
    bind.stride = sizeof(Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attr[2];
    attr[0].location = 0; attr[0].binding = 0; attr[0].format = VK_FORMAT_R32G32_SFLOAT;    attr[0].offset = 0;
    attr[1].location = 1; attr[1].binding = 0; attr[1].format = VK_FORMAT_R32G32B32_SFLOAT; attr[1].offset = 8;

    VkPipelineVertexInputStateCreateInfo vis;
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.pNext = NULL;
    vis.flags = 0;
    vis.vertexBindingDescriptionCount = 1;
    vis.pVertexBindingDescriptions = &bind;
    vis.vertexAttributeDescriptionCount = 2;
    vis.pVertexAttributeDescriptions = attr;

    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo vp;
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.pNext = NULL;
    vp.flags = 0;
    vp.viewportCount = 1;
    vp.pViewports = NULL; /* dynamic */
    vp.scissorCount = 1;
    vp.pScissors = NULL;  /* dynamic */

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    ms.minSampleShading = 1.0f;
    ms.pSampleMask = NULL;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba;
    cba.blendEnable = VK_FALSE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.pNext = NULL;
    cb.flags = 0;
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_COPY;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    cb.blendConstants[0] = cb.blendConstants[1] = cb.blendConstants[2] = cb.blendConstants[3] = 0.0f;

    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds;
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.pNext = NULL;
    ds.flags = 0;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyns;

    VkPipelineLayoutCreateInfo plci;
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pNext = NULL;
    plci.flags = 0;
    plci.setLayoutCount = 0;
    plci.pSetLayouts = NULL;
    plci.pushConstantRangeCount = 0;
    plci.pPushConstantRanges = NULL;

    if (vk->d.vkCreatePipelineLayout(vk->device, &plci, NULL, &vk->pl) != VK_SUCCESS)
        fatalA("vkCreatePipelineLayout failed.");

    VkGraphicsPipelineCreateInfo gp;
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.pNext = NULL;
    gp.flags = 0;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vis;
    gp.pInputAssemblyState = &ia;
    gp.pTessellationState = NULL;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = NULL;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = vk->pl;
    gp.renderPass = vk->rp;
    gp.subpass = 0;
    gp.basePipelineHandle = VK_NULL_HANDLE;
    gp.basePipelineIndex = -1;

    if (vk->d.vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &gp, NULL, &vk->pipe) != VK_SUCCESS)
        fatalA("vkCreateGraphicsPipelines failed.");

    vk->d.vkDestroyShaderModule(vk->device, vs, NULL);
    vk->d.vkDestroyShaderModule(vk->device, fs, NULL);

    /* Framebuffers */
    for (uint32_t i = 0; i < vk->image_count; i++) {
        VkImageView atts[1] = { vk->views[i] };
        VkFramebufferCreateInfo fci;
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.pNext = NULL;
        fci.flags = 0;
        fci.renderPass = vk->rp;
        fci.attachmentCount = 1;
        fci.pAttachments = atts;
        fci.width = vk->extent.width;
        fci.height = vk->extent.height;
        fci.layers = 1;

        if (vk->d.vkCreateFramebuffer(vk->device, &fci, NULL, &vk->fbs[i]) != VK_SUCCESS)
            fatalA("vkCreateFramebuffer failed.");
    }

    /* Command pool + buffers */
    VkCommandPoolCreateInfo cpci;
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.pNext = NULL;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = vk->qfam;
    if (vk->d.vkCreateCommandPool(vk->device, &cpci, NULL, &vk->cmdpool) != VK_SUCCESS)
        fatalA("vkCreateCommandPool failed.");

    VkCommandBufferAllocateInfo cbai;
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.pNext = NULL;
    cbai.commandPool = vk->cmdpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = vk->image_count;
    if (vk->d.vkAllocateCommandBuffers(vk->device, &cbai, vk->cmdbufs) != VK_SUCCESS)
        fatalA("vkAllocateCommandBuffers failed.");

    /* Sync */
    VkSemaphoreCreateInfo sci3 = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
    if (vk->d.vkCreateSemaphore(vk->device, &sci3, NULL, &vk->sem_acquire) != VK_SUCCESS) fatalA("sem acquire failed");
    if (vk->d.vkCreateSemaphore(vk->device, &sci3, NULL, &vk->sem_render) != VK_SUCCESS) fatalA("sem render failed");

    VkFenceCreateInfo fci2;
    fci2.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci2.pNext = NULL;
    fci2.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vk->d.vkCreateFence(vk->device, &fci2, NULL, &vk->fence) != VK_SUCCESS) fatalA("fence failed");

    /* Vertex buffer (host visible) */
    vk->max_vertices = 6 + (256u * 6u); /* background(6) + 256 bars(6 each) */

    VkBufferCreateInfo bci;
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = NULL;
    bci.flags = 0;
    bci.size = (VkDeviceSize)vk->max_vertices * sizeof(Vertex);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bci.queueFamilyIndexCount = 0;
    bci.pQueueFamilyIndices = NULL;

    if (vk->d.vkCreateBuffer(vk->device, &bci, NULL, &vk->vbuf) != VK_SUCCESS)
        fatalA("vkCreateBuffer failed.");

    VkMemoryRequirements mr;
    vk->d.vkGetBufferMemoryRequirements(vk->device, vk->vbuf, &mr);

    uint32_t mt = find_mem_type(&vk->d, vk->phys, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == 0xFFFFFFFFu) fatalA("No host visible memory type.");

    VkMemoryAllocateInfo mai;
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = NULL;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;

    if (vk->d.vkAllocateMemory(vk->device, &mai, NULL, &vk->vmem) != VK_SUCCESS)
        fatalA("vkAllocateMemory failed.");
    if (vk->d.vkBindBufferMemory(vk->device, vk->vbuf, vk->vmem, 0) != VK_SUCCESS)
        fatalA("vkBindBufferMemory failed.");
    if (vk->d.vkMapMemory(vk->device, vk->vmem, 0, VK_WHOLE_SIZE, 0, &vk->vmap) != VK_SUCCESS)
        fatalA("vkMapMemory failed.");
}

static void vk_shutdown(VkDemo* vk) {
    if (!vk->device) return;

    vk->d.vkDeviceWaitIdle(vk->device);

    if (vk->vmap) vk->d.vkUnmapMemory(vk->device, vk->vmem);
    if (vk->vbuf) vk->d.vkDestroyBuffer(vk->device, vk->vbuf, NULL);
    if (vk->vmem) vk->d.vkFreeMemory(vk->device, vk->vmem, NULL);

    if (vk->fence) vk->d.vkDestroyFence(vk->device, vk->fence, NULL);
    if (vk->sem_render) vk->d.vkDestroySemaphore(vk->device, vk->sem_render, NULL);
    if (vk->sem_acquire) vk->d.vkDestroySemaphore(vk->device, vk->sem_acquire, NULL);

    if (vk->cmdpool) vk->d.vkDestroyCommandPool(vk->device, vk->cmdpool, NULL);

    for (uint32_t i = 0; i < vk->image_count; i++) {
        if (vk->fbs[i]) vk->d.vkDestroyFramebuffer(vk->device, vk->fbs[i], NULL);
        if (vk->views[i]) vk->d.vkDestroyImageView(vk->device, vk->views[i], NULL);
    }

    if (vk->pipe) vk->d.vkDestroyPipeline(vk->device, vk->pipe, NULL);
    if (vk->pl) vk->d.vkDestroyPipelineLayout(vk->device, vk->pl, NULL);
    if (vk->rp) vk->d.vkDestroyRenderPass(vk->device, vk->rp, NULL);

    if (vk->swapchain) vk->d.vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);

    vk->d.vkDestroyDevice(vk->device, NULL);

    if (vk->surface) vk->d.vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
    if (vk->instance) vk->d.vkDestroyInstance(vk->instance, NULL);

    if (vk->d.vulkan) FreeLibrary(vk->d.vulkan);
}

/* Build vertices: background + bar graph */
static uint32_t build_vertices(Vertex* out, uint32_t maxv, const uint32_t* samples, uint32_t sample_count,
                               float occ_ratio)
{
    (void)maxv;

    uint32_t n = 0;

    /* Background: two triangles, subtle gradient */
    {
        float top_r = 0.02f, top_g = 0.03f, top_b = 0.06f;
        float bot_r = 0.01f, bot_g = 0.01f, bot_b = 0.02f;

        out[n++] = (Vertex){ -1.f, -1.f, bot_r, bot_g, bot_b };
        out[n++] = (Vertex){  1.f, -1.f, bot_r, bot_g, bot_b };
        out[n++] = (Vertex){  1.f,  1.f, top_r, top_g, top_b };

        out[n++] = (Vertex){ -1.f, -1.f, bot_r, bot_g, bot_b };
        out[n++] = (Vertex){  1.f,  1.f, top_r, top_g, top_b };
        out[n++] = (Vertex){ -1.f,  1.f, top_r, top_g, top_b };
    }

    /* Bar graph area (bottom band) */
    float x0 = -0.98f, x1 = 0.98f;
    float y_base = -0.90f;
    float y_max  =  0.70f;
    float w = (x1 - x0) / (float)sample_count;

    /* Color shifts toward red as occupancy rises */
    float base_r = 0.15f + 0.80f * occ_ratio;
    float base_g = 0.80f - 0.60f * occ_ratio;
    float base_b = 0.30f + 0.20f * (1.0f - occ_ratio);

    /* find max sample for scaling */
    uint32_t maxs = 1;
    for (uint32_t i = 0; i < sample_count; i++) if (samples[i] > maxs) maxs = samples[i];

    for (uint32_t i = 0; i < sample_count; i++) {
        float h = (float)samples[i] / (float)maxs;
        float xb0 = x0 + (float)i * w;
        float xb1 = xb0 + w * 0.90f; /* small gap */
        float yt0 = y_base;
        float yt1 = y_base + h * (y_max - y_base);

        /* slight brightness variation across bars */
        float t = (float)i / (float)(sample_count - 1);
        float r = base_r * (0.7f + 0.3f * t);
        float g = base_g * (0.8f + 0.2f * (1.0f - t));
        float b = base_b;

        out[n++] = (Vertex){ xb0, yt0, r, g, b };
        out[n++] = (Vertex){ xb1, yt0, r, g, b };
        out[n++] = (Vertex){ xb1, yt1, r, g, b };

        out[n++] = (Vertex){ xb0, yt0, r, g, b };
        out[n++] = (Vertex){ xb1, yt1, r, g, b };
        out[n++] = (Vertex){ xb0, yt1, r, g, b };
    }
    return n;
}

static void vk_draw(VkDemo* vk, uint32_t vertex_count) {
    if (vk->d.vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
        fatalA("vkWaitForFences failed.");
    vk->d.vkResetFences(vk->device, 1, &vk->fence);

    uint32_t imageIndex = 0;
    VkResult ar = vk->d.vkAcquireNextImageKHR(vk->device, vk->swapchain, 1000000000ull,
                                             vk->sem_acquire, VK_NULL_HANDLE, &imageIndex);
    if (ar != VK_SUCCESS) fatalA("vkAcquireNextImageKHR failed (resize not handled in this minimal demo).");

    VkCommandBuffer cb = vk->cmdbufs[imageIndex];
    vk->d.vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo bi;
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.pNext = NULL;
    bi.flags = 0;
    bi.pInheritanceInfo = NULL;
    vk->d.vkBeginCommandBuffer(cb, &bi);

    VkClearValue cv;
    cv.color.float32[0] = 0.0f;
    cv.color.float32[1] = 0.0f;
    cv.color.float32[2] = 0.0f;
    cv.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rbi;
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.pNext = NULL;
    rbi.renderPass = vk->rp;
    rbi.framebuffer = vk->fbs[imageIndex];
    rbi.renderArea.offset.x = 0;
    rbi.renderArea.offset.y = 0;
    rbi.renderArea.extent = vk->extent;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &cv;

    vk->d.vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp;
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)vk->extent.width;
    vp.height = (float)vk->extent.height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vk->d.vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc;
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent = vk->extent;
    vk->d.vkCmdSetScissor(cb, 0, 1, &sc);

    vk->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipe);

    VkDeviceSize off = 0;
    vk->d.vkCmdBindVertexBuffers(cb, 0, 1, &vk->vbuf, &off);
    vk->d.vkCmdDraw(cb, vertex_count, 1, 0, 0);

    vk->d.vkCmdEndRenderPass(cb);
    vk->d.vkEndCommandBuffer(cb);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo si;
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = NULL;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vk->sem_acquire;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk->sem_render;

    if (vk->d.vkQueueSubmit(vk->queue, 1, &si, vk->fence) != VK_SUCCESS)
        fatalA("vkQueueSubmit failed.");

    VkPresentInfoKHR pi;
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.pNext = NULL;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk->sem_render;
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk->swapchain;
    pi.pImageIndices = &imageIndex;
    pi.pResults = NULL;

    if (vk->d.vkQueuePresentKHR(vk->queue, &pi) != VK_SUCCESS)
        fatalA("vkQueuePresentKHR failed.");
}

/* -------------------- Win32 window -------------------- */

static volatile LONG g_running = 1;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    if (m == WM_CLOSE) { InterlockedExchange(&g_running, 0); DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { InterlockedExchange(&g_running, 0); PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

/* -------------------- Entry point (no CRT) -------------------- */

void WINAPI WinMainCRTStartup(void) {
    /* Create ring + producer thread */
    SPSC_Ring* q = spsc_create(1u << 16, sizeof(Event));
    if (!q) fatalA("Failed to create ring buffer.");

    ProducerCtx pc;
    pc.q = q;
    pc.running = 1;

    HANDLE prod = CreateThread(NULL, 0, producer_thread, &pc, 0, NULL);
    if (!prod) fatalA("CreateThread failed.");

    /* Window (no console) */
    WNDCLASSW wc;
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = NULL;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = L"SPSC_VK_DEMO";
    if (!RegisterClassW(&wc)) fatalA("RegisterClassW failed.");

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX; /* not resizable */
    RECT r = { 0, 0, 1280, 720 };
    AdjustWindowRect(&r, style, FALSE);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SPSC Ring Buffer (Vulkan) - CRT-free",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) fatalA("CreateWindowExW failed.");
    ShowWindow(hwnd, SW_SHOW);

    VkDemo vk;
    copy_bytes(&vk, &(VkDemo){0}, sizeof(vk)); /* zero without CRT memset trick (struct literal ok) */
    vk_init(&vk, hwnd);

    /* Sample history for the bar graph */
    enum { SAMPLE_N = 256 };
    uint32_t samples[SAMPLE_N];
    for (uint32_t i = 0; i < SAMPLE_N; i++) samples[i] = 0;

    Vertex* verts = (Vertex*)vk.vmap;

    MSG msg;
    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        /* Drain queue: count events this frame */
        uint32_t drained = 0;
        Event e;
        while (spsc_try_dequeue(q, &e)) drained++;

        /* push into sample history (simple shift; cheap at 256) */
        for (uint32_t i = 0; i + 1 < SAMPLE_N; i++) samples[i] = samples[i + 1];
        samples[SAMPLE_N - 1] = drained;

        float occ = 0.0f;
        {
            uint32_t sz = spsc_size(q);
            uint32_t cap = spsc_usable_capacity(q);
            occ = cap ? ((float)sz / (float)cap) : 0.0f;
            if (occ < 0.0f) occ = 0.0f;
            if (occ > 1.0f) occ = 1.0f;
        }

        uint32_t vcount = build_vertices(verts, vk.max_vertices, samples, SAMPLE_N, occ);
        vk_draw(&vk, vcount);
    }

    /* stop producer */
    InterlockedExchange(&pc.running, 0);
    WaitForSingleObject(prod, INFINITE);
    CloseHandle(prod);

    vk_shutdown(&vk);
    spsc_destroy(q);

    ExitProcess(0);
}