// Battery.
//
// The board senses the cell itself -- no external divider needed. On this board GPIO6 reads 2.07 V, stable to a millivolt,
// which doubled is 4.14 V -- a LiPo held up by the charger. That is the sense
// line, on a divider that halves the cell.


#include "battery.h"
#include <Arduino.h>

static const int   PIN_BATT     = 6;        // board's own BAT sense 
static const float DIVIDER      = 2.0f;     // board divider
static const int   SAMPLES      = 16;       

// Trim this if a multimeter disagrees with the reported voltage
static const float CALIBRATION  = 1.0f;

static float gVolts = 0.0f;
static float gRaw   = 0.0f;

bool batteryBegin()
{
    analogSetPinAttenuation(PIN_BATT, ADC_11db);   // full scale, ~3.1 V

    // Report the raw reading once, before anything else claims the pin.
    uint32_t mv = 0;
    for (int i = 0; i < 32; i++) mv += analogReadMilliVolts(PIN_BATT);
    mv /= 32;
    Serial.printf("[batt] GPIO%d reads %u mV -> %.2f V at /%.0f divider\n",
                  PIN_BATT, (unsigned)mv, mv / 1000.0f * DIVIDER, DIVIDER);
    return true;
}

// Resting voltage to charge. 

static const struct { float v; int pct; } kCurve[] = {
    { 4.15f, 100 }, { 4.05f, 95 }, { 3.95f, 85 }, { 3.85f, 70 },
    { 3.80f,  60 }, { 3.75f, 50 }, { 3.70f, 40 }, { 3.65f, 30 },
    { 3.60f,  20 }, { 3.55f, 10 }, { 3.50f,  0 },
};

static int percentFor(float v)
{
    const int n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (v >= kCurve[0].v)     return 100;
    if (v <= kCurve[n - 1].v) return 0;
    for (int i = 1; i < n; i++) {
        if (v >= kCurve[i].v) {
            const float span = kCurve[i - 1].v - kCurve[i].v;
            const float f    = (v - kCurve[i].v) / span;
            return kCurve[i].pct + (int)(f * (kCurve[i - 1].pct - kCurve[i].pct));
        }
    }
    return 0;
}

float batteryVolts()
{
    uint32_t mv = 0;
    for (int i = 0; i < SAMPLES; i++) mv += analogReadMilliVolts(PIN_BATT);
    const float v = (mv / (float)SAMPLES) / 1000.0f * DIVIDER * CALIBRATION;

    // Heavy smoothing on purpose. The amplifier draws in bursts and pulls the cell down while a sound plays.
    if (gVolts <= 0.0f) gVolts = v;            
    else                gVolts += 0.02f * (v - gVolts);

 
    gRaw = v;
    return gVolts;
}

int batteryPercent()
{
    // Always sample. 
    return percentFor(batteryVolts());
}

// Charging is inferred, since the board exposes no charger status line.
//
//   falling          -> not charging, whatever the level
//   rising           -> charging, at any level
//   flat and high    -> charging (a full cell holds flat in constant voltage)
//

static const int  HIST = 4;                  // seconds of history
static float      gHist[HIST];
static int        gHistN = 0;
static uint32_t   gHistAt = 0;
static bool       gCharging = false;
static bool       gKnown = false;
static bool       gBusy = false;
static uint32_t   gBusyUntil = 0;

void batterySetBusy(bool busy)
{
    gBusy = busy;
    // Keep ignoring edges briefly after the load stops:
    if (busy) gBusyUntil = millis() + 2500;
}

// Nothing is decided for the first few seconds after power-up. 
static const uint32_t SETTLE_MS = 5000;

bool batteryCharging()
{
    const uint32_t now = millis();
    if (gRaw <= 0.0f) return false;

    static uint32_t gFirstAt = 0;
    if (gFirstAt == 0) gFirstAt = now;
    if (now - gFirstAt < SETTLE_MS) {
        gHistN  = 0;
        gHistAt = now;
        return false;
    }
    if (gBusy || (int32_t)(now - gBusyUntil) < 0) {
        gHistN  = 0;                          // discard what the load polluted
        gHistAt = now;
        return gCharging;
    }

    if (gHistAt == 0 || now - gHistAt >= 1000) {
        gHistAt = now;
        for (int i = HIST - 1; i > 0; i--) gHist[i] = gHist[i - 1];
        gHist[0] = gRaw;
        if (gHistN < HIST) gHistN++;
    }

    if (gHistN < HIST) {
        if (!gKnown) gCharging = false;
        return gCharging;
    }

    const float step = gHist[0] - gHist[HIST - 1];    // change over ~3 s
    if (step >  0.020f) { gCharging = true;  gKnown = true; }
    else if (step < -0.020f) { gCharging = false; gKnown = true; }
    else if (!gKnown)   { gCharging = false; }

    return gCharging;
}
