#include <openal_api.h>

#pragma pack(push, 8)
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#pragma pack(pop)

static void *g_openal;

#if defined(__APPLE__)
#pragma pack(push, 8)
#include <stdint.h>
#include <stdio.h>
#pragma pack(pop)
// The sound card's period for this game. OpenAL Soft mixes in CoreAudio's
// own callback, as many frames as the output asks for each time: 512 at
// 48 kHz by default, so every 10.7 ms it had to have mixed the next piece,
// and any time the system was late with it (a busy GPU, the game's threads,
// Game Mode) was a gap in the sound -- sound dropping out in the menus and
// in play while the game itself ran smoothly. A longer period gives it room.
// THIEF_AUDIO_FRAMES: frames a period (default 2048; 0: as the system has it).
namespace {
struct sCAAddress { uint32_t selector, scope, element; };
typedef int32_t (*tCAGet)(uint32_t, const sCAAddress *, uint32_t, const void *, uint32_t *, void *);
typedef int32_t (*tCASet)(uint32_t, const sCAAddress *, uint32_t, const void *, uint32_t, const void *);
constexpr uint32_t FourCC(const char *s)
{
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}
void SetOutputPeriod()
{
    static void *ca = dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio", RTLD_NOW | RTLD_LOCAL);
    if (!ca)
        return;
    tCAGet get = reinterpret_cast<tCAGet>(dlsym(ca, "AudioObjectGetPropertyData"));
    tCASet set = reinterpret_cast<tCASet>(dlsym(ca, "AudioObjectSetPropertyData"));
    if (!get || !set)
        return;
    const uint32_t glob = FourCC("glob"), out = glob;   // (the period is the device's, as JUCE sets it)
    uint32_t device = 0, size = sizeof(device);
    sCAAddress a{FourCC("dOut"), glob, 0};
    if (get(1 /* the system */, &a, 0, nullptr, &size, &device) != 0 || !device)
        return;
    char name[256] = "?";
    size = sizeof(name) - 1;
    a = {FourCC("name"), glob, 0};
    if (get(device, &a, 0, nullptr, &size, name) != 0)
        strcpy(name, "?");
    uint32_t transport = 0;
    size = sizeof(transport);
    a = {FourCC("tran"), glob, 0};
    get(device, &a, 0, nullptr, &size, &transport);
    double rate = 0;
    size = sizeof(rate);
    a = {FourCC("nsrt"), glob, 0};
    get(device, &a, 0, nullptr, &size, &rate);
    uint32_t frames = 0;
    size = sizeof(frames);
    a = {FourCC("fsiz"), out, 0};
    get(device, &a, 0, nullptr, &size, &frames);
    struct { double lo, hi; } range{0, 0};
    size = sizeof(range);
    a = {FourCC("fsz#"), out, 0};
    get(device, &a, 0, nullptr, &size, &range);
    long want = 2048;
    if (const char *e = getenv("THIEF_AUDIO_FRAMES"))
        want = strtol(e, nullptr, 10);
    uint32_t now = frames;
    if (want > 0) {
        uint32_t f = uint32_t(want);
        if (range.hi > 0 && f > range.hi) f = uint32_t(range.hi);
        if (range.lo > 0 && f < range.lo) f = uint32_t(range.lo);
        a = {FourCC("fsiz"), out, 0};
        set(device, &a, 0, nullptr, sizeof(f), &f);
        size = sizeof(now);
        get(device, &a, 0, nullptr, &size, &now);
    }
    char tr[5] = {char(transport >> 24), char(transport >> 16), char(transport >> 8), char(transport), 0};
    for (char &c : tr)
        if (c && (c < 32 || c > 126)) c = '?';
    fprintf(stderr, "CoreAudio: output \"%s\" (%s), %.0f Hz, period %u frames -> %u (allowed %.0f-%.0f)\n",
            name, transport ? tr : "?", rate, frames, now, range.lo, range.hi);
}

bool NameHas(const char *name, const char *const *words)
{
    char low[256];
    size_t i = 0;
    for (; name[i] && i < sizeof(low) - 1; ++i)
        low[i] = (name[i] >= 'A' && name[i] <= 'Z') ? char(name[i] - 'A' + 'a') : name[i];
    low[i] = 0;
    for (; *words; ++words)
        if (strstr(low, *words))
            return true;
    return false;
}
}

// Headphones: the Mac's headphone socket in use (its data source 'hdpn'), a
// Bluetooth output that isn't a speaker by its name (AirPods, Beats, most
// Bluetooth audio on a Mac), or a USB one that says it is.
int OpenALOutputHeadphones(char *outName, int len)
{
    static void *ca = dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio", RTLD_NOW | RTLD_LOCAL);
    if (outName && len > 0)
        outName[0] = 0;
    if (!ca)
        return -1;
    tCAGet get = reinterpret_cast<tCAGet>(dlsym(ca, "AudioObjectGetPropertyData"));
    if (!get)
        return -1;
    const uint32_t glob = FourCC("glob");
    uint32_t device = 0, size = sizeof(device);
    sCAAddress a{FourCC("dOut"), glob, 0};
    if (get(1, &a, 0, nullptr, &size, &device) != 0 || !device)
        return -1;
    char name[256] = "";
    size = sizeof(name) - 1;
    a = {FourCC("name"), glob, 0};
    if (get(device, &a, 0, nullptr, &size, name) != 0)
        name[0] = 0;
    if (outName && len > 0) {
        strncpy(outName, name, size_t(len) - 1);
        outName[len - 1] = 0;
    }
    uint32_t transport = 0;
    size = sizeof(transport);
    a = {FourCC("tran"), glob, 0};
    get(device, &a, 0, nullptr, &size, &transport);
    static const char *const kPhones[] = {"airpods", "beats", "buds", "headphone", "headset", "earphone",
                                          "earbud", "wh-", "wf-", "quietcomfort", "qc", nullptr};
    static const char *const kSpeakers[] = {"speaker", "homepod", "soundbar", "sonos", "boom", "flip",
                                            "charge", "soundlink", "tv", "display", "receiver", nullptr};
    if (transport == FourCC("blue") || transport == FourCC("blea"))
        return NameHas(name, kSpeakers) && !NameHas(name, kPhones) ? 0 : 1;
    if (transport == FourCC("bltn")) {
        uint32_t source = 0;
        size = sizeof(source);
        a = {FourCC("ssrc"), FourCC("outp"), 0};
        if (get(device, &a, 0, nullptr, &size, &source) == 0)
            return source == FourCC("hdpn") ? 1 : 0;
        return NameHas(name, kPhones) ? 1 : 0;
    }
    return NameHas(name, kPhones) ? 1 : 0;
}
#else
int OpenALOutputHeadphones(char *name, int len)
{
    if (name && len > 0)
        name[0] = 0;
    return -1;
}
#endif

template <class T>
static bool LoadProc(T &target, const char *name)
{
    target = reinterpret_cast<T>(dlsym(g_openal, name));
    return target != nullptr;
}

bool OpenALLoad(sOpenALApi *api)
{
    if (!api)
        return false;
    memset(api, 0, sizeof(*api));
    // WSLg exports audio through PulseAudio. Its ALSA probe can only emit
    // misleading no-card diagnostics after a Pulse failure.
    if (getenv("WSL2_GUI_APPS_ENABLED") && !getenv("ALSOFT_DRIVERS"))
        setenv("ALSOFT_DRIVERS", "pulse", 0);
    if (!g_openal)
    {
#if defined(__ANDROID__)
        // OpenAL Soft is built and packaged with the APK. Android shared
        // libraries are unversioned, unlike the usual Linux SONAME.
        g_openal = dlopen("libopenal.so", RTLD_NOW | RTLD_LOCAL);
#endif
#if defined(__APPLE__)
        SetOutputPeriod();
        // Prefer OpenAL Soft (EFX reverb and occlusion): the copy bundled in
        // the app, then Homebrew's keg-only install. Apple's deprecated
        // OpenAL.framework works as a last resort, without EFX.
        static const char *const kMacOpenAL[] = {
            "@executable_path/../Frameworks/libopenal.1.dylib",
            "@executable_path/libopenal.1.dylib",
            "/opt/homebrew/opt/openal-soft/lib/libopenal.1.dylib",
            "/usr/local/opt/openal-soft/lib/libopenal.1.dylib",
            "libopenal.1.dylib",
            "/System/Library/Frameworks/OpenAL.framework/OpenAL",
        };
        for (const char *name : kMacOpenAL)
            if (!g_openal)
                g_openal = dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
        if (!g_openal)
            g_openal = dlopen("libopenal.so.1", RTLD_NOW | RTLD_LOCAL);
    }
    if (!g_openal)
        return false;
#define LOAD(name) if (!LoadProc(api->name, #name)) { OpenALUnload(); return false; }
    LOAD(alcOpenDevice) LOAD(alcCloseDevice) LOAD(alcCreateContext)
    LOAD(alcMakeContextCurrent) LOAD(alcProcessContext) LOAD(alcSuspendContext)
    LOAD(alcDestroyContext) LOAD(alcIsExtensionPresent) LOAD(alcGetProcAddress)
    LOAD(alGetError) LOAD(alGetProcAddress) LOAD(alGenSources)
    LOAD(alDeleteSources) LOAD(alSourcePlay) LOAD(alSourcePause)
    LOAD(alSourceStop) LOAD(alSourcei) LOAD(alSourcef) LOAD(alSource3f)
    LOAD(alSource3i) LOAD(alGetSourcei) LOAD(alGetSourcef) LOAD(alGenBuffers)
    LOAD(alDeleteBuffers) LOAD(alBufferData) LOAD(alSourceQueueBuffers)
    LOAD(alSourceUnqueueBuffers) LOAD(alListener3f) LOAD(alListenerfv)
    LOAD(alDopplerFactor) LOAD(alSpeedOfSound) LOAD(alDistanceModel)
#undef LOAD
    return true;
}

void OpenALUnload()
{
    //Causes crash because audio threads still active so we leave open for process lifetime
    // if (g_openal)
    //     dlclose(g_openal);
    // g_openal = nullptr;
}
