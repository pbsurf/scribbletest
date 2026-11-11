// Headless platform layer for WriteTest: implements the ugui platform API (svggui_platform.h) with no window or
//  native events; the SW framebuffer is just memory.  Events pushed by the app (incl. from other threads, e.g.
//  SvgGui timer thread, sync network thread) are queued and dispatched by platformProcessEvents() as usual.
// Unlike the real platform layers, this does not provide main() - see testmain.cpp
// GL (optional, see headlessSetGL()): surfaceless EGL context (Mesa), w/ an offscreen framebuffer object bound in
//  place of the window's default framebuffer, which doesn't exist; app's GL calls go through libglvnd as usual

#include "headless.h"
#include "ugui/svggui_platform.h"
#include "ulib/image.h"
#include "ulib/platformutil.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
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

// GL
static bool wantGL = false;
static EGLDisplay eglDpy = EGL_NO_DISPLAY;
static EGLContext eglCtx = EGL_NO_CONTEXT;
static GLuint winFBO = 0, winRB = 0;
static std::string glRenderer;

void headlessSetGL(bool enable) { wantGL = enable; }
bool headlessUsingGL() { return eglCtx != EGL_NO_CONTEXT; }
const char* headlessGLRenderer() { return glRenderer.c_str(); }

static void shutdownGL()
{
  if(eglCtx != EGL_NO_CONTEXT) {
    if(winFBO) { glDeleteFramebuffers(1, &winFBO); winFBO = 0; }
    if(winRB) { glDeleteRenderbuffers(1, &winRB); winRB = 0; }
    eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(eglDpy, eglCtx);
    eglCtx = EGL_NO_CONTEXT;
  }
  if(eglDpy != EGL_NO_DISPLAY) {
    eglTerminate(eglDpy);
    eglDpy = EGL_NO_DISPLAY;
  }
}

static bool initGL()
{
  // surfaceless platform (EGL_MESA_platform_surfaceless) needs no window system or GPU device
  eglDpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if(eglDpy == EGL_NO_DISPLAY || !eglInitialize(eglDpy, NULL, NULL)) {
    PLATFORM_LOG("WriteTest: unable to initialize EGL surfaceless display\n");
    eglDpy = EGL_NO_DISPLAY;
    return false;
  }
  if(!eglBindAPI(EGL_OPENGL_API)) {
    PLATFORM_LOG("WriteTest: eglBindAPI(EGL_OPENGL_API) failed\n");
    shutdownGL();
    return false;
  }
  // same as linuxmain.cpp: GL 3.3 (core profile is default)
  const EGLint ctxattribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
  // EGL_KHR_no_config_context and EGL_KHR_surfaceless_context
  eglCtx = eglCreateContext(eglDpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxattribs);
  if(eglCtx == EGL_NO_CONTEXT || !eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, eglCtx)) {
    PLATFORM_LOG("WriteTest: unable to create EGL context (error 0x%x)\n", eglGetError());
    if(eglCtx != EGL_NO_CONTEXT) { eglDestroyContext(eglDpy, eglCtx); eglCtx = EGL_NO_CONTEXT; }
    shutdownGL();
    return false;
  }
  // no default framebuffer, so create one to stand in for the window; app (nvglu) binds its own FBO for drawing,
  //  then blits to and rebinds previously bound FBO, i.e. this one
  glGenRenderbuffers(1, &winRB);
  glBindRenderbuffer(GL_RENDERBUFFER, winRB);
  // sRGB, like typical window framebuffers (e.g. Mesa GLX), so that blit from app's sRGB framebuffer w/
  //  GL_FRAMEBUFFER_SRGB enabled doesn't convert to linear; no effect if GL_FRAMEBUFFER_SRGB is disabled
  glRenderbufferStorage(GL_RENDERBUFFER, GL_SRGB8_ALPHA8, headlessWin.width, headlessWin.height);
  glGenFramebuffers(1, &winFBO);
  glBindFramebuffer(GL_FRAMEBUFFER, winFBO);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, winRB);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    PLATFORM_LOG("WriteTest: window framebuffer incomplete\n");
    shutdownGL();
    return false;
  }
  glViewport(0, 0, headlessWin.width, headlessWin.height);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  const char* renderer = (const char*)glGetString(GL_RENDERER);
  const char* version = (const char*)glGetString(GL_VERSION);
  glRenderer = std::string(renderer ? renderer : "?") + " (" + (version ? version : "?") + ")";
  return true;
}

PlatformWindow* platformInit(PlatformConfig& cfg)
{
  if(!(wantGL && (cfg.flags & PlatformConfig::USE_GL) && initGL()))
    cfg.flags &= ~PlatformConfig::USE_GL;  // SW rendering
  // SRGB flag left as is, as in linuxmain.cpp (only relevant for GLES; w/ SW, app's sRGB pref enables linear
  //  blending in SW renderer)
  return &headlessWin;
}

Image headlessReadWindow()
{
  int w = headlessWin.width, h = headlessWin.height;
  Image img(w, h);
  uint32_t* out = (uint32_t*)img.pixels();
  if(headlessUsingGL()) {
    std::vector<uint32_t> px(size_t(w)*h);
    GLint prev = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, winFBO);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev);
    for(int y = 0; y < h; ++y)  // GL rows are bottom to top
      memcpy(out + size_t(y)*w, px.data() + size_t(h - 1 - y)*w, w*4);
  }
  else {
    const PlatformFramebuffer fb = platformGetSWFramebuffer();
    const uint32_t* px = (const uint32_t*)fb.pixels;
    for(size_t ii = 0; ii < size_t(w)*h; ++ii) {
      uint32_t c = px[ii];
      out[ii] = ((c >> fb.rshift) & 0xFF) | (((c >> fb.gshift) & 0xFF) << 8) | (((c >> fb.bshift) & 0xFF) << 16) | 0xFF000000u;
    }
  }
  return img;
}

PlatformFramebuffer platformGetSWFramebuffer()
{
  swFramebuffer.resize(size_t(headlessWin.width)*headlessWin.height);
  // same channel order as X11 (linuxmain.cpp)
  return PlatformFramebuffer{swFramebuffer.data(), headlessWin.width, headlessWin.height, 16, 8, 0, 24};
}

void platformPresent() { if(headlessUsingGL()) glFlush(); }
WindowPos platformGetWindowPos() { return WindowPos{0, 0, headlessWin.width, headlessWin.height, 0, 0}; }
void platformShutdown() { shutdownGL(); swFramebuffer.clear(); swFramebuffer.shrink_to_fit(); }

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
