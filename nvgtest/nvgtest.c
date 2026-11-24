// nvgtest: headless nanovgXC renderer test and benchmark tool
// - renders scenes w/ nanovg_vtex into an offscreen framebuffer (EGL context w/o window) and saves PNGs;
//  `--bench N` times each scene instead (see benchScene())
// - desktop GL 3.3 (Mesa EGL surfaceless) by default; GLES 3 w/ NVGTEST_GLES defined (pbuffer surface; used for
//  Android, where nvgtest runs as a native executable via adb shell - see `make android` and ab.sh -a)
// - `--compare DIR_A DIR_B` compares two sets of PNGs exactly (for A/B testing of nanovgXC changes; see ab.sh)
// First stage of the plan in nanovgXC/HANDOFF-overlap.md ("Testing infrastructure"): text scenes only, for
//  now, to check SDF text changes (nanovgXC/HANDOFF-subpixel.md).  Will move to the nanovgXC repo once stable.
// Scenes: text_* (atlas text), svg_* / write_* (SVG files, incl. Write documents w/ handwriting), paths_*
//  (synthetic: small/big paths, axis-aligned rects, strokes, fill rules/no AA, paint types)

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef NVGTEST_GLES
#include <GLES3/gl3.h>
#define NANOVG_GLES3_IMPLEMENTATION
#else
#include "glad.h"
#define NANOVG_GL3_IMPLEMENTATION
#endif
#define NANOVG_SW_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_vtex.h"
#include "nanovg_gl_utils.h"
#include "nanovg_sw.h"

#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

// data files (font, SVGs) are looked up by name in --data DIR if given, otherwise in NVGTEST_DATA_DIRS
#ifndef NVGTEST_DATA_DIRS
#error "NVGTEST_DATA_DIRS (':'-separated list of directories w/ data files) must be defined"
#endif

static const char* dataDir = NULL;

// returns path of data file `name`, or NULL if not found; returned string valid until next call
static const char* findData(const char* name)
{
  static char path[1024];
  const char* dirs = dataDir ? dataDir : NVGTEST_DATA_DIRS;
  while(*dirs) {
    const char* end = strchr(dirs, ':');
    int len = end ? (int)(end - dirs) : (int)strlen(dirs);
    snprintf(path, sizeof(path), "%.*s/%s", len, dirs, name);
    struct stat st;
    if(stat(path, &st) == 0)
      return path;
    dirs += end ? len + 1 : len;
  }
  return NULL;
}

// SDF atlas generation w/ nanovg_sw - same as createFontstash() in nanovgXC/example/example_sdl.c

typedef struct { NVGcontext* vg; float* fbuff; int fbuffw, fbuffh; float sdfScale, sdfOffset; } SDFcontext;

static const float INITIAL_SDF_DIST = 1E6f;

static void sdfRender(void* uptr, void* fontimpl, unsigned char* output,
      int outWidth, int outHeight, int outStride, float scale, int padding, int glyph)
{
  SDFcontext* ctx = (SDFcontext*)uptr;
  nvgBeginFrame(ctx->vg, 0, 0, 1);
  nvgDrawSTBTTGlyph(ctx->vg, (stbtt_fontinfo*)fontimpl, scale, padding, glyph);
  nvgEndFrame(ctx->vg);
  for(int iy = 0; iy < outHeight; ++iy) {
    for(int ix = 0; ix < outWidth; ++ix) {
      float sd = ctx->fbuff[ix + iy*ctx->fbuffw] * ctx->sdfScale + ctx->sdfOffset;
      output[ix + iy*outStride] = (unsigned char)(0.5f + glnvg__minf(glnvg__maxf(sd, 0.f), 255.f));
      ctx->fbuff[ix + iy*ctx->fbuffw] = INITIAL_SDF_DIST;
    }
  }
}

static void sdfDelete(void* uptr)
{
  SDFcontext* ctx = (SDFcontext*)uptr;
  nvgswDelete(ctx->vg);
  free(ctx->fbuff);
  free(ctx);
}

static FONScontext* createFontstash(int nvgFlags, int maxAtlasFontPx)
{
  FONSparams params;
  memset(&params, 0, sizeof(FONSparams));
  params.flags = FONS_ZERO_TOPLEFT | FONS_DELAY_LOAD;
  params.flags |= (nvgFlags & NVG_SDF_TEXT) ? FONS_SDF : FONS_SUMMED;
  params.sdfPadding = 4;
  params.sdfPixelDist = 32.0f;

  SDFcontext* ctx = malloc(sizeof(SDFcontext));
  ctx->fbuffh = ctx->fbuffw = maxAtlasFontPx + 2*params.sdfPadding + 16;
  ctx->sdfScale = -params.sdfPixelDist;
  ctx->sdfOffset = 127;
  ctx->fbuff = malloc(ctx->fbuffw*ctx->fbuffh*sizeof(float));
  for(int ii = 0; ii < ctx->fbuffw*ctx->fbuffh; ++ii)
    ctx->fbuff[ii] = INITIAL_SDF_DIST;
  ctx->vg = nvgswCreate(NVG_AUTOW_DEFAULT | NVG_NO_FONTSTASH | NVGSW_PATHS_XC | NVGSW_SDFGEN);
  nvgswSetFramebuffer(ctx->vg, ctx->fbuff, ctx->fbuffw, ctx->fbuffh, params.sdfPadding, 0,0,0);

  params.userPtr = ctx;
  params.userSDFRender = sdfRender;
  params.userDelete = sdfDelete;
  return fonsCreateInternal(&params);
}

// EGL surfaceless context (same approach as scribbletest/headless.cpp)

static EGLDisplay eglDpy = EGL_NO_DISPLAY;
static EGLContext eglCtx = EGL_NO_CONTEXT;
static EGLSurface eglSurf = EGL_NO_SURFACE;

#ifdef NVGTEST_GLES
// GLES 3 context w/ small pbuffer surface (we render to our own FBO); on desktop Mesa, set EGL_PLATFORM=surfaceless
//  to avoid needing a display
static int initGL(void)
{
  eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if(eglDpy == EGL_NO_DISPLAY || !eglInitialize(eglDpy, NULL, NULL)) {
    fprintf(stderr, "nvgtest: unable to initialize EGL display\n");
    return 0;
  }
  const EGLint cfgattribs[] = { EGL_RENDERABLE_TYPE, 0x40 /*EGL_OPENGL_ES3_BIT_KHR*/, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
  EGLConfig cfg;
  EGLint ncfg = 0;
  if(!eglChooseConfig(eglDpy, cfgattribs, &cfg, 1, &ncfg) || ncfg < 1) {
    fprintf(stderr, "nvgtest: no suitable EGL config (error 0x%x)\n", eglGetError());
    return 0;
  }
  const EGLint surfattribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
  eglSurf = eglCreatePbufferSurface(eglDpy, cfg, surfattribs);
  const EGLint ctxattribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  eglCtx = eglCreateContext(eglDpy, cfg, EGL_NO_CONTEXT, ctxattribs);
  if(eglSurf == EGL_NO_SURFACE || eglCtx == EGL_NO_CONTEXT || !eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx)) {
    fprintf(stderr, "nvgtest: unable to create EGL context (error 0x%x)\n", eglGetError());
    return 0;
  }
  return 1;
}
#else
static int initGL(void)
{
  eglDpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  if(eglDpy == EGL_NO_DISPLAY || !eglInitialize(eglDpy, NULL, NULL)) {
    fprintf(stderr, "nvgtest: unable to initialize EGL surfaceless display\n");
    return 0;
  }
  if(!eglBindAPI(EGL_OPENGL_API)) {
    fprintf(stderr, "nvgtest: eglBindAPI(EGL_OPENGL_API) failed\n");
    return 0;
  }
  const EGLint ctxattribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
  eglCtx = eglCreateContext(eglDpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxattribs);
  if(eglCtx == EGL_NO_CONTEXT || !eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, eglCtx)) {
    fprintf(stderr, "nvgtest: unable to create EGL context (error 0x%x)\n", eglGetError());
    return 0;
  }
  if(!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
    fprintf(stderr, "nvgtest: gladLoadGL failed\n");
    return 0;
  }
  return 1;
}
#endif

static void shutdownGL(void)
{
  if(eglCtx != EGL_NO_CONTEXT) {
    eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(eglDpy, eglCtx);
  }
  if(eglSurf != EGL_NO_SURFACE)
    eglDestroySurface(eglDpy, eglSurf);
  if(eglDpy != EGL_NO_DISPLAY)
    eglTerminate(eglDpy);
}

// scenes

static char ascii[96];  // printable ASCII

static void fillRect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor c)
{
  nvgBeginPath(vg);
  nvgRect(vg, x, y, w, h);
  nvgFillColor(vg, c);
  nvgFill(vg);
}

// rotate string so different chars are visible at large sizes
static const char* asciiFrom(int offset) { return ascii + (offset % 95); }

static void sceneTextSizes(NVGcontext* vg, int w, int h)
{
  static const float sizes[] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24, 28, 32, 40, 48};
  int n = sizeof(sizes)/sizeof(sizes[0]);
  float y = 4;
  nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
  for(int pass = 0; pass < 2; ++pass) {
    for(int ii = 0; ii < n; ++ii) {
      y += sizes[ii]*1.25f;
      nvgFontSize(vg, sizes[ii]);
      nvgText(vg, 8, y, pass ? asciiFrom(sizes[ii] > 16 ? 47 : 0) : ascii, NULL);
    }
    if(pass == 0) {  // second pass: smaller sizes again, shifted by 1/3 px
      nvgTranslate(vg, 1.0f/3, 1.0f/3);
      y += 8;
    }
  }
}

static void sceneTextFrac(NVGcontext* vg, int w, int h)
{
  static const float sizes[] = {8, 11, 16};
  static const float offsets[] = {0, 0.1f, 0.25f, 0.5f, 0.75f};
  float y = 4;
  nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
  for(int ii = 0; ii < 3; ++ii) {
    nvgFontSize(vg, sizes[ii]);
    for(int jj = 0; jj < 5; ++jj) {
      for(int kk = 0; kk < 5; ++kk) {
        y += sizes[ii]*1.25f;
        nvgText(vg, 8 + offsets[jj], floorf(y) + offsets[kk], ascii, NULL);
      }
    }
  }
}

static void sceneTextBlur(NVGcontext* vg, int w, int h)
{
  static const float sizes[] = {8, 12, 20, 32};
  static const float blurs[] = {0, 0.5f, 1, 2, 3};
  float y = 4;
  nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
  for(int ii = 0; ii < 4; ++ii) {
    nvgFontSize(vg, sizes[ii]);
    for(int jj = 0; jj < 5; ++jj) {
      y += sizes[ii]*1.3f;
      nvgFontBlur(vg, blurs[jj]);
      nvgText(vg, 8, y, ascii, NULL);
    }
  }
  nvgFontBlur(vg, 0);
}

static void sceneTextColors(NVGcontext* vg, int w, int h)
{
  static const float sizes[] = {7, 9, 12, 16, 24};
  NVGcolor bg[4] = { nvgRGBA(0,0,0,255), nvgRGBA(40,60,160,255), nvgRGBA(255,255,255,255), nvgRGBA(128,128,128,255) };
  NVGcolor fg[4] = { nvgRGBA(255,255,255,255), nvgRGBA(250,200,40,255), nvgRGBA(0,0,0,128), nvgRGBA(90,90,90,255) };
  for(int ii = 0; ii < 4; ++ii) {
    float x0 = (ii % 2)*w/2, y0 = (ii / 2)*h/2;
    fillRect(vg, x0, y0, w/2, h/2, bg[ii]);
    nvgScissor(vg, x0, y0, w/2, h/2);
    nvgFillColor(vg, fg[ii]);
    float y = y0 + 4;
    for(int jj = 0; jj < 5; ++jj) {
      for(int kk = 0; kk < 2; ++kk) {
        y += sizes[jj]*1.25f;
        nvgFontSize(vg, sizes[jj]);
        nvgText(vg, x0 + 8, y, asciiFrom(kk*40), NULL);
      }
    }
    nvgResetScissor(vg);
  }
}

static void sceneTextXform(NVGcontext* vg, int w, int h)
{
  float y = 30;
  nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
  for(int ii = 0; ii < 7; ++ii) {
    for(int jj = 0; jj < 2; ++jj) {
      nvgSave(vg);
      nvgTranslate(vg, ii == 4 ? w - 8 : 8, y);  // mirrored text is drawn right to left
      switch(ii) {
        case 0: nvgScale(vg, 1.5f, 1); break;
        case 1: nvgScale(vg, 1, 1.5f); break;
        case 2: nvgScale(vg, 0.7f, 0.7f); break;
        case 3: nvgScale(vg, 1.0f/3, 1.0f/3); break;
        case 4: nvgScale(vg, -1, 1); break;
        case 5: nvgRotate(vg, 0.05f); break;
        case 6: nvgSkewX(vg, 0.3f); break;
      }
      nvgFontSize(vg, jj ? 20 : 12);
      nvgText(vg, 0, 0, ascii, NULL);
      nvgRestore(vg);
      y += jj ? 50 : 30;
    }
  }
}

// dense page of text, for benchmarking (like textPerformance() in nanovgXC/example/tests.c)
static void textPage(NVGcontext* vg, int w, int h, float size)
{
  nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
  nvgFontSize(vg, size);
  int line = 0;
  for(float y = size; y < h + size; y += size*1.2f, ++line)
    nvgText(vg, 4 - (line % 7)*size, y, asciiFrom(line*13), NULL);
}

static void sceneTextPage8(NVGcontext* vg, int w, int h) { textPage(vg, w, h, 8); }
static void sceneTextPage12(NVGcontext* vg, int w, int h) { textPage(vg, w, h, 12); }
static void sceneTextPage24(NVGcontext* vg, int w, int h) { textPage(vg, w, h, 24); }


// path scenes

// SVG documents, parsed on first use (not included in --bench timing)
typedef struct { const char* file; NSVGimage* image; float bounds[4]; int failed; } SvgDoc;
static SvgDoc svgTiger = {"tiger.svg"};
static SvgDoc svgOpt = {"Opt_page1.svg"};  // Write document (handwriting)
static SvgDoc svgIntro = {"Intro.svg"};  // Write intro document (handwriting, icons)

static NSVGimage* loadSvg(SvgDoc* doc)
{
  if(doc->image || doc->failed)
    return doc->image;
  const char* path = findData(doc->file);
  doc->image = path ? nsvgParseFromFile(path, "px", 96.0f) : NULL;
  if(!doc->image) {
    fprintf(stderr, "nvgtest: unable to load %s\n", doc->file);
    doc->failed = 1;
    return NULL;
  }
  // content bounds (after viewBox transform, which is scale + translate) - Write documents have no width/height
  const float* vx = doc->image->viewXform;
  float* b = doc->bounds;
  b[0] = b[1] = 1E10f; b[2] = b[3] = -1E10f;
  for(NSVGshape* shape = doc->image->shapes; shape; shape = shape->next) {
    if(shape->fill.type == NSVG_PAINT_NONE && shape->stroke.type == NSVG_PAINT_NONE) continue;
    b[0] = fminf(b[0], vx[0]*shape->bounds[0] + vx[4]);
    b[1] = fminf(b[1], vx[3]*shape->bounds[1] + vx[5]);
    b[2] = fmaxf(b[2], vx[0]*shape->bounds[2] + vx[4]);
    b[3] = fmaxf(b[3], vx[3]*shape->bounds[3] + vx[5]);
  }
  return doc->image;
}

// nanosvg color is RGBA bytes packed as uint32 (little endian), same as NVGcolor
static NVGcolor nvgColUint(unsigned int col)
{
  NVGcolor c;
  c.c = col;
  return c;
}

static NVGpaint linearGradToNVGpaint(NVGcontext* vg, NSVGgradient* g)
{
  // only simple (2-stop) gradients
  NSVGgradientStop* s0 = &g->stops[0];
  NSVGgradientStop* s1 = &g->stops[g->nstops > 1 ? 1 : 0];
  float* t = g->xform;
  float x0 = t[4], y0 = t[5], dx = t[2], dy = t[3];
  return nvgLinearGradient(vg, x0 + s0->offset*dx, y0 + s0->offset*dy,
      x0 + s1->offset*dx, y0 + s1->offset*dy, nvgColUint(s0->color), nvgColUint(s1->color));
}

// based on nsvgRender() in nanovgXC/example/tests.c (+ fill rule); draws in SVG coords after viewBox transform
static void nsvgRender(NVGcontext* vg, NSVGimage* image)
{
  static const int nsvgCap[] = {NVG_BUTT, NVG_ROUND, NVG_SQUARE};
  static const int nsvgJoin[] = {NVG_MITER, NVG_ROUND, NVG_BEVEL};
  const float* t = image->viewXform;
  nvgSave(vg);
  nvgTransform(vg, t[0], t[1], t[2], t[3], t[4], t[5]);
  for(NSVGshape* shape = image->shapes; shape != NULL; shape = shape->next) {
    if(!shape->paths || (shape->fill.type == NSVG_PAINT_NONE && shape->stroke.type == NSVG_PAINT_NONE)) continue;
    nvgBeginPath(vg);
    for(NSVGpath* path = shape->paths; path != NULL; path = path->next) {
      nvgMoveTo(vg, path->pts[0], path->pts[1]);
      for(int i = 1; i < path->npts-1; i += 3) {
        float* p = &path->pts[i*2];
        if(fabsf((p[2] - p[0]) * (p[5] - p[1]) - (p[4] - p[0]) * (p[3] - p[1])) < 0.001f)
          nvgLineTo(vg, p[4], p[5]);
        else
          nvgBezierTo(vg, p[0], p[1], p[2], p[3], p[4], p[5]);
      }
      if(path->closed)
        nvgClosePath(vg);
      nvgPathWinding(vg, NVG_AUTOW);
    }
    if(shape->fill.type == NSVG_PAINT_COLOR || shape->fill.type == NSVG_PAINT_LINEAR_GRADIENT) {
      if(shape->fill.type == NSVG_PAINT_COLOR)
        nvgFillColor(vg, nvgColUint(shape->fill.color));  // fill.color includes opacity
      else
        nvgFillPaint(vg, linearGradToNVGpaint(vg, shape->fill.gradient));
      nvgFillRule(vg, shape->fillRule == NSVG_FILLRULE_EVENODD ? NVG_EVENODD : NVG_NONZERO);
      nvgFill(vg);
    }
    if(shape->stroke.type == NSVG_PAINT_COLOR) {
      nvgStrokeColor(vg, nvgColUint(shape->stroke.color));
      nvgStrokeWidth(vg, shape->strokeWidth);
      nvgLineCap(vg, nsvgCap[(int)shape->strokeLineCap]);
      nvgLineJoin(vg, nsvgJoin[(int)shape->strokeLineJoin]);
      nvgStroke(vg);
    }
  }
  nvgFillRule(vg, NVG_NONZERO);
  nvgRestore(vg);
}

// draw doc scaled by zoom relative to fitting whole doc in frame, w/ point (fx, fy) (fraction of doc bounds) at
//  center of frame
static void svgScene(NVGcontext* vg, int w, int h, SvgDoc* doc, float zoom, float fx, float fy)
{
  NSVGimage* image = loadSvg(doc);
  if(!image) return;
  const float* b = doc->bounds;
  float bw = b[2] - b[0], bh = b[3] - b[1];
  float s = zoom*fminf(w/bw, h/bh);
  nvgSave(vg);
  nvgTranslate(vg, w/2.0f, h/2.0f);
  nvgScale(vg, s, s);
  nvgTranslate(vg, -(b[0] + fx*bw), -(b[1] + fy*bh));
  nsvgRender(vg, image);
  nvgRestore(vg);
}

static void sceneSvgTiger(NVGcontext* vg, int w, int h) { svgScene(vg, w, h, &svgTiger, 1, 0.5f, 0.5f); }
static void sceneSvgTigerZ3(NVGcontext* vg, int w, int h) { svgScene(vg, w, h, &svgTiger, 3, 0.45f, 0.4f); }
// whole page: many small strokes; zoomed: long strokes spanning many tiles
static void sceneWriteOpt(NVGcontext* vg, int w, int h) { svgScene(vg, w, h, &svgOpt, 1, 0.5f, 0.5f); }
static void sceneWriteOptZ4(NVGcontext* vg, int w, int h) { svgScene(vg, w, h, &svgOpt, 4, 0.4f, 0.3f); }
static void sceneWriteIntro(NVGcontext* vg, int w, int h) { svgScene(vg, w, h, &svgIntro, 1, 0.5f, 0.5f); }
static void sceneWriteIntroZ4(NVGcontext* vg, int w, int h) { svgScene(vg, w, h, &svgIntro, 4, 0.5f, 0.15f); }

// many small paths (like smallPathsTest() in nanovgXC/example/tests.c)
static void scenePathsSmall(NVGcontext* vg, int w, int h)
{
  nvgFillColor(vg, nvgRGBA(0, 0, 255, 128));
  for(int y = 0; y < h; y += 20) {
    for(int x = 0; x < w; x += 20) {
      nvgBeginPath(vg);
      nvgMoveTo(vg, x, y);
      for(int dy = 0; dy < 50; dy += 3)
        nvgLineTo(vg, x + 4*(dy%2), y + dy);
      nvgLineTo(vg, x + 8, y + 50);
      nvgLineTo(vg, x + 8, y);
      nvgClosePath(vg);
      nvgFill(vg);
    }
  }
}

// large non-convex paths w/ many edges, spanning many tiles, overlapping, semi-transparent
static void scenePathsBig(NVGcontext* vg, int w, int h)
{
  float R = 0.4f*fminf(w, h);
  for(int k = 0; k < 6; ++k) {
    float cx = w*(0.25f + 0.25f*(k % 3)), cy = h*(0.35f + 0.3f*(k / 3));
    nvgBeginPath(vg);
    for(int ii = 0; ii < 360; ++ii) {
      float a = ii*2*NVG_PI/360;
      float r = R*(0.65f + 0.35f*sinf(7*a + k));
      ii ? nvgLineTo(vg, cx + r*cosf(a), cy + r*sinf(a)) : nvgMoveTo(vg, cx + r*cosf(a), cy + r*sinf(a));
    }
    nvgClosePath(vg);
    nvgFillColor(vg, nvgHSLA(k/6.0f, 0.7f, 0.5f, 100));
    nvgFill(vg);
  }
}

// axis-aligned rects and rounded rects (UI-like; half the edges of rects are vertical), not pixel aligned
static void scenePathsAxis(NVGcontext* vg, int w, int h)
{
  int ii = 0;
  for(float y = 4.3f; y < h - 40; y += 37.7f) {
    for(float x = 4.1f; x < w - 120; x += 123.3f, ++ii) {
      float rw = 20 + (ii*37 % 100), rh = 8 + (ii*13 % 28);
      nvgBeginPath(vg);
      if(ii % 3 == 0)
        nvgRect(vg, x, y, rw, rh);
      else
        nvgRoundedRect(vg, x, y, rw, rh, ii % 3 == 1 ? 3 : rh/2);
      nvgFillColor(vg, nvgHSLA((ii % 17)/17.0f, 0.6f, 0.45f, ii % 2 ? 255 : 160));
      nvgFill(vg);
      // thin rects: 1 px and 0.5 px wide, at fractional positions
      nvgBeginPath(vg);
      nvgRect(vg, x + rw + 3.5f, y, ii % 2 ? 1 : 0.5f, rh);
      nvgRect(vg, x, y + rh + 2.25f, rw, ii % 2 ? 1 : 0.5f);
      nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
      nvgFill(vg);
    }
  }
}

static void scenePathsStrokes(NVGcontext* vg, int w, int h)
{
  static const float widths[] = {0.25f, 0.5f, 1, 1.5f, 2, 3, 5, 10, 20};
  static const int caps[] = {NVG_BUTT, NVG_ROUND, NVG_SQUARE};
  static const int joins[] = {NVG_MITER, NVG_ROUND, NVG_BEVEL};
  static float dashes[] = {12, 6, 3, 6, -1};
  nvgStrokeColor(vg, nvgRGBA(0, 0, 0, 220));
  // fans of straight lines at many angles
  for(int ii = 0; ii < 9; ++ii) {
    float cx = 60 + ii*(w - 120)/8.0f, cy = 80;
    nvgStrokeWidth(vg, widths[ii]);
    nvgBeginPath(vg);
    for(int jj = 0; jj < 12; ++jj) {
      float a = jj*NVG_PI/12;
      nvgMoveTo(vg, cx, cy);
      nvgLineTo(vg, cx + 55*cosf(a), cy + 55*sinf(a));
    }
    nvgStroke(vg);
  }
  // zigzag polylines w/ each cap and join, plus dashed
  for(int ii = 0; ii < 9; ++ii) {
    float y0 = 180 + ii*((h - 260)/9.0f);
    nvgLineCap(vg, caps[ii % 3]);
    nvgLineJoin(vg, joins[ii / 3]);
    nvgStrokeWidth(vg, 2 + 2*ii);
    nvgStrokeColor(vg, nvgHSLA(ii/9.0f, 0.6f, 0.4f, 200));
    if(ii == 8) nvgDashArray(vg, dashes);
    nvgBeginPath(vg);
    for(int jj = 0; jj < 10; ++jj) {
      float x = 40 + jj*(w*0.6f/10), y = y0 + (jj % 2 ? 40 : 0) + 0.3f*jj;
      jj ? nvgLineTo(vg, x, y) : nvgMoveTo(vg, x, y);
    }
    nvgStroke(vg);
    nvgDashArray(vg, NULL);
  }
  // stroked curves
  nvgLineCap(vg, NVG_BUTT);
  nvgLineJoin(vg, NVG_MITER);
  for(int ii = 0; ii < 6; ++ii) {
    nvgStrokeWidth(vg, widths[ii + 2]);
    nvgStrokeColor(vg, nvgRGBA(20, 20, 160, 220));
    nvgBeginPath(vg);
    nvgCircle(vg, w*0.82f, h*0.5f, 15 + ii*0.03f*w);
    nvgStroke(vg);
  }
}

static void star(NVGcontext* vg, float cx, float cy, float r, int n, int step)
{
  for(int ii = 0; ii < n; ++ii) {
    float a = -NVG_PI/2 + (ii*step % n)*2*NVG_PI/n;
    ii ? nvgLineTo(vg, cx + r*cosf(a), cy + r*sinf(a)) : nvgMoveTo(vg, cx + r*cosf(a), cy + r*sinf(a));
  }
  nvgClosePath(vg);
}

// fill rules, self-overlapping paths, retraced strokes, no AA
static void scenePathsRules(NVGcontext* vg, int w, int h)
{
  float r = fminf(w, h)*0.09f;
  for(int aa = 0; aa < 2; ++aa) {
    nvgShapeAntiAlias(vg, !aa);
    float y = (aa ? 0.5f : 0.12f)*h;
    for(int rule = 0; rule < 2; ++rule) {
      nvgFillRule(vg, rule ? NVG_EVENODD : NVG_NONZERO);
      nvgFillColor(vg, nvgRGBA(rule ? 160 : 0, 40, rule ? 0 : 160, 200));
      float x0 = (rule ? 0.5f : 0.02f)*w;
      // pentagram and 7/3 star (self-intersecting)
      nvgBeginPath(vg); star(vg, x0 + r, y, r, 5, 2); nvgFill(vg);
      nvgBeginPath(vg); star(vg, x0 + 3.2f*r, y, r, 7, 3); nvgFill(vg);
      // overlapping subpaths: 1st and 2nd same direction (nonzero: filled, even-odd: hole), 3rd opposite
      //  direction (hole for both where it overlaps 2nd)
      nvgBeginPath(vg);
      nvgRect(vg, x0, y + 1.3f*r, 1.5f*r, r);
      nvgRect(vg, x0 + 0.7f*r, y + 1.6f*r, 1.5f*r, r);
      nvgRect(vg, x0 + 1.7f*r, y + 1.9f*r, 1.2f*r, r);
      nvgPathWinding(vg, NVG_CW);
      nvgFill(vg);
      // convex polygon (drawn as triangle fan when AA is off)
      nvgBeginPath(vg);
      star(vg, x0 + 3.6f*r, y + 2*r, 0.6f*r, 9, 1);
      nvgFill(vg);
    }
  }
  nvgShapeAntiAlias(vg, 1);
  nvgFillRule(vg, NVG_NONZERO);
  // retraced stroke (like vertical stem of handwritten 'd'): polyline going down and back up nearly the same way
  nvgStrokeColor(vg, nvgRGBA(0, 0, 0, 255));
  nvgLineCap(vg, NVG_ROUND);
  nvgLineJoin(vg, NVG_ROUND);
  for(int ii = 0; ii < 6; ++ii) {
    float x = 0.05f*w + ii*0.15f*w, y0 = 0.82f*h, len = 0.12f*h;
    nvgStrokeWidth(vg, 1 + ii);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x, y0);
    nvgLineTo(vg, x + 0.3f*ii, y0 + len);
    nvgLineTo(vg, x + 0.5f + 0.2f*ii, y0 + 0.1f);
    nvgStroke(vg);
  }
  nvgLineCap(vg, NVG_BUTT);
  nvgLineJoin(vg, NVG_MITER);
}

// gradients, image pattern, rotated scissor - the fill shader's other paint types
static void scenePathsPaint(NVGcontext* vg, int w, int h)
{
  static int img = 0;
  if(!img) {
    unsigned char data[64*64*4];
    for(int y = 0; y < 64; ++y) {
      for(int x = 0; x < 64; ++x) {
        unsigned char* p = &data[4*(y*64 + x)];
        int c = ((x/8 + y/8) % 2) ? 230 : 40;
        p[0] = c; p[1] = (unsigned char)(x*4); p[2] = (unsigned char)(y*4); p[3] = 255;
      }
    }
    img = nvgCreateImageRGBA(vg, 64, 64, NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY, data);
  }
  float cw = w/3.0f, ch = h/2.0f;
  NVGpaint paints[5];
  paints[0] = nvgLinearGradient(vg, 0, 0, cw, ch, nvgRGBA(255, 0, 0, 255), nvgRGBA(0, 0, 255, 128));
  paints[1] = nvgRadialGradient(vg, cw*1.5f, ch*0.5f, 10, ch*0.45f, nvgRGBA(255, 255, 0, 255), nvgRGBA(0, 128, 0, 255));
  paints[2] = nvgBoxGradient(vg, cw*2 + 30, 40, cw - 60, ch - 80, 20, 30, nvgRGBA(0, 0, 0, 200), nvgRGBA(0, 0, 0, 0));
  paints[3] = nvgImagePattern(vg, 0, ch, 64, 64, 0.3f, img, 1.0f);
  paints[4] = nvgImagePattern(vg, cw, ch, 200, 150, 0, img, 0.7f);
  for(int ii = 0; ii < 5; ++ii) {
    float x = (ii % 3)*cw, y = (ii / 3)*ch;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 10, y + 10, cw - 20, ch - 20, 25);
    nvgFillPaint(vg, paints[ii]);
    nvgFill(vg);
  }
  // rotated scissor
  nvgSave(vg);
  nvgTranslate(vg, cw*2.5f, ch*1.5f);
  nvgRotate(vg, 0.4f);
  nvgScissor(vg, -cw*0.3f, -ch*0.3f, cw*0.6f, ch*0.6f);
  nvgBeginPath(vg);
  star(vg, 0, 0, ch*0.45f, 5, 2);
  nvgFillColor(vg, nvgRGBA(200, 60, 0, 230));
  nvgFill(vg);
  nvgRestore(vg);
}

typedef void (*SceneFn)(NVGcontext* vg, int w, int h);
typedef struct { const char* name; SceneFn fn; } Scene;

static const Scene scenes[] = {
  {"text_sizes", sceneTextSizes},
  {"text_frac", sceneTextFrac},
  {"text_blur", sceneTextBlur},
  {"text_colors", sceneTextColors},
  {"text_xform", sceneTextXform},
  {"text_page8", sceneTextPage8},
  {"text_page12", sceneTextPage12},
  {"text_page24", sceneTextPage24},
  {"svg_tiger", sceneSvgTiger},
  {"svg_tiger_z3", sceneSvgTigerZ3},
  {"write_opt", sceneWriteOpt},
  {"write_opt_z4", sceneWriteOptZ4},
  {"write_intro", sceneWriteIntro},
  {"write_intro_z4", sceneWriteIntroZ4},
  {"paths_small", scenePathsSmall},
  {"paths_big", scenePathsBig},
  {"paths_axis", scenePathsAxis},
  {"paths_strokes", scenePathsStrokes},
  {"paths_rules", scenePathsRules},
  {"paths_paint", scenePathsPaint},
};
static const int nScenes = sizeof(scenes)/sizeof(scenes[0]);

// rendering

typedef struct {
  NVGcontext* vg;
  NVGLUframebuffer* fb;
  int w, h;
  int srgb;
  int repeat;  // draw scene this many times per frame (for benchmarking, so per-frame overhead is less significant)
  int gpuRepeat;  // submit the frame's GL draws this many times (nanovg CPU work done once) - see main()
  unsigned char* swbuf;  // --sw: render w/ nanovg_sw into this RGBA buffer (top row first) instead of GL
} Renderer;

static double nowSec(void);

// if tFlush != NULL, it is set to time just before nvgEndFrame (where nanovg issues all GL draw calls)
static void renderScene(Renderer* r, const Scene* scene, double* tFlush)
{
  if(r->swbuf)
    memset(r->swbuf, 0xff, (size_t)r->w*r->h*4);
  else {
    nvgluBindFramebuffer(r->fb);
    nvgluSetViewport(0, 0, r->w, r->h);
    nvgluClear(nvgRGBAf(1, 1, 1, 1));
  }
  nvgBeginFrame(r->vg, r->w, r->h, 1.0f);
  for(int ii = 0; ii < r->repeat; ++ii) {
    nvgSave(r->vg);
    scene->fn(r->vg, r->w, r->h);
    nvgRestore(r->vg);
  }
  if(tFlush)
    *tFlush = nowSec();
  nvgEndFrame(r->vg);
  // vtex resets calls at start of frame, not end, so flushing again draws the same frame again
  NVGparams* params = nvgInternalParams(r->vg);
  for(int ii = 1; ii < r->gpuRepeat; ++ii)
    params->renderFlush(params->userPtr);
}

static int saveScene(Renderer* r, const char* path)
{
  if(r->swbuf)
    return stbi_write_png(path, r->w, r->h, 4, r->swbuf, r->w*4);
  unsigned char* px = malloc((size_t)r->w*r->h*4);
  unsigned char* flipped = malloc((size_t)r->w*r->h*4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  nvgluReadPixels(r->fb, px);
  for(int y = 0; y < r->h; ++y)  // GL rows are bottom to top
    memcpy(flipped + (size_t)y*r->w*4, px + (size_t)(r->h - 1 - y)*r->w*4, r->w*4);
  int ok = stbi_write_png(path, r->w, r->h, 4, flipped, r->w*4);
  free(px);
  free(flipped);
  return ok;
}

static double nowSec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec*1E-9;
}

static int cmpDouble(const void* a, const void* b)
{
  double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

// GPU time measured w/ GL_TIME_ELAPSED query around the frame, read back synchronously (so no overlap between
//  frames; not available for GLES, reported as 0).  Some drivers report unrealistic GPU times (e.g. VMware SVGA3D: timer queries appear not to include
//  host GPU execution), so we also report "flush" = wall time from nvgEndFrame (where nanovg issues all GL draw
//  calls) until glFinish returns, i.e. GL submission + GPU execution, and "cpu" = nanovg time before
//  nvgEndFrame (path flattening, text layout, etc.)
static void benchScene(Renderer* r, const Scene* scene, int nframes, int warmup)
{
  int sw = r->swbuf != NULL;  // --sw: "flush" = nanovg_sw rasterization time (no GL calls)
#ifndef NVGTEST_GLES
  GLuint query = 0;
  if(!sw) glGenQueries(1, &query);
#endif
  double* gpu = malloc(nframes*sizeof(double));
  double* cpu = malloc(nframes*sizeof(double));
  double* wall = malloc(nframes*sizeof(double));
  for(int ii = -warmup; ii < nframes; ++ii) {
    if(!sw) glFinish();
    GLuint64 ns = 0;
#ifndef NVGTEST_GLES
    if(!sw) glBeginQuery(GL_TIME_ELAPSED, query);
#endif
    double t0 = nowSec(), t1 = 0;
    renderScene(r, scene, &t1);
#ifndef NVGTEST_GLES
    if(!sw) glEndQuery(GL_TIME_ELAPSED);
#endif
    if(!sw) glFinish();
    double t2 = nowSec();
#ifndef NVGTEST_GLES
    if(!sw) glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);  // blocks until available
#endif
    if(ii >= 0) {
      gpu[ii] = ns*1E-6;
      cpu[ii] = (t1 - t0)*1E3;
      wall[ii] = (t2 - t1)*1E3;
    }
  }
  qsort(gpu, nframes, sizeof(double), cmpDouble);
  qsort(cpu, nframes, sizeof(double), cmpDouble);
  qsort(wall, nframes, sizeof(double), cmpDouble);
  printf("%-14s flush med %8.3f min %8.3f  gpu med %8.3f min %8.3f  cpu med %8.3f ms\n", scene->name,
      wall[nframes/2], wall[0], gpu[nframes/2], gpu[0], cpu[nframes/2]);
  fflush(stdout);
  free(gpu);
  free(cpu);
  free(wall);
#ifndef NVGTEST_GLES
  if(!sw) glDeleteQueries(1, &query);
#endif
}

// exact comparison of two directories of PNGs

static unsigned char* loadPNG(const char* path, int* w, int* h)
{
  int n;
  return stbi_load(path, w, h, &n, 4);
}

static int hasSuffix(const char* s, const char* suffix)
{
  size_t ls = strlen(s), lsuf = strlen(suffix);
  return ls >= lsuf && strcmp(s + ls - lsuf, suffix) == 0;
}

static int nameCmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

// returns number of images which differ (or are missing)
static int compareDirs(const char* dirA, const char* dirB, const char* diffDir)
{
  DIR* d = opendir(dirA);
  if(!d) { fprintf(stderr, "nvgtest: cannot open %s\n", dirA); return 1; }
  char* names[256];
  int nnames = 0;
  struct dirent* ent;
  while((ent = readdir(d)) && nnames < 256) {
    if(hasSuffix(ent->d_name, ".png"))
      names[nnames++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, nnames, sizeof(char*), nameCmp);
  if(diffDir)
    mkdir(diffDir, 0755);

  int nbad = 0;
  char pathA[1024], pathB[1024], pathD[1024];
  for(int ii = 0; ii < nnames; ++ii) {
    snprintf(pathA, sizeof(pathA), "%s/%s", dirA, names[ii]);
    snprintf(pathB, sizeof(pathB), "%s/%s", dirB, names[ii]);
    int wa, ha, wb, hb;
    unsigned char* a = loadPNG(pathA, &wa, &ha);
    unsigned char* b = loadPNG(pathB, &wb, &hb);
    if(!a || !b || wa != wb || ha != hb) {
      printf("%-24s %s\n", names[ii], !b ? "MISSING in B" : "SIZE MISMATCH");
      ++nbad;
    }
    else {
      size_t npx = (size_t)wa*ha;
      size_t ndiff = 0;
      int maxdiff = 0;
      long hist[4] = {0};  // pixels w/ max channel diff 1, 2-3, 4-15, >= 16
      // NVGTEST_LIST_DIFFS=N: print first N differing pixels (x y, RGBA in A and B)
      const char* listenv = getenv("NVGTEST_LIST_DIFFS");
      size_t nlist = listenv ? (size_t)atoi(listenv) : 0;
      for(size_t jj = 0; jj < npx; ++jj) {
        int pxdiff = 0;
        for(int c = 0; c < 4; ++c) {
          int dc = abs((int)a[4*jj + c] - (int)b[4*jj + c]);
          if(dc > pxdiff) pxdiff = dc;
        }
        if(pxdiff) {
          if(ndiff < nlist) {
            const unsigned char *pa = &a[4*jj], *pb = &b[4*jj];
            printf("  %zu %zu: %d %d %d %d -> %d %d %d %d\n", jj % wa, jj / wa,
                pa[0], pa[1], pa[2], pa[3], pb[0], pb[1], pb[2], pb[3]);
          }
          ++ndiff;
          ++hist[pxdiff == 1 ? 0 : (pxdiff < 4 ? 1 : (pxdiff < 16 ? 2 : 3))];
          if(pxdiff > maxdiff) maxdiff = pxdiff;
        }
      }
      if(ndiff) {
        ++nbad;
        printf("%-24s DIFF  pixels %zu  max %d  (1: %ld, 2-3: %ld, 4-15: %ld, >=16: %ld)\n",
            names[ii], ndiff, maxdiff, hist[0], hist[1], hist[2], hist[3]);
        if(diffDir) {
          // faded A, w/ differing pixels in red (brighter = larger diff; any diff is at least half intensity)
          unsigned char* img = malloc(npx*4);
          for(size_t jj = 0; jj < npx; ++jj) {
            int pxdiff = 0;
            for(int c = 0; c < 4; ++c) {
              int dc = abs((int)a[4*jj + c] - (int)b[4*jj + c]);
              if(dc > pxdiff) pxdiff = dc;
            }
            unsigned char* p = &img[4*jj];
            if(pxdiff) {
              int v = 128 + pxdiff*8;
              p[0] = v > 255 ? 255 : v; p[1] = 0; p[2] = 0;
            }
            else {
              int luma = (a[4*jj] + a[4*jj+1] + a[4*jj+2])/3;
              p[0] = p[1] = p[2] = 192 + luma/4;
            }
            p[3] = 255;
          }
          snprintf(pathD, sizeof(pathD), "%s/%s", diffDir, names[ii]);
          stbi_write_png(pathD, wa, ha, 4, img, wa*4);
          free(img);
        }
      }
      else
        printf("%-24s same\n", names[ii]);
    }
    stbi_image_free(a);
    stbi_image_free(b);
    free(names[ii]);
  }
  if(nnames == 0) {
    printf("nvgtest: no PNGs in %s\n", dirA);
    return 1;
  }
  return nbad;
}

static void usage(void)
{
  printf("usage: nvgtest [options]               render scenes to PNGs (or time them w/ --bench)\n"
         "       nvgtest --compare DIR_A DIR_B [--diff DIR]   compare PNGs exactly; exit code 1 if any differ\n"
         "         (env NVGTEST_LIST_DIFFS=N lists first N differing pixels of each image)\n"
         "options:\n"
         "  --out DIR        output directory for PNGs (default .)\n"
         "  --scene PATTERN  scenes to render (repeatable, shell wildcards, e.g. 'write_*'; default all);\n"
         "                   --list to list scenes\n"
         "  --data DIR       directory w/ data files (font, SVGs; default " NVGTEST_DATA_DIRS ")\n"
         "  --sdf 0|1        SDF text (default 1) or summed text\n"
         "  --srgb 0|1       sRGB-aware rendering (default 1)\n"
         "  --atlas PX       nvgAtlasTextThreshold (default 48; atlas font size is 2x this)\n"
         "  --size WxH       framebuffer size (default 1024x1024)\n"
         "  --bench N        time N frames of each scene (after warmup) instead of saving PNGs\n"
         "  --warmup N       warmup frames for --bench (default 10)\n"
         "  --repeat K       draw each scene K times per frame (default 1)\n"
         "  --gpu-repeat K   submit each frame's GL draws K times (CPU work once; default 1) - use for benchmarking\n"
         "                   on mobile: w/ --repeat, the GPU is idle during nanovg CPU work and may be downclocked\n"
         "  --font PATH      .ttf to use (default Roboto-Regular.ttf from data dirs)\n"
         "  --sw 0|1         render w/ nanovg_sw (XC paths, single thread) instead of GL; for --bench, flush =\n"
         "                   rasterization time (nvgEndFrame)\n");
}

int main(int argc, char* argv[])
{
  const char* outDir = ".";
  const char* sceneNames[64];
  int nSceneNames = 0;
  int sw = 0, sdf = 1, srgb = 1, w = 1024, h = 1024, bench = 0, warmup = 10, repeat = 1, gpuRepeat = 1;
  float atlasPx = 48;
  const char* fontPath = NULL;

  for(int ii = 1; ii < argc; ++ii) {
    const char* arg = argv[ii];
    const char* val = ii + 1 < argc ? argv[ii + 1] : NULL;
    if(strcmp(arg, "--compare") == 0) {
      if(ii + 2 >= argc) { usage(); return 2; }
      const char* diffDir = (ii + 4 < argc && strcmp(argv[ii+3], "--diff") == 0) ? argv[ii+4] : NULL;
      return compareDirs(argv[ii+1], argv[ii+2], diffDir) ? 1 : 0;
    }
    else if(strcmp(arg, "--list") == 0) {
      for(int jj = 0; jj < nScenes; ++jj)
        printf("%s\n", scenes[jj].name);
      return 0;
    }
    else if(strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) { usage(); return 0; }
    else if(!val) { usage(); return 2; }
    else if(strcmp(arg, "--out") == 0) outDir = val;
    else if(strcmp(arg, "--scene") == 0 && nSceneNames < 64) sceneNames[nSceneNames++] = val;
    else if(strcmp(arg, "--sdf") == 0) sdf = atoi(val);
    else if(strcmp(arg, "--srgb") == 0) srgb = atoi(val);
    else if(strcmp(arg, "--sw") == 0) sw = atoi(val);
    else if(strcmp(arg, "--atlas") == 0) atlasPx = atof(val);
    else if(strcmp(arg, "--size") == 0) sscanf(val, "%dx%d", &w, &h);
    else if(strcmp(arg, "--bench") == 0) bench = atoi(val);
    else if(strcmp(arg, "--warmup") == 0) warmup = atoi(val);
    else if(strcmp(arg, "--repeat") == 0) repeat = atoi(val);
    else if(strcmp(arg, "--gpu-repeat") == 0) gpuRepeat = atoi(val);
    else if(strcmp(arg, "--font") == 0) fontPath = val;
    else if(strcmp(arg, "--data") == 0) dataDir = val;
    else { usage(); return 2; }
    ++ii;
  }

  for(int c = 32; c < 127; ++c)
    ascii[c - 32] = (char)c;
  ascii[95] = '\0';

  // select scenes
  const Scene* selected[64];
  int nselected = 0;
  if(nSceneNames == 0) {
    for(int jj = 0; jj < nScenes; ++jj)
      selected[nselected++] = &scenes[jj];
  }
  for(int ii = 0; ii < nSceneNames; ++ii) {
    int found = 0;
    for(int jj = 0; jj < nScenes && nselected < 64; ++jj) {
      if(fnmatch(sceneNames[ii], scenes[jj].name, 0) == 0) { selected[nselected++] = &scenes[jj]; found = 1; }
    }
    if(!found) { fprintf(stderr, "nvgtest: no scene matches %s\n", sceneNames[ii]); return 2; }
  }

  if(!sw && !initGL())
    return 1;
  fprintf(stderr, "nvgtest: %s (%s); %s text, srgb %d, %dx%d\n", sw ? "nanovg_sw" : (const char*)glGetString(GL_RENDERER),
      sw ? "XC" : (const char*)glGetString(GL_VERSION), sdf ? "SDF" : "summed", srgb, w, h);

  Renderer r;
  memset(&r, 0, sizeof(r));
  r.w = w; r.h = h; r.srgb = srgb; r.repeat = repeat > 0 ? repeat : 1; r.gpuRepeat = gpuRepeat > 0 ? gpuRepeat : 1;
  int nvgFlags = NVG_NO_FONTSTASH | NVG_AUTOW_DEFAULT | (sdf ? NVG_SDF_TEXT : 0) | (srgb ? NVG_SRGB : 0);
  if(sw) {
    r.vg = nvgswCreate(nvgFlags | NVGSW_PATHS_XC);
    r.swbuf = malloc((size_t)w*h*4);
    if(r.vg) nvgswSetFramebuffer(r.vg, r.swbuf, w, h, 0, 8, 16, 24);
  }
  else
    r.vg = nvglCreate(nvgFlags);
  if(!r.vg) { fprintf(stderr, "nvgtest: nvg create failed\n"); return 1; }
  FONScontext* fons = createFontstash(nvgFlags, 2*atlasPx);
  nvgSetFontStash(r.vg, fons);
  if(!fontPath)
    fontPath = findData("Roboto-Regular.ttf");
  if(!fontPath || nvgCreateFont(r.vg, "sans", fontPath) < 0) {
    fprintf(stderr, "nvgtest: unable to load font %s\n", fontPath ? fontPath : "Roboto-Regular.ttf");
    return 1;
  }
  nvgFontFace(r.vg, "sans");
  nvgAtlasTextThreshold(r.vg, atlasPx);
  if(!sw) {
    r.fb = nvgluCreateFramebuffer(r.vg, w, h, NVGLU_NO_NVG_IMAGE | (srgb ? NVG_IMAGE_SRGB : 0));
    if(!r.fb) { fprintf(stderr, "nvgtest: unable to create framebuffer\n"); return 1; }
    nvgluSetFramebufferSRGB(srgb);
  }

  int result = 0;
  if(!bench)
    mkdir(outDir, 0755);
  for(int ii = 0; ii < nselected; ++ii) {
    if(bench)
      benchScene(&r, selected[ii], bench, warmup);
    else {
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s.png", outDir, selected[ii]->name);
      renderScene(&r, selected[ii], NULL);
      if(!saveScene(&r, path)) {
        fprintf(stderr, "nvgtest: error writing %s\n", path);
        result = 1;
      }
    }
  }
  // NVGTEST_DUMP_ATLAS=<file.png>: save font atlas (SDF only; summed atlas is float) after rendering, for debugging
  const char* atlasPath = getenv("NVGTEST_DUMP_ATLAS");
  if(atlasPath && sdf) {
    int aw = 0, ah = 0;
    const unsigned char* data = fonsGetTextureData(fons, &aw, &ah);
    if(data && !stbi_write_png(atlasPath, aw, ah, 1, data, aw))
      fprintf(stderr, "nvgtest: error writing %s\n", atlasPath);
  }

  GLenum err = sw ? GL_NO_ERROR : glGetError();
  if(err != GL_NO_ERROR) {
    fprintf(stderr, "nvgtest: GL error 0x%x\n", err);
    result = 1;
  }

  if(sw) {
    nvgswDelete(r.vg);
    free(r.swbuf);
  }
  else {
    nvgluDeleteFramebuffer(r.fb);
    nvglDelete(r.vg);
    shutdownGL();
  }
  return result;
}
