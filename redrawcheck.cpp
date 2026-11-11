// Redraw check - see redrawcheck.h
// Note the extra full redraw could mask bugs that depend on state changed by drawing.

#include "redrawcheck.h"
#include "application.h"
#include "ugui/svggui.h"
#include "ugui/svggui_platform.h"
#include "ulib/painter.h"
#include "ulib/fileutil.h"
#include "ulib/stringutil.h"
#include "ulib/platformutil.h"
#include "nanovg.h"
#include "nanovg_sw.h"
#include "nanovg_gl_utils.h"
#include "imagecompare.h"
#include <vector>
#include <cstring>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

// frame pixels in native format (32 bits/pixel w/ given channel shifts), top row first
struct Frame {
  std::vector<uint32_t> px;
  int width = 0, height = 0;
  int rshift = 0, gshift = 8, bshift = 16;

  bool sameSize(const Frame& f) const { return width == f.width && height == f.height; }
  Image toImage() const
  {
    Image img(width, height);
    uint32_t* out = (uint32_t*)img.pixels();
    for(size_t ii = 0; ii < px.size(); ++ii) {
      uint32_t c = px[ii];
      out[ii] = ((c >> rshift) & 0xFF) | (((c >> gshift) & 0xFF) << 8) | (((c >> bshift) & 0xFF) << 16) | 0xFF000000u;
    }
    return img;
  }
};

static std::string redrawCheckDir;
static std::string dumpPrefix;
static bool logMinorFrames = true;
static RedrawCheckStats stats;
static Frame partialFrame, fullFrame;
static Frame prevFrame;  // previous frame (partial redraw), to find pixels changed outside dirty rect
static NVGLUframebuffer* fullGLFB = NULL;  // GL: full redraw
static int frameNum = 0;
static int nDumps = 0;
static const int MAX_DUMPS = 50;

static void outlineRect(Image& img, Rect r, uint32_t color)
{
  if(img.isNull() || !r.isValid()) return;
  int l = std::max(0, int(r.left)), t = std::max(0, int(r.top));
  int rt = std::min(img.width - 1, int(r.right) - 1), b = std::min(img.height - 1, int(r.bottom) - 1);
  uint32_t* px = (uint32_t*)img.pixels();
  for(int x = l; x <= rt; ++x) { px[t*img.width + x] = color;  px[b*img.width + x] = color; }
  for(int y = t; y <= b; ++y) { px[y*img.width + l] = color;  px[y*img.width + rt] = color; }
}

static std::string fmtRect(const Rect& r)
{
  return fstring("%.0f,%.0f %.0fx%.0f", r.left, r.top, r.width(), r.height());
}

static void checkChangedOutside(const Frame& partial, const Rect& dirty)
{
  if(partial.sameSize(prevFrame)) {
    int nout = 0;
    Rect outbbox;
    for(int y = 0; y < partial.height; ++y) {
      for(int x = 0; x < partial.width; ++x) {
        size_t ii = size_t(y)*partial.width + x;
        if(partial.px[ii] != prevFrame.px[ii] && !dirty.contains(Point(x + 0.5, y + 0.5))) {
          ++nout;
          outbbox.rectUnion(Rect::ltwh(x, y, 1, 1));
        }
      }
    }
    if(nout > 0) {
      std::string msg = fstring("%sframe %d (dirty %s): %d px changed outside dirty rect, bbox %s", dumpPrefix.c_str(),
          frameNum, fmtRect(dirty).c_str(), nout, fmtRect(outbbox).c_str());
      PLATFORM_LOG("Redraw check %s\n", msg.c_str());
      ++stats.outside;
      if(stats.firstFail.empty()) stats.firstFail = msg;
    }
  }
  prevFrame = partial;
}

static void compareFrames(const Frame& partialf, const Frame& fullf, const Rect& dirty)
{
  if(partialf.sameSize(fullf) && memcmp(partialf.px.data(), fullf.px.data(), partialf.px.size()*4) == 0)
    return;
  Image partial = partialf.toImage();
  Image full = fullf.toImage();
  // partial and full redraw should be identical except for rounding (seen: +/-1 at AA edges of strokes)
  ImageCompareParams params;
  params.tol = 2;
  ImageCompareResult res = compareImages(full, partial, params);
  if(res.verdict == ImageCompareResult::PASS)
    return;  // only alpha differed
  if(res.verdict == ImageCompareResult::MINOR) {
    ++stats.minor;
    if(logMinorFrames && (stats.minor <= 10 || stats.minor % 100 == 0))
      PLATFORM_LOG("Redraw check %sframe %d (dirty %s): MINOR (%d total): %s\n", dumpPrefix.c_str(), frameNum,
          fmtRect(dirty).c_str(), stats.minor, res.summary().c_str());
    return;
  }
  ++stats.fails;
  std::string msg = fstring("%sframe %d (dirty %s): %s", dumpPrefix.c_str(), frameNum, fmtRect(dirty).c_str(),
      res.summary().c_str());
  PLATFORM_LOG("Redraw check FAIL %s\n", msg.c_str());
  if(stats.firstFail.empty()) stats.firstFail = msg;
  if(++nDumps > MAX_DUMPS)
    return;
  std::string base = redrawCheckDir + "/" + dumpPrefix + fstring("frame%05d_", frameNum);
  saveImagePNG(partial, base + "partial.png");
  saveImagePNG(full, base + "full.png");
  outlineRect(res.diffImg, dirty, 0xFFFF0000u);  // blue (RGBA bytes)
  saveImagePNG(res.diffImg, base + "diff.png");
}

// redraw entire window w/ current painter target
static void fullRedraw(int w, int h)
{
  Painter* painter = Application::painter;
  SvgGui* gui = Application::gui;
  bool prevFullRedraw = gui->fullRedraw;
  gui->fullRedraw = true;
  gui->closedWindowBounds.rectUnion(painter->deviceRect);  // force redraw even though nothing is dirty
  painter->deviceRect = Rect::wh(w, h);
  gui->layoutAndDraw(painter);
  painter->endFrame();
  gui->fullRedraw = prevFullRedraw;
}

static void setFrameFormat(Frame& f, int w, int h, int rshift, int gshift, int bshift)
{
  f.width = w;  f.height = h;
  f.rshift = rshift;  f.gshift = gshift;  f.bshift = bshift;
}

static void checkRedrawSW(const Rect& dirty)
{
  PlatformFramebuffer fb = platformGetSWFramebuffer();
  if(!fb.pixels) return;
  size_t npx = size_t(fb.width)*fb.height;
  setFrameFormat(partialFrame, fb.width, fb.height, fb.rshift, fb.gshift, fb.bshift);
  partialFrame.px.assign((const uint32_t*)fb.pixels, (const uint32_t*)fb.pixels + npx);
  checkChangedOutside(partialFrame, dirty);

  // full redraw into scratch buffer, filled w/ magenta so any pixels not drawn show up
  setFrameFormat(fullFrame, fb.width, fb.height, fb.rshift, fb.gshift, fb.bshift);
  fullFrame.px.assign(npx, (0xFFu << fb.rshift) | (0xFFu << fb.bshift) | (0xFFu << fb.ashift));
  Painter* painter = Application::painter;
  nvgswSetFramebuffer(painter->vg, fullFrame.px.data(), fb.width, fb.height, fb.rshift, fb.gshift, fb.bshift, fb.ashift);
  fullRedraw(fb.width, fb.height);
  nvgswSetFramebuffer(painter->vg, fb.pixels, fb.width, fb.height, fb.rshift, fb.gshift, fb.bshift, fb.ashift);
  compareFrames(partialFrame, fullFrame, dirty);
}

// read currently bound (read) framebuffer, flipping to top row first
static void readGLFramebuffer(Frame& f, int w, int h)
{
  static std::vector<uint32_t> buf;
  setFrameFormat(f, w, h, 0, 8, 16);  // GL_RGBA, GL_UNSIGNED_BYTE on little endian
  buf.resize(size_t(w)*h);
  f.px.resize(size_t(w)*h);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
  for(int y = 0; y < h; ++y)  // GL rows are bottom to top
    memcpy(&f.px[size_t(y)*w], &buf[size_t(h - 1 - y)*w], w*4);
}

// GL frame has been blitted from app's offscreen framebuffer to window framebuffer (currently bound); we read
//  window, draw full frame to our own offscreen framebuffer (same format as app's) and blit that to window and
//  read it, so both go through the same format conversion.  Window is overwritten, but app blits its entire
//  offscreen framebuffer to window every frame.
static void checkRedrawGL(const Rect& dirty)
{
  Painter* painter = Application::painter;
  int w = int(painter->deviceRect.width()), h = int(painter->deviceRect.height());
  if(w <= 0 || h <= 0) return;
  GLint winFBO = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &winFBO);
  readGLFramebuffer(partialFrame, w, h);
  checkChangedOutside(partialFrame, dirty);

  int fbflags = NVGLU_NO_NVG_IMAGE | Application::glFramebufferFlags();
  if(!fullGLFB)
    fullGLFB = nvgluCreateFramebuffer(painter->vg, 0, 0, fbflags);
  if(!fullGLFB) {
    PLATFORM_LOG("Redraw check: unable to create GL framebuffer\n");
    return;
  }
  glFinish();  // some drivers (VMware SVGA3D) corrupt in-flight buffers updated by endFrame() - see application.cpp
  nvgluBindFramebuffer(fullGLFB);
  nvgluSetFramebufferSize(fullGLFB, w, h, fbflags);
  nvgluSetScissor(0, 0, 0, 0);  // disable scissor (app sets it to dirty rect)
  nvgluClear(nvgRGBA(255, 0, 255, 255));  // so any pixels not drawn show up
  fullRedraw(w, h);
  glFinish();
  nvgluBlitFramebuffer(fullGLFB, winFBO);  // rebinds window framebuffer
  readGLFramebuffer(fullFrame, w, h);
  compareFrames(partialFrame, fullFrame, dirty);
}

static void checkRedraw(const Rect& dirty)
{
  ++frameNum;
  ++stats.frames;
  if(Application::glRender)
    checkRedrawGL(dirty);
  else
    checkRedrawSW(dirty);
}

void redrawCheckEnable(const std::string& outDir, const std::string& filePrefix, bool logMinor)
{
  redrawCheckDir = outDir;
  dumpPrefix = filePrefix;
  logMinorFrames = logMinor;
  createPath(redrawCheckDir);
  prevFrame = Frame();  // frames drawn while disabled weren't seen
  stats = RedrawCheckStats();
  Application::frameHook = checkRedraw;
}

void redrawCheckDisable()
{
  Application::frameHook = NULL;
  prevFrame = Frame();
}

const RedrawCheckStats& redrawCheckStats() { return stats; }
void redrawCheckResetStats() { stats = RedrawCheckStats(); }
void redrawCheckSetPrefix(const std::string& filePrefix) { dumpPrefix = filePrefix; }
