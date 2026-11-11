// Enables redraw check (see redrawcheck.h) for all frames at startup - linked into WriteRedrawCheck
//  (`make DEBUG=1 redrawcheck`), which is Write plus this file.  Works w/ SW and GL renderers.
// Failures are written to $WRITE_REDRAW_CHECK, or ./redrawcheck if not set.

#include "redrawcheck.h"
#include "application.h"
#include "ugui/svggui_platform.h"
#include "ulib/geom.h"
#include "ulib/fileutil.h"
#include "ulib/platformutil.h"
#include <cstdlib>

// enable on first frame instead of in static initializer, since redrawcheck.cpp's statics may not be
//  constructed yet (Application::frameHook is constant initialized, so setting it here is safe)
static void firstFrameHook(const Rect& dirty)
{
  const char* dir = getenv("WRITE_REDRAW_CHECK");
  std::string outdir = dir && dir[0] ? dir : "redrawcheck";
  redrawCheckEnable(outdir);
  PLATFORM_LOG("Redraw check enabled; writing failures to %s\n", canonicalPath(outdir).c_str());
  Application::frameHook(dirty);
}

static struct RedrawCheckInit {
  RedrawCheckInit() { Application::frameHook = firstFrameHook; }
} redrawCheckInit;
