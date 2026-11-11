#pragma once

#include "ulib/image.h"

// test-only API of the headless platform layer (headless.cpp)

// use GL (EGL surfaceless context w/ offscreen framebuffer standing in for window) if app requests it; must be
//  called before platformInit() (i.e. before appInit()); returns false if EGL isn't available
void headlessSetGL(bool enable);
bool headlessUsingGL();
// GL renderer string, or "" for SW
const char* headlessGLRenderer();
// current window contents (GL: window framebuffer; SW: framebuffer) as RGBA image, top row first
Image headlessReadWindow();
