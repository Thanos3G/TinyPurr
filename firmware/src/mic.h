#pragma once
#include <stdint.h>
#include <stddef.h>


bool  micBegin();


void  micSuspend();
void  micResume();
bool  micIsRunning();

// Most recent decoded block: peak and RMS, both 0.0 .. 1.0. 
float micPeak();
float micRms();

// Raw diagnostics, for checking the capture is alive at all.
uint32_t micBitsPerSecond();
uint32_t micSamplesDecoded();

// 1 = every captured bit, 2 = even bits, 3 = odd bits. 
void  micSetStride(int s);
int   micGetStride();

// Makeup gain. 
void  micSetGain(float g);
float micGetGain();

// ---------------------------------------------------------------- capture ---

bool micSnapshot(float *out, size_t n);

// Short-window level, ~30 ms, 0.0 .. 1.0. 
float micLevel();

// Samples captured since boot
uint32_t micTotalSamples();

// Raw I2S sample range over the last reporting window, for setting the scale.
int32_t micRawMin();
int32_t micRawMax();


void micSetSignExtend(bool on);
bool micGetSignExtend();
void micSetDcBlock(bool on);
bool micGetDcBlock();

// Newest n samples, cut so the speech energy centroid lands at centroidFrac of the window (0..1). The training clips all sit at 0.45, so that is the value to pass. 
bool micSnapshotAligned(float *out, size_t n, float centroidFrac, float *foundAt);

// Absolute energy a window must reach before it is considered speech at all.
void  micSetMinEnergy(float e);
float micGetMinEnergy();

// Discard everything captured so far. 
void micFlush();
