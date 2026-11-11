#pragma once

#include <string>
#include "pugixml.hpp"

// Semantic comparison of saved documents (XHTML w/ SVG) for tests
// Both documents are parsed and compared element by element (whitespace-only text is ignored):
// - PASS: text identical
// - MINOR: same elements, attributes and text except for formatting (whitespace, attribute order, number format
//    such as "1.000" vs "1") and numbers differing by at most `numTol`
// - FAIL: anything else (incl. failure to parse)
// Numbers are found in attribute values and text by scanning for decimal numbers, so non-numeric parts (path
//  commands, names, ids) must match exactly.  Since numTol < 1, integers (ids, colors, etc.) must match exactly.
//  Values starting with "data:" are compared as opaque strings.

struct DocCompareParams {
  double numTol = 0.05;
  bool svgOnly = false;  // compare only first <svg> element (e.g. for sync test, where <head> differs)
};

struct DocCompareResult {
  enum Verdict { PASS = 0, MINOR = 1, FAIL = 2 } verdict = PASS;
  int nFormat = 0;  // values differing only in formatting
  int nNumeric = 0;  // values w/ numbers differing within tolerance
  double maxNumDiff = 0;
  bool whitespace = false;  // documents differ, but not in any compared value (i.e. whitespace or attribute order)
  std::string failWhere;  // location and description of first difference causing FAIL
  std::string minorExample;  // first MINOR difference

  std::string summary() const;
};

DocCompareResult compareDocs(const std::string& ref, const std::string& out, const DocCompareParams& params = {});
