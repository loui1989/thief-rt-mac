#pragma pack(push, 8)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#pragma pack(pop)

#include <lgsndi.h>
#include <sndplatform.h>

namespace {
// A periodic timer (the mixer's fades). Stopped at once: the thread used to
// sleep its whole period before it saw it was stopped, and the game waited
// for it -- up to 33 ms of the game loop each time a fade ended (the "Sound
// Client" hitches every few seconds in play).
struct Timer {
   std::mutex m;
   std::condition_variable cv;
   bool active = true;
   std::thread thread;
};
std::mutex g_mutex;
std::unordered_map<uint32, Timer *> g_timers;
std::atomic<uint32> g_nextTimer{1};
}

uint32 SndTimeMs(void)
{
   using namespace std::chrono;
   return static_cast<uint32>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32 SndStartPeriodicTimer(uint32 period, SndTimerCallback callback, void *user)
{
   Timer *timer = new Timer;
   uint32 id = g_nextTimer++;
   timer->thread = std::thread([timer, period, callback, user] {
      const auto step = std::chrono::milliseconds(std::max<uint32>(period, 1));
      auto next = std::chrono::steady_clock::now() + step;
      std::unique_lock<std::mutex> lock(timer->m);
      while (timer->active) {
         if (timer->cv.wait_until(lock, next, [timer] { return !timer->active; }))
            break;
         next += step;
         const auto now = std::chrono::steady_clock::now();
         if (next < now)
            next = now + step;   // (late: no burst of catching up)
         lock.unlock();
         callback(user);
         lock.lock();
      }
   });
   std::lock_guard<std::mutex> lock(g_mutex); g_timers[id] = timer; return id;
}

void SndEndPeriodicTimer(uint32 id)
{
   Timer *timer = nullptr;
   { std::lock_guard<std::mutex> lock(g_mutex); auto it = g_timers.find(id);
     if (it == g_timers.end()) return; timer = it->second; g_timers.erase(it); }
   {
      std::lock_guard<std::mutex> lock(timer->m);
      timer->active = false;
   }
   timer->cv.notify_all();
   if (timer->thread.joinable() && timer->thread.get_id() != std::this_thread::get_id()) {
      timer->thread.join();
      delete timer;
   } else if (timer->thread.joinable()) {
      timer->thread.detach();   // (stopped from its own callback: it ends by itself)
   } else
      delete timer;
}

void SndStopPeriodicTimer(uint32 id, uint32) { SndEndPeriodicTimer(id); }

void SndPrioritizeStreamingThread(void) {}
void cSndMixer::InitPlatformState(void) { mpDisplayDevice = nullptr; mpWinDisplayDevice = nullptr; }
void cSndMixer::ReleasePlatformState(void) { mpDisplayDevice = nullptr; mpWinDisplayDevice = nullptr; }
int cSndMixer::BlockDisplay(void) { return 0; }
void cSndMixer::ReleaseDisplay(int) {}
