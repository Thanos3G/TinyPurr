// Cat eyes for the Waveshare ESP32-S3-LCD-1.3 (240x240 ST7789VW).
//
// Loops angry -> happy -> open, five seconds each, with a blink handing over
// between moods, and a sound to match. Each eye is drawn
// into its own off-screen sprite and pushed whole, so nothing flickers.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>

#include "audio.h"
#include "mic.h"
#include "battery.h"
#include <TinyML_Cat_inferencing.h>

// ---------------------------------------------------------------- hardware --
static const int      PIN_LCD_BL      = 20;  // backlight, NPN driver, active HIGH
static const uint8_t  SCREEN_ROTATION = 2;   // the 240x240 window sits at row offset 80

static TFT_eSPI    tft;
static TFT_eSprite eye(&tft);

// ------------------------------------------------------------------ layout --
static const int BOX_W = 108;                        // one eye's sprite
static const int BOX_H = 132;
static const int BOX_Y = (240 - BOX_H) / 2;
static const int BOX_L_X = 8;
static const int BOX_R_X = 240 - 8 - BOX_W;
static const int CX = BOX_W / 2;                     // eye centre inside the sprite
static const int CY = BOX_H / 2;

// ----------------------------------------------------------------- palette --
static const uint16_t BG        = TFT_BLACK;
static const uint16_t COL_ANGRY = 0xF9C6;            // red
static const uint16_t COL_HAPPY = 0x67F6;            // mint
static const uint16_t COL_OPEN  = 0x071F;            // cyan

static const uint16_t COL_BORED = 0x3A6D;            // dim slate, half asleep

enum Mood { MOOD_BORED = 0, MOOD_ANGRY, MOOD_HAPPY, MOOD_OPEN, MOOD_COUNT };

static const uint16_t moodColour[MOOD_COUNT] = { COL_BORED, COL_ANGRY, COL_HAPPY, COL_OPEN };
static const char*    moodName[MOOD_COUNT]   = { "bored", "angry", "happy", "open" };

// ------------------------------------------------------------------ timing --
// The cat sits bored and silent, listening. A recognised word triggers a  reaction
static const uint32_t FRAME_MS = 33;     // ~30 fps

// Every reaction lasts the same five seconds, whatever the mood
static const uint32_t REACT_MS = 5000;
static const uint32_t reactMs[MOOD_COUNT] = { 0, REACT_MS, REACT_MS, REACT_MS };

static Mood     gMood      = MOOD_BORED;
static uint32_t gReactEnds = 0;

// The screen goes dark after ten minutes of inactivity
static const uint32_t BACKLIGHT_MS = 10UL * 60UL * 1000UL;
static uint32_t gLastReact = 0;
static bool     gBacklightOn = true;

static void backlight(bool on)
{
    if (on == gBacklightOn) return;
    gBacklightOn = on;
    digitalWrite(PIN_LCD_BL, on ? HIGH : LOW);
}

static void react(Mood m)
{
    gLastReact = millis();
    backlight(true);

    gMood      = m;
    gReactEnds = millis() + reactMs[m];
    switch (m) {
    case MOOD_ANGRY: audioPlay(SND_GROWL); break;
    case MOOD_HAPPY: audioPlay(SND_MEOW); break;
    case MOOD_OPEN:  audioPlay(SND_PURR);  break;
    default: break;
    }
    Serial.printf("[cat-eyes] %s\n", moodName[m]);
}

// --------------------------------------------------------------- listening --
// The cat sits silent until it hears something. 
static const float    MIN_CONFIDENCE = 0.70f;
static const float    MIN_MARGIN     = 0.35f;
static const uint32_t COOLDOWN_MS    = 900;

static float *gWindow = nullptr;               // EI_CLASSIFIER_RAW_SAMPLE_COUNT

static volatile int   gHeardMood = -1;         // set by the task, consumed by loop()
static volatile float gHeardConf = 0.0f;

static int windowGetData(size_t offset, size_t length, float *out)
{
    memcpy(out, gWindow + offset, length * sizeof(float));
    return 0;
}

// Label order comes from the model, so map by name rather than by index.
static Mood moodForLabel(const char *label)
{
    if (!strcmp(label, "no"))    return MOOD_ANGRY;
    if (!strcmp(label, "yes"))   return MOOD_OPEN;
    if (!strcmp(label, "happy")) return MOOD_HAPPY;
    return MOOD_BORED;                          // background
}

static void listenTask(void *)
{
    // Three alignments of the same audio, plainly averaged
    static const float kFracs[3] = { 0.38f, 0.45f, 0.52f };

    uint32_t nextAllowed = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        if (!gWindow || !micIsRunning()) continue;
        if (gMood != MOOD_BORED) continue;               // deaf while reacting
        if ((int32_t)(millis() - nextAllowed) < 0) continue;

        float sum[EI_CLASSIFIER_LABEL_COUNT] = { 0 };
        int   votes = 0;

        for (int a = 0; a < 3; a++) {
            float at = 0.0f;
            // Refuses until the word can actually be centred, so a word that
            // was only just spoken waits for its tail 
            if (!micSnapshotAligned(gWindow, EI_CLASSIFIER_RAW_SAMPLE_COUNT,
                                    kFracs[a], &at)) continue;

            signal_t signal;
            signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
            signal.get_data     = &windowGetData;

            ei_impulse_result_t r = { 0 };
            if (run_classifier(&signal, &r, false) != EI_IMPULSE_OK) continue;

            for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                sum[i] += r.classification[i].value;
            }
            votes++;
        }

        if (votes == 0) continue;                        // nothing to align to

        for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) sum[i] /= (float)votes;

        int best = 0, second = -1;
        for (uint16_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            if (sum[i] > sum[best]) best = i;
        }
        for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            if ((int)i != best && (second < 0 || sum[i] > sum[second])) second = (int)i;
        }
        const float margin = sum[best] - sum[second];
        const char *label  = ei_classifier_inferencing_categories[best];

        Serial.printf("[heard] %-10s %.2f  margin %.2f  (%d votes)", label,
                      sum[best], margin, votes);
        for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            Serial.printf("  %s=%.2f", ei_classifier_inferencing_categories[i], sum[i]);
        }

        const Mood m = moodForLabel(label);
        if (m != MOOD_BORED && sum[best] >= MIN_CONFIDENCE && margin >= MIN_MARGIN) {
            gHeardConf = sum[best];
            gHeardMood = (int)m;                 // loop() reacts, on its own core
            Serial.println("  ->");
        } else {
            Serial.println("  (ignored)");
        }
        nextAllowed = millis() + COOLDOWN_MS;
    }
}

// ------------------------------------------------------------------ power --
// A battery glyph above the eyes. 

static const int BATT_W = 34, BATT_H = 15;
static const int BATT_X = 240 - BATT_W - 12, BATT_Y = 10;

static void drawBolt(int cx, int cy, uint16_t col)
{
    tft.fillTriangle(cx + 1, cy - 6, cx - 4, cy + 1, cx + 1, cy + 1, col);
    tft.fillTriangle(cx - 1, cy + 6, cx + 4, cy - 1, cx - 1, cy - 1, col);
}

static void drawBattery()
{
    batteryVolts();                  // sample first, the rest reads what it set
    const int pct = batteryPercent();

    // Green while there is plenty, amber past two thirds used, red near empty.
    uint16_t col = 0x3606;                                   // green
    if (pct <= 15)      col = 0xF986;                        // red
    else if (pct <= 35) col = 0xFD20;                        // amber

    tft.drawRoundRect(BATT_X, BATT_Y, BATT_W, BATT_H, 2, 0x630C);
    tft.fillRect(BATT_X + BATT_W + 1, BATT_Y + 4, 3, 7, 0x630C);   // the nub

    const int inner = BATT_W - 4;
    const int fill  = (inner * pct) / 100;
    tft.fillRect(BATT_X + 2, BATT_Y + 2, inner, BATT_H - 4, TFT_BLACK);
    if (fill > 0) tft.fillRect(BATT_X + 2, BATT_Y + 2, fill, BATT_H - 4, col);

    if (batteryCharging()) {
        drawBolt(BATT_X + BATT_W / 2, BATT_Y + BATT_H / 2, TFT_WHITE);
    }
}


// ---------------------------------------------------------------- drawing ---

// A four-point twinkle, built from two crossed lenses.
static void sparkle(int x, int y, int r, uint16_t c)
{
    const int w = r / 3 + 1;
    eye.fillTriangle(x, y - r, x - w, y, x + w, y, c);
    eye.fillTriangle(x, y + r, x - w, y, x + w, y, c);
    eye.fillTriangle(x - r, y, x, y - w, x, y + w, c);
    eye.fillTriangle(x + r, y, x, y - w, x, y + w, c);
}

// lid: 1.0 fully open, 0.0 fully shut.  gx/gy: where the pupil is looking.
static void drawEye(bool left, Mood mood, float lid, float gx, float gy, float anim)
{
    const uint16_t col  = moodColour[mood];
    const uint16_t glow = tft.alphaBlend(70, col, BG);

    eye.fillSprite(BG);

    if (mood == MOOD_BORED) {
        // Half shut and looking down
        const int w = 74, h = 104;
        const int x = CX - w / 2;
        const int y = CY - h / 2;
        eye.fillSmoothRoundRect(x - 3, y - 3, w + 6, h + 6, (w + 6) / 2, glow, BG);
        eye.fillSmoothRoundRect(x, y, w, h, w / 2, col, glow);

        const int px = CX + (int)(gx * 0.35f);
        const int py = CY + 14 + (int)(gy * 0.5f);
        eye.fillSmoothRoundRect(px - 8, py - 30, 16, 60, 8, BG, col);

        // The lid
        eye.fillRect(0, 0, BOX_W, CY - 6, BG);
        eye.fillSmoothRoundRect(x - 6, CY - 34, w + 12, 30, 12, BG, col);
    } else if (mood == MOOD_HAPPY) {
        // Closed, curving upwards
        eye.drawSmoothArc(CX, CY + 30, 57, 40, 117, 243, glow, BG,   true);
        eye.drawSmoothArc(CX, CY + 30, 52, 38, 120, 240, col,  glow, true);

        // Twinkles, off the outer corner of each eye, breathing out of phase.
        const uint16_t spk = tft.alphaBlend(150, TFT_WHITE, col);
        const float    ph  = anim * 2.6f + (left ? 0.0f : 1.7f);
        const int      sx  = left ? 17 : BOX_W - 17;
        const int      sx2 = left ? 38 : BOX_W - 38;
        sparkle(sx,  24, 7 + (int)(2.5f * (1.0f + sinf(ph))),        spk);
        sparkle(sx2,  9, 3 + (int)(1.5f * (1.0f + sinf(ph + 2.1f))), spk);
    } else {
        const int w = (mood == MOOD_ANGRY) ? 76 : 74;
        const int h = (mood == MOOD_ANGRY) ? 96 : 104;
        const int x = CX - w / 2;
        const int y = CY - h / 2;

        eye.fillSmoothRoundRect(x - 4, y - 4, w + 8, h + 8, (w + 8) / 2, glow, BG);
        eye.fillSmoothRoundRect(x, y, w, h, w / 2, col, glow);

        // Vertical slit pupil, narrowed when angry.
        const int pw = (mood == MOOD_ANGRY) ? 10 : 16;
        const int ph = h - 26;
        const int px = CX + (int)gx;
        const int py = CY + (int)gy;
        eye.fillSmoothRoundRect(px - pw / 2, py - ph / 2, pw, ph, pw / 2, BG, col);
        eye.fillSmoothCircle(px + pw / 2 + 7, py - ph / 4, 5,
                             tft.alphaBlend(210, TFT_WHITE, col), col);

        if (mood == MOOD_ANGRY) {
            // Brow slanting down towards the nose
            const int top = y - 12;
            if (left) eye.fillTriangle(0, top, BOX_W, top, BOX_W, top + 72, BG);
            else      eye.fillTriangle(0, top, BOX_W, top, 0,     top + 72, BG);
        }
    }

    if (lid < 1.0f) {
        const int half = (int)(CY * lid);
        eye.fillRect(0, 0, BOX_W, CY - half, BG);
        eye.fillRect(0, CY + half, BOX_W, BOX_H - (CY + half), BG);
    }

    eye.pushSprite(left ? BOX_L_X : BOX_R_X, BOX_Y);
}

// ------------------------------------------------------------------ setup ---
void setup()
{
    Serial.begin(115200);

    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);      

    tft.init();
    tft.setRotation(SCREEN_ROTATION);
    tft.fillScreen(BG);

    // PSRAM is on for the model. TFT_eSPI would otherwise put the sprite there
    eye.setAttribute(PSRAM_ENABLE, false);
    eye.setColorDepth(16);
    if (eye.createSprite(BOX_W, BOX_H) == nullptr) {
        Serial.println("[cat-eyes] sprite allocation failed");
        tft.setTextColor(TFT_RED, BG);
        tft.drawString("sprite alloc failed", 10, 110);
    }

    drawEye(true,  MOOD_BORED, 1.0f, 0, 0, 0);
    drawEye(false, MOOD_BORED, 1.0f, 0, 0, 0);

    digitalWrite(PIN_LCD_BL, HIGH);
    gLastReact = millis();          // the ten minutes start from boot
    Serial.println("[cat-eyes] bored and listening");

    // Both want I2S0 and only one PDM port exists, so they take turns
    batteryBegin();
    micBegin();
    audioBegin();

    gWindow = (float *)heap_caps_malloc(EI_CLASSIFIER_RAW_SAMPLE_COUNT * sizeof(float),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gWindow) {
        Serial.println("[cat-eyes] window alloc failed; not listening");
    } else {
        // Core 0, alongside the mic and audio tasks
        xTaskCreatePinnedToCore(listenTask, "listen", 8192, NULL, 3, NULL, 0);
        Serial.printf("[cat-eyes] listening: %d labels, confidence %.2f, margin %.2f\n",
                      EI_CLASSIFIER_LABEL_COUNT, MIN_CONFIDENCE, MIN_MARGIN);
    }

}

// ------------------------------------------------------------------- loop ---
void loop()
{
    static uint32_t t0 = millis();

    const uint32_t frameStart = millis();
    const uint32_t elapsed    = frameStart - t0;

    // A reaction times out back to bored.
    if (gMood != MOOD_BORED && (int32_t)(frameStart - gReactEnds) >= 0) {
        gMood = MOOD_BORED;
        audioPlay(SND_NONE);
     
        micFlush();
        Serial.println("[cat-eyes] bored");
    }

    // The listener runs on the other core
    if (gHeardMood >= 0) {
        const Mood m = (Mood)gHeardMood;
        gHeardMood = -1;
        Serial.printf("[cat-eyes] reacting to %s (%.2f)\n", moodName[m], gHeardConf);
        react(m);
    }

    // Bored blinks slowly every few seconds; a reaction opens the eyes fully.
    float lid = 1.0f;
    if (gMood == MOOD_BORED) {
        const uint32_t cyc = elapsed % 5200;
        if (cyc < 260) {
            const float h = 130.0f;
            lid = (cyc < 130) ? (1.0f - cyc / h) : ((cyc - 130) / h);
        }
    }

    const float gx   = 8.0f * sinf(elapsed * 0.0011f);
    const float gy   = 3.0f * sinf(elapsed * 0.0007f);
    const float anim = elapsed * 0.001f;

    drawEye(true,  gMood, lid, gx, gy, anim);
    drawEye(false, gMood, lid, gx, gy, anim);

    if (gBacklightOn && millis() - gLastReact >= BACKLIGHT_MS) backlight(false);

    // Once a second is plenty: the reading is heavily smoothed.
    static uint32_t battAt = 0;
    if (millis() - battAt >= 1000) {
        battAt = millis();
        batterySetBusy(gMood != MOOD_BORED);
        drawBattery();

    }

    const uint32_t spent = millis() - frameStart;
    if (spent < FRAME_MS) delay(FRAME_MS - spent);
}
