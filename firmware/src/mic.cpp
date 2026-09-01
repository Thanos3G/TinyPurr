// Software PDM decode for the microphone. See mic.h for why.
//
// I2S1 runs as an ordinary 32-bit receiver. Its bit clock becomes the mic's


#include "mic.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include "esp_heap_caps.h"

#ifndef MIC_HW_PDM
#define MIC_HW_PDM 1
#endif

#if MIC_HW_PDM
static const i2s_port_t MIC_PORT = I2S_NUM_0;
#else
static const i2s_port_t MIC_PORT = I2S_NUM_1;
#endif

static const int PIN_MIC_CLK  = 7;   // H1 pin 8 -> mic CLK
static const int PIN_MIC_DATA = 8;   // H1 pin 7 -> mic DATA
static const int PIN_MIC_WS   = 4;   // H2 pin 7, nothing connected: I2S needs a
                                     

// 32000 x 32 bits x 2 slots = 2.048 MHz bit clock, the usual rate for these
// parts, and we capture BOTH slots so none of the mic's bits are discarded.
// The mic only drives on one clock phase, so half the captured bits are the
// idle half of the frame
static const int MIC_RATE   = 32000;
static const int DECIMATE   = 64;     // counted in *valid* bits
static const int READ_WORDS = 256;
static const int OUT_RATE   = 16000;

static volatile float    gPeak = 0.0f, gRms = 0.0f;
static volatile uint32_t gBps = 0, gDecoded = 0;
// The I2S driver's spinlock is core-bound
static volatile bool     gWantRun   = false;  
static volatile bool     gInstalled = false;  
static volatile int      gStride = 2;   // 1 = all bits, 2 = even, 3 = odd

static volatile float    gGain   = 100.0f; 


// The mic task writes and the inference task reads, with no lock. 
static const size_t   RING_N = 32000;          // 2 s at 16 kHz
static const size_t   FRAME  = 800;            // 50 ms, the alignment resolution
static int16_t       *gRing  = nullptr;
static volatile size_t   gRingHead  = 0;       // next slot to write
static volatile uint32_t gRingTotal = 0;       // samples ever written
static volatile float    gLevel     = 0.0f;    // ~30 ms RMS envelope
static volatile int32_t  gRawMin = 0, gRawMax = 0;   // raw I2S range, for scaling
static volatile bool     gSignExt = true;   // treat the low 16 bits as signed
static volatile bool     gDcBlock = true;   // remove the mic's idle offset
static volatile float    gMinEnergy = 120.0f;
// The I2S receiver emits a full-scale spike on the first block after install --
// visible as "raw -32768..1211" the moment the speaker hands the port back.

static const int         SETTLE_SAMPLES = 4800;
static int               gSettle = SETTLE_SAMPLES;
static float             gLevelSq   = 0.0f;

static bool micInstall();  

// Set bits per byte.
static uint8_t popTab[256];

static void buildPopTab()
{
    for (int i = 0; i < 256; i++) {
        int c = 0;
        for (int b = 0; b < 8; b++) if (i & (1 << b)) c++;
        popTab[i] = (uint8_t)c;
    }
}

#if MIC_HW_PDM
static void micTask(void*)
{
    static int32_t raw[READ_WORDS];
    float  peak = 0.0f;
    double sumSq = 0.0;
    uint32_t nOut = 0, bits = 0, lastMs = millis();
    float  dc = 0.0f;
    int32_t rawMin = INT32_MAX, rawMax = INT32_MIN;

    for (;;) {
        if (gWantRun && !gInstalled) { if (micInstall()) { gInstalled = true; gSettle = SETTLE_SAMPLES; } }
        if (!gWantRun && gInstalled) { i2s_driver_uninstall(MIC_PORT); gInstalled = false; }
        if (!gInstalled) { vTaskDelay(pdMS_TO_TICKS(4)); continue; }

        size_t got = 0;
        if (i2s_read(MIC_PORT, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) continue;
        const int n = got / 4;
        bits += n * 32;

        for (int i = 0; i < n; i++) {
            // The PDM receiver hands back a SIGNED 16-bit sample in the low half of each 32-bit word. 
            if (gSettle > 0) { gSettle--; continue; }  

            const int32_t sv = gSignExt ? (int32_t)(int16_t)(raw[i] & 0xFFFF)
                                        : raw[i];
            if (sv < rawMin) rawMin = sv;
            if (sv > rawMax) rawMax = sv;

            // The mic idles around +1190 rather than zero, so the offset has to come out before anything else looks at the sample.
            dc += 0.001f * ((float)sv - dc);

            float pcm = ((float)sv - (gDcBlock ? dc : 0.0f)) * (gGain / 16.0f);
            if (pcm >  32767.0f) pcm =  32767.0f;
            if (pcm < -32768.0f) pcm = -32768.0f;

            // Stored as int16 PCM, that is the scale Edge Impulse wants
            if (gRing) {
                gRing[gRingHead] = (int16_t)pcm;
                gRingHead = (gRingHead + 1) % RING_N;
                gRingTotal++;
            }

            const float x = pcm / 32768.0f;
            // ~30 ms time constant long enough not to chatter on individual pulses, short enough to catch a word.
            gLevelSq += 0.002f * (x * x - gLevelSq);
            gLevel    = sqrtf(gLevelSq);

            const float a = x < 0.0f ? -x : x;
            if (a > peak) peak = a;
            sumSq += (double)x * x;
            nOut++;
        }

        const uint32_t now = millis();
        if (now - lastMs >= 500) {
            gPeak = peak;
            gRms  = (nOut > 0) ? sqrtf((float)(sumSq / nOut)) : 0.0f;
            gBps  = (uint32_t)((uint64_t)bits * 1000 / (now - lastMs));
            gDecoded = nOut;
            gRawMin = rawMin; gRawMax = rawMax;
            peak = 0.0f; sumSq = 0.0; nOut = 0; bits = 0; lastMs = now;
            rawMin = INT32_MAX; rawMax = INT32_MIN;
        }
    }
}
#else
static void micTask(void*)
{
    static uint32_t raw[READ_WORDS];

    int      acc     = 0;      // set bits in the current window
    int      nbits   = 0;      // bits accumulated so far
    float    dc      = 0.0f;   // running mean, for DC blocking
    float    lp1 = 0.0f, lp2 = 0.0f;
    float    peak    = 0.0f;
    double   sumSq   = 0.0;
    uint32_t nOut    = 0;
    uint32_t bits    = 0;
    uint32_t lastMs  = millis();

    for (;;) {
        if (gWantRun && !gInstalled) { if (micInstall()) gInstalled = true; }
        if (!gWantRun && gInstalled) { i2s_driver_uninstall(MIC_PORT); gInstalled = false; }
        if (!gInstalled) { vTaskDelay(pdMS_TO_TICKS(4)); continue; }

        size_t got = 0;
        if (i2s_read(MIC_PORT, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) continue;
        const int words = got / 4;
        bits += words * 32;

        for (int w = 0; w < words; w++) {
            const uint32_t v = raw[w];

            if (gStride == 1) {
                acc += popTab[v & 0xFF] + popTab[(v >> 8) & 0xFF]
                     + popTab[(v >> 16) & 0xFF] + popTab[(v >> 24) & 0xFF];
                nbits += 32;
            } else {
                // Every other bit: the mic only drives on one clock phase, so
                // half of what we capture is the idle half of the frame. 
                const int start = (gStride == 3) ? 1 : 0;
                for (int b = start; b < 32; b += 2) if (v & (1u << b)) acc++;
                nbits += 16;
            }

            if (nbits >= DECIMATE) {
                // a balanced stream sits at half the window.
                float x = (float)acc - (float)nbits * 0.5f;
                x /= (float)nbits * 0.5f;          // -1 .. 1

                dc += 0.001f * (x - dc);           // block the offset
                x  -= dc;

                lp1 += 0.35f * (x - lp1);
                lp2 += 0.35f * (lp1 - lp2);
                x = lp2 * gGain;
                if (x >  1.0f) x =  1.0f;
                if (x < -1.0f) x = -1.0f;

                const float a = x < 0.0f ? -x : x;
                if (a > peak) peak = a;
                sumSq += (double)x * x;
                nOut++;

                acc = 0;
                nbits = 0;
            }
        }

        const uint32_t now = millis();
        if (now - lastMs >= 500) {
            gPeak    = peak;
            gRms     = (nOut > 0) ? sqrtf((float)(sumSq / nOut)) : 0.0f;
            gBps     = (uint32_t)((uint64_t)bits * 1000 / (now - lastMs));
            gDecoded = nOut;
            peak = 0.0f; sumSq = 0.0; nOut = 0; bits = 0;
            lastMs = now;
        }
    }
}

#endif  // MIC_HW_PDM

static bool micInstall()
{
    i2s_config_t cfg = {};
#if MIC_HW_PDM
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    cfg.sample_rate          = OUT_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = 0;
    cfg.dma_buf_count        = 4;
    cfg.dma_buf_len          = 512;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = false;
    cfg.fixed_mclk           = 0;

    esp_err_t err = i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[mic] driver_install failed: %s\n", esp_err_to_name(err));
        return false;
    }
    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = -1;
    pins.ws_io_num    = PIN_MIC_CLK;      // PDM clock out to the mic
    pins.data_out_num = -1;
    pins.data_in_num  = PIN_MIC_DATA;
    err = i2s_set_pin(MIC_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[mic] set_pin failed: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(MIC_PORT);
        return false;
    }
    i2s_zero_dma_buffer(MIC_PORT);
    return true;
#else
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = MIC_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;  // keep every bit
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 512;
    cfg.use_apll             = false;

    esp_err_t err = i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[mic] driver_install failed: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_MIC_CLK;      // this is the mic's clock
    pins.ws_io_num    = PIN_MIC_WS;       // unconnected, but I2S insists
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = PIN_MIC_DATA;

    err = i2s_set_pin(MIC_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[mic] set_pin failed: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(MIC_PORT);
        return false;
    }

    return true;
#endif
}

bool micBegin()
{
    buildPopTab();
    gRing = (int16_t *)heap_caps_malloc(RING_N * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gRing) Serial.println("[mic] ring buffer alloc failed; no classification");
    gWantRun = true;
    // Core 0, the same core the audio task runs on.
    xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 4, NULL, 0);
    Serial.printf("[mic] PDM on I2S0: CLK GPIO%d, DATA GPIO%d, %d Hz\n",
                  PIN_MIC_CLK, PIN_MIC_DATA, OUT_RATE);
    return true;
}

// Release I2S0 so the speaker can take it. The task idles meanwhile.
void micSuspend()
{
    gWantRun = false;
    for (int i = 0; i < 60 && gInstalled; i++) vTaskDelay(pdMS_TO_TICKS(4));
}

void micResume() { gWantRun = true; }

bool micIsRunning() { return gInstalled; }


float    micPeak()          { return gPeak; }
float    micRms()           { return gRms; }
uint32_t micBitsPerSecond() { return gBps; }
uint32_t micSamplesDecoded() { return gDecoded; }
void     micSetStride(int s) { gStride = (s >= 1 && s <= 3) ? s : 1; }
void     micSetGain(float g) { gGain = (g < 1.0f) ? 1.0f : (g > 400.0f ? 400.0f : g); }
float    micGetGain()        { return gGain; }
int      micGetStride()      { return gStride; }

bool micSnapshot(float *out, size_t n)
{
    if (!gRing || n > RING_N) return false;
    if (gRingTotal < n) return false;

   
    size_t start = (gRingHead + RING_N - n) % RING_N;
    for (size_t i = 0; i < n; i++) {
        out[i] = (float)gRing[(start + i) % RING_N];
    }
    return true;
}

float    micLevel()        { return gLevel; }
uint32_t micTotalSamples() { return gRingTotal; }

int32_t micRawMin() { return gRawMin; }
int32_t micRawMax() { return gRawMax; }

void micSetSignExtend(bool on) { gSignExt = on; }
bool micGetSignExtend()        { return gSignExt; }
void micSetDcBlock(bool on)    { gDcBlock = on; }
bool micGetDcBlock()           { return gDcBlock; }

// Extract a window positioned so the speech sits where the model expects it.


bool micSnapshotAligned(float *out, size_t n, float centroidFrac, float *foundAt)
{
    if (!gRing || n > RING_N) return false;

    const size_t avail = (gRingTotal < RING_N) ? gRingTotal : RING_N;
    if (avail < n + FRAME) return false;

    const size_t head  = gRingHead;
    const size_t base  = (head + RING_N - avail) % RING_N;   
    const size_t nf    = avail / FRAME;

    // Energy envelope, one value per 50 ms.
    float env[RING_N / FRAME];
    float emax = 0.0f;
    for (size_t f = 0; f < nf; f++) {
        double sum = 0.0;
        for (size_t i = 0; i < FRAME; i++) {
            const float v = (float)gRing[(base + f * FRAME + i) % RING_N];
            sum += (double)v * v;
        }
        env[f] = sqrtf((float)(sum / FRAME));
        if (env[f] > emax) emax = env[f];
    }
    // Speech has to stand out from the room, not clear a fixed number.

    float sorted_env[RING_N / FRAME];
    for (size_t f = 0; f < nf; f++) sorted_env[f] = env[f];
    for (size_t a = 1; a < nf; a++) {               
        const float k = sorted_env[a];
        size_t b = a;
        while (b > 0 && sorted_env[b - 1] > k) { sorted_env[b] = sorted_env[b - 1]; b--; }
        sorted_env[b] = k;
    }
    const float floorEst = sorted_env[nf / 4];       // 25th percentile = the room
    if (emax < floorEst * 2.5f || emax < gMinEnergy) return false;

    // Energy-weighted centroid over the frames that actually carry the word.
    // Quiet frames are excluded so a long tail of silence cannot drag it.
    double wsum = 0.0, w = 0.0;
    for (size_t f = 0; f < nf; f++) {
        if (env[f] < emax * 0.25f) continue;
        const double e = (double)env[f];
        wsum += e * (double)(f * FRAME + FRAME / 2);
        w    += e;
    }
    if (w <= 0.0) return false;
    const long centroid = (long)(wsum / w);

    // Place the window so the centroid lands at centroidFrac of it, then clamp
    // so the window stays inside what we actually have.
    // Waiting a moment for the tail to arrive costs ~500 ms and turns eleven
    // misaligned guesses per utterance into one aligned answer.
    const long start = centroid - (long)(centroidFrac * (float)n);
    if (start < 0 || start > (long)(avail - n)) return false;

    for (size_t i = 0; i < n; i++) {
        out[i] = (float)gRing[(base + (size_t)start + i) % RING_N];
    }
    if (foundAt) *foundAt = (float)(centroid - start) / (float)n;
    return true;
}

void  micSetMinEnergy(float e) { gMinEnergy = (e < 1.0f) ? 1.0f : e; }
float micGetMinEnergy()        { return gMinEnergy; }

void micFlush()
{
    gRingTotal = 0;
    gRingHead  = 0;
    gLevelSq   = 0.0f;
    gLevel     = 0.0f;
}
