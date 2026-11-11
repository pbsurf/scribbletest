// WriteTest: runs Write's tests without a window, using the headless platform layer (headless.cpp)
// The full app is created (with ScribbleApp::testMode set, so user's config and documents are not touched), then
//  tests are run in place of the event loop.

#include "scribbletest.h"
#include "imagecompare.h"
#include "scribbleapp.h"
#include "application.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifndef SCRIBBLE_TEST_PATH
#define SCRIBBLE_TEST_PATH "."
#endif

static int usage(const char* argv0)
{
  fprintf(stderr, "Usage: %s [--sync <server>] [--perf] [<test folder>]\n"
      "       %s --compare [--tol <n>] <ref.png> <out.png> [<diff.png>]\n"
      "  Runs tests in <test folder> (default: " SCRIBBLE_TEST_PATH ")\n"
      "  --sync <server>  also run sync tests using whiteboard server <server>\n"
      "  --perf           run performance test instead of regular tests\n"
      "  --strict         count MINOR render differences as failures\n"
      "  --compare        compare two images as render tests do (exit code: 0 PASS, 1 MINOR, 2 FAIL)\n"
      "  Exit code is 1 if any test fails, else 0\n", argv0, argv0);
  return 3;
}

static int compareMain(int argc, char* argv[])
{
  ImageCompareParams params;
  int argi = 2;
  if(argi + 1 < argc && strcmp(argv[argi], "--tol") == 0) {
    params.tol = atoi(argv[argi + 1]);
    argi += 2;
  }
  int nfiles = argc - argi;
  if(nfiles < 2 || nfiles > 3)
    return usage(argv[0]);
  Image ref = loadImageFile(argv[argi]);
  Image out = loadImageFile(argv[argi + 1]);
  ImageCompareResult res = compareImages(ref, out, params);
  printf("%s: %s\n", ImageCompareResult::verdictStr(res.verdict), res.summary().c_str());
  if(nfiles > 2 && !res.diffImg.isNull())
    saveImagePNG(res.diffImg, argv[argi + 2]);
  return int(res.verdict);
}

int main(int argc, char* argv[])
{
  std::string testPath = SCRIBBLE_TEST_PATH;
  std::string syncServer;
  bool perf = false;
  bool strict = false;
  if(argc > 1 && strcmp(argv[1], "--compare") == 0)
    return compareMain(argc, argv);
  for(int ii = 1; ii < argc; ++ii) {
    if(strcmp(argv[ii], "--sync") == 0 && ii + 1 < argc)
      syncServer = argv[++ii];
    else if(strcmp(argv[ii], "--perf") == 0)
      perf = true;
    else if(strcmp(argv[ii], "--strict") == 0)
      strict = true;
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
      res = test.nFailed > 0 || (strict && test.nMinor > 0) ? 1 : 0;
    }
  }

  appShutdown();
  return res;
}
