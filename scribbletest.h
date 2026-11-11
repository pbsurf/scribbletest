#ifndef SCRIBBLETEST_H
#define SCRIBBLETEST_H

#include "scribbledoc.h"
#include "bookmarkview.h"
#include "imagecompare.h"
#include "doccompare.h"
#include <set>

class ScribbleTest
{
friend class ScribbleApp;
public:
  // refpath: folder w/ test inputs and refs; outpath: folder for output files (must exist)
  ScribbleTest(const std::string& refpath, const std::string& outpath);
  ScribbleTest(ScribbleDoc* sd, BookmarkView* bv, ScribbleMode* sm, const std::string& outpath);
  ~ScribbleTest();

  // run sync tests (and sync version of all other tests) if syncServer is not empty
  void runAll(const std::string& syncServer = "");
  void performanceTest();
  void syncSlaveMsg(std::string msg, int level);
  // redraw tests use the app's main window, so use ScribbleTest(app->activeDoc(), ...) constructor
  void runRedrawTests();

  // set by caller before runAll(): tests to run (all if empty); tests for which refs should be updated from
  //  output if they don't PASS (or all tests if updateAll)
  std::set<int> onlyTests;
  std::set<int> updateTests;
  bool updateAll = false;

  // results to be read by caller
  std::string resultStr;
  std::string reportFile;  // HTML report written by runAll()
  int nFailed = 0;
  int nMinor = 0;  // tests w/ MINOR render differences (not counted as failures unless --strict)
  int nUpdated = 0;  // tests w/ refs updated (not counted as failed or minor)

private:
  ScribbleConfig* scribbleConfig;
  ScribbleDoc* scribbleDoc;
  ScribbleArea* scribbleArea;
  ScribbleMode* scribbleMode;
  BookmarkView* bookmarkArea;
  Image* screenImg;
  Painter* screenPaint;
  Rect screenRect;
  std::string refPath;
  std::string outPath;
  int currTest = 0;
  // document and full resolution render comparison; reset before each test, so a test can loosen them if needed
  DocCompareParams docParams;
  ImageCompareParams renderParams;
  // thumbnails are only checked loosely, as a test of thumbnail generation
  ImageCompareParams thumbParams = {64, 0};

  struct TestReport {
    struct Img { std::string label; std::string png; };  // png is encoded PNG data
    int test;
    ImageCompareResult::Verdict verdict = ImageCompareResult::PASS;
    bool updated = false;
    std::vector<std::string> notes;
    std::string contentDiff;
    std::vector<Img> renderImgs;
    std::vector<Img> thumbImgs;
  };
  void checkTestOutput(const std::string& refbase, const std::string& outbase, TestReport& rep);
  void writeReport(const std::vector<TestReport>& reports, int ntests, int runms);

  // shared whiteboard testing
  pugi::xml_document swbXML;
  ScribbleTest* syncSlave;
  int syncTestNum;
  bool waitForSyncDone;
  void startSyncTest(int testnum);
  void waitForSync();
  void checkStrokeMap(ScribbleDoc* doc, const char* msg);

  static const int pen = INPUTSOURCE_PEN;
  static const int press = INPUTEVENT_PRESS;
  static const int release = INPUTEVENT_RELEASE;
  void undo() { scribbleDoc->doCommand(ID_UNDO); }
  void redo() { scribbleDoc->doCommand(ID_REDO); }
  void doCommand(int cmd);
  void ie(Dim x, Dim y, Dim p, int src, int ev = 0, int mm = 0);
  void mtinput(inputevent_t ev1, Dim x1, Dim y1, inputevent_t ev2, Dim x2, Dim y2);
  void ss(Dim offset);
  void s1(Dim xoffset, Dim yoffset);
  void s2(Dim xoffset, Dim yoffset);
  void s3(Dim xoffset, Dim yoffset);
  void f2(Dim xoffset, Dim yoffset);
  void hr(Dim xoffset, Dim yoffset);
  void s3();
  void s4();

  void test0();
  void test1();
  void test2();
  void test3();
  void test4();
  void test5();
  void test6();
  void test7();
  void test8();
  void test9();
  void test10();
  void test11();
  void test12();
  void test13();
  void test14();
  void test15();
  void test16();
  void synctest01();
  void synctest01slave1();
  void synctest01slave2();

  // redraw tests
  bool redrawFrames = false;  // draw frame (Application::layoutAndDraw()) after each input event
  std::string redrawScale;
  std::string redrawStep;
  void redrawStepBegin(const char* step);
  // GL vs SW renderer comparison (GL only)
  struct RendererResult { std::string name; ImageCompareResult::Verdict verdict; std::string summary; int maxDiff; };
  std::vector<RendererResult> rendererResults;
  // FAIL if any pixel differs by more than tol (after 3x3 neighborhood matching - see imagecompare.h); max seen w/
  //  llvmpipe and SVGA3D: 49, at pixel-aligned high contrast edges (GL gives a few % coverage to adjacent pixel)
  ImageCompareParams rendererParams = {64, 0};
  void compareRenderers();
  void drawFrame();
  void setUIScale(int dpi);
  void zigzag(Dim x0, Dim y0, int n, Dim dx, Dim dy);
  void redrawScript();
};

#endif
