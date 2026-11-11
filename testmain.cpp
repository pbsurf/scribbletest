// WriteTest: runs Write's tests without a window, using the headless platform layer (headless.cpp)
// The full app is created (with ScribbleApp::testMode set, so user's config and documents are not touched), then
//  tests are run in place of the event loop.

#include "scribbletest.h"
#include "scribbleapp.h"
#include "application.h"
#include <cstdio>
#include <cstring>

#ifndef SCRIBBLE_TEST_PATH
#define SCRIBBLE_TEST_PATH "."
#endif

static int usage(const char* argv0)
{
  fprintf(stderr, "Usage: %s [--sync <server>] [--perf] [<test folder>]\n"
      "  Runs tests in <test folder> (default: " SCRIBBLE_TEST_PATH ")\n"
      "  --sync <server>  also run sync tests using whiteboard server <server>\n"
      "  --perf           run performance test instead of regular tests\n"
      "  Exit code is 0 if all tests pass (thumbnail mismatches are reported but not counted as failures)\n", argv0);
  return 2;
}

int main(int argc, char* argv[])
{
  std::string testPath = SCRIBBLE_TEST_PATH;
  std::string syncServer;
  bool perf = false;
  for(int ii = 1; ii < argc; ++ii) {
    if(strcmp(argv[ii], "--sync") == 0 && ii + 1 < argc)
      syncServer = argv[++ii];
    else if(strcmp(argv[ii], "--perf") == 0)
      perf = true;
    else if(argv[ii][0] == '-')
      return usage(argv[0]);
    else
      testPath = argv[ii];
  }

  ScribbleApp::testMode = true;
  // don't pass our args to ScribbleApp
  char* appargv[] = { argv[0], NULL };
  if(!appInit(1, appargv)) {
    fprintf(stderr, "WriteTest: app initialization failed\n");
    return 2;
  }

  int res = 0;
  {
    ScribbleApp* app = ScribbleApp::app;
    if(perf) {
      ScribbleTest test(app->activeDoc(), app->bookmarkArea, app->scribbleMode, testPath);
      test.performanceTest();
      printf("%s\n", test.resultStr.c_str());
    }
    else {
      ScribbleTest test(testPath);
      test.runAll(syncServer);
      printf("%s\n", test.resultStr.c_str());
      res = test.nFailed > 0 ? 1 : 0;
    }
  }

  appShutdown();
  return res;
}
