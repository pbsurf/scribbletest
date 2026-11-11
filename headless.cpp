// Headless platform layer for WriteTest: implements the ugui platform API (svggui_platform.h) with no window or
//  native events; the SW framebuffer is just memory.  Events pushed by the app (incl. from other threads, e.g.
//  SvgGui timer thread, sync network thread) are queued and dispatched by platformProcessEvents() as usual.
// Unlike the real platform layers, this does not provide main() - see testmain.cpp

#include "ugui/svggui_platform.h"
#include "ulib/image.h"
#include "ulib/platformutil.h"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdlib>

struct SDL_Window { int width, height; };

static SDL_Window headlessWin = {1000, 800};
static std::vector<uint32_t> swFramebuffer;

static std::mutex eventQueueMutex;
static std::condition_variable eventQueueCond;
static std::deque<SDL_Event> eventQueue;
static std::deque< std::function<void()> > taskQueue;  // also protected by eventQueueMutex
static bool wakeRequested = false;  // also protected by eventQueueMutex
static void (*eventHandler)(SDL_Event*) = NULL;

void platformSetEventHandler(void (*fn)(SDL_Event*)) { eventHandler = fn; }

void PLATFORM_WakeEventLoop()
{
  {
    std::lock_guard<std::mutex> lock(eventQueueMutex);
    wakeRequested = true;
  }
  eventQueueCond.notify_all();
}

// dispatch without holding lock, since handler may push events or enter nested event loop
static bool dispatchQueuedEvent()
{
  SDL_Event event;
  std::function<void()> task;
  {
    std::lock_guard<std::mutex> lock(eventQueueMutex);
    if(!taskQueue.empty()) {
      task = std::move(taskQueue.front());
      taskQueue.pop_front();
    }
    else if(!eventQueue.empty()) {
      event = eventQueue.front();
      eventQueue.pop_front();
    }
    else
      return false;
  }
  if(task) { task(); }
  else if(eventHandler) { eventHandler(&event); }
  return true;
}

// reentrant: may be called from eventHandler (nested event loop)
// - with wait = true, waits at most 100 ms for an event, so that callers polling for a condition not signaled by
//  an event can't hang forever (real platforms would wait for the next mouse move, etc.)
bool platformProcessEvents(bool wait)
{
  bool any = false;
  while(dispatchQueuedEvent()) { any = true; }
  if(any || !wait) { return true; }
  {
    std::unique_lock<std::mutex> lock(eventQueueMutex);
    eventQueueCond.wait_for(lock, std::chrono::milliseconds(100),
        []{ return wakeRequested || !eventQueue.empty() || !taskQueue.empty(); });
    wakeRequested = false;
  }
  while(dispatchQueuedEvent()) {}
  return true;
}

PlatformWindow* platformInit(PlatformConfig& cfg)
{
  cfg.flags &= ~(PlatformConfig::USE_GL | PlatformConfig::SRGB);  // SW rendering only
  return &headlessWin;
}

PlatformFramebuffer platformGetSWFramebuffer()
{
  swFramebuffer.resize(size_t(headlessWin.width)*headlessWin.height);
  // same channel order as X11 (linuxmain.cpp)
  return PlatformFramebuffer{swFramebuffer.data(), headlessWin.width, headlessWin.height, 16, 8, 0, 24};
}

void platformPresent() {}
WindowPos platformGetWindowPos() { return WindowPos{0, 0, headlessWin.width, headlessWin.height, 0, 0}; }
void platformShutdown() { swFramebuffer.clear(); swFramebuffer.shrink_to_fit(); }

struct HeadlessCursor : public PlatformCursor {};
std::unique_ptr<PlatformCursor> platformCreateCursor(const Image* image, int hot_x, int hot_y)
{
  return std::make_unique<HeadlessCursor>();
}
void platformSetCursor(const PlatformCursor* cursor) {}
void platformSetFullscreen(bool enable) {}
void platformMessageBox(std::string title, std::string msg) { PLATFORM_LOG("%s: %s\n", title.c_str(), msg.c_str()); }
bool platformOpenURL(const char* url) { PLATFORM_LOG("WriteTest: not opening URL %s\n", url); return false; }
void platformKeepScreenOn(bool on) {}
void* platformGetNativeWindow() { return NULL; }
void PLATFORM_setImeText(const char* text, int selStart, int selEnd) {}

// Linux-specific (linuxmain.cpp): async request for external clipboard contents; returns 0 (and never calls
//  callback) if there is no external clipboard owner, which is always the case here
int requestClipboard(SDL_Window* sdlwin, void (*callback)(const unsigned char* buff, size_t len, int is_image))
{
  return 0;
}

void platformRunOnAppThread(std::function<void()> fn)
{
  {
    std::lock_guard<std::mutex> lock(eventQueueMutex);
    taskQueue.push_back(std::move(fn));
  }
  eventQueueCond.notify_all();
}

// SDL shims

Uint32 SDL_GetTicks()
{
  static Timestamp t0 = mSecSinceEpoch();
  return mSecSinceEpoch() - t0;
}

static std::string currClipboard;
char* SDL_GetClipboardText() { return strndup(currClipboard.data(), currClipboard.size()); }
SDL_bool SDL_HasClipboardText() { return !currClipboard.empty() ? SDL_TRUE : SDL_FALSE; }
int SDL_SetClipboardText(const char* text) { currClipboard = text; return 0; }
void SDL_free(void* mem) { free(mem); }

SDL_Keymod SDL_GetModState() { return KMOD_NONE; }

static bool textInputActive = true;
void SDL_SetTextInputRect(SDL_Rect* rect) {}
SDL_bool SDL_IsTextInputActive() { return textInputActive ? SDL_TRUE : SDL_FALSE; }
void SDL_StartTextInput() { textInputActive = true; }
void SDL_StopTextInput() { textInputActive = false; }

void SDL_RaiseWindow(SDL_Window* window) {}
void SDL_SetWindowTitle(SDL_Window* win, const char* title) {}
void SDL_GetWindowSize(SDL_Window* win, int* w, int* h)
{
  if(w) { *w = headlessWin.width; }
  if(h) { *h = headlessWin.height; }
}
void SDL_GetWindowPosition(SDL_Window* win, int* x, int* y) { if(x) { *x = 0; } if(y) { *y = 0; } }
void SDL_DestroyWindow(SDL_Window* win) {}
SDL_Window* SDL_GetWindowFromID(Uint32 id) { return NULL; }

int SDL_GetDisplayBounds(int display, SDL_Rect* rect)
{
  if(display != 0) { return -1; }
  *rect = SDL_Rect{0, 0, 1920, 1080};
  return 0;
}
int SDL_GetWindowDisplayIndex(SDL_Window* win) { return 0; }
void SDL_GL_SwapWindow(SDL_Window* win) {}

int SDL_PeepEvents(SDL_Event* events, int numevents, SDL_eventaction action, Uint32 minType, Uint32 maxType)
{
  if(action != SDL_ADDEVENT) return 0;
  {
    std::lock_guard<std::mutex> lock(eventQueueMutex);
    eventQueue.insert(eventQueue.end(), events, events + numevents);
  }
  eventQueueCond.notify_all();
  return numevents;
}

int SDL_PushEvent(SDL_Event* event)
{
  return SDL_PeepEvents(event, 1, SDL_ADDEVENT, 0, 0);
}
