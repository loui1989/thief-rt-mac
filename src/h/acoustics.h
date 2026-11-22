// Traced sound: Steam Audio (unixsrc/sound/acoustics.cpp) works out from the level's own
// walls how each place sounds -- its reverb, from rays bounced round it, and whether each
// sound has a clear line to the listener -- and the game's sound (OpenAL Soft) plays it so.
// The game's own sound paths (through rooms and doors) and the guards' hearing are unchanged.
// Off, or without libphonon, everything is as the game had it.
#ifndef __ACOUSTICS_H
#define __ACOUSTICS_H

#ifdef __cplusplus
extern "C" {
#endif

// an EAX reverb (OpenAL Soft's EFX names), from the traced place
typedef struct sAcousticReverb
{
   float gain, gainHF, gainLF;
   float decay, decayHFRatio, decayLFRatio;
   float reflectionsGain, reflectionsDelay;
   float lateGain, lateDelay;
   float density, diffusion;
   float reflectionsPan[3], latePan[3];   // listener space: x right, y up, z ahead; length = focus
} sAcousticReverb;

// once a frame, from the sound's frame (appsfx.cpp): the listener (the world's axes, feet)
void Acoustics_Frame(const float head[3], const float ahead[3], const float up[3]);

// the reverb of where the listener is, if traced sound is on and has it (1), else 0
int Acoustics_GetReverb(sAcousticReverb *out);

// how clear sound slot `slot`'s line to the listener is, from its source at src (feet):
// 1 in plain view, toward 0 behind walls (sound reaching round corners loses its highs)
float Acoustics_SourceClear(int slot, const float src[3]);
void Acoustics_SourceStop(int slot);

// settings (sound_settings.txt, beside rt_settings.txt)
int Acoustics_Enabled(void);
void Acoustics_SetEnabled(int on);
int Acoustics_HrtfMode(void);            // 0 automatic (headphones), 1 always, 2 never
void Acoustics_SetHrtfMode(int mode);
const char *Acoustics_Status(void);      // a line for the settings panel

// (openal.cpp) applies the HRTF mode to the playing device
void OpenALApplyHrtf(int mode);
const char *OpenALHrtfStatus(void);
void OpenALCheckHeadphones(void);        // (each frame; looks every 2 s)

#ifdef __cplusplus
}
#endif

#endif
