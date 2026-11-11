#include "imagecompare.h"
#include "ulib/fileutil.h"
#include "ulib/stringutil.h"
#include <algorithm>
#include <fstream>
#include <cstdlib>

// max per-channel difference between RGBA pixels
static int pxDiff(const unsigned char* a, const unsigned char* b)
{
  int d = 0;
  for(int c = 0; c < 4; ++c)
    d = std::max(d, std::abs(int(a[c]) - int(b[c])));
  return d;
}

// is pixel `px` within tol of any pixel in 3x3 neighborhood of (x,y) in img?
static bool matchesNeighborhood(const unsigned char* px, const Image& img, int x, int y, int tol)
{
  int w = img.getWidth(), h = img.getHeight();
  const unsigned char* bytes = img.constBytes();
  for(int yy = std::max(0, y-1); yy <= std::min(h-1, y+1); ++yy) {
    for(int xx = std::max(0, x-1); xx <= std::min(w-1, x+1); ++xx) {
      if(pxDiff(px, bytes + 4*(yy*w + xx)) <= tol)
        return true;
    }
  }
  return false;
}

ImageCompareResult compareImages(const Image& ref, const Image& out, const ImageCompareParams& params)
{
  ImageCompareResult res;
  int w = ref.getWidth(), h = ref.getHeight();
  if(w <= 0 || h <= 0 || w != out.getWidth() || h != out.getHeight() || ref.isNull() || out.isNull()) {
    res.sizeMismatch = true;
    res.verdict = ImageCompareResult::FAIL;
    return res;
  }
  const unsigned char* rb = ref.constBytes();
  const unsigned char* ob = out.constBytes();
  if(memcmp(rb, ob, w*h*4) == 0)
    return res;

  res.diffImg = Image(w, h);
  unsigned char* db = res.diffImg.bytes();
  for(int y = 0; y < h; ++y) {
    for(int x = 0; x < w; ++x) {
      int idx = 4*(y*w + x);
      const unsigned char* rp = rb + idx;
      const unsigned char* op = ob + idx;
      unsigned char* dp = db + idx;
      int d = pxDiff(rp, op);
      bool sig = d > params.tol
          && !(matchesNeighborhood(op, ref, x, y, params.tol) && matchesNeighborhood(rp, out, x, y, params.tol));
      if(d > 0) {
        ++res.nDiff;
        res.maxDiff = std::max(res.maxDiff, d);
      }
      if(sig) {
        ++res.nSig;
        dp[0] = 255; dp[1] = 0; dp[2] = 0;  // red
      }
      else if(d > 0) {
        dp[0] = 255; dp[1] = 160; dp[2] = 0;  // orange
      }
      else {
        // faded grayscale of ref
        int lum = (299*rp[0] + 587*rp[1] + 114*rp[2])/1000;
        dp[0] = dp[1] = dp[2] = 255 - (255 - lum)/4;
      }
      dp[3] = 255;
    }
  }
  res.verdict = res.nSig > params.maxSig ? ImageCompareResult::FAIL : ImageCompareResult::MINOR;
  return res;
}

const char* ImageCompareResult::verdictStr(Verdict v)
{
  return v == PASS ? "PASS" : v == MINOR ? "MINOR" : "FAIL";
}

std::string ImageCompareResult::summary() const
{
  if(sizeMismatch)
    return "missing or different size";
  if(verdict == PASS)
    return "identical";
  return fstring("%d px differ (max %d), %d significant", nDiff, maxDiff, nSig);
}

Image loadImageFile(const std::string& filename)
{
  std::vector<char> buff;
  if(!readFile(&buff, filename.c_str()) || buff.empty())
    return Image(0, 0);
  return Image::decodeBuffer(buff.data(), buff.size());
}

bool saveImagePNG(const Image& img, const std::string& filename)
{
  auto enc = img.encodePNG();
  std::ofstream strm(filename.c_str(), std::ios::binary);
  strm.write((const char*)enc.data(), enc.size());
  return bool(strm);
}
