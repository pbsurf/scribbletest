#ifndef SCRIBBLETEST_H
#define SCRIBBLETEST_H

#include "scribbledoc.h"
#include "bookmarkview.h"

class ScribbleTest : public QObject
{
  Q_OBJECT

public:
  ScribbleTest(const QString &path);
  ScribbleTest(ScribbleDoc* sd, BookmarkView* bv, ScribbleMode* sm);
  ~ScribbleTest();

  void runAll(bool runsynctest = false);
  void performanceTest();
  void inputTest();

  // result string to be read by caller
  fstring resultStr;

private slots:
  void syncSlaveMsg(QString msg, int level);

private:
  ScribbleConfig* scribbleConfig;
  ScribbleDoc* scribbleDoc;
  ScribbleArea* scribbleArea;
  ScribbleMode* scribbleMode;
  BookmarkView* bookmarkArea;
  QImage* screenImg;
  Painter* screenPaint;
  Rect screenRect;
  QString outPath;

  ScribbleTest* syncSlave;
  int syncTestNum;
  bool waitForSyncDone;
  void startSyncTest(int testnum);

  static const int pen = INPUTSOURCE_PEN;
  static const int press = INPUTEVENT_PRESS;
  static const int release = INPUTEVENT_RELEASE;
  void undo() { scribbleDoc->doCommand(ID_UNDO); }
  void redo() { scribbleDoc->doCommand(ID_REDO); }
  void doCommand(int cmd) { scribbleDoc->doCommand(cmd); }
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
  static void printEvent(QObject* obj, QEvent* event);

protected:
  bool eventFilter(QObject* obj, QEvent* event);
};

#endif
