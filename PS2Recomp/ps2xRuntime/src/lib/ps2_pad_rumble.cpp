// DualShock 2 rumble on the host's controller (Linux evdev force feedback).
// After PR 11 by felipmz (github.com/GenissonEmilio/ShadowOfTheColossusPC),
// with: the device chosen on this thread from the kernel's own names (no
// raylib calls off the window thread), opened again when it goes away, long
// effects renewed, and an end to them when the game stops asking.
// Kept apart from ps2_pad.cpp: <linux/input.h> defines KEY_W, KEY_UP, ... as
// macros with evdev codes, which would replace raylib's keys of the same name.
#include "runtime/ps2_pad.h"
#include "runtime/native_tuning.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{
    // The first game controller with rumble: Steam's virtual pad ("X-Box 360
    // pad", what the game reads under Steam) before any other.
    int openRumbleDevice()
    {
        int best = -1;
        bool bestSteam = false;
        char bestName[256] = {};
        constexpr size_t kBits = 8u * sizeof(unsigned long);
        for (int i = 0; i < 64; ++i)
        {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
            const int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0)
                continue;
            unsigned long ff[FF_CNT / kBits + 1] = {}, keys[KEY_CNT / kBits + 1] = {};
            char name[256] = {};
            const bool rumble = ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0 && ((ff[FF_RUMBLE / kBits] >> (FF_RUMBLE % kBits)) & 1ul);
            const bool gamepad = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && ((keys[BTN_GAMEPAD / kBits] >> (BTN_GAMEPAD % kBits)) & 1ul);
            ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            const bool steam = std::strstr(name, "X-Box 360 pad") != nullptr;
            if (rumble && gamepad && (best < 0 || (steam && !bestSteam)))
            {
                if (best >= 0)
                    close(best);
                best = fd;
                bestSteam = steam;
                std::memcpy(bestName, name, sizeof(name));
            }
            else
                close(fd);
        }
        static char told[256] = "?";
        if (std::strcmp(told, bestName) != 0)
        {
            std::memcpy(told, bestName, sizeof(told));
            std::fprintf(stderr, best >= 0 ? "[pad] rumble on %s\n" : "[pad] no controller with rumble%s\n", bestName);
        }
        return best;
    }

    // The motors as last asked for, and the thread that plays them: the game
    // thread never waits on the controller.
    std::mutex g_mutex;
    std::condition_variable g_wake;
    bool g_small = false, g_changed = false;
    float g_large = 0.0f;
    std::chrono::steady_clock::time_point g_asked;

    void rumbleThread()
    {
        using namespace std::chrono;
        int fd = -1;
        ff_effect effect{};
        effect.id = -1;
        bool playing = false;
        uint16_t lastStrong = 0, lastWeak = 0;
        auto started = steady_clock::now(), nextOpen = started;
        for (;;)
        {
            bool small = false;
            float large = 0.0f;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_wake.wait_for(lock, playing ? milliseconds(500) : milliseconds(60000), [] { return g_changed; });
                g_changed = false;
                small = g_small;
                large = g_large;
                if (steady_clock::now() - g_asked > seconds(8)) // forgotten on
                    small = false, large = g_large = 0.0f, g_small = false;
            }
            const float strength = native_renderer::tuning().rumble;
            const uint16_t strong = static_cast<uint16_t>(std::clamp(large * strength, 0.0f, 1.0f) * 0xFFFF);
            const uint16_t weak = small ? static_cast<uint16_t>(std::clamp(0.6f * strength, 0.0f, 1.0f) * 0xFFFF) : 0;
            const auto now = steady_clock::now();
            if (!strong && !weak && !playing)
                continue;
            if (strong == lastStrong && weak == lastWeak && playing && now - started < milliseconds(1400))
                continue;
            if (fd < 0)
            {
                if (now < nextOpen)
                    continue;
                nextOpen = now + seconds(3);
                fd = openRumbleDevice();
                effect = {};
                effect.id = -1;
                playing = false;
                if (fd < 0)
                    continue;
            }
            input_event play{};
            play.type = EV_FF;
            bool ok = true;
            if (!strong && !weak)
            {
                if (effect.id >= 0)
                {
                    play.code = static_cast<uint16_t>(effect.id);
                    play.value = 0;
                    ok = write(fd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play));
                }
                playing = false;
            }
            else
            {
                effect.type = FF_RUMBLE;
                effect.u.rumble.strong_magnitude = strong;
                effect.u.rumble.weak_magnitude = weak;
                effect.replay.length = 2000; // renewed while it lasts: it ends by itself if the game does
                ok = ioctl(fd, EVIOCSFF, &effect) >= 0;
                if (ok)
                {
                    play.code = static_cast<uint16_t>(effect.id);
                    play.value = 1;
                    ok = write(fd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play));
                }
                playing = ok;
                started = now;
            }
            lastStrong = strong;
            lastWeak = weak;
            if (!ok) // the controller went away
            {
                close(fd);
                fd = -1;
                playing = false;
                lastStrong = lastWeak = 0;
            }
            // At most ~60 changes a second: the game may set the motors many times a frame.
            std::this_thread::sleep_for(milliseconds(16));
        }
    }
}
#endif

void PSPadBackend::setRumble(bool small, float large)
{
#if defined(__linux__) && !defined(__ANDROID__)
    if (native_renderer::tuning().rumble <= 0.0f)
        small = false, large = 0.0f;
    static bool started = false; // game thread
    if (!started)
    {
        if (!small && large <= 0.0f)
            return;
        started = true;
        std::thread(rumbleThread).detach();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_asked = std::chrono::steady_clock::now();
        if (g_small == small && g_large == large)
            return;
        g_small = small;
        g_large = large;
        g_changed = true;
    }
    g_wake.notify_one();
#else
    (void)small;
    (void)large;
#endif
}
