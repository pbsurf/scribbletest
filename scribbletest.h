#ifndef SCRIBBLETEST_H
#define SCRIBBLETEST_H

#include "scribblearea.h"
#include "bookmarkview.h"

class ScribbleTest {
public:
  ScribbleTest(const char* cfgfile = NULL);
  ScribbleTest(ScribbleArea* sa, BookmarkView* bv, ScribbleMode* sm);
  ~ScribbleTest();

  void runAll(const char* path);
  void performanceTest();
  void inputTest();

  // result string to be read by caller
  fstring resultStr;

private:
  ScribbleConfig* scribbleConfig;
  ScribbleArea* scribbleArea;
  ScribbleMode* scribbleMode;
  BookmarkView* bookmarkArea;
  QImage* screenImg;
  Painter* screenPaint;
  Rect screenRect;
  const char* outPath;

  static const int pen = INPUTSOURCE_PEN;
  static const int press = INPUTEVENT_PRESS;
  static const int release = INPUTEVENT_RELEASE;
  bool testCompareFiles(const char* f1, const char* f2);
  void ie(Dim x, Dim y, Dim p, int src, int ev = 0, int mm = 0);
  void ss(Dim offset);
  void s1(Dim xoffset, Dim yoffset);
  void s2(Dim xoffset, Dim yoffset);
  void s3(Dim xoffset, Dim yoffset);
  void f2(Dim xoffset, Dim yoffset);
  void hr(Dim xoffset, Dim yoffset);

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
};

// debug event filter - prints name of every event received
class DebugEventFilter : public QObject
{
  Q_OBJECT
public:
  //DebugEventFilter(QObject* parent) : QObject(parent) {}
  static const char* getEventName(const QEvent* event);

protected:
  bool eventFilter(QObject* obj, QEvent* event);
};

#endif
