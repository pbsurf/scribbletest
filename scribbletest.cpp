#include "scribbletest.h"
#include <QtGui>

// Ideally, these tests should be run under valgrind to help check for memory leaks
// renaming out files to refs (Linux):  for i in {0..10}; do mv "test${i}_out.html" "test${i}_ref.html"; done;

// runAll run times (tests 0 - 10):
//  Debian-NDK VM (Qt 4.7 gcc): 440 ms (debug build), 12600 ms (valgrind, 2nd run)
//  Dev-XP VM (Qt 4.8.4, MSVC 2010): 1300 ms (debug), 453 ms (release, standalone)
//  X61T: TBD
// standalone means not started from Qt Creator (so no qDebug output)

// Just get main view working ... deal with bookmarks later
ScribbleTest::ScribbleTest(const char* cfgfile)
{
  scribbleConfig = new ScribbleConfig();
  scribbleConfig->set("singleFile", true);
  // don't write out rule lines - they just clutter the output
  scribbleConfig->set("saveRuleLines", false);
  // don't use panBorder
  scribbleConfig->set("panFromEdge", false);
  //scribbleConfig->loadConfigFile(cfgfile);
  Painter::initPaintSystem();
  scribbleMode = new ScribbleMode(scribbleConfig);
  // NOTE: if you remove or change this, you must find another way to init erase, select, ins space modes!
  scribbleMode->setRuled(true);
  scribbleMode->setMode(MODE_STROKE);
  scribbleArea = new ScribbleArea(NULL, scribbleConfig, scribbleMode);
  bookmarkArea = new BookmarkView(NULL, scribbleConfig, scribbleArea);
  // setup ScribbleArea
  scribbleArea->newDocument();
  // don't add extra points to test strokes
  StrokeBuilder::MAX_POINT_DIST = 1000;

  screenRect = Rect(0,0,600,800);
  screenImg = new QImage(QSize(screenRect.width(), screenRect.height()), QImage::Format_RGB32);
  screenPaint = new Painter(screenImg);
  scribbleArea->screenRect = screenRect;
  scribbleArea->doPaintEvent(screenPaint);  // ensure that ScribbleView::imgPaint is inited
  //scribbleArea->setGeometry(Painter::rectToQRect(screenRect));
}

ScribbleTest::ScribbleTest(ScribbleArea* sa, BookmarkView* bv, ScribbleMode* sm)
{
  scribbleArea = sa;
  bookmarkArea = bv;
  scribbleMode = sm;
  scribbleConfig = NULL;
  screenImg = NULL;
  screenPaint = NULL;
}

ScribbleTest::~ScribbleTest()
{
  if(scribbleConfig) {
    delete screenPaint;
    delete screenImg;
    delete bookmarkArea;
    delete scribbleArea;
    delete scribbleMode;
    delete scribbleConfig;
  }
  bookmarkArea = NULL;
  scribbleArea = NULL;
  scribbleMode = NULL;
  scribbleConfig = NULL;
}

bool ScribbleTest::testCompareFiles(const char* f1, const char* f2)
{
  simplebuf b1;
  simplebuf b2;
  readall(&b1, f1);
  readall(&b2, f2);
  if(b1.pos != b2.pos || b1.pos == 0)
    return false;
  for(unsigned int ii = 0; ii < b1.pos; ii++) {
    if(b1.data[ii] != b2.data[ii])
      return false;
  }
  return true;
}

// a bunch of integration tests ... any "*_out.html" files present after test indicate a failure
// TODO: add some tests to capture scribbleArea->imgPaint->image and compare to a ref

void ScribbleTest::runAll(const char* path)
{
  int nFailed = 0;
  char buff[256];
  char reffile[256];
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
    &ScribbleTest::test12
  };

  Timestamp runAllTime = mSecSinceEpoch();
  for(unsigned int ii = 0; ii < sizeof(tests)/sizeof(tests[0]); ii++) {
    scribbleArea->newDocument();
    // attempt to open input file; fails quietly if not present
    // OH NOES!!! buffer overrun danger here!
    sprintf(buff, "%s/test/test%d_in.html", path, ii);
    scribbleArea->openDocument(buff);
    // reset mode
    scribbleMode->setRuled(true);
    scribbleMode->setMode(MODE_STROKE);
    scribbleArea->setPen(ScribblePen(Painter::BLACK, 1, 0));
    // wow...
    (this->*tests[ii])();

    scribbleArea->doPaintEvent(screenPaint); //, screenRect);
    sprintf(buff, "%s/test/test%d_out.png", path, ii);
    screenImg->save(buff, "png");
    // write output file
    Element::SVG_NO_TIMESTAMP = true;
    sprintf(buff, "%s/test/test%d_out.html", path, ii);
    scribbleArea->saveDocument(buff);
    // compare output to reference; tests of one-file-per-page must handle the svg files themselves
    sprintf(reffile, "%s/test/test%d_ref.html", path, ii);
    if(testCompareFiles(buff, reffile))
      remove(buff);
    else
      nFailed++;
  }
  runAllTime = mSecSinceEpoch() - runAllTime;
  resultStr.format("Tests completed in %d ms with %d failed tests.", int(runAllTime), nFailed);
}

void ScribbleTest::performanceTest()
{
  scribbleArea->gotoPos(0, Point(0,0));
  scribbleMode->setMode(MODE_STROKE);
  scribbleArea->setPen(ScribblePen(Painter::BLACK, 1, 2));
  scribbleArea->frameCount = 0;
  Timestamp t0 = mSecSinceEpoch();
  // draw many strokes
  for(Dim y = 20; y < 800; y += 20) {
    for(Dim x = 20; x < 800; x += 20) {
      QApplication::processEvents();
      s3(x, y);
      scribbleArea->doRefresh();
    }
  }
  Timestamp t1 = mSecSinceEpoch();
  int drawTime = t1 - t0;
  t0 = t1;
  int drawFrames = scribbleArea->frameCount;
  scribbleArea->frameCount = 0;
  // select them all
  scribbleArea->doCommand(ID_SELALL);
  // move them around
  for(int ii = 0; ii < 10; ii++) {
    QApplication::processEvents();
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
    QApplication::processEvents();
    ie(400, 16*ABS((ii % 100) - 50), 0, pen);
    scribbleArea->doRefresh();
  }
  QApplication::processEvents();
  ie(0, 0, 1, pen, release);
  scribbleArea->doRefresh();
  QApplication::processEvents();
  t1 = mSecSinceEpoch();
  int panTime = t1 - t0;
  t0 = t1;
  int panFrames = scribbleArea->frameCount;
  resultStr.format(
      "Draw: %d frames in %d ms (%f FPS)\nMove: %d frames in %d ms (%f FPS)\nPan: %d frames in %d ms (%f FPS)",
      drawFrames, drawTime, (drawFrames*1000.0)/drawTime, moveFrames, moveTime, (moveFrames*1000.0)/moveTime,
      panFrames, panTime, (panFrames*1000.0)/panTime);
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
  scribbleArea->scribbleInput->doInputEvent(x, y, p, (inputsource_t)src, (inputevent_t)ev, mm);
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
  ScribblePen oldpen = scribbleArea->getPen();
  scribbleArea->setPen(ScribblePen(Painter::BLACK, 1.2, 2, false));
  ie(10 + xoffset, -14.8 + yoffset, 0.3, pen, press);
  ie(-9.9 + xoffset, 0.1 + yoffset, 0.7, pen);
  ie(9.8 + xoffset, 15.1 + yoffset, 0.5, pen);
  ie(0, 0, 0, pen, release);
  scribbleArea->setPen(oldpen);
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

void ScribbleTest::test0()
{
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
}

// similar to test0, but loads file test1_in.html
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
  scribbleArea->undo();
  ie(213.000, 51.000, 0.000, 1, 1, 0);
  ie(211.000, 51.000, 0.000, 1, 0, 0);
  ie(210.000, 51.000, 0.000, 1, 0, 0);
  ie(206.000, 52.000, 0.000, 1, 0, 0);
  ie(201.000, 54.000, 0.000, 1, 0, 0);
  ie(194.000, 57.000, 0.000, 1, 0, 0);
  ie(188.000, 60.000, 0.000, 1, 0, 0);
  ie(182.000, 64.000, 0.000, 1, 0, 0);
  ie(179.000, 66.000, 0.000, 1, 0, 0);
  ie(175.000, 68.000, 0.000, 1, 0, 0);
  ie(174.000, 69.000, 0.000, 1, 0, 0);
  ie(173.000, 70.000, 0.000, 1, 0, 0);
  ie(172.000, 71.000, 0.000, 1, 0, 0);
  ie(172.000, 72.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(176.000, 46.000, 0.000, 1, 1, 0);
  ie(178.000, 47.000, 0.000, 1, 0, 0);
  ie(181.000, 51.000, 0.000, 1, 0, 0);
  ie(185.000, 56.000, 0.000, 1, 0, 0);
  ie(190.000, 63.000, 0.000, 1, 0, 0);
  ie(194.000, 68.000, 0.000, 1, 0, 0);
  ie(197.000, 72.000, 0.000, 1, 0, 0);
  ie(202.000, 78.000, 0.000, 1, 0, 0);
  ie(203.000, 80.000, 0.000, 1, 0, 0);
  ie(205.000, 82.000, 0.000, 1, 0, 0);
  ie(205.000, 83.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
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
  ie(217.000, 46.000, 0.000, 1, 1, 0);
  ie(215.000, 46.000, 0.000, 1, 0, 0);
  ie(212.000, 48.000, 0.000, 1, 0, 0);
  ie(209.000, 51.000, 0.000, 1, 0, 0);
  ie(203.000, 55.000, 0.000, 1, 0, 0);
  ie(198.000, 58.000, 0.000, 1, 0, 0);
  ie(192.000, 62.000, 0.000, 1, 0, 0);
  ie(190.000, 63.000, 0.000, 1, 0, 0);
  ie(187.000, 66.000, 0.000, 1, 0, 0);
  ie(184.000, 68.000, 0.000, 1, 0, 0);
  ie(181.000, 71.000, 0.000, 1, 0, 0);
  ie(178.000, 72.000, 0.000, 1, 0, 0);
  ie(174.000, 75.000, 0.000, 1, 0, 0);
  ie(172.000, 76.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(179.000, 54.000, 0.000, 1, 1, 0);
  ie(180.000, 54.000, 0.000, 1, 0, 0);
  ie(181.000, 54.000, 0.000, 1, 0, 0);
  ie(182.000, 55.000, 0.000, 1, 0, 0);
  ie(184.000, 58.000, 0.000, 1, 0, 0);
  ie(187.000, 62.000, 0.000, 1, 0, 0);
  ie(192.000, 68.000, 0.000, 1, 0, 0);
  ie(194.000, 71.000, 0.000, 1, 0, 0);
  ie(198.000, 75.000, 0.000, 1, 0, 0);
  ie(201.000, 80.000, 0.000, 1, 0, 0);
  ie(205.000, 82.000, 0.000, 1, 0, 0);
  ie(208.000, 84.000, 0.000, 1, 0, 0);
  ie(210.000, 85.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
}

// this test is mainly intended for finding memory leaks (so run it under valgrind)
// also, this test will now fail if undo does not preserve z-order
void ScribbleTest::test3()
{
  ss(0);
  hr(300, 300);
  ss(5);
  scribbleArea->selectAll();
  scribbleArea->deleteSelection();
  scribbleArea->undo();
  scribbleArea->undo();
  ss(10);
  ss(15);
  scribbleArea->undo();
  ss(20);
  scribbleArea->selectAll();
  scribbleArea->copyToClipboard();
  scribbleArea->deleteSelection();
  ss(30);
  ss(40);
  scribbleArea->selectAll();
  scribbleArea->copyToClipboard();
  scribbleArea->doPaste();
  ss(50);
  scribbleArea->undo();
  scribbleArea->undo();
  scribbleArea->doPaste();
  scribbleArea->deleteSelection();
  ss(60);
  scribbleArea->undo();
  scribbleArea->undo();
  ss(70);
  scribbleArea->doPaste();
  scribbleArea->deleteSelection();
  scribbleArea->undo();
  scribbleArea->undo();
  ss(80);
}

// test deletion of page with active selection
void ScribbleTest::test4()
{
  ss(0);
  scribbleArea->nextPage(true);
  ss(10);
  ss(20);
  scribbleArea->selectAll();
  scribbleArea->copyToClipboard();
  scribbleArea->deletePage(1);
  scribbleArea->undo();
}

// test setting page properties and growing page
void ScribbleTest::test5()
{
  // write out rule lines for this test
  scribbleArea->cfg->set("saveRuleLines", true);
  PageProperties props(600, 800, 30, 30, 30, 0xFFFFFF00, 0x7F0000FF);
  ss(0);
  ss(10);
  scribbleArea->setPageProperties(&props, false, true, false);
  ss(20);
  // add new page
  scribbleArea->nextPage(true);
  ss(0);
  // grow new page
  ie(104.4, 900, 0, pen, press);
  ie(124.5, 903, 0, pen);
  ie(131.8, 900, 0, pen);
  ie(0, 0, 0, pen, release);
  ss(20);
}

// test opening file (test6_in.html) to saved position and lasso selection
void ScribbleTest::test6()
{
  // make a lasso selection
  scribbleMode->setMode(20);
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
}

// test insert space reflow
void ScribbleTest::test7()
{
  // easier to figure out what's going on if screen space and Dim space are the same
  scribbleArea->gotoPos(0, Point(0,0));
  PageProperties props(700, 800, 0, 40, 100);
  scribbleArea->setPageProperties(&props, false, true, false);
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
  scribbleArea->undo();

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
  scribbleArea->setPageProperties(&props, false, true, false);
  // add new page
  s2(400,60);
  scribbleArea->nextPage(true);
  scribbleArea->gotoPos(0, Point(0,0));
  scribbleArea->selectAll();
  scribbleArea->deleteSelection();
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
  // drag selection to second page
  ie(200, 420, 0, pen, press);
  ie(210, 750, 0, pen);
  ie(215, 1300, 0, pen);
  ie(0, 0, 0, pen, release);
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
  scribbleArea->setPageProperties(&props, false, true, false);
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

  // Must draw bookmarks so that findBookmark will work
  scribbleArea->getDocument()->drawBookmarks(screenPaint, NULL);

  scribbleArea->nextPage(true);
  // create hyperref, then convert (along with another stroke) to point to bookmark on first page
  hr(150, 100);
  s2(200, 100);
  scribbleArea->selectAll();
  scribbleArea->createHyperRef(scribbleArea->getDocument()->findBookmark(20));

  // add bookmark to 2nd page ... id should not be serialized for this one
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(135, 150, 0, pen, press);
  ie(106, 180, 0, pen);
  ie(65, 200, 0, pen);
  ie(0, 0, 0, pen, release);

  // return to first page and copy the bookmark - the was crashing before we fixed clone bug with idStr
  scribbleArea->prevPage();
  scribbleArea->selectAll();
  scribbleArea->copyToClipboard();
  scribbleArea->doPaste();
  scribbleArea->deleteSelection();
  scribbleArea->nextPage();
}

// test changing properties and ins space ruled with hyperref
void ScribbleTest::test10()
{
  // test creating new doc with different view modes (added because of crash)
  scribbleConfig->set("viewMode", 0);
  scribbleArea->newDocument();
  scribbleConfig->set("viewMode", 2);
  scribbleArea->newDocument();
  scribbleConfig->set("viewMode", 1);
  scribbleArea->newDocument();

  scribbleArea->gotoPos(0, Point(0,0));
  hr(150, 100);
  s2(200, 100);
  f2(250, 100);
  scribbleArea->selectAll();

  scribbleArea->setPenColor(Painter::BLUE);
  scribbleArea->setPenWidth(2);
  scribbleArea->setPenWidth(3);
  scribbleArea->undo();

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
  scribbleArea->setPageProperties(&props, false, true, false);
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
  StrokeBuilder::MAX_POINT_DIST = 2;
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
  StrokeBuilder::MAX_POINT_DIST = 1000;
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
  scribbleArea->getDocument()->drawBookmarks(screenPaint, NULL);
  scribbleArea->doCommand(ID_UNDO);
  // back to our regularly scheduled programming...
  s2(200, 179);
  f2(250, 182);
  hr(300, 181);
  scribbleArea->doCommand(ID_SELALL);
  // resize selection
  ie(320, 198, 0, pen, press);
  ie(350, 250, 0, pen);
  ie(450, 300, 0, pen);
  ie(0, 0, 0, pen, release);
  // clear selection
  s2(550, 550);
  // apply free eraser to image
  scribbleMode->setMode(MODE_ERASEFREE);
  ie(100, 40, 0, pen, press);
  ie(150, 90, 0, pen);
  ie(200, 140, 0, pen);
  ie(0, 0, 0, pen, release);
}
