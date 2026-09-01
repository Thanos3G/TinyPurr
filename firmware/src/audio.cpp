// Audio out for the PAM8302.
// GPIO1 drives the amplifier's SD pin, so the amp is powered down whenever
// nothing is sounding, gaps between repeats included.
// Everything is synthesised a sample at a time.

#include "audio.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include "mic.h"
#include <math.h>

static const i2s_port_t PORT         = I2S_NUM_0;
static const int        PIN_PDM_DATA = 12;  // amp A+
static const int        PIN_PDM_CLK  = 5;   // H2 pin 8
static const int        PIN_AMP_SD   = 10;  // -> amp SD
static const int        SR           = 48000;
static const int        BLOCK        = 128;

static const float TWO_PI_F = 6.2831853f;

static volatile float    gVolume  = 0.15f;  
static volatile Sound    gWant    = SND_NONE;
static volatile uint32_t gTrigger = 0;      

// ------------------------------------------------------------------- tools --
static uint32_t rng = 0x12345678u;
static inline float noise()
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(int32_t)rng / 2147483648.0f;
}

// Deterministic per-repeat spread, roughly -1..1. The same repeat always gives the same value, so a sound is stable while it plays but differs from the last.
static inline float hashf(uint32_t n)
{
    n = n * 1664525u + 1013904223u;
    n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15;
    return (float)(int32_t)n / 2147483648.0f;
}

// Cubic soft clip
static inline float softclip(float x)
{
    if (x >=  1.0f) return  1.0f;
    if (x <= -1.0f) return -1.0f;
    return 1.5f * (x - x * x * x / 3.0f);
}

struct OnePole {
    float y, a;
    void set(float fc) { a = 1.0f - expf(-TWO_PI_F * fc / SR); y = 0.0f; }
    float lp(float x)  { y += a * (x - y); return y; }
};

// Slow random control signal, roughly -1..1. Noise used to *steer* a parameter, never mixed into the audio.

struct Ctl {
    OnePole a, b, c, d;
    float   g;
    void set(float hz, float gain) { a.set(hz); b.set(hz); c.set(hz); d.set(hz); g = gain; }
    float next() {
        float v = d.lp(c.lp(b.lp(a.lp(noise())))) * g;
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        return v;
    }
};

// A 1024 entry sine table, the growl and purr together run nearly forty  oscillator lookups a sample at 48 kHz.
static float sinTab[1025];
static bool  sinTabReady = false;

static void buildSinTab()
{
    if (sinTabReady) return;
    for (int i = 0; i <= 1024; i++) sinTab[i] = sinf(TWO_PI_F * i / 1024.0f);
    sinTabReady = true;
}

// Phase is in turns, not radians.
static inline float osc(float ph)
{
    ph -= floorf(ph);
    const float x  = ph * 1024.0f;
    const int   i  = (int)x;
    const float fr = x - (float)i;
    return sinTab[i] + (sinTab[i + 1] - sinTab[i]) * fr;
}

// ------------------------------------------------------------------ voices --
static const int NHARM = 14;

static OnePole grLp;                             // growl
static Ctl     jitC, shimC, ratC, fmtC;
static float   grPh, roughPh, subPh, wobPh;
static float   grF1, grF2, grBase;
static float   grAmp[NHARM + 1], grTgt[NHARM + 1], kPow[NHARM + 1];

// The purr is harmonics of a 21 Hz pulse train.
static const float PURR_RATE   = 21.0f;
static const float PURR_CENTRE = 1000.0f;
static const float PURR_WIDTH  = 250.0f;
static const int   PMAX        = 24;

static OnePole pLp;                              // purr
static uint8_t pIdx[PMAX];
static float   pA[PMAX], pT[PMAX];
static int     pN;
static float   purrSlow;

static OnePole meowLp;                             // meow
static float   oscPhase, trillPhase, purrPhase, swellPhase;

static uint32_t msToN(uint32_t ms) { return (uint32_t)((uint64_t)ms * SR / 1000); }

// Growl and meow repeat on a cycle
static const uint32_t GROWL_N   = 1900 * SR / 1000;
static const uint32_t GROWL_CYC = 2600 * SR / 1000;
static const uint32_t MEOW_N   =  340 * SR / 1000;
static const uint32_t MEOW_CYC =  820 * SR / 1000;

static void resetVoices()
{
    grLp.set(3000);
    jitC.set(11, 46.0f); shimC.set(23, 32.0f); ratC.set(4, 77.0f); fmtC.set(2.5f, 97.0f);
    grPh = roughPh = subPh = wobPh = 0.0f;
    grF1 = 1050.0f; grF2 = 1950.0f; grBase = 180.0f;
    for (int k = 0; k <= NHARM; k++) {
        grAmp[k] = grTgt[k] = 0.0f;
        kPow[k]  = powf((float)(k < 1 ? 1 : k), 0.7f);
    }

    pLp.set(850);
    purrSlow = 0.0f;
    pN = 0;
    for (int k = 2; k <= 200 && pN < PMAX; k++) {
        const float f = PURR_RATE * k;
        if (f < 110.0f)  continue;
        if (f > 4200.0f) break;
        const float d = (f - PURR_CENTRE) / PURR_WIDTH;
        if (1.0f / (1.0f + d * d) < 0.06f) continue;
        pIdx[pN] = (uint8_t)k;
        pA[pN]   = 0.0f;
        pT[pN]   = 0.0f;
        pN++;
    }

    meowLp.set(2100);
    oscPhase = trillPhase = purrPhase = swellPhase = 0.0f;
}

// Linear attack / release envelope over a fixed length one shot.
static float shot(uint32_t pos, uint32_t total, uint32_t atk, uint32_t rel)
{
    if (pos < atk)         return (float)pos / atk;
    if (total - pos < rel) return (float)(total - pos) / rel;
    return 1.0f;
}

static float render(Sound s, uint32_t pos)
{
    switch (s) {

    case SND_GROWL: {
        const uint32_t rep = pos / GROWL_CYC;
        const uint32_t ph  = pos - rep * GROWL_CYC;
        if (ph >= GROWL_N) return 0.0f;                 // the gap between growls

        const float t = (float)ph / GROWL_N;

        const float jit  = jitC.next();                 // pitch wobble
        const float shim = shimC.next();                // amplitude irregularity
        const float ratr = ratC.next();                 // rattle rate drift
        const float fmov = fmtC.next();                 // the mouth moving

        // The pitch arc moves over 1.9 s, so evaluating powf and sinf for it every sample was waste enough to starve the audio ring. Only the jitter needs per sample resolution.
        const float f0 = grBase * (1.0f + 0.075f * jit);

        if ((pos & 127) == 0) {
            // Arch up and back down, ending where it started. Sliding away to 34 Hz put every harmonic below the formants, which tilted the spectrum towards the top of the stack and made the tail whine.
            const float arc = sinf((float)M_PI * powf(t, 0.70f));
            grBase = (155.0f + 95.0f * arc) * (1.0f + 0.15f * hashf(rep));

            grF1 = 1050.0f * (1.0f + 0.18f * fmov + 0.10f * hashf(rep + 17u));
            grF2 = 1950.0f * (1.0f - 0.14f * fmov + 0.10f * hashf(rep + 53u));
            float sum = 0.0f;
            for (int k = 1; k <= NHARM; k++) {
                const float f = f0 * k;
                if (f > 6500.0f) { grTgt[k] = 0.0f; continue; }
                const float d1 = (f - grF1) / 450.0f;
                const float d2 = (f - grF2) / 620.0f;
                const float g  = 0.18f + 1.0f / (1.0f + d1 * d1)
                                       + 0.65f / (1.0f + d2 * d2);
                grTgt[k] = g / kPow[k];
                sum += grTgt[k];
            }
            if (sum > 0.0f) for (int k = 1; k <= NHARM; k++) grTgt[k] /= sum;
        }

        grPh += f0 / SR;
        grPh -= floorf(grPh);

        // Glide towards the block's targets rather than stepping onto them
        float v = 0.0f;
        for (int k = 1; k <= NHARM; k++) {
            grAmp[k] += 0.004f * (grTgt[k] - grAmp[k]);
            v += grAmp[k] * osc(grPh * (float)k);
        }

        // Roughness
        const float ratHz = 20.0f * (1.0f + 0.22f * hashf(rep + 91u))
                                  * (1.0f + 0.55f * ratr);
        roughPh += ratHz / SR;        if (roughPh >= 1.0f) roughPh -= 1.0f;
        subPh   += (f0 * 0.5f) / SR;  if (subPh   >= 1.0f) subPh   -= 1.0f;
        wobPh   += 6.7f / SR;         if (wobPh   >= 1.0f) wobPh   -= 1.0f;

        float r0 = 0.5f + 0.5f * osc(roughPh);
        r0 *= r0;
        const float rough = 0.62f + 0.38f * r0;         // never chops to silence
        const float sub   = 0.84f + 0.16f * osc(subPh);
        const float wob   = 0.88f + 0.12f * osc(wobPh);

        v = grLp.lp(v * rough * sub * wob * (1.0f + 0.28f * shim));
        v *= 1.0f - 0.25f * t;                          

        return v * shot(ph, GROWL_N, msToN(260), msToN(460)) * 1.9f;
    }

    case SND_MEOW: {
        // A cat meow
        const uint32_t rep = pos / MEOW_CYC;
        const uint32_t ph  = pos - rep * MEOW_CYC;
        if (ph >= MEOW_N) return 0.0f;                 // the gap between meows

        const float t      = (float)ph / MEOW_N;
        const float detune = 1.0f + 0.13f * hashf(rep + 7u);
        const float f      = (820.0f + 460.0f * sinf((float)M_PI * t)) * detune;

        oscPhase += TWO_PI_F * f / SR;
        if (oscPhase > TWO_PI_F) oscPhase -= TWO_PI_F;
        trillPhase += 40.0f * (1.0f + 0.20f * hashf(rep + 29u)) / SR;
        if (trillPhase > 1.0f) trillPhase -= 1.0f;

        const float tone  = sinf(oscPhase) + 0.14f * sinf(2.0f * oscPhase);
        const float trill = 0.60f + 0.40f * sinf(TWO_PI_F * trillPhase);

        return meowLp.lp(tone * trill) * shot(ph, MEOW_N, msToN(55), msToN(150)) * 0.58f;
    }

    case SND_PURR: {
        purrSlow += 0.13f / SR;
        if (purrSlow >= 1.0f) purrSlow -= 1.0f;
        const float f0 = PURR_RATE * (1.0f + 0.06f * osc(purrSlow));

        if ((pos & 127) == 0) {
            float sum = 0.0f;
            for (int q = 0; q < pN; q++) {
                const float d = (f0 * pIdx[q] - PURR_CENTRE) / PURR_WIDTH;
                pT[q] = 1.0f / (1.0f + d * d);
                sum  += pT[q];
            }
            if (sum > 0.0f) for (int q = 0; q < pN; q++) pT[q] /= sum;
        }

        purrPhase += f0 / SR;
        purrPhase -= floorf(purrPhase);

        float v = 0.0f;
        for (int q = 0; q < pN; q++) {
            pA[q] += 0.004f * (pT[q] - pA[q]);
            v += pA[q] * osc(purrPhase * (float)pIdx[q]);
        }

        swellPhase += 0.45f / SR;                       // slow breathing
        if (swellPhase >= 1.0f) swellPhase -= 1.0f;
        const float swell = 0.72f + 0.28f * osc(swellPhase);

        float fade = 1.0f;                             
        const uint32_t atk = msToN(380);
        if (pos < atk) fade = (float)pos / atk;
        // Rounded off and pulled back.
        return pLp.lp(v) * swell * fade * 1.25f;
    }

    default:
        return 0.0f;
    }
}

static bool voiced(Sound s, uint32_t pos)
{
    switch (s) {
    case SND_GROWL: return (pos % GROWL_CYC) < GROWL_N;
    case SND_MEOW: return (pos % MEOW_CYC) < MEOW_N;
    case SND_PURR:  return true;
    default:        return false;
    }
}

// Cycle length of the repeating sounds.
static uint32_t cycleOf(Sound s)
{
    switch (s) {
    case SND_GROWL: return GROWL_CYC;
    case SND_MEOW: return MEOW_CYC;
    default:        return 0;
    }
}

// The speaker only holds I2S0 while it is actually sounding; these hand the port back and forth with the microphone.
static bool gTxInstalled = false;
static bool txInstall();
static void txRelease();

// -------------------------------------------------------------------- task --
static void audioTask(void*)
{
    static int16_t buf[BLOCK];
    static int16_t st[BLOCK * 2];
    Sound    cur     = SND_NONE;
    uint32_t seen    = 0;
    uint32_t pos     = 0;
    bool     ampOn   = false;
    float    limGain = 1.0f;

    resetVoices();

    for (;;) {
        if (gTrigger != seen) {
            seen = gTrigger;
            cur  = gWant;
            pos  = 0;
            resetVoices();
        }

        const uint32_t cyc0 = cycleOf(cur);

        // Wake the amp 15 ms before sound arrives and drop it as soon as there  is none, gaps between repeats included. The I2S port follows the same schedule, so the microphone gets it back in every gap.
        const bool want = voiced(cur, pos + msToN(15));
        if (want != ampOn) { digitalWrite(PIN_AMP_SD, want ? HIGH : LOW); ampOn = want; }

        if (want && !gTxInstalled) txInstall();
        if (!want) {
            txRelease();
            pos += BLOCK;              
            if (cyc0 && pos >= cyc0 * 64) pos -= cyc0 * 64;
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }

        const uint32_t cyc = cyc0;

        for (int i = 0; i < BLOCK; i++) {
            const float raw = render(cur, pos) * gVolume;

            const float mag = (raw < 0.0f ? -raw : raw) * limGain;
            if (mag > 0.92f) limGain *= 0.92f / mag;
            else             limGain += (1.0f - limGain) * 0.00002f;
            if (limGain > 1.0f) limGain = 1.0f;

            buf[i] = (int16_t)(softclip(raw * limGain) * 32200.0f);

            pos++;
            if (cyc && pos >= cyc * 64) pos -= cyc * 64;   // no overflow
        }

        for (int i = 0; i < BLOCK; i++) { st[2 * i] = buf[i]; st[2 * i + 1] = buf[i]; }
        size_t wrote = 0;
        i2s_write(PORT, st, BLOCK * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    }
}

// ------------------------------------------------------------------ public --
// PDM exists only on I2S0 and the microphone wants it too, so the speaker takes  the port only while it is actually making a sound and hands it straight back.
static bool txInstall()
{
    if (gTxInstalled) return true;
    micSuspend();                       // the mic must let go first

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM);
    cfg.sample_rate          = SR;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 6;
    cfg.dma_buf_len          = 256;
    // Clock the modulator from the audio PLL.
    cfg.use_apll             = true;
    cfg.tx_desc_auto_clear   = true;

    esp_err_t err = i2s_driver_install(PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_driver_install failed: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = I2S_PIN_NO_CHANGE;
    pins.ws_io_num    = PIN_PDM_CLK;
    pins.data_out_num = PIN_PDM_DATA;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_set_pin failed: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(PORT);
        micResume();
        return false;
    }

    gTxInstalled = true;
    return true;
}

// Give I2S0 back to the microphone.
static void txRelease()
{
    if (!gTxInstalled) return;
    i2s_zero_dma_buffer(PORT);
    i2s_driver_uninstall(PORT);
    gTxInstalled = false;
    micResume();
}

bool audioBegin()
{
    buildSinTab();

    pinMode(PIN_AMP_SD, OUTPUT);
    digitalWrite(PIN_AMP_SD, LOW);      // amp asleep until there is something to play

    // The mic holds the port while idle.
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 5, NULL, 0);
    Serial.printf("[audio] PDM TX on GPIO%d, %d Hz, volume %.2f (shares I2S0 with the mic)\n",
                  PIN_PDM_DATA, SR, gVolume);
    return true;
}

void audioPlay(Sound s)
{
    gWant = s;
    gTrigger++;
}

void  audioSetVolume(float v) { gVolume = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }
float audioGetVolume()        { return gVolume; }
