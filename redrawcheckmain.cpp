// Enables redraw check (see redrawcheck.h) for all frames at startup - linked into WriteRedrawCheck
//  (`make DEBUG=1 redrawcheck`), which is Write plus this file.  Requires SW renderer (glRender = 0).
// Failures are written to $WRITE_REDRAW_CHECK, or ./redrawcheck if not set.

#include "redrawcheck.h"
#include "application.h"
#include "ugui/svggui_platform.h"
#include "ulib/geom.h"
#include "ulib/fileutil.h"
#include "ulib/platformutil.h"
#include <cstdlib>

// enable on first frame instead of in static initializer, since redrawcheck.cpp's statics may not be
//  constructed yet (Application::swFrameHook is constant initialized, so setting it here is safe)
static void firstFrameHook(const PlatformFramebuffer& fb, const Rect& dirty)
{
  const char* dir = getenv("WRITE_REDRAW_CHECK");
  std::string outdir = dir && dir[0] ? dir : "redrawcheck";
  redrawCheckEnable(outdir);
  PLATFORM_LOG("Redraw check enabled (SW renderer only); writing failures to %s\n", canonicalPath(outdir).c_str());
  Application::swFrameHook(fb, dirty);
}

static struct RedrawCheckInit {
  RedrawCheckInit() { Application::swFrameHook = firstFrameHook; }
} redrawCheckInit;
