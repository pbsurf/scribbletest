// Redraw check - debugging aid for partial redraw with SW renderer
// After each SW frame, the whole window is redrawn into a scratch buffer and compared to the framebuffer
//  produced by the normal (partial) redraw, and pixels changed outside the frame's dirty rect are reported.
//  Failures are logged and frameN_{partial,full,diff}.png written (dirty rect outlined in blue in diff image).
//  If the framebuffer is correct but the window isn't, the problem is in presentation (platform layer).
// Linked into WriteRedrawCheck (`make DEBUG=1 redrawcheck`), which is Write w/ this file; requires SW renderer
//  (glRender = 0).  Output dir is $WRITE_REDRAW_CHECK, or ./redrawcheck if not set.
// Note the extra full redraw could mask bugs that depend on state changed by drawing.

#include "application.h"
#include "ugui/svggui.h"
#include "ugui/svggui_platform.h"
#include "ulib/painter.h"
#include "ulib/fileutil.h"
#include "ulib/stringutil.h"
#include "ulib/platformutil.h"
#include "nanovg.h"
#include "nanovg_sw.h"
#include "imagecompare.h"
#include <vector>
#include <cstring>

static std::string redrawCheckDir;
static std::vector<uint32_t> fullFB;  // full redraw
static std::vector<uint32_t> prevFB;  // previous frame, to find pixels changed outside dirty rect
static int frameNum = 0, nFails = 0, nMinor = 0;
static const int MAX_DUMPS = 50;

static Image fbToImage(const uint32_t* px, const PlatformFramebuffer& fb)
{
  Image img(fb.width, fb.height);
  uint32_t* out = (uint32_t*)img.pixels();
  for(size_t ii = 0; ii < size_t(fb.width)*fb.height; ++ii) {
    uint32_t c = px[ii];
    out[ii] = ((c >> fb.rshift) & 0xFF) | (((c >> fb.gshift) & 0xFF) << 8) | (((c >> fb.bshift) & 0xFF) << 16) | 0xFF000000u;
  }
  return img;
}

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

static void checkChangedOutside(const PlatformFramebuffer& fb, const Rect& dirty)
{
  size_t npx = size_t(fb.width)*fb.height;
  const uint32_t* fbpx = (const uint32_t*)fb.pixels;
  if(prevFB.size() == npx) {
    int nout = 0;
    Rect outbbox;
    for(int y = 0; y < fb.height; ++y) {
      for(int x = 0; x < fb.width; ++x) {
        size_t ii = size_t(y)*fb.width + x;
        if(fbpx[ii] != prevFB[ii] && !dirty.contains(Point(x + 0.5, y + 0.5))) {
          ++nout;
          outbbox.rectUnion(Rect::ltwh(x, y, 1, 1));
        }
      }
    }
    if(nout > 0)
      PLATFORM_LOG("Redraw check frame %d (dirty %s): %d px changed outside dirty rect, bbox %s\n",
          frameNum, fmtRect(dirty).c_str(), nout, fmtRect(outbbox).c_str());
  }
  prevFB.assign(fbpx, fbpx + npx);
}

static void checkRedraw(const PlatformFramebuffer& fb, const Rect& dirty)
{
  if(!fb.pixels) return;
  ++frameNum;
  checkChangedOutside(fb, dirty);

  // full redraw into scratch buffer, filled w/ magenta so any pixels not drawn show up
  size_t npx = size_t(fb.width)*fb.height;
  uint32_t sentinel = (0xFFu << fb.rshift) | (0xFFu << fb.bshift) | (0xFFu << fb.ashift);
  fullFB.assign(npx, sentinel);
  Painter* painter = Application::painter;
  SvgGui* gui = Application::gui;
  nvgswSetFramebuffer(painter->vg, fullFB.data(), fb.width, fb.height, fb.rshift, fb.gshift, fb.bshift, fb.ashift);
  bool prevFullRedraw = gui->fullRedraw;
  gui->fullRedraw = true;
  gui->closedWindowBounds.rectUnion(painter->deviceRect);  // force redraw even though nothing is dirty
  painter->deviceRect = Rect::wh(fb.width, fb.height);
  gui->layoutAndDraw(painter);
  painter->endFrame();
  gui->fullRedraw = prevFullRedraw;
  nvgswSetFramebuffer(painter->vg, fb.pixels, fb.width, fb.height, fb.rshift, fb.gshift, fb.bshift, fb.ashift);

  if(memcmp(fullFB.data(), fb.pixels, npx*4) == 0)
    return;
  Image full = fbToImage(fullFB.data(), fb);
  Image partial = fbToImage((const uint32_t*)fb.pixels, fb);
  // partial and full redraw should be identical except for rounding (seen: +/-1 at AA edges of strokes)
  ImageCompareParams params;
  params.tol = 2;
  ImageCompareResult res = compareImages(full, partial, params);
  if(res.verdict == ImageCompareResult::PASS)
    return;  // only alpha differed
  if(res.verdict == ImageCompareResult::MINOR) {
    if(++nMinor <= 10 || nMinor % 100 == 0)
      PLATFORM_LOG("Redraw check frame %d (dirty %s): MINOR (%d total): %s\n", frameNum,
          fmtRect(dirty).c_str(), nMinor, res.summary().c_str());
    return;
  }
  ++nFails;
  PLATFORM_LOG("Redraw check frame %d (dirty %s): FAIL (%d total): %s\n", frameNum,
      fmtRect(dirty).c_str(), nFails, res.summary().c_str());
  if(nFails > MAX_DUMPS)
    return;
  std::string base = redrawCheckDir + fstring("/frame%05d_", frameNum);
  saveImagePNG(partial, base + "partial.png");
  saveImagePNG(full, base + "full.png");
  outlineRect(res.diffImg, dirty, 0xFFFF0000u);  // blue (RGBA bytes)
  saveImagePNG(res.diffImg, base + "diff.png");
}

// installed at startup
static struct RedrawCheckInit {
  RedrawCheckInit()
  {
    const char* dir = getenv("WRITE_REDRAW_CHECK");
    redrawCheckDir = dir && dir[0] ? dir : "redrawcheck";
    createPath(redrawCheckDir);
    PLATFORM_LOG("Redraw check enabled (SW renderer only); writing failures to %s\n",
        canonicalPath(redrawCheckDir).c_str());
    Application::swFrameHook = checkRedraw;
  }
} redrawCheckInit;
