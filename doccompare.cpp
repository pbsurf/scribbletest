#include "doccompare.h"
#include "ulib/stringutil.h"
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>

typedef DocCompareResult R;

// split value into alternating text and numbers: returns non-numeric text w/ each number replaced by '\x01'
static std::string tokenize(const char* s, std::vector<double>& nums)
{
  std::string text;
  const char* p = s;
  while(*p) {
    bool numstart = isdigit((unsigned char)p[0])
        || ((p[0] == '-' || p[0] == '+' || p[0] == '.') && isdigit((unsigned char)p[1]))
        || ((p[0] == '-' || p[0] == '+') && p[1] == '.' && isdigit((unsigned char)p[2]));
    // a digit following a letter or '#' is not a separate number (e.g. ids, hex colors)
    if(numstart && p > s && (isalpha((unsigned char)p[-1]) || p[-1] == '#' || p[-1] == '_')
        && !strchr("MmLlHhVvCcSsQqTtAaZzEe", p[-1])) {
      // treat rest of identifier as text
      while(isalnum((unsigned char)*p) || *p == '_') text += *p++;
      continue;
    }
    if(numstart) {
      char* end = NULL;
      double v = strtod(p, &end);
      if(end && end > p) {
        nums.push_back(v);
        text += '\x01';
        p = end;
        continue;
      }
    }
    text += *p++;
  }
  return text;
}

struct DocComparer {
  const DocCompareParams& params;
  DocCompareResult& res;
  DocComparer(const DocCompareParams& p, DocCompareResult& r) : params(p), res(r) {}

  void fail(const std::string& where, const std::string& what)
  {
    if(res.verdict != R::FAIL) {
      res.verdict = R::FAIL;
      res.failWhere = where + ": " + what;
    }
  }

  void minor(const std::string& where, const char* ref, const char* out)
  {
    if(res.verdict == R::PASS)
      res.verdict = R::MINOR;
    if(res.minorExample.empty()) {
      std::string r(ref), o(out);
      if(r.size() > 80) r = r.substr(0, 80) + "...";
      if(o.size() > 80) o = o.substr(0, 80) + "...";
      res.minorExample = where + ": \"" + r + "\" -> \"" + o + "\"";
    }
  }

  void compareValues(const std::string& where, const char* ref, const char* out)
  {
    if(strcmp(ref, out) == 0) return;
    if(strncmp(ref, "data:", 5) == 0 || strncmp(out, "data:", 5) == 0)
      return fail(where, "data differs");
    std::vector<double> rnums, onums;
    std::string rtext = tokenize(ref, rnums);
    std::string otext = tokenize(out, onums);
    if(rtext != otext || rnums.size() != onums.size()) {
      std::string r(ref), o(out);
      if(r.size() > 120) r = r.substr(0, 120) + "...";
      if(o.size() > 120) o = o.substr(0, 120) + "...";
      return fail(where, "\"" + r + "\" -> \"" + o + "\"");
    }
    double maxdiff = 0;
    for(size_t ii = 0; ii < rnums.size(); ++ii)
      maxdiff = std::max(maxdiff, std::abs(rnums[ii] - onums[ii]));
    if(maxdiff > params.numTol + 1e-9)  // epsilon for floating point error in difference
      return fail(where, fstring("numbers differ by up to %g: \"%s\" -> \"%s\"", maxdiff,
          std::string(ref).substr(0, 120).c_str(), std::string(out).substr(0, 120).c_str()));
    if(maxdiff > 0) {
      ++res.nNumeric;
      res.maxNumDiff = std::max(res.maxNumDiff, maxdiff);
    }
    else
      ++res.nFormat;
    minor(where, ref, out);
  }

  static bool isThumbnail(const pugi::xml_node& n)
  {
    return strcmp(n.attribute("id").value(), "thumbnail") == 0;
  }

  static std::vector<pugi::xml_node> children(const pugi::xml_node& node)
  {
    std::vector<pugi::xml_node> res;
    for(pugi::xml_node c : node.children()) {
      if(c.type() == pugi::node_doctype || c.type() == pugi::node_declaration || isThumbnail(c))
        continue;
      res.push_back(c);
    }
    return res;
  }

  void compareNodes(const pugi::xml_node& ref, const pugi::xml_node& out, const std::string& where)
  {
    if(res.verdict == R::FAIL) return;
    if(ref.type() != out.type() || strcmp(ref.name(), out.name()) != 0)
      return fail(where, fmtNode(ref) + " -> " + fmtNode(out));
    if(ref.type() == pugi::node_pcdata || ref.type() == pugi::node_cdata || ref.type() == pugi::node_comment)
      return compareValues(where + "/text()", ref.value(), out.value());

    // attributes: same set of names required; order differences are formatting
    std::vector<pugi::xml_attribute> rattrs(ref.attributes_begin(), ref.attributes_end());
    std::vector<pugi::xml_attribute> oattrs(out.attributes_begin(), out.attributes_end());
    for(const pugi::xml_attribute& ra : rattrs) {
      pugi::xml_attribute oa = out.attribute(ra.name());
      if(!oa)
        return fail(where, fstring("attribute %s removed", ra.name()));
      compareValues(where + "/@" + ra.name(), ra.value(), oa.value());
      if(res.verdict == R::FAIL) return;
    }
    for(const pugi::xml_attribute& oa : oattrs) {
      if(!ref.attribute(oa.name()))
        return fail(where, fstring("attribute %s added (\"%s\")", oa.name(), oa.value()));
    }

    std::vector<pugi::xml_node> rkids = children(ref), okids = children(out);
    size_t n = std::min(rkids.size(), okids.size());
    for(size_t ii = 0; ii < n && res.verdict != R::FAIL; ++ii) {
      compareNodes(rkids[ii], okids[ii], where + fstring("/%s[%d]",
          rkids[ii].type() == pugi::node_element ? rkids[ii].name() : "#text", int(ii+1)));
    }
    if(res.verdict != R::FAIL && rkids.size() != okids.size()) {
      const pugi::xml_node& extra = rkids.size() > okids.size() ? rkids[n] : okids[n];
      fail(where, fstring("%d children -> %d children (first %s: %s)", int(rkids.size()), int(okids.size()),
          rkids.size() > okids.size() ? "missing" : "extra", fmtNode(extra).c_str()));
    }
  }

  static std::string fmtNode(const pugi::xml_node& n)
  {
    if(n.type() == pugi::node_element) {
      std::string s = std::string("<") + n.name();
      int nattr = 0;
      for(pugi::xml_attribute a : n.attributes()) {
        if(++nattr > 4) { s += " ..."; break; }
        std::string v = a.value();
        s += fstring(" %s=\"%s\"", a.name(), v.size() > 40 ? (v.substr(0, 40) + "...").c_str() : v.c_str());
      }
      return s + ">";
    }
    std::string v = n.value();
    return "text \"" + (v.size() > 60 ? v.substr(0, 60) + "..." : v) + "\"";
  }
};

static pugi::xml_node findSvg(const pugi::xml_node& node)
{
  return node.find_node([](pugi::xml_node n){ return strcmp(n.name(), "svg") == 0; });
}

DocCompareResult compareDocs(const std::string& ref, const std::string& out, const DocCompareParams& params)
{
  DocCompareResult res;
  if(ref == out)
    return res;
  pugi::xml_document rdoc, odoc;
  // parse_default drops whitespace-only text
  if(!rdoc.load_buffer(ref.data(), ref.size()) || !odoc.load_buffer(out.data(), out.size())) {
    res.verdict = R::FAIL;
    res.failWhere = "unable to parse document";
    return res;
  }
  pugi::xml_node rroot = rdoc, oroot = odoc;
  if(params.svgOnly) {
    rroot = findSvg(rdoc);
    oroot = findSvg(odoc);
    if(!rroot || !oroot) {
      res.verdict = R::FAIL;
      res.failWhere = "<svg> not found";
      return res;
    }
  }
  DocComparer cmp(params, res);
  cmp.compareNodes(rroot, oroot, "");
  // documents differ, but not in anything compared: whitespace or attribute order
  if(res.verdict == R::PASS && !params.svgOnly) {
    res.verdict = R::MINOR;
    res.whitespace = true;
  }
  return res;
}

std::string DocCompareResult::summary() const
{
  if(verdict == PASS) return "identical";
  if(verdict == FAIL) return failWhere;
  std::vector<std::string> parts;
  if(nNumeric > 0) parts.push_back(fstring("%d values w/ numeric differences (max %g)", nNumeric, maxNumDiff));
  if(nFormat > 0) parts.push_back(fstring("%d values w/ formatting differences", nFormat));
  if(whitespace) parts.push_back("whitespace or attribute order differences");
  std::string s = joinStr(parts, ", ");
  if(!minorExample.empty()) s += "; e.g. " + minorExample;
  return s;
}
