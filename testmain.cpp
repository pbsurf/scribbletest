// WriteTest: runs Write's tests without a window, using the headless platform layer (headless.cpp)
// The full app is created (with ScribbleApp::testMode set, so user's config and documents are not touched), then
//  tests are run in place of the event loop.

#include "scribbletest.h"
#include "imagecompare.h"
#include "scribbleapp.h"
#include "application.h"
#include "ulib/fileutil.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifndef SCRIBBLE_TEST_PATH
#define SCRIBBLE_TEST_PATH "."
#endif

static int usage(const char* argv0)
{
  fprintf(stderr, "Usage: %s [options] [<test folder>]\n"
      "       %s --compare [--tol <n>] <ref.png> <out.png> [<diff.png>]\n"
      "  Runs tests using inputs and refs in <test folder> (default: " SCRIBBLE_TEST_PATH ")\n"
      "  --out <dir>          folder for output files and report.html (default: testout next to executable)\n"
      "  --only=<n,...>       run only these tests\n"
      "  --update=<n,...|all> replace refs w/ output for these tests if they don't PASS\n"
      "  --strict             count MINOR render differences as failures\n"
      "  --sync <server>      also run sync tests using whiteboard server <server>\n"
      "  --perf               run performance test instead of regular tests\n"
      "  --compare            compare two images as render tests do (exit code: 0 PASS, 1 MINOR, 2 FAIL)\n"
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

// parse comma separated list of test numbers; returns false on error
static bool parseTestList(const char* s, std::set<int>& tests)
{
  for(const std::string& item : splitStr<std::vector>(s, ',', true)) {
    char* end = NULL;
    long n = strtol(item.c_str(), &end, 10);
    if(!end || *end || n < 0) return false;
    tests.insert(int(n));
  }
  return !tests.empty();
}

int main(int argc, char* argv[])
{
  std::string testPath = SCRIBBLE_TEST_PATH;
  std::string outPath;
  std::string syncServer;
  std::set<int> onlyTests, updateTests;
  bool updateAll = false;
  bool perf = false;
  bool strict = false;
  if(argc > 1 && strcmp(argv[1], "--compare") == 0)
    return compareMain(argc, argv);
  for(int ii = 1; ii < argc; ++ii) {
    StringRef arg(argv[ii]);
    if(arg == "--sync" && ii + 1 < argc)
      syncServer = argv[++ii];
    else if(arg == "--out" && ii + 1 < argc)
      outPath = argv[++ii];
    else if(arg.startsWith("--only=")) {
      if(!parseTestList(argv[ii] + 7, onlyTests)) {
        fprintf(stderr, "WriteTest: invalid test list: %s\n", argv[ii]);
        return 3;
      }
    }
    else if(arg == "--update=all")
      updateAll = true;
    else if(arg.startsWith("--update=")) {
      if(!parseTestList(argv[ii] + 9, updateTests)) {
        fprintf(stderr, "WriteTest: invalid test list: %s\n", argv[ii]);
        return 3;
      }
    }
    else if(arg == "--update") {
      fprintf(stderr, "WriteTest: --update requires test numbers or 'all', e.g. --update=3,5\n");
      return 3;
    }
    else if(arg == "--perf")
      perf = true;
    else if(arg == "--strict")
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

  if(outPath.empty())
    outPath = FSPath(Application::appDir, "testout").path;
  if(!createPath(outPath)) {
    fprintf(stderr, "WriteTest: unable to create output folder %s\n", outPath.c_str());
    appShutdown();
    return 3;
  }

  int res = 0;
  {
    ScribbleApp* app = ScribbleApp::app;
    if(perf) {
      ScribbleTest test(app->activeDoc(), app->bookmarkArea, app->scribbleMode, outPath);
      test.performanceTest();
      printf("%s\n", test.resultStr.c_str());
    }
    else {
      ScribbleTest test(testPath, outPath);
      test.onlyTests = onlyTests;
      test.updateTests = updateTests;
      test.updateAll = updateAll;
      test.runAll(syncServer);
      printf("%s\n", test.resultStr.c_str());
      res = test.nFailed > 0 || (strict && test.nMinor > 0) ? 1 : 0;
    }
  }

  appShutdown();
  return res;
}
