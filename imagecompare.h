#pragma once

#include <string>
#include "ulib/image.h"

// Fuzzy image comparison for render tests
// Each pixel is compared to the same pixel in the other image; a pixel that differs by more than `tol` (max over
//  RGBA channels, 0 - 255) is "significant" unless it is within `tol` of some pixel in the 3x3 neighborhood of the
//  same position in the other image, checked in both directions (out vs. ref and ref vs. out).  So sub-pixel
//  shifts of edges (antialiasing changes) and small color changes are not significant, but a missing or extra
//  stroke is (the symmetric check is what catches missing thin lines).
// Result: PASS if images are identical, MINOR if no more than `maxSig` significant pixels, otherwise FAIL.

struct ImageCompareParams {
  int tol = 32;
  int maxSig = 0;
};

struct ImageCompareResult {
  enum Verdict { PASS = 0, MINOR = 1, FAIL = 2 } verdict = PASS;
  bool sizeMismatch = false;
  int nDiff = 0;  // pixels differing at all
  int nSig = 0;  // significant differences
  int maxDiff = 0;  // max per-channel difference over all pixels
  Image diffImg = Image(0, 0);  // faded ref w/ minor diffs in orange, significant in red; only created if images differ

  std::string summary() const;
  static const char* verdictStr(Verdict v);
};

ImageCompareResult compareImages(const Image& ref, const Image& out, const ImageCompareParams& params = {});

Image loadImageFile(const std::string& filename);
bool saveImagePNG(const Image& img, const std::string& filename);
