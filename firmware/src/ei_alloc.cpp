// Where the Edge Impulse model's memory comes from.
//

#include <Arduino.h>
#include <string.h>
#include "esp_heap_caps.h"

// Overridable so the split can be A/B tested without editing code:
//   -DPSRAM_THRESHOLD=2048        large blocks in PSRAM 
//   -DPSRAM_THRESHOLD=0x7FFFFFFF  everything internal
#ifndef PSRAM_THRESHOLD
#define PSRAM_THRESHOLD 2048
#endif

#define EI_ALIGN 16

static size_t g_ps_bytes = 0, g_int_bytes = 0, g_ps_n = 0, g_int_n = 0;

static void *ei_alloc_impl(size_t bytes, bool zero)
{
    if (bytes == 0) return nullptr;
    const size_t padded = (bytes + (EI_ALIGN - 1)) & ~(size_t)(EI_ALIGN - 1);

    //if the preferred pool cannot serve the block the other one is tried, so a full PSRAM never turns into a crash.
    const bool want_psram = (padded >= PSRAM_THRESHOLD);
    void *p = nullptr;

    if (want_psram) {
        p = heap_caps_aligned_alloc(EI_ALIGN, padded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) { g_ps_bytes += padded; g_ps_n++; }
    }
    if (!p) {
        p = heap_caps_aligned_alloc(EI_ALIGN, padded, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) { g_int_bytes += padded; g_int_n++; }
    }
    if (!p && !want_psram) {
        p = heap_caps_aligned_alloc(EI_ALIGN, padded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) { g_ps_bytes += padded; g_ps_n++; }
    }

    if (p && zero) memset(p, 0, padded);
    return p;
}

void *ei_malloc(size_t size)                 { return ei_alloc_impl(size, false); }
void *ei_calloc(size_t nitems, size_t size)  { return ei_alloc_impl(nitems * size, true); }
void  ei_free(void *ptr)                     { if (ptr) heap_caps_aligned_free(ptr); }

void eiAllocReport()
{
    Serial.printf("[ei] memory: %u B in %u PSRAM blocks, %u B in %u internal blocks\n",
                  (unsigned)g_ps_bytes, (unsigned)g_ps_n,
                  (unsigned)g_int_bytes, (unsigned)g_int_n);
}
