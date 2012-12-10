#ifndef SCRIBBLETEST_H
#define SCRIBBLETEST_H

#include "scribblearea.h"
#include "bookmarkview.h"

class ScribbleTest {
public:
  ScribbleTest(const char* cfgfile = NULL);
  ~ScribbleTest();

  int runAll(const char *path);

private:
  ScribbleConfig* scribbleConfig;
  ScribbleArea* scribbleArea;
  ScribbleMode* scribbleMode;
  BookmarkView* bookmarkArea;
  QImage* screenImg;
  Painter* screenPaint;
  Rect screenRect;

  static const int pen = ScribbleArea::INPUTSOURCE_PEN;
  static const int press = ScribbleArea::INPUTEVENT_PRESS;
  static const int release = ScribbleArea::INPUTEVENT_RELEASE;
  bool testCompareFiles(const char* f1, const char* f2);
  void ie(Dim x, Dim y, Dim p, int src, int ev = 0, int mm = 0);
  void ss(Dim offset);

  void test0();
  void test1();
  void test2();
  void test3();
  void test4();
};

#endif
