#pragma once
#include <stdint.h>

// Battery sense through a resistor divider on an ADC pin. See battery.cpp for
// the wiring and why the percentage is not a straight line.

bool  batteryBegin();

// Smoothed cell voltage. Zero until the first reading settles.
float batteryVolts();

// 0..100, from a LiPo discharge curve rather than a linear map.
int   batteryPercent();

// True while the voltage looks like a charger is holding the cell up.
bool  batteryCharging();

// Tell the battery code when something heavy is running (the amplifier). Edge
// detection is paused meanwhile: a growl sags the cell by tens of millivolts,
// which looks exactly like a charger being unplugged.
void batterySetBusy(bool busy);
