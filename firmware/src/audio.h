#pragma once
#include <stdint.h>

enum Sound : uint8_t {
    SND_NONE = 0,
    SND_GROWL,     // repeats on a 2.6 s cycle while angry
    SND_MEOW,     // repeats on a 0.82 s cycle (the trill, a cat "mrrp")
    SND_PURR,      // loops continuously until something else is played
};

bool  audioBegin();
void  audioPlay(Sound s);      // retriggers even if the same sound is playing
void  audioSetVolume(float v); // 0.0 .. 1.0
float audioGetVolume();
