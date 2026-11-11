#pragma once

#include <string>

// Redraw check - debugging aid for partial redraw (SW or GL renderer): after each frame, the whole window is
//  redrawn into a scratch buffer and compared to the framebuffer produced by the normal (partial) redraw, and
//  pixels changed outside the frame's dirty rect are reported.  Installed via Application::frameHook.
// Used by WriteTest (redraw tests) and WriteRedrawCheck (Write w/ check enabled for all frames - see
//  redrawcheckmain.cpp).

struct RedrawCheckStats {
  int frames = 0;  // frames checked
  int fails = 0;  // frames where partial redraw differs significantly from full redraw
  int minor = 0;  // frames w/ only small differences (rounding)
  int outside = 0;  // frames w/ pixels changed outside dirty rect
  std::string firstFail;  // description of first failure (incl. outside dirty rect)
};

// outDir: folder for frame*_{partial,full,diff}.png for failures (created if needed); filePrefix is prepended
//  to file names; logMinor: also log MINOR frames (FAIL and outside dirty rect are always logged)
void redrawCheckEnable(const std::string& outDir, const std::string& filePrefix = "", bool logMinor = true);
void redrawCheckDisable();
// stats since last enable or reset
const RedrawCheckStats& redrawCheckStats();
void redrawCheckResetStats();
// set prefix for dump file names (e.g. to identify the current test step); stats are not reset
void redrawCheckSetPrefix(const std::string& filePrefix);
