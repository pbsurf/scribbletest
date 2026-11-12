#include "scribbletest.h"
#include <fstream>
#include <thread>
#include <functional>

#include "ugui/svggui_platform.h"
#include "usvg/svgparser.h"
#include "usvg/svgwriter.h"
#include "application.h"
#include "strokebuilder.h"
#include "scribblesync.h"
#include "scribbleapp.h"
#include "scribbleinput.h"
#include "page.h"
#include "redrawcheck.h"
#include "headless.h"
#include "ugui/svggui.h"

// Ideally, these tests should be run under valgrind to help check for memory leaks
// to accept new output as refs, use `WriteTest --update=<test numbers>` (see testmain.cpp)

// runAll run times (tests 0 - 10):
//  Debian-NDK VM (Qt 4.7 gcc): 440 ms (debug build), 12600 ms (valgrind, 2nd run)
//  Dev-XP VM (Qt 4.8.4, MSVC 2010): 1300 ms (debug), 453 ms (release, standalone)
//  X61T: TBD
// standalone means not started from Qt Creator (so no qDebug output)

// Known sync test issues: 9 (randomStr for bookmark id), 12 (numerical), 14 (numerical)
ScribbleTest::ScribbleTest(const std::string& refpath, const std::string& outpath)
{
  scribbleConfig = new ScribbleConfig();
  scribbleConfig->set("singleFile", true);
  scribbleConfig->set("reduceFileSize", 0);
  // don't use panBorder
  scribbleConfig->set("panFromEdge", false);
  scribbleConfig->set("popupToolbar", false);
  // disable input smoothing
  scribbleConfig->set("inputSmoothing", 0);
  // include thumbnail in test output as a check of rendering
  //  but don't draw page num since fonts are different on different platforms
  scribbleConfig->set("saveThumbnail", 2);
  // don't rely on default page setup
  scribbleConfig->set("pageWidth", 768.0);
  scribbleConfig->set("pageHeight", 1024.0);
  scribbleConfig->set("xRuling", 0.0);
  scribbleConfig->set("yRuling", 40.0);
  scribbleConfig->set("marginLeft", 100.0);
  scribbleConfig->set("ruleColor", Color(Color::BLUE).argb());
  //scribbleConfig->migrateConfig();  // convert xRuling, yRuling, marginLeft to RuleLayer
  // disable DPI detection
  scribbleConfig->set("screenDPI", 150);  //int(ScribbleView::DEFAULT_DPI));
  // disable pen detection (so that mouse input doesn't get disabled)
  scribbleConfig->set("penType", ScribbleInput::DETECTED_PEN);
  //Painter::initPaintSystem();
  scribbleMode = new ScribbleMode(scribbleConfig);
  // NOTE: if you remove or change this, you must find another way to init erase, select, ins space modes!
  scribbleMode->setRuled(true);
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc = new ScribbleDoc(ScribbleApp::app, scribbleConfig, scribbleMode);
  scribbleArea = new ScribbleArea();
  scribbleDoc->addArea(scribbleArea);
  bookmarkArea = new BookmarkView(scribbleConfig, scribbleDoc);
  //bookmarkArea->setScribbleDoc(scribbleDoc);
  syncSlave = NULL;
  waitForSyncDone = false;
  refPath = refpath;
  outPath = outpath;
  // setup ScribbleArea
  scribbleDoc->newDocument();
  screenRect = Rect::ltwh(0,0,600,800);
  screenImg = new Image(screenRect.width(), screenRect.height());
  screenPaint = new Painter(Painter::PAINT_SW, screenImg);  //| Painter::SRGB_AWARE
  scribbleArea->screenRect = screenRect;
  screenPaint->beginFrame();
  scribbleArea->doPaintEvent(screenPaint);  // ensure that ScribbleView::imgPaint is inited
  screenPaint->endFrame();
  //scribbleArea->setGeometry(Painter::rectToQRect(screenRect));
  swbXML.append_child("swb");
  swbXML.first_child().append_attribute("token").set_value("SCRIBBLE_SYNC_TEST");
  swbXML.first_child().append_attribute("name");
}

ScribbleTest::ScribbleTest(ScribbleDoc* sd, BookmarkView* bv, ScribbleMode* sm, const std::string& outpath)
{
  outPath = outpath;
  scribbleDoc = sd;
  scribbleArea = sd->activeArea;
  scribbleMode = sd->scribbleMode;
  bookmarkArea = bv;
  scribbleConfig = NULL;
  screenImg = NULL;
  screenPaint = NULL;
  syncSlave = NULL;
}

ScribbleTest::~ScribbleTest()
{
  if(syncSlave) {
    // ensure disconnect SDL event has been processed before deleting scribbleDoc!
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  //SDL_Delay(20);
    ScribbleApp::processEvents();
    delete syncSlave;
  }
  if(scribbleConfig) {
    delete screenPaint;
    delete screenImg;
    delete bookmarkArea;
    delete scribbleDoc;
    delete scribbleArea;
    delete scribbleMode;
    delete scribbleConfig;
  }
}

static void distributeTransform(Document* doc)
{
  for(Page* page : doc->pages) {
    for(SvgNode* node : page->contentNode->children()) {
      if(node->type() == SvgNode::PATH && node->hasTransform()) {
        SvgPath* path = static_cast<SvgPath*>(node);
        path->m_path.transform(path->getTransform());
        float w = path->getFloatAttr("stroke-width");
        if(!std::isnan(w))
          path->setAttr<float>("stroke-width", w*path->getTransform().avgScale());
        path->setTransform(Transform2D());
      }
    }
  }
}

void ScribbleTest::startSyncTest(int testnum)
{
  syncTestNum = testnum;
  scribbleDoc->newDocument();
  swbXML.first_child().attribute("name").set_value(fstring("testdoc%d", testnum).c_str());
  scribbleDoc->openSharedDoc(scribbleConfig->String("syncServer"), swbXML.first_child(), false);
  scribbleDoc->scribbleSync->userMessage = [this](std::string msg, int level){ syncSlaveMsg(msg, level); };
  waitForSyncDone = true;
}

void ScribbleTest::checkStrokeMap(ScribbleDoc* doc, const char* msg)
{
  int n = 0;
  for(unsigned int pp = 0; pp < doc->document->pages.size(); pp++)
    n += doc->document->pages[pp]->strokeCount();
  int m = doc->scribbleSync->strokemap.size();
  if(n != m)
    SCRIBBLE_LOG("%s - strokemap count incorrect: %d strokes in document, %d strokes in strokemap", msg, n, m);
}

// wait on disconnect message to save and check slave document and advance to next test
// note that the slave doesn't load any documents
void ScribbleTest::syncSlaveMsg(std::string msg, int level)
{
  if(!StringRef(msg).contains("testuser1 disconnected"))
    return;

  // check strokemap count
  checkStrokeMap(scribbleDoc, "slave (testuser2)");
  distributeTransform(scribbleDoc->document);
  // write output file
  std::string outfile = fstring("%s/test%d_out.html", outPath.c_str(), syncTestNum);
  std::string reffile = fstring("%s/test%d_ref.html", refPath.c_str(), syncTestNum);
  scribbleDoc->saveDocument(outfile.c_str());
  // compare output to reference; tests of one-file-per-page must handle the svg files themselves
  DocCompareParams params;
  params.svgOnly = true;
  DocCompareResult res = compareDocs(readFile(reffile.c_str()), readFile(outfile.c_str()), params);
  if(res.verdict == DocCompareResult::PASS)
    scribbleDoc->document->deleteFiles();  //remove(outfile.c_str());
  else if(res.verdict == DocCompareResult::MINOR) {
    SCRIBBLE_LOG("sync test%d MINOR: %s\n", syncTestNum, res.summary().c_str());
    ++nMinor;
  }
  else {
    SCRIBBLE_LOG("sync test%d FAIL: %s\n", syncTestNum, res.summary().c_str());
    ++nFailed;
  }
  waitForSyncDone = false;
}

// remove thumbnail <img> (if present) so that document content can be compared separately from thumbnail
static std::string stripThumbnail(std::string html)
{
  size_t pos = html.find("id='thumbnail'");
  if(pos == std::string::npos) return html;
  size_t start = html.rfind('<', pos);
  size_t end = html.find("/>", pos);
  if(start == std::string::npos || end == std::string::npos) return html;
  return html.erase(start, end + 2 - start);
}

// unified-diff-like excerpt of first differing region of two texts (for report)
static std::string diffExcerpt(const std::string& ref, const std::string& out)
{
  auto rl = splitStr<std::vector>(ref.c_str(), '\n');
  auto ol = splitStr<std::vector>(out.c_str(), '\n');
  size_t ii = 0;
  while(ii < rl.size() && ii < ol.size() && rl[ii] == ol[ii]) ++ii;
  size_t jr = rl.size(), jo = ol.size();
  while(jr > ii && jo > ii && rl[jr-1] == ol[jo-1]) { --jr; --jo; }
  // if first differing lines differ only far from the start, show lines starting shortly before the difference
  size_t col = 0;
  if(ii < rl.size() && ii < ol.size()) {
    while(col < rl[ii].size() && col < ol[ii].size() && rl[ii][col] == ol[ii][col]) ++col;
  }
  size_t start = col > 100 ? col - 60 : 0;
  auto clip = [start](const std::string& line) {
    std::string res = start > 0 ? "..." + (start < line.size() ? line.substr(start) : "") : line;
    return res.size() > 240 ? res.substr(0, 240) + " ..." : res;
  };
  const size_t maxlines = 12;
  std::string res = fstring("@@ line %d @@\n", int(ii+1));
  for(size_t k = ii > 2 ? ii-2 : 0; k < ii; ++k) res += "  " + clip(rl[k]) + "\n";
  for(size_t k = ii; k < jr && k < ii + maxlines; ++k) res += "- " + clip(rl[k]) + "\n";
  if(jr > ii + maxlines) res += fstring("- ... (%d more lines)\n", int(jr - ii - maxlines));
  for(size_t k = ii; k < jo && k < ii + maxlines; ++k) res += "+ " + clip(ol[k]) + "\n";
  if(jo > ii + maxlines) res += fstring("+ ... (%d more lines)\n", int(jo - ii - maxlines));
  return res;
}

static std::string encodePNGStr(const Image& img)
{
  auto enc = img.encodePNG();
  return std::string((const char*)enc.data(), enc.size());
}

// compare test output to refs: document content must match exactly; thumbnail and full resolution render of
//  screen are compared w/ tolerance (see imagecompare.h).  For differences, diagnostic files are written to
//  outPath and images are saved in rep for the HTML report.
void ScribbleTest::checkTestOutput(const std::string& refbase, const std::string& outbase, TestReport& rep)
{
  typedef ImageCompareResult R;
  std::string outfile = outbase + "_out.html";
  std::string reffile = refbase + "_ref.html";
  std::string outhtml = readFile(outfile.c_str());
  std::string refhtml = readFile(reffile.c_str());
  if(refhtml.empty()) {
    rep.verdict = R::FAIL;
    rep.notes.push_back("no ref document");
  }
  else {
    std::string refcontent = stripThumbnail(refhtml);
    std::string outcontent = stripThumbnail(outhtml);
    DocCompareResult doc = compareDocs(refcontent, outcontent, docParams);
    if(doc.verdict != DocCompareResult::PASS) {
      rep.verdict = std::max(rep.verdict, R::Verdict(doc.verdict));
      rep.notes.push_back(std::string("document ") + R::verdictStr(R::Verdict(doc.verdict)) + ": " + doc.summary());
      rep.contentDiff = diffExcerpt(refcontent, outcontent);
    }

    Image refthumb = ScribbleDoc::extractThumbnail(reffile.c_str());
    Image outthumb = ScribbleDoc::extractThumbnail(outfile.c_str());
    R thumb = compareImages(refthumb, outthumb, thumbParams);
    if(thumb.verdict != R::PASS) {
      rep.verdict = std::max(rep.verdict, thumb.verdict);
      rep.notes.push_back("thumbnail " + std::string(R::verdictStr(thumb.verdict)) + ": " + thumb.summary());
      if(!refthumb.isNull()) {
        saveImagePNG(refthumb, outbase + "_thumb_ref.png");
        rep.thumbImgs.push_back({"ref", encodePNGStr(refthumb)});
      }
      if(!outthumb.isNull()) {
        saveImagePNG(outthumb, outbase + "_thumb_out.png");
        rep.thumbImgs.push_back({"out", encodePNGStr(outthumb)});
      }
      if(!thumb.diffImg.isNull()) {
        saveImagePNG(thumb.diffImg, outbase + "_thumb_diff.png");
        rep.thumbImgs.push_back({"diff", encodePNGStr(thumb.diffImg)});
      }
    }
  }

  std::string renderref = refbase + "_render_ref.png";
  R render = compareImages(loadImageFile(renderref), *screenImg, renderParams);
  if(render.verdict != R::PASS) {
    rep.verdict = std::max(rep.verdict, render.verdict);
    std::string refpng = readFile(renderref.c_str());
    rep.notes.push_back(refpng.empty() ? "no render ref"
        : "render " + std::string(R::verdictStr(render.verdict)) + ": " + render.summary());
    if(!refpng.empty())
      rep.renderImgs.push_back({"ref", refpng});
    saveImagePNG(*screenImg, outbase + "_render_out.png");
    rep.renderImgs.push_back({"out", encodePNGStr(*screenImg)});
    if(!render.diffImg.isNull()) {
      saveImagePNG(render.diffImg, outbase + "_render_diff.png");
      rep.renderImgs.push_back({"diff", encodePNGStr(render.diffImg)});
    }
  }

  // keep output document only if it differs from ref (for inspection or to update ref)
  if(outhtml == refhtml)
    scribbleDoc->document->deleteFiles();
}

static std::string htmlEscape(const std::string& s)
{
  std::string res;
  for(char c : s) {
    if(c == '&') res += "&amp;";
    else if(c == '<') res += "&lt;";
    else if(c == '>') res += "&gt;";
    else res += c;
  }
  return res;
}

// self-contained HTML report (images embedded) of tests that did not PASS
void ScribbleTest::writeReport(const std::vector<TestReport>& reports, int ntests, int runms)
{
  typedef ImageCompareResult R;
  std::string body;
  std::vector<std::string> passed;
  // FAIL first, then MINOR, then updated
  auto rank = [](const TestReport& r) { return r.updated ? 0 : r.verdict == R::FAIL ? 2 : 1; };
  std::vector<const TestReport*> sorted;
  for(const TestReport& r : reports) {
    if(r.verdict == R::PASS) passed.push_back(std::to_string(r.test));
    else sorted.push_back(&r);
  }
  std::stable_sort(sorted.begin(), sorted.end(),
      [&](const TestReport* a, const TestReport* b) { return rank(*a) > rank(*b); });

  auto imgRow = [](const char* title, const std::vector<TestReport::Img>& imgs) {
    if(imgs.empty()) return std::string();
    std::string row = fstring("<h3>%s</h3><div class='row' onclick='this.classList.toggle(\"full\")'>", title);
    for(const TestReport::Img& img : imgs)
      row += "<figure><img src='data:image/png;base64," + base64_encode((const unsigned char*)img.png.data(),
          img.png.size()) + "'><figcaption>" + img.label + "</figcaption></figure>";
    return row + "</div>";
  };

  for(const TestReport* r : sorted) {
    const char* v = R::verdictStr(r->verdict);
    std::string cls = r->updated ? "updated" : (r->verdict == R::FAIL ? "fail" : "minor");
    body += fstring("<section class='%s'><h2>test%d: %s</h2><ul>", cls.c_str(), r->test,
        r->updated ? fstring("UPDATED (was %s)", v).c_str() : v);
    for(const std::string& note : r->notes)
      body += "<li>" + htmlEscape(note) + "</li>";
    body += "</ul>";
    if(!r->contentDiff.empty())
      body += "<h3>document</h3><pre>" + htmlEscape(r->contentDiff) + "</pre>";
    body += imgRow("render", r->renderImgs);
    body += imgRow("thumbnail", r->thumbImgs);
    body += "</section>\n";
  }

  std::string html = "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>WriteTest report</title><style>\n"
      "body { font-family: sans-serif; margin: 16px; background: #fff; color: #222; }\n"
      "section { border-left: 6px solid #888; padding: 4px 12px; margin: 16px 0; }\n"
      "section.fail { border-color: #d22; } section.minor { border-color: #e90; } section.updated { border-color: #27c; }\n"
      "h2 { margin: 4px 0; font-size: 1.2em; } h3 { margin: 8px 0 4px; font-size: 1em; color: #555; }\n"
      ".row { display: flex; gap: 12px; cursor: zoom-in; } .row figure { flex: 1 1 0; min-width: 0; margin: 0; }\n"
      ".row img { width: 100%; border: 1px solid #ccc; display: block; image-rendering: pixelated; }\n"
      ".row.full { flex-direction: column; cursor: zoom-out; } .row.full img { width: auto; max-width: none; }\n"
      "figcaption { text-align: center; }\n"
      "pre { background: #f4f4f4; padding: 8px; overflow-x: auto; font-size: 12px; }\n"
      "</style></head><body>\n";
  html += fstring("<h1>WriteTest: %d failed, %d minor, %d updated, %d passed of %d (%d ms)</h1>\n",
      nFailed, nMinor, nUpdated, int(passed.size()), ntests, runms);
  html += "<p>Diff images: faded ref, orange = minor difference, red = significant difference."
      " Click images to toggle full size.</p>\n";
  html += body;
  html += "<p>Passed: " + (passed.empty() ? std::string("none") : joinStr(passed, ", ")) + "</p>\n</body></html>\n";

  reportFile = outPath + "/report.html";
  std::ofstream strm(reportFile.c_str(), std::ios::binary);
  strm << html;
}

// a bunch of integration tests: each test is run on a fresh document (or testN_in.html if present), then output
//  is compared to testN_ref.html and testN_render_ref.png - see checkTestOutput().  Output files are written to
//  outPath only for tests that did not PASS, along with report.html.

void ScribbleTest::runAll(const std::string& syncServer)
{
  bool runsynctest = !syncServer.empty();
  if(runsynctest && !syncRelay.empty())
    swbXML.first_child().append_attribute("relay").set_value(syncRelay.c_str());
  nFailed = 0;
  nMinor = 0;
  nUpdated = 0;
  std::vector<std::string> slFailed, slMinor, slUpdated;
  std::string details;
  std::vector<TestReport> reports;
  void (ScribbleTest::*tests[])() = {
    &ScribbleTest::test0,
    &ScribbleTest::test1,
    &ScribbleTest::test2,
    &ScribbleTest::test3,
    &ScribbleTest::test4,
    &ScribbleTest::test5,
    &ScribbleTest::test6,
    &ScribbleTest::test7,
    &ScribbleTest::test8,
    &ScribbleTest::test9,
    &ScribbleTest::test10,
    &ScribbleTest::test11,
    &ScribbleTest::test12,
    &ScribbleTest::test13,
    &ScribbleTest::test14,
    &ScribbleTest::test15,
    &ScribbleTest::test16,
    &ScribbleTest::synctest01
  };
  unsigned int totaltests = sizeof(tests)/sizeof(tests[0]);
  unsigned int nonsynctests = totaltests - 1;  // last test is for sync only
  unsigned int ntests = runsynctest ? totaltests : nonsynctests;
  std::string badtests;
  for(int t : onlyTests) { if(t < 0 || t >= int(ntests)) badtests += fstring(" %d", t); }
  for(int t : updateTests) { if(t < 0 || t >= int(ntests)) badtests += fstring(" %d", t); }
  if(!badtests.empty()) {
    resultStr = fstring("Invalid test number(s):%s (valid: 0 - %d)", badtests.c_str(), ntests - 1);
    nFailed = 1;
    return;
  }

  // remove output from previous run
  for(unsigned int ii = 0; ii < totaltests; ++ii) {
    for(const char* suffix : {"_out.html", "_render_out.png", "_render_diff.png",
        "_thumb_ref.png", "_thumb_out.png", "_thumb_diff.png"})
      removeFile(fstring("%s/test%d%s", outPath.c_str(), ii, suffix));
  }
  removeFile(outPath + "/report.html");

  // wait for sync condition w/ timeout so that a lost message or connection fails the run instead of hanging it
  int syncTimeouts = 0;
  auto syncWait = [&](std::function<bool()> cond, const char* what) {
    Timestamp t0 = mSecSinceEpoch();
    while(!cond()) {
      if(mSecSinceEpoch() - t0 > 20000) {
        SCRIBBLE_LOG("sync test%d FAIL: timeout waiting for %s\n", int(currTest), what);
        ++syncTimeouts;
        return false;
      }
      ScribbleApp::processEvents();
    }
    return true;
  };

  // create sync slave for sync test
  if(runsynctest) {
    // we are testuser1; slave is testuser2
    scribbleConfig->set("syncServer", syncServer.c_str());
    scribbleConfig->set("syncUser", "testuser1");
    syncSlave = new ScribbleTest(refPath, outPath);
    syncSlave->scribbleConfig->set("syncUser", "testuser2");
    syncSlave->scribbleConfig->set("syncServer", scribbleConfig->String("syncServer"));
    syncSlave->nFailed = 0;
  }

  Dim unitsPerPx = ScribbleView::unitsPerPx;
  ScribbleView::unitsPerPx = 1;
  // these are set in ScribbleApp::loadConfig() ... would be better to pass our scribbleConfig to it
  //RectSelector::HANDLE_SIZE = 4;
  SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 1;
  Page::BLANK_Y_RULING = scribbleConfig->Float("blankYRuling");  // don't serialize timestamp

  Element::SVG_NO_TIMESTAMP = true;
  SvgWriter::DEFAULT_PATH_DATA_REL = false;
  srandpp(1);  // randomStr now used for bookmark ids
  Timestamp runAllTime = mSecSinceEpoch();
  for(unsigned int ii = 0; true; ii++) {
    if(ii < ntests && !onlyTests.empty() && !onlyTests.count(ii))
      continue;
    // wait for sync slave to finish previous test before resetting global state below - slave saves its document
    //  when it sees us disconnect (in newDocument())
    if(syncSlave) {
      doCommand(ID_RESETZOOM);  // cancels any pending action, which can send a final update to slave
      scribbleDoc->newDocument();
      if(!syncWait([&](){ return !syncSlave->waitForSyncDone; }, "slave to finish previous test"))
        break;
    }
    std::string refbase = fstring("%s/test%d", refPath.c_str(), ii);
    std::string outbase = fstring("%s/test%d", outPath.c_str(), ii);
    docParams = DocCompareParams();
    renderParams = ImageCompareParams();
    // reset global state that tests may change (e.g. test15 switches to relative path data), so that each test's
    //  result doesn't depend on which tests ran before it (--only)
    SvgWriter::DEFAULT_PATH_DATA_REL = false;
    doCommand(ID_RESETZOOM);
    scribbleDoc->newDocument();
    // attempt to open input file; fails quietly if not present
    Document::loadresult_t res = scribbleDoc->openDocument((refbase + "_in.html").c_str());
    if(res == Document::LOAD_NONFATAL) {
      scribbleDoc->checkAndClearErrors();
      scribbleDoc->cfg->set("test_loadErrors", true);
    }
    // reset mode
    scribbleMode->setRuled(true);
    scribbleMode->setMode(MODE_STROKE);
    scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_ROUND));
    scribbleDoc->app->bookmarkColor = Color::BLUE;
    // prevent unintended updates while inspecting:
    scribbleDoc->cfg->set("savePrompt", true);

    if(syncSlave) {
      if(ii >= totaltests) {
        syncSlave->scribbleDoc->newDocument();  // disconnect sync
        break;
      }
      // create whiteboard
      //SCRIBBLE_LOG("\nRUNNING TEST %d\n\n", ii);
      swbXML.first_child().attribute("name").set_value(fstring("testdoc%d", ii).c_str());
      scribbleDoc->openSharedDoc(scribbleConfig->String("syncServer"), swbXML.first_child(), true);
      // wait until we connect
      if(!syncWait([&](){ return scribbleDoc->scribbleSync->isSyncActive(); }, "master to connect"))
        break;
      // connect the slave
      syncSlave->startSyncTest(ii);
      // wait until slave connects
      if(!syncWait([&](){ return syncSlave->scribbleDoc->scribbleSync->isSyncActive(); }, "slave to connect"))
        break;
    }
    else if(ii >= nonsynctests)
      break;

    // array of pointers to member functions, wow...
    currTest = ii;
    (this->*tests[ii])();

    // when running sync test, slave writes output
    if(syncSlave) {
      // check strokemap count
      checkStrokeMap(scribbleDoc, "master (testuser1)");
      continue;
    }

    // distribute transform to stroke points to match old behavior ... remove this later(?)
    distributeTransform(scribbleDoc->document);
    // render screen at full resolution (full repaint, so independent of previous tests)
    screenPaint->beginFrame();
    scribbleArea->doPaintEvent(screenPaint);
    screenPaint->endFrame();
    // include dirtyCount in output file as a check on undo system
    int dirtycount = scribbleDoc->document->dirtyCount;
    for(unsigned int pp = 0; pp < scribbleDoc->document->pages.size(); pp++)
      dirtycount += scribbleDoc->document->pages[pp]->dirtyCount;
    scribbleDoc->cfg->set("test_dirtyCount", dirtycount);
    // write output file
    std::string outfile = outbase + "_out.html";
    if(!scribbleDoc->saveDocument(outfile.c_str()))
      SCRIBBLE_LOG("ScribbleTest: error saving %s", outfile.c_str());
    // compare output to reference; tests of one-file-per-page must handle the svg files themselves
    reports.emplace_back();
    TestReport& rep = reports.back();
    rep.test = ii;
    checkTestOutput(refbase, outbase, rep);
    ImageCompareResult::Verdict verdict = rep.verdict;
    if(verdict != ImageCompareResult::PASS && (updateAll || updateTests.count(ii))) {
      // output document is only kept if it differs from ref
      if(getFileSize(outfile) > 0 && !copyFile(outfile, refbase + "_ref.html"))
        rep.notes.push_back("error updating ref document");
      if(!saveImagePNG(*screenImg, refbase + "_render_ref.png"))
        rep.notes.push_back("error updating render ref");
      rep.updated = true;
      slUpdated.push_back(std::to_string(ii));
      nUpdated++;
    }
    else if(verdict == ImageCompareResult::FAIL) {
      slFailed.push_back(std::to_string(ii));
      nFailed++;
    }
    else if(verdict == ImageCompareResult::MINOR) {
      slMinor.push_back(std::to_string(ii));
      nMinor++;
    }
    if(verdict != ImageCompareResult::PASS) {
      details += fstring("test%d %s%s - %s\n", ii, rep.updated ? "UPDATED, was " : "",
          ImageCompareResult::verdictStr(verdict), joinStr(rep.notes, "; ").c_str());
    }
  }
  runAllTime = mSecSinceEpoch() - runAllTime;
  // restore global config
  srandpp(mSecSinceEpoch());
  SvgWriter::DEFAULT_PATH_DATA_REL = true;
  Element::SVG_NO_TIMESTAMP = false;
  ScribbleView::unitsPerPx = unitsPerPx;
  ScribbleApp::app->loadConfig();
  if(syncSlave) {
    nFailed = syncSlave->nFailed + syncTimeouts;
    nMinor = syncSlave->nMinor;
  }
  resultStr = details + fstring("%d tests completed in %d ms: %d failed (%s), %d minor (%s)",
      int(reports.size()), int(runAllTime), nFailed, joinStr(slFailed, ", ").c_str(),
      nMinor, joinStr(slMinor, ", ").c_str());
  if(nUpdated > 0)
    resultStr += fstring(", refs updated for %d (%s)", nUpdated, joinStr(slUpdated, ", ").c_str());
  if(!syncSlave) {
    writeReport(reports, int(reports.size()), int(runAllTime));
    if(int(reports.size()) > int(std::count_if(reports.begin(), reports.end(),
        [](const TestReport& r){ return r.verdict == ImageCompareResult::PASS; })))
      resultStr += "\nReport: " + reportFile;
  }
}

void ScribbleTest::performanceTest()
{
  scribbleArea->gotoPos(0, Point(0,0));
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  scribbleArea->frameCount = 0;
  Timestamp t0 = mSecSinceEpoch();

  // calibration - allocate 64 MB to ensure buffer doesn't fit in cache
  const int bufferSize = 8192*8192;
  Dim* calBuffer = new Dim[8192*8192];
  for(int ii = 0; ii < bufferSize; ++ii)
    calBuffer[ii] = Dim(ii*4 + 5);
  for(int ii = 0; ii < bufferSize; ++ii)
    calBuffer[ii] = sqrt(calBuffer[bufferSize-ii-1]*2.3 + 3.4);
  delete[] calBuffer;

  Timestamp t1 = mSecSinceEpoch();
  int calibrationTime = t1 - t0;
  t0 = t1;

  // draw many strokes
  for(Dim y = 20; y < 800; y += 20) {
    for(Dim x = 20; x < 800; x += 20) {
      platformProcessEvents(false);  // don't block waiting for events
      s3(x, y);
      scribbleArea->doRefresh();
    }
  }
  t1 = mSecSinceEpoch();
  int drawTime = t1 - t0;
  t0 = t1;
  int drawFrames = scribbleArea->frameCount;
  scribbleArea->frameCount = 0;
  // select them all
  doCommand(ID_SELALL);
  // move them around
  for(int ii = 0; ii < 10; ii++) {
    platformProcessEvents(false);  // don't block waiting for events
    s3(400, 400);
    scribbleArea->doRefresh();
  }
  t1 = mSecSinceEpoch();
  int moveTime = t1 - t0;
  t0 = t1;
  int moveFrames = scribbleArea->frameCount;
  scribbleArea->frameCount = 0;
  // scroll up and down a bunch
  scribbleMode->setMode(MODE_PAN);
  ie(400, 800, 0, pen, press);
  for(int ii = 0; ii < 2000; ii++) {
    // cleverly generate a triangle wave
    platformProcessEvents(false);  // don't block waiting for events
    ie(400, 16*ABS((ii % 100) - 50), 0, pen);
    scribbleArea->doRefresh();
  }
  platformProcessEvents(false);  // don't block waiting for events
  ie(0, 0, 1, pen, release);
  scribbleArea->doRefresh();
  platformProcessEvents(false);  // don't block waiting for events
  t1 = mSecSinceEpoch();
  int panTime = t1 - t0;
  t0 = t1;
  int panFrames = scribbleArea->frameCount;

  // save, then load file
  std::string outfile = outPath + "/perftest.html";
  scribbleDoc->cfg->set("singleFile", true);
  scribbleDoc->saveDocument(outfile.c_str());
  t1 = mSecSinceEpoch();
  int saveTime = t1 - t0;
  t0 = t1;
  scribbleDoc->openDocument(outfile.c_str());
  t1 = mSecSinceEpoch();
  int loadTime = t1 - t0;
  t0 = t1;
  int fileSize = getFileSize(outfile);
  scribbleDoc->document->deleteFiles();   //remove(outfile.c_str());

  resultStr = fstring(
      "Calibration: %d ms\nDraw: %d frames in %d ms (%f FPS)\nMove: %d frames in %d ms (%f FPS)\nPan: %d frames in %d ms (%f FPS)"
      "\nSave: %d bytes in %d ms\nLoad: %d ms", calibrationTime,
      drawFrames, drawTime, (drawFrames*1000.0)/drawTime, moveFrames, moveTime, (moveFrames*1000.0)/moveTime,
      panFrames, panTime, (panFrames*1000.0)/panTime, fileSize, saveTime, loadTime);
}

// long stroke
void ScribbleTest::s3(Dim xoffset, Dim yoffset)
{
  static const Dim xang[] = {1, 0, -1, 0};
  static const Dim yang[] = {0, -1, 0, 1};
  ie(xoffset, yoffset, 1, pen, press);
  for(int ii = 1; ii <= 100; ii++)
    ie(xoffset + ii*xang[ii % 4]/10.0, yoffset + ii*yang[ii % 4]/10.0, 1, pen);
  ie(0, 0, 1, pen, release);
}

void ScribbleTest::ie(Dim x, Dim y, Dim p, int src, int ev, int mm)
{
  scribbleArea->scribbleInput->doInputEvent(x, y, p, (inputsource_t)src, (inputevent_t)ev, mm, 0);
  if(redrawFrames) drawFrame();
}

void ScribbleTest::doCommand(int cmd)
{
  scribbleDoc->doCommand(cmd);
  if(redrawFrames) drawFrame();
}

void ScribbleTest::mtinput(inputevent_t ev1, Dim x1, Dim y1, inputevent_t ev2, Dim x2, Dim y2)
{
  InputEvent ievent(INPUTSOURCE_TOUCH, MODEMOD_NONE);
  if(ev1 != INPUTEVENT_NONE)
    ievent.points.push_back(InputPoint(ev1, x1, y1, 1));  // pressure = 1
  if(ev2 != INPUTEVENT_NONE)
    ievent.points.push_back(InputPoint(ev2, x2, y2, 1));
  scribbleArea->scribbleInput->doInputEvent(ievent);
  if(redrawFrames) drawFrame();
}

// draw a simple stroke
void ScribbleTest::ss(Dim offset)
{
  ie(104.4 + offset, 184.4 + offset, 0, pen, press);
  ie(124.5 + offset, 162.2 + offset, 0, pen);
  ie(131.8 + offset, 167.3 + offset, 0, pen);
  ie(0, 0, 0, pen, release);
}

void ScribbleTest::s1(Dim xoffset, Dim yoffset)
{
  ie(104.4 + xoffset, 184.4 + yoffset, 0, pen, press);
  ie(124.5 + xoffset, 162.2 + yoffset, 0, pen);
  ie(131.8 + xoffset, 167.3 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
}

// stroke centered on (0,0)
void ScribbleTest::s2(Dim xoffset, Dim yoffset)
{
  ie(10 + xoffset, -14.8 + yoffset, 0, pen, press);
  ie(-9.9 + xoffset, 0.1 + yoffset, 0, pen);
  ie(9.8 + xoffset, 15.1 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
}

// Filled stroke similar to s2
void ScribbleTest::f2(Dim xoffset, Dim yoffset)
{
  ScribblePen oldpen = *scribbleDoc->app->getPen();
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1.2, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  ie(10 + xoffset, -14.8 + yoffset, 0.3, pen, press);
  ie(-9.9 + xoffset, 0.1 + yoffset, 0.7, pen);
  ie(9.8 + xoffset, 15.1 + yoffset, 0.5, pen);
  ie(0, 0, 0, pen, release);
  scribbleDoc->app->setPen(oldpen);
}

// MultiStroke (HyperRef) consisting of two strokes
void ScribbleTest::hr(Dim xoffset, Dim yoffset)
{
  ie(-10.1 + xoffset, -14.8 + yoffset, 0, pen, press);
  ie(9.8 + xoffset, 0.1 + yoffset, 0, pen);
  ie(-9.7 + xoffset, 15.1 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);

  ie(-10 + 10 + xoffset, -13.8 + yoffset, 0, pen, press);
  ie(9.9 + 10 + xoffset, 0.1 + yoffset, 0, pen);
  ie(-9.8 + 10 + xoffset, 12.4 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);

  scribbleMode->setMode(MODE_SELECTRECT);
  ie(-12 + xoffset, -17 + yoffset, 0, pen, press);
  ie(xoffset, yoffset, 0, pen);
  ie(22 + xoffset, 17 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
  scribbleArea->createHyperRef("http://www.styluslabs.com");
  // have to clear the selection ourselves now
  scribbleArea->clearSelection();
}

void ScribbleTest::s3()
{
  ie(213.000, 51.000, 0.200, 1, 1, 0);
  ie(211.000, 51.000, 0.250, 1, 0, 0);
  ie(210.000, 51.000, 0.320, 1, 0, 0);
  ie(206.000, 52.000, 0.390, 1, 0, 0);
  ie(201.000, 54.000, 0.420, 1, 0, 0);
  ie(194.000, 57.000, 0.450, 1, 0, 0);
  ie(188.000, 60.000, 0.440, 1, 0, 0);
  ie(182.000, 64.000, 0.370, 1, 0, 0);
  ie(179.000, 66.000, 0.390, 1, 0, 0);
  ie(175.000, 68.000, 0.310, 1, 0, 0);
  ie(174.000, 69.000, 0.280, 1, 0, 0);
  ie(173.000, 70.000, 0.220, 1, 0, 0);
  ie(172.000, 71.000, 0.210, 1, 0, 0);
  ie(172.000, 72.000, 0.170, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(176.000, 46.000, 0.190, 1, 1, 0);
  ie(178.000, 47.000, 0.260, 1, 0, 0);
  ie(181.000, 51.000, 0.330, 1, 0, 0);
  ie(185.000, 56.000, 0.370, 1, 0, 0);
  ie(190.000, 63.000, 0.420, 1, 0, 0);
  ie(194.000, 68.000, 0.390, 1, 0, 0);
  ie(197.000, 72.000, 0.340, 1, 0, 0);
  ie(202.000, 78.000, 0.250, 1, 0, 0);
  ie(203.000, 80.000, 0.280, 1, 0, 0);
  ie(205.000, 82.000, 0.230, 1, 0, 0);
  ie(205.000, 83.000, 0.190, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
}

void ScribbleTest::s4()
{
  ie(217.000, 46.000, 0.200, 1, 1, 0);
  ie(215.000, 46.000, 0.250, 1, 0, 0);
  ie(212.000, 48.000, 0.320, 1, 0, 0);
  ie(209.000, 51.000, 0.390, 1, 0, 0);
  ie(203.000, 55.000, 0.420, 1, 0, 0);
  ie(198.000, 58.000, 0.450, 1, 0, 0);
  ie(192.000, 62.000, 0.440, 1, 0, 0);
  ie(190.000, 63.000, 0.370, 1, 0, 0);
  ie(187.000, 66.000, 0.390, 1, 0, 0);
  ie(184.000, 68.000, 0.310, 1, 0, 0);
  ie(181.000, 71.000, 0.280, 1, 0, 0);
  ie(178.000, 72.000, 0.220, 1, 0, 0);
  ie(174.000, 75.000, 0.210, 1, 0, 0);
  ie(172.000, 76.000, 0.170, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(179.000, 54.000, 0.200, 1, 1, 0);
  ie(180.000, 54.000, 0.250, 1, 0, 0);
  ie(181.000, 54.000, 0.320, 1, 0, 0);
  ie(182.000, 55.000, 0.390, 1, 0, 0);
  ie(184.000, 58.000, 0.420, 1, 0, 0);
  ie(187.000, 62.000, 0.450, 1, 0, 0);
  ie(192.000, 68.000, 0.440, 1, 0, 0);
  ie(194.000, 71.000, 0.370, 1, 0, 0);
  ie(198.000, 75.000, 0.390, 1, 0, 0);
  ie(201.000, 80.000, 0.310, 1, 0, 0);
  ie(205.000, 82.000, 0.280, 1, 0, 0);
  ie(208.000, 84.000, 0.220, 1, 0, 0);
  ie(210.000, 85.000, 0.210, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
}

void ScribbleTest::test0()
{
  //scribbleArea->gotoPos(0, Point(0,0));
  ie(104.4, 184.4, 0, pen, press);
  ie(124.5, 162.2, 0, pen);
  ie(131.8, 167.3, 0, pen);
  ie(0, 0, 0, pen, release);

  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(14.4, 56.4, 0, pen, press);
  ie(16.5, 85.2, 0, pen);
  ie(16.5, 99.9, 0, pen);
  ie(15.8, 112.3, 0, pen);
  ie(0, 0, 0, pen, release);

  // pinch zoom (will also test rounding to nearest zoom level)
  mtinput(INPUTEVENT_PRESS, 120, 140, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 135, 128, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 132, 125, INPUTEVENT_PRESS, 345, 327);
  mtinput(INPUTEVENT_MOVE, 144, 137, INPUTEVENT_MOVE, 340, 317);
  mtinput(INPUTEVENT_MOVE, 200, 197, INPUTEVENT_MOVE, 290, 295);
  mtinput(INPUTEVENT_MOVE, 208, 204, INPUTEVENT_MOVE, 278, 275);  // zoom step >2 or <0.5 now cancels zoom
  mtinput(INPUTEVENT_MOVE, 216, 210, INPUTEVENT_MOVE, 264, 256);
  mtinput(INPUTEVENT_RELEASE, 216, 210, INPUTEVENT_MOVE, 262, 256);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_MOVE, 260, 254);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_MOVE, 260, 246);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_RELEASE, 260, 246);

  // test cancellation of touch
  mtinput(INPUTEVENT_PRESS, 120, 140, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 135, 128, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 132, 125, INPUTEVENT_PRESS, 345, 327);
  mtinput(INPUTEVENT_MOVE, 144, 137, INPUTEVENT_MOVE, 340, 317);
  // this doesn't do anything - rather the pen input from s2(...) call cancels and starts drawing
  //mtinput(INPUTEVENT_CANCEL, 200, 197, INPUTEVENT_CANCEL, 290, 295);

  // draw something else
  s2(301, 402);
}

// similar to test0, but loads file test1_in.html - a damaged (truncated) file to test error robustness
void ScribbleTest::test1()
{
  ie(104.4, 184.4, 0, pen, press);
  ie(114.5, 172.2, 0, pen);
  ie(121.8, 177.3, 0, pen);
  ie(0, 0, 0, pen, release);

  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(14.4, 56.4, 0, pen, press);
  ie(16.5, 86.2, 0, pen);
  ie(15.8, 113.3, 0, pen);
  ie(0, 0, 0, pen, release);
}

// draw an "X", select and move it, draw another "X"
void ScribbleTest::test2()
{
  ie(104.4, 184.4, 0, 1, 1, 0);
  ie(114.5, 172.2, 0, 1, 0, 0);
  ie(121.8, 177.3, 0, 1, 0, 0);
  ie(0, 0, 0, 1, -1, 0);
  undo();
  s3();
  ie(154.000, 29.000, 0.000, 1, 1, 2);
  ie(154.000, 30.000, 0.000, 1, 0, 0);
  ie(155.000, 30.000, 0.000, 1, 0, 0);
  ie(157.000, 33.000, 0.000, 1, 0, 0);
  ie(160.000, 36.000, 0.000, 1, 0, 0);
  ie(167.000, 46.000, 0.000, 1, 0, 0);
  ie(174.000, 55.000, 0.000, 1, 0, 0);
  ie(188.000, 67.000, 0.000, 1, 0, 0);
  ie(195.000, 76.000, 0.000, 1, 0, 0);
  ie(203.000, 85.000, 0.000, 1, 0, 0);
  ie(212.000, 96.000, 0.000, 1, 0, 0);
  ie(216.000, 102.000, 0.000, 1, 0, 0);
  ie(221.000, 108.000, 0.000, 1, 0, 0);
  ie(224.000, 112.000, 0.000, 1, 0, 0);
  ie(227.000, 115.000, 0.000, 1, 0, 0);
  ie(228.000, 117.000, 0.000, 1, 0, 0);
  ie(229.000, 117.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(193.000, 65.000, 0.000, 1, 1, 0);
  ie(194.000, 65.000, 0.000, 1, 0, 0);
  ie(197.000, 68.000, 0.000, 1, 0, 0);
  ie(204.000, 73.000, 0.000, 1, 0, 0);
  ie(214.000, 81.000, 0.000, 1, 0, 0);
  ie(228.000, 90.000, 0.000, 1, 0, 0);
  ie(261.000, 116.000, 0.000, 1, 0, 0);
  ie(294.000, 142.000, 0.000, 1, 0, 0);
  ie(329.000, 166.000, 0.000, 1, 0, 0);
  ie(387.000, 218.000, 0.000, 1, 0, 0);
  ie(425.000, 257.000, 0.000, 1, 0, 0);
  ie(483.000, 315.000, 0.000, 1, 0, 0);
  ie(509.000, 363.000, 0.000, 1, 0, 0);
  ie(544.000, 438.000, 0.000, 1, 0, 0);
  ie(559.000, 476.000, 0.000, 1, 0, 0);
  ie(569.000, 535.000, 0.000, 1, 0, 0);
  ie(569.000, 562.000, 0.000, 1, 0, 0);
  ie(559.000, 602.000, 0.000, 1, 0, 0);
  ie(549.000, 625.000, 0.000, 1, 0, 0);
  ie(534.000, 649.000, 0.000, 1, 0, 0);
  ie(524.000, 663.000, 0.000, 1, 0, 0);
  ie(509.000, 683.000, 0.000, 1, 0, 0);
  ie(502.000, 693.000, 0.000, 1, 0, 0);
  ie(491.000, 707.000, 0.000, 1, 0, 0);
  ie(486.000, 714.000, 0.000, 1, 0, 0);
  ie(483.000, 719.000, 0.000, 1, 0, 0);
  ie(480.000, 723.000, 0.000, 1, 0, 0);
  ie(479.000, 726.000, 0.000, 1, 0, 0);
  ie(478.000, 728.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(176.000, 70.000, 0.000, 1, 1, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  s4();
}

// this test is mainly intended for finding memory leaks (so run it under valgrind)
// also, this test will now fail if undo does not preserve z-order
void ScribbleTest::test3()
{
  ss(0);
  hr(300, 300);
  ss(5);
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  undo();
  undo();
  ss(10);
  ss(15);
  undo();
  ss(20);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_DELSEL);
  ss(30);
  ss(40);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_PASTE);
  ss(50);
  undo();
  undo();
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  ss(60);
  undo();
  undo();
  ss(70);
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  undo();
  undo();
  ss(80);
  // none of the tests actually covered pasted strokes - there was a SWB bug
  doCommand(ID_PASTE);
}

// test deletion of page with active selection
void ScribbleTest::test4()
{
  ss(0);
  doCommand(ID_NEXTPAGENEW);
  ss(10);
  ss(20);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_DELPAGE);
  undo();
  // delete strokes from page, then undo that - added in response to SWB bug
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  undo();

  doCommand(ID_NEXTPAGE);  // make sure we're on second page
  //doCommand(ID_PASTE);
  scribbleMode->setMode(MODE_PAGESEL);
  // tap on page
  ie(300, 400, 0, pen, press);
  ie(301, 399, 0, pen);
  ie(0, 0, 0, pen, release);
  doCommand(ID_CUTSEL);

  doCommand(ID_NEXTSCREEN);  // want to paste after first page
  doCommand(ID_PASTE);
  doCommand(ID_PASTE);
  scribbleArea->gotoPage(0);  // restore view
}

// test setting page properties and growing page
void ScribbleTest::test5()
{
  PageProperties props(600, 800, 30, 30, 30, Color::YELLOW, Color(0, 0, 0xFF, 0x7F));
  ss(0);
  ss(10);
  scribbleDoc->setPageProperties(&props, false, true, false);
  ss(20);
  // add new page
  doCommand(ID_NEXTPAGENEW);
  ss(0);
  // grow new page ... off page strokes can't grow page any more, so delete afterwards!
  ie(104.4, 900, 0, pen, press);
  ie(124.5, 803, 0, pen);
  ie(131.8, 900, 0, pen);
  ie(0, 0, 0, pen, release);
  scribbleArea->recentStrokeSelect();
  doCommand(ID_DELSEL);
  ss(20);
}

// test opening file (test6_in.html) to saved position and lasso selection
void ScribbleTest::test6()
{
  // make a lasso selection
  scribbleMode->setMode(MODE_SELECTLASSO);
  ie(475.000, 199.000, 0.000, 1, 1, 0);
  ie(450.000, 175.000, 0.000, 1, 0, 0);
  ie(329.000, 269.000, 0.000, 1, 0, 0);
  ie(435.000, 386.000, 0.000, 1, 0, 0);
  ie(553.000, 268.000, 0.000, 1, 0, 0);
  ie(516.000, 240.000, 0.000, 1, 0, 0);
  ie(436.000, 317.000, 0.000, 1, 0, 0);
  ie(387.000, 269.000, 0.000, 1, 0, 0);
  ie(452.000, 215.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  // tap selection to switch to ruled mode
  ie(442, 262, 0, pen, press);
  ie(441, 262, 0, pen);
  ie(0, 0, 0, pen, release);
  // now move the selection
  ie(441.000, 263.000, 0.000, 1, 1, 0);
  ie(432.000, 319.000, 0.000, 1, 0, 0);
  ie(431.000, 326.000, 0.000, 1, 0, 0);
  ie(431.000, 342.000, 0.000, 1, 0, 0);
  ie(429.000, 399.000, 0.000, 1, 0, 0);
  ie(429.000, 407.000, 0.000, 1, 0, 0);
  ie(424.000, 454.000, 0.000, 1, 0, 0);
  ie(425.000, 473.000, 0.000, 1, 0, 0);
  ie(429.000, 476.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);

  // tests added for dup sel (and for resaving page containing jpeg image)
  doCommand(ID_NEXTPAGE);
  // ruled select in left margin
  scribbleMode->setMode(MODE_SELECTRULED);
  ie(24, 61, 0, pen, press);
  ie(23, 139, 0, pen);
  ie(0, 0, 0, pen, release);
  doCommand(ID_DUPSEL);
  doCommand(ID_PREVPAGE);
}

// test insert space reflow
void ScribbleTest::test7()
{
  // easier to figure out what's going on if screen space and Dim space are the same
  scribbleArea->gotoPos(0, Point(0,0));
  PageProperties props(700, 800, 0, 40, 100);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // create 3 lines of "text";  note: width of ss is 27.4
  for(int offset = 15; offset < 500; offset += 139) {
    s1(offset, 0);
    s1(offset + 30, 0);
    s1(offset + 60, 0);
    s1(offset + 90, 0);
  }
  for(int offset = 5; offset < 500; offset += 141) {
    s1(offset, 40);
    s1(offset + 30, 40);
    s1(offset + 60, 40);
    s1(offset + 90, 40);
  }
  for(int offset = 10; offset < 500; offset += 140) {
    s1(offset, 80);
    s1(offset + 30, 80);
    s1(offset + 60, 80);
    s1(offset + 90, 80);
  }
  // insert space on first line
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(102, 180, 0, pen, press);
  ie(200, 180, 0, pen);
  ie(310, 180, 0, pen);
  ie(0, 0, 0, pen, release);

  // insert some more space, then undo
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(102, 180, 0, pen, press);
  ie(200, 180, 0, pen);
  ie(310, 180, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // test non-ruled insert space
  s1(0, 280);
  s1(30, 280);
  s1(60, 280);
  s1(90, 280);
  scribbleMode->setMode(MODE_INSSPACEVERT);
  ie(102, 420, 0, pen, press);
  ie(103, 440, 0, pen);
  ie(105, 470, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test ruled select and moving strokes between pages
void ScribbleTest::test8()
{
  PageProperties props(700, 800, 0, 40, 100);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // add new page
  s2(400,60);
  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(0, Point(0,0));
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  s2(200, 380);
  s2(150, 420);
  s2(200, 420);
  f2(250, 420);
  s2(600, 420);
  s2(200, 460);
  scribbleMode->setMode(MODE_SELECTRULED);
  ie(110, 420, 0, pen, press);
  ie(275, 420, 0, pen);
  ie(350, 420, 0, pen);
  ie(0, 0, 0, pen, release);
  // hack to workaround new behavior that dropping selection outside screen area will not move
  scribbleArea->screenRect = Rect::ltwh(0, 0, 600, 1400);
  // drag selection to second page
  ie(200, 420, 0, pen, press);
  ie(210, 750, 0, pen);
  ie(215, 1300 - 40, 0, pen);  // -40 accounts for change of interpage gap from 60 to 20
  ie(0, 0, 0, pen, release);
  scribbleArea->screenRect = screenRect;
  // put pen down to clear selection
  s2(500, 1300);

  // return to first page and test erasers
  scribbleArea->gotoPos(0, Point(0,0));
  s2(200, 60);
  s2(150, 100);
  s2(200, 100);
  s2(250, 100);
  s2(600, 100);
  s2(200, 140);
  scribbleMode->setMode(MODE_ERASERULED);
  ie(110, 100, 0, pen, press);
  ie(150, 100, 0, pen);
  ie(200, 100, 0, pen);
  ie(250, 130, 0, pen);
  ie(150, 130, 0, pen);
  ie(0, 0, 0, pen, release);
  scribbleMode->setMode(MODE_ERASESTROKE);
  ie(599, 62, 0, pen, press);
  ie(600, 100, 0, pen);
  ie(602, 139, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test ruled mode tools on unruled page
void ScribbleTest::test9()
{
  scribbleArea->gotoPos(0, Point(0,0));
  PageProperties props(700, 800, 0, 0, 0);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // create 4 lines of "text"
  for(int offset = 125; offset < 600; offset += 125) {
    s2(offset, 41);
    s2(offset + 25, 37);
    s2(offset + 50, 42);
    s2(offset + 75, 40);
  }
  for(int offset = 125; offset < 600; offset += 118) {
    s2(offset, 82);
    s2(offset + 25, 79);
    s2(offset + 50, 77);
    s2(offset + 75, 81);
  }
  for(int offset = 115; offset < 600; offset += 121) {
    s2(offset, 118);
    s2(offset + 25, 117);
    s2(offset + 50, 123);
    s2(offset + 75, 120);
  }
  for(int offset = 120; offset < 600; offset += 120) {
    s2(offset, 160);
    s2(offset + 26, 161);
    s2(offset + 53, 157);
    s2(offset + 76, 159);
  }
  // add a bookmark
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(135, 110, 0, pen, press);
  ie(106, 140, 0, pen);
  ie(60, 160, 0, pen);
  ie(0, 0, 0, pen, release);
  // insert space
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(235, 110, 0, pen, press);
  ie(206, 140, 0, pen);
  ie(184, 195, 0, pen);
  ie(0, 0, 0, pen, release);
  // ruled erase
  scribbleMode->setMode(MODE_ERASERULED);
  ie(60, 90, 0, pen, press);
  ie(140, 92, 0, pen);
  ie(200, 87, 0, pen);
  ie(290, 86, 0, pen);
  ie(385, 89, 0, pen);
  ie(0, 0, 0, pen, release);

  // drop a bookmark off page, then undo - added because this used to cause crash on undo
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(50, 50, 0, pen, press);
  ie(20, 20, 0, pen);
  ie(-100, -100, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();
  redo();

  // must refresh bookmarks so that findBookmark will work
  //bookmarkArea->repaintBookmarks();
  screenPaint->beginFrame();  bookmarkArea->doPaintEvent(screenPaint);  screenPaint->endFrame();

  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  // create hyperref, then convert (along with another stroke) to point to bookmark on first page
  hr(150, 100);
  s2(200, 100);
  doCommand(ID_SELALL);
  scribbleArea->createHyperRef(bookmarkArea->findBookmark(scribbleDoc->document, 20));

  // add bookmark to 2nd page ... id should not be serialized for this one
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(135, 150, 0, pen, press);
  ie(106, 180, 0, pen);
  ie(65, 200, 0, pen);
  ie(0, 0, 0, pen, release);

  // return to first page and copy the bookmark - the was crashing before we fixed clone bug with idStr
  doCommand(ID_PREVPAGE);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  // make sure *cut* and paste preserves id
  doCommand(ID_SELALL);
  doCommand(ID_CUTSEL);
  doCommand(ID_PASTE);
  doCommand(ID_NEXTPAGENEW);
}

// test changing properties and ins space ruled with hyperref
void ScribbleTest::test10()
{
  // test creating new doc with different view modes (added because of crash)
  if(!syncSlave) {
    scribbleConfig->set("viewMode", 0);
    scribbleDoc->newDocument();
    scribbleConfig->set("viewMode", 2);
    scribbleDoc->newDocument();
    scribbleConfig->set("viewMode", 1);
    scribbleDoc->newDocument();
  }

  scribbleArea->gotoPos(0, Point(0,0));
  hr(150, 100);
  s2(200, 100);
  f2(250, 100);
  doCommand(ID_SELALL);

  scribbleDoc->activeArea->setStrokeProperties(StrokeProperties(Color::BLUE, -1));
  scribbleDoc->activeArea->setStrokeProperties(StrokeProperties(Color::INVALID_COLOR, 2));
  scribbleDoc->activeArea->setStrokeProperties(StrokeProperties(Color::INVALID_COLOR, 3));
  undo();

  // added due to bug: failed to create undo items for translated multistrokes
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(104, 100, 0, pen, press);
  ie(195, 98, 0, pen);
  ie(244, 103, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // Note pen not longer changed when selection active
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(104, 100, 0, pen, press);
  ie(195, 98, 0, pen);
  ie(298, 103, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test column detection and free erase
void ScribbleTest::test11()
{
  scribbleArea->gotoPos(0, Point(0,0));
  PageProperties props(1200, 800, 0, 40, 100);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // create 2 columns with 4 lines of "text" each
  for(int col = 0; col < 1000; col += 550) {
    for(int offset = 125; offset < 600; offset += 125) {
      s2(col + offset, 61);
      s2(col + offset + 25, 67);
      s2(col + offset + 50, 62);
      s2(col + offset + 75, 60);
    }
    for(int offset = 130; offset < 600; offset += 119) {
      s2(col + offset, 102);
      s2(col + offset + 25, 99);
      s2(col + offset + 50, 97);
      s2(col + offset + 75, 101);
    }
    for(int offset = 125; offset < 600; offset += 121) {
      s2(col + offset, 138);
      s2(col + offset + 25, 137);
      s2(col + offset + 50, 143);
      s2(col + offset + 75, 140);
    }
    for(int offset = 120; offset < 600; offset += 120) {
      s2(col + offset, 180);
      s2(col + offset + 26, 181);
      s2(col + offset + 53, 177);
      s2(col + offset + 76, 179);
    }
  }
  // need dense points
  //PointDensityFilter::MAX_POINT_DIST = 2;
  // draw column divider - points need to be dense and we've disabled MAX_POINT_DIST
  ie(640, 20, 0, pen, press);
  ie(605, 220, 0, pen);
  ie(0, 0, 0, pen, release);

  // insert space
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(225, 62, 0, pen, press);
  ie(350, 64, 0, pen);
  ie(505, 63, 0, pen);
  ie(0, 0, 0, pen, release);

  // apply free eraser to stroke, filled stroke, hyperref
  // ... also want a stroke which will be broken into > 2 pieces
  ie(140, 420, 0, pen, press);
  ie(145, 340, 0, pen);
  ie(150, 420, 0, pen);
  ie(155, 340, 0, pen);
  ie(160, 420, 0, pen);
  ie(0, 0, 0, pen, release);
  s2(200, 379);
  f2(250, 382);
  hr(300, 381);
  scribbleMode->setMode(MODE_ERASEFREE);
  ie(100, 380, 0, pen, press);
  ie(140, 380, 0, pen);
  ie(200, 380, 0, pen);
  ie(290, 380, 0, pen);
  ie(400, 380, 0, pen);
  ie(0, 0, 0, pen, release);
  //PointDensityFilter::MAX_POINT_DIST = 1000;
}

// test scaling ... test12_in.html includes an image
void ScribbleTest::test12()
{
  // easier to figure out what's going on if screen space and Dim space are the same
  scribbleArea->gotoPos(0, Point(0,0));
  // test added because of crash
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(135, 110, 0, pen, press);
  ie(106, 100, 0, pen);
  ie(60, 60, 0, pen);
  ie(0, 0, 0, pen, release);
  //scribbleDoc->document->drawBookmarks(screenPaint, Rect());
  screenPaint->beginFrame();  bookmarkArea->doPaintEvent(screenPaint);  screenPaint->endFrame();
  undo();
  // back to our regularly scheduled programming...
  s2(200, 179);
  f2(250, 182);
  hr(300, 181);
  doCommand(ID_SELALL);
  // tap the selection to change to move sel free mode (since selectAll no longer ignores moveSelMode) ... nevermind
  //ie(300, 160, 0, pen, press);
  //ie(298, 162, 0, pen);
  //ie(0, 0, 0, pen, release);
  // resize selection
  ie(320, 198, 0, pen, press);
  ie(350, 250, 0, pen);
  ie(450, 300, 0, pen);
  ie(0, 0, 0, pen, release);
  // clear selection
  s2(550, 550);
  doCommand(ID_UNDO);
  doCommand(ID_REDO);
  // apply free eraser to image
  scribbleMode->setMode(MODE_ERASEFREE);
  ie(100, 40, 0, pen, press);
  ie(150, 90, 0, pen);
  ie(200, 140, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test13 - test multiple file documents, since these have been the source of the two data loss bugs found so
//  far in Write
void ScribbleTest::test13()
{
  // setup output path
  std::string sfilename = outPath + u8"/test13_\u4E0B\u5348.html";
  const char* filename = sfilename.c_str();

  // create page 1
  scribbleArea->gotoPos(0, Point(0,0));
  s2(100, 100);
  // create page 2
  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(200, 200);
  // create page 3
  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(2, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(300, 300);
  // undo and redo page creation ... added because of a crash
  doCommand(ID_UNDO);
  doCommand(ID_UNDO);
  doCommand(ID_REDO);
  doCommand(ID_REDO);
  // return to page 1
  scribbleArea->gotoPos(0, Point(0,0));
  // save
  if(!syncSlave) {
    //scribbleArea->cfg->set("singleFile", false);
    scribbleDoc->saveDocument(filename, Document::SAVE_MULTIFILE);
    // reopen; only page 1 should be loaded
    scribbleDoc->openDocument(filename);
  }

  // change page properties - apply to all
  PageProperties props(0, 0, 30, 30, 30, Color::YELLOW, Color(0, 0, 0xFF, 0x7F));
  scribbleDoc->setPageProperties(&props, true, false, false);
  // save and reopen
  if(!syncSlave) {
    scribbleDoc->saveDocument(filename);
    scribbleDoc->openDocument(filename);
  }

  // insert a new page after page 1
  doCommand(ID_PAGEAFTER);
  scribbleArea->gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(150, 150);
  // save and reopen
  if(!syncSlave) {
    scribbleDoc->saveDocument(filename);
    scribbleDoc->openDocument(filename);
  }

  // remove page 3 (formerly page 2)
  doCommand(ID_NEXTPAGENEW);
  doCommand(ID_DELPAGE);
  // save and reopen
  if(!syncSlave) {
    scribbleDoc->saveDocument(filename);
    scribbleDoc->openDocument(filename);
    // load all pages before deleting SVG files
    scribbleDoc->document->ensurePagesLoaded();
    // return to single file config for comparision of file result
    //scribbleArea->cfg->set("singleFile", true);
    // remove SVG files
    scribbleDoc->document->deleteFiles();   //ScribbleDoc::deleteDocument(filename);

    // test other file types
    std::string svgfile = outPath + u8"/test13_\u4E0B\u5348.svg";
    scribbleDoc->saveDocument(svgfile.c_str());
    scribbleDoc->openDocument(svgfile.c_str());
    scribbleDoc->document->ensurePagesLoaded();
    scribbleDoc->document->deleteFiles();   //ScribbleDoc::deleteDocument(svgfile.c_str());

    std::string svgzfile = outPath + u8"/test13_\u4E0B\u5348.svgz";
    scribbleDoc->saveDocument(svgzfile.c_str());
    scribbleDoc->openDocument(svgzfile.c_str());
    scribbleDoc->document->ensurePagesLoaded();
    scribbleDoc->document->deleteFiles();   //ScribbleDoc::deleteDocument(svgzfile.c_str());
  }
}

void ScribbleTest::test14()
{
  scribbleMode->moveSelMode = MODE_MOVESELFREE;  // selectAll no longer ignores moveSelMode
  // easier to figure out what's going on if screen space and Dim space are the same
  scribbleArea->gotoPos(0, Point(0,0));

  s2(200, 179);
  f2(250, 182);
  hr(300, 181);
  doCommand(ID_SELALL);
  // test scale selection with one negative and one positive scale factor
  ie(188, 162, 0, pen, press); // top left corner
  ie(350, 162, 0, pen);
  ie(450, 162, 0, pen);
  ie(0, 0, 0, pen, release);

  // test scale w/ stroke width scaling
  ie(450, 198, 0, pen, press, MODEMOD_PENBTN);
  ie(500, 250, 0, pen, INPUTEVENT_MOVE, MODEMOD_PENBTN);
  ie(550, 300, 0, pen, INPUTEVENT_MOVE, MODEMOD_PENBTN);
  ie(0, 0, 0, pen, release);

  // test rotation
  ie(435, 140, 0, pen, press);
  ie(460, 160, 0, pen);
  ie(490, 190, 0, pen);
  ie(0, 0, 0, pen, release);

  // why not...
  doCommand(ID_UNDO);
  doCommand(ID_REDO);

  // test smoothing and simplification
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1.2, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  scribbleDoc->cfg->set("inputSimplify", 4);
  s3();
  scribbleDoc->cfg->set("inputSmoothing", 4);
  s4();
  scribbleDoc->cfg->set("inputSmoothing", 0);
  scribbleDoc->cfg->set("inputSimplify", 0);
}

// add snap to grid and line drawing tests too!
void ScribbleTest::test15()
{
  // switch to relative path data
  SvgWriter::DEFAULT_PATH_DATA_REL = true;

  scribbleArea->gotoPos(0, Point(0,0));
  s2(200, 179);
  f2(250, 182);
  hr(300, 181);

  // make sure realToStr doesn't write "-0"
  ie(400, 180, 0, pen, press);
  ie(410, 180.0004, 0, pen);
  ie(420, 180, 0, pen);
  ie(0, 0, 0, pen, release);

  // test z-order for highlighter (draw under)
  scribbleDoc->app->setPen(ScribblePen(Color::YELLOW, 20, ScribblePen::TIP_CHISEL | ScribblePen::DRAW_UNDER));
  s2(250, 190);
  undo();
  redo();

  s2(310, 180);
  scribbleMode->setMode(MODE_ERASESTROKE);
  ie(300, 180, 0, pen, press);
  ie(300, 180, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // test of reflow that should fail w/ cmpRuled bug (not sorting strokes on line left-to-right)
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_ROUND));
  for(int offset = 15; offset < 500; offset += 139) {
    s1(offset + 60, 120);
    s1(offset + 90, 120);
    s1(offset, 120);
    s1(offset + 30, 120);
  }
  // insert space
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(102, 300, 0, pen, press);
  ie(200, 300, 0, pen);
  ie(310, 300, 0, pen);
  ie(0, 0, 0, pen, release);

  // test erase on negative insert space
  s2(180 + 60, 420);
  s2(180 + 90, 420);
  s2(180,      420);
  s2(180 + 30, 420);

  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(180 + 45, 420, 0, pen, press);
  ie(180, 420, 0, pen);
  ie(140, 420, 0, pen);
  ie(0, 0, 0, pen, release);

  SvgDocument* svgDoc = SvgParser().parseFragment(
      "<g stroke='green' transform='translate(50, 50)'><rect fill='rgba(0, 0, 255, 0.8)' stroke='none' x='0' y='0' width='20' height='20'/>"
      "<path fill='red' d='M10 10 l2 4 1 2 0 3 -1 1 -1 0z'/></g>");
  Clipboard* clip = scribbleDoc->app->importExternalDoc(svgDoc);
  scribbleDoc->app->clipboard.reset(clip);
  scribbleDoc->app->clipboardPage = NULL;

  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  undo();
  undo();
  doCommand(ID_PASTE);
}

// touch input draws with singleTouchMode = INPUTMODE_DRAW, but not with INPUTMODE_NONE (formerly inputTest())
void ScribbleTest::test16()
{
  ScribbleInput* input = scribbleArea->scribbleInput.get();
  inputmode_t prevmode = input->singleTouchMode;
  input->singleTouchMode = INPUTMODE_DRAW;
  // these two strokes should form a diamond
  ie(150, 150, 0.25, INPUTSOURCE_TOUCH, press);
  ie(200, 200, 0.75, INPUTSOURCE_TOUCH);
  ie(250, 150, 0.5, INPUTSOURCE_TOUCH);
  ie(250, 150, 0, INPUTSOURCE_TOUCH, release);

  ie(150, 150, 0.25, INPUTSOURCE_TOUCH, press);
  ie(200, 100, 0.75, INPUTSOURCE_TOUCH);
  ie(250, 150, 0.5, INPUTSOURCE_TOUCH);
  ie(250, 150, 0, INPUTSOURCE_TOUCH, release);

  input->singleTouchMode = INPUTMODE_NONE;
  // this attempted stroke should NOT appear
  ie(150, 200, 0.25, INPUTSOURCE_TOUCH, press);
  ie(200, 150, 0.75, INPUTSOURCE_TOUCH);
  ie(250, 200, 0.5, INPUTSOURCE_TOUCH);
  ie(250, 200, 0, INPUTSOURCE_TOUCH, release);
  input->singleTouchMode = prevmode;
}

// back and forth test for whiteboard

void ScribbleTest::waitForSync()
{
  // wait until all items are sent and echoed back from server, then wait until slave has received all items (same number of byyes)
  // alternative, wait until master and slave stroke counts are equal?
  ScribbleSync* ss = scribbleDoc->scribbleSync;
  while(ss->canSendHist() || ss->rcvdPos != ss->sentPos || ss->bytesRcvd > syncSlave->scribbleDoc->scribbleSync->bytesRcvd)
    ScribbleApp::processEvents();
}

void ScribbleTest::synctest01()
{
  scribbleArea->gotoPos(0, Point(0,0));
  hr(150, 100);
  s2(200, 100);
  f2(250, 100);
  doCommand(ID_SELALL);

  waitForSync();
  syncSlave->synctest01slave1();

  // move the selection
  ie(200, 100, 0, pen, press);
  ie(210, 120, 0, pen);
  ie(220, 160, 0, pen);
  ie(0, 0, 0, pen, release);

  waitForSync();
  syncSlave->synctest01slave2();
}

void ScribbleTest::synctest01slave1()
{
  scribbleArea->gotoPos(0, Point(0,0));
  s2(550, 550);
  // apply free eraser to image
  scribbleMode->setMode(MODE_ERASERULED);
  ie(245, 101, 0, pen, press);
  ie(270, 99, 0, pen);
  ie(300, 100, 0, pen);
  ie(0, 0, 0, pen, release);
}

void ScribbleTest::synctest01slave2()
{
  // restore the deleted stroke
  undo();

  // disconnect and reconnect to whiteboard (master's currTest is the index of this test)
  startSyncTest(syncTestNum);
  // wait for connection
  while(!scribbleDoc->scribbleSync->isSyncActive())
    ScribbleApp::processEvents();
}

// Redraw tests: unlike the other tests, these use the app's main window (so drawing goes through SvgGui and
//  Application::layoutAndDraw() as in the real app), drawing a frame after every input event w/ the redraw
//  check (redrawcheck.h) comparing each partial redraw to a full redraw.  This catches bugs like drawing outside
//  the dirty rect or not including everything that changed in the dirty rect - e.g. gaps in in-progress stroke.
//  Run at several UI scales, since non-integer scales exposed clip rounding bugs.

void ScribbleTest::drawFrame()
{
  scribbleArea->doRefresh();  // as ScribbleWidget does after each input event
  platformProcessEvents(false);  // dispatch queued events (e.g. window resize) w/o waiting
  Application::layoutAndDraw();
}

// dump file names: <scale>_<step>_frameN_...
void ScribbleTest::redrawStepBegin(const char* step)
{
  compareRenderers();  // state at end of previous step
  redrawStep = step;
  redrawCheckSetPrefix(redrawScale + "_" + step);
}

// GL only: draw current state of window w/ SW renderer and compare to GL, so differences are due only to
//  renderer (both draw from same app state in same process)
void ScribbleTest::compareRenderers()
{
  if(!Application::glRender || redrawStep.empty()) return;
  Painter* glp = Application::painter;
  SvgGui* gui = Application::gui;
  // draw all text as paths for GL too (SW can't use text atlas - see below); full redraw via normal app path
  float atlasthresh = glp->atlasTextThresh;
  glp->setAtlasTextThreshold(0);
  gui->closedWindowBounds.rectUnion(gui->windows.front()->winBounds());
  Application::layoutAndDraw();
  int w = int(glp->deviceRect.width()), h = int(glp->deviceRect.height());
  Image glimg = headlessReadWindow();
  // restore text atlas and redraw so following partial redraws are consistent
  glp->setAtlasTextThreshold(atlasthresh);
  gui->closedWindowBounds.rectUnion(gui->windows.front()->winBounds());
  Application::layoutAndDraw();
  if(glimg.width != w || glimg.height != h) return;
  Image swimg(w, h);
  {
    Painter swp(Painter::PAINT_SW | (glp->createFlags & Painter::SRGB_AWARE), &swimg);
    // no text atlas: fontstash atlas is shared by all nanovg contexts, but only the first gets an atlas texture
    //  (and dirty region is cleared by first to upload), so SW draws all text as paths, unlike app's painter
    swp.deviceRect = Rect::wh(w, h);
    bool prevFullRedraw = gui->fullRedraw;
    gui->fullRedraw = true;
    gui->closedWindowBounds.rectUnion(swp.deviceRect);  // force redraw
    gui->layoutAndDraw(&swp);
    swp.endFrame();
    gui->fullRedraw = prevFullRedraw;
  }
  ImageCompareResult res = compareImages(swimg, glimg, rendererParams);
  std::string name = redrawScale + "_" + redrawStep;
  rendererResults.push_back({name, res.verdict, res.summary(), res.maxDiff});
  // small differences are expected between renderers, so only FAIL is reported
  if(res.verdict == ImageCompareResult::FAIL) {
    PLATFORM_LOG("GL vs SW FAIL %s: %s\n", name.c_str(), res.summary().c_str());
    std::string base = outPath + "/redraw/glsw_" + name;
    saveImagePNG(swimg, base + "sw.png");
    saveImagePNG(glimg, base + "gl.png");
    if(!res.diffImg.isNull())
      saveImagePNG(res.diffImg, base + "diff.png");
  }
}

void ScribbleTest::setUIScale(int dpi)
{
  Application::setupUIScale(dpi);
  // resize window to match new scale, as when window size changes
  SDL_Event event = {0};
  event.type = SDL_WINDOWEVENT;
  event.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
  SDL_PushEvent(&event);
  drawFrame();
}

// stroke w/ n segments in a zigzag, w/ varying pressure; then hover away from end
void ScribbleTest::zigzag(Dim x0, Dim y0, int n, Dim dx, Dim dy)
{
  ie(x0, y0, 0.5, pen, press);
  for(int ii = 1; ii <= n; ++ii)
    ie(x0 + ii*dx, y0 + ((ii/4) % 2 ? 4 - ii % 4 : ii % 4)*dy, 0.3 + 0.7*(ii % 5)/4.0, pen);
  ie(0, 0, 0, pen, release);
  for(int ii = 1; ii <= 5; ++ii)
    ie(x0 + n*dx + ii*3, y0 + ii*4, 0, pen);  // hover
}

void ScribbleTest::redrawScript()
{
  scribbleDoc->newDocument();
  scribbleArea->drawCursor = 2;  // draw hover cursor (as on mobile)
  scribbleMode->setMode(MODE_STROKE);
  drawFrame();

  redrawStepBegin("hover_");
  for(int ii = 0; ii < 20; ++ii)
    ie(150 + ii*7, 120 + ii*3, 0, pen);

  redrawStepBegin("flatpen_");  // default pen (filled stroke)
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1.6, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 0.9, 2.0));
  zigzag(100, 150, 40, 4.3, 7.7);
  redrawStepBegin("roundpen_");  // stroked path
  scribbleDoc->app->setPen(ScribblePen(Color::BLUE, 4, ScribblePen::TIP_ROUND));
  zigzag(110, 220, 30, 5.1, 9.3);
  redrawStepBegin("roundprpen_");
  scribbleDoc->app->setPen(ScribblePen(Color::RED, 3, ScribblePen::TIP_ROUND | ScribblePen::WIDTH_PR, 0.8, 2.0));
  zigzag(105, 290, 30, 5.7, 6.1);
  redrawStepBegin("highlighter_");
  scribbleDoc->app->setPen(ScribblePen(Color(255, 127, 255, 127), 34, ScribblePen::TIP_CHISEL | ScribblePen::DRAW_UNDER));
  zigzag(90, 160, 12, 15.3, 20.2);

  redrawStepBegin("erase_");
  scribbleMode->setMode(MODE_ERASE);
  ie(130, 140, 0, pen, press);
  for(int ii = 1; ii <= 15; ++ii)
    ie(130 + ii*2.3, 140 + ii*12.1, 0, pen);
  ie(0, 0, 0, pen, release);

  redrawStepBegin("select_");
  scribbleMode->setMode(MODE_SELECTRECT);
  ie(95.5, 200.5, 0, pen, press);
  for(int ii = 1; ii <= 10; ++ii)
    ie(95.5 + ii*21.3, 200.5 + ii*13.7, 0, pen);
  ie(0, 0, 0, pen, release);
  redrawStepBegin("movesel_");
  ie(200, 260, 0, pen, press);
  for(int ii = 1; ii <= 10; ++ii)
    ie(200 + ii*3.7, 260 + ii*5.3, 0, pen);
  ie(0, 0, 0, pen, release);
  doCommand(ID_SELALL);
  doCommand(ID_UNDO);

  redrawStepBegin("undo_");
  for(int ii = 0; ii < 4; ++ii)
    doCommand(ID_UNDO);
  for(int ii = 0; ii < 4; ++ii)
    doCommand(ID_REDO);

  redrawStepBegin("inssp_");
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(14.4, 156.4, 0, pen, press);
  ie(16.5, 185.2, 0, pen);
  ie(16.5, 199.9, 0, pen);
  ie(15.8, 212.3, 0, pen);
  ie(0, 0, 0, pen, release);

  redrawStepBegin("pinch_");
  mtinput(INPUTEVENT_PRESS, 120, 140, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 135, 128, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 132, 125, INPUTEVENT_PRESS, 345, 327);
  mtinput(INPUTEVENT_MOVE, 144, 137, INPUTEVENT_MOVE, 340, 317);
  mtinput(INPUTEVENT_MOVE, 200, 197, INPUTEVENT_MOVE, 290, 295);
  mtinput(INPUTEVENT_MOVE, 216, 210, INPUTEVENT_MOVE, 264, 256);
  mtinput(INPUTEVENT_RELEASE, 216, 210, INPUTEVENT_MOVE, 262, 256);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_MOVE, 260, 254);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_RELEASE, 260, 246);

  redrawStepBegin("zoomedpen_");
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1.6, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 0.9, 2.0));
  zigzag(120, 330, 30, 4.9, 8.3);
  doCommand(ID_RESETZOOM);
  redrawStepBegin("end_");
}

void ScribbleTest::runRedrawTests()
{
  // UI scales: 1, a non-integer scale < 1 (where stroke gaps were found), and > 1
  const int dpis[] = {150, 114, 171};
  removeDir(outPath + "/redraw");  // remove output from previous run
  std::vector<std::string> failed;
  std::string details;
  Timestamp t0 = mSecSinceEpoch();
  int nframes = 0;
  redrawFrames = true;
  for(int dpi : dpis) {
    std::string scalestr = fstring("scale%.2f", dpi/150.0);
    redrawScale = scalestr;
    redrawStep.clear();
    setUIScale(dpi);
    redrawCheckEnable(outPath + "/redraw", scalestr + "_", false);
    redrawScript();
    redrawCheckDisable();
    const RedrawCheckStats& st = redrawCheckStats();
    nframes += st.frames;
    if(st.fails > 0 || st.outside > 0) {
      failed.push_back(scalestr);
      details += fstring("redraw FAIL - %s: %d of %d frames differ from full redraw, %d w/ pixels changed outside"
          " dirty rect; first: %s\n", scalestr.c_str(), st.fails, st.frames, st.outside, st.firstFail.c_str());
    }
    // GL vs SW comparisons for this scale (GL only)
    int nrfail = 0, nrcmp = 0, maxrdiff = 0;
    std::string firstrfail;
    for(const RendererResult& r : rendererResults) {
      if(r.name.compare(0, scalestr.size(), scalestr) != 0) continue;
      ++nrcmp;
      maxrdiff = std::max(maxrdiff, r.maxDiff);
      if(r.verdict == ImageCompareResult::FAIL && nrfail++ == 0)
        firstrfail = r.name + ": " + r.summary;
    }
    if(nrfail > 0) {
      failed.push_back(scalestr + " GL vs SW");
      details += fstring("redraw FAIL - %s: GL differs from SW render in %d of %d comparisons; first: %s\n",
          scalestr.c_str(), nrfail, nrcmp, firstrfail.c_str());
    }
    else if(nrcmp > 0)
      details += fstring("GL vs SW %s: %d comparisons, max difference %d (tolerance %d)\n", scalestr.c_str(),
          nrcmp, maxrdiff, rendererParams.tol);
    else if(st.minor > 0)
      details += fstring("redraw MINOR - %s: %d of %d frames w/ small differences from full redraw\n",
          scalestr.c_str(), st.minor, st.frames);
  }
  redrawFrames = false;
  Application::setupUIScale(0);  // restore default
  nFailed = int(failed.size());
  resultStr = details + fstring("Redraw tests: %d frames at %d scales in %d ms: %d failed (%s)%s", nframes,
      int(sizeof(dpis)/sizeof(dpis[0])), int(mSecSinceEpoch() - t0), nFailed, joinStr(failed, ", ").c_str(),
      nFailed > 0 ? ("; see " + outPath + "/redraw/").c_str() : "");
}
