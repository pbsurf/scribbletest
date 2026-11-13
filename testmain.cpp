// WriteTest: runs Write's tests without a window, using the headless platform layer (headless.cpp)
// The full app is created (with ScribbleApp::testMode set, so user's config and documents are not touched), then
//  tests are run in place of the event loop.

#include "scribbletest.h"
#include "headless.h"
#include "imagecompare.h"
#include "doccompare.h"
#include "scribbleapp.h"
#include "application.h"
#include "ulib/fileutil.h"
#include "swbserver.h"
#include "authproxy.h"
#include <thread>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifndef SCRIBBLE_TEST_PATH
#define SCRIBBLE_TEST_PATH "."
#endif

// LeakSanitizer: ignore leaks inside GL drivers (e.g. Mesa's libgallium leaks at exit w/ some drivers)
extern "C" const char* __lsan_default_suppressions()
{
  return "leak:libgallium\nleak:libEGL_mesa\nleak:libGLX_mesa\nleak:libglapi\n";
}

static int usage(const char* argv0)
{
  fprintf(stderr, "Usage: %s [options] [<test folder>]\n"
      "       %s --compare [--tol <n>] <ref.png> <out.png> [<diff.png>]\n"
      "  Runs tests using inputs and refs in <test folder> (default: " SCRIBBLE_TEST_PATH ")\n"
      "  --out <dir>          folder for output files and report.html (default: testout next to executable)\n"
      "  --only=<n,...>       run only these tests\n"
      "  --update=<n,...|all> replace refs w/ output for these tests if they don't PASS\n"
      "  --strict             count MINOR render differences as failures\n"
      "  --sync <server>      run sync tests using whiteboard server <server>[:port]; 'local' to use a server\n"
      "                       running in this process (on a free port)\n"
      "  --sync-relay=<mode>  'http' (default for local server) to use HTTP relay on API port (via HttpClient),\n"
      "                       'port1' for raw relay on port + 1 (default for other servers), or 'http-auth' for\n"
      "                       HTTP relay through reverse proxy w/ HTTP Basic auth (local server only)\n"
      "  --perf               run performance test instead of regular tests\n"
      "  --redraw             run only redraw tests (partial vs. full redraw of app window at several UI scales);\n"
      "                       these also run after regular tests unless --only, --update, or --sync is given\n"
      "  --no-redraw          don't run redraw tests\n"
      "  --server             run only whiteboard server tests (HTTP API, relay protocol); these also\n"
      "                       run after regular tests unless --only, --update, --sync, or --redraw is given\n"
      "  --no-server          don't run whiteboard server tests\n"
      "  --gl                 use GL renderer for app window (EGL surfaceless context, e.g. Mesa llvmpipe;\n"
      "                       set LIBGL_ALWAYS_SOFTWARE=1 to force llvmpipe); fails if GL is unavailable\n"
      "  --automation <path>  instead of running tests, run app (headless) driven by text commands read from\n"
      "                       <path> (e.g. a FIFO; '-' for stdin) w/ replies written to <path>.out; see uidrive.sh\n"
      "                       and 'help' command\n"
      "  --compare            compare two images (.png) or documents (.html, .svg) as tests do\n"
      "                       (exit code: 0 PASS, 1 MINOR, 2 FAIL); --tol is pixel or numeric tolerance\n"
      "  Exit code is 1 if any test fails, else 0\n", argv0, argv0);
  return 3;
}

static int compareMain(int argc, char* argv[])
{
  ImageCompareParams params;
  DocCompareParams docparams;
  int argi = 2;
  if(argi + 1 < argc && strcmp(argv[argi], "--tol") == 0) {
    params.tol = atoi(argv[argi + 1]);
    docparams.numTol = atof(argv[argi + 1]);
    argi += 2;
  }
  int nfiles = argc - argi;
  if(nfiles < 2 || nfiles > 3)
    return usage(argv[0]);
  StringRef reffile(argv[argi]);
  if(reffile.endsWith(".html") || reffile.endsWith(".svg")) {
    DocCompareResult res = compareDocs(readFile(argv[argi]), readFile(argv[argi + 1]), docparams);
    const char* v[] = {"PASS", "MINOR", "FAIL"};
    printf("%s: %s\n", v[res.verdict], res.summary().c_str());
    return int(res.verdict);
  }
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
  std::string syncRelay;  // empty: default
  std::string automation;
  std::set<int> onlyTests, updateTests;
  bool updateAll = false;
  bool perf = false;
  bool strict = false;
  bool useGL = false;
  int redraw = 1;  // 0: skip, 1: after regular tests (if no --only, --update, --sync), 2: redraw tests only
  int serverTests = 1;  // same as redraw
  // line buffered so output isn't lost (e.g. if sanitizer exits early) and stays in order w/ stderr in logs
  setvbuf(stdout, NULL, _IOLBF, 0);
  if(argc > 1 && strcmp(argv[1], "--compare") == 0)
    return compareMain(argc, argv);
  for(int ii = 1; ii < argc; ++ii) {
    StringRef arg(argv[ii]);
    if(arg == "--sync" && ii + 1 < argc)
      syncServer = argv[++ii];
    else if(arg == "--sync-relay=http" || arg == "--sync-relay=port1" || arg == "--sync-relay=http-auth")
      syncRelay = argv[ii] + 13;
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
    else if(arg == "--redraw")
      redraw = 2;
    else if(arg == "--no-redraw")
      redraw = 0;
    else if(arg == "--server")
      serverTests = 2;
    else if(arg == "--no-server")
      serverTests = 0;
    else if(arg == "--gl")
      useGL = true;
    else if(arg == "--automation" && ii + 1 < argc)
      automation = argv[++ii];
    else if(argv[ii][0] == '-')
      return usage(argv[0]);
    else
      testPath = argv[ii];
  }

  ScribbleApp::testMode = true;
  headlessSetGL(useGL);
  // don't pass our args to ScribbleApp
  char* appargv[] = { argv[0], NULL };
  if(!appInit(1, appargv)) {
    fprintf(stderr, "WriteTest: app initialization failed\n");
    return 2;
  }
  if(useGL && !Application::glRender) {
    fprintf(stderr, "WriteTest: GL requested but not available\n");
    appShutdown();
    return 2;
  }
  printf("WriteTest: %s renderer%s%s\n", Application::glRender ? "GL" : "SW",
      Application::glRender ? ": " : "", headlessGLRenderer());

  if(!automation.empty()) {
    if(!Application::startAutomation(automation)) {
      appShutdown();
      return 2;
    }
    while(Application::processEvents())
      Application::layoutAndDraw();
    appShutdown();
    return 0;
  }

  if(outPath.empty())
    outPath = FSPath(Application::appDir, "testout").path;
  if(!createPath(outPath)) {
    fprintf(stderr, "WriteTest: unable to create output folder %s\n", outPath.c_str());
    appShutdown();
    return 3;
  }

  // in-process whiteboard server (same server as `Write --server`)
  std::unique_ptr<SWBServer> localServer;
  std::thread localServerThread;
  std::unique_ptr<AuthProxy> authProxy;
  bool syncBasicAuth = false;
  if(syncServer == "local") {
    SWBServerOptions opts;
    // http-auth: HTTP relay through reverse proxy w/ HTTP Basic auth
    syncBasicAuth = syncRelay == "http-auth";
    opts.trustProxy = syncBasicAuth;
    opts.host = "127.0.0.1";
    opts.port = 0;  // choose free port
    opts.enableTest = true;
    opts.delDelay = 0;  // sync tests reuse whiteboard names
    opts.logLevel = 1;  // warnings only
    localServer.reset(new SWBServer(opts));
    if(!localServer->start()) {
      fprintf(stderr, "WriteTest: unable to start local whiteboard server\n");
      appShutdown();
      return 2;
    }
    localServerThread = std::thread([&](){ localServer->run(); });
    syncServer = "127.0.0.1:" + std::to_string(localServer->apiPort());
    if(syncBasicAuth) {
      authProxy.reset(new AuthProxy(localServer->apiPort(), [](const std::string& u, const std::string& p){
        return p == "pw-" + u; }));
      syncServer = "127.0.0.1:" + std::to_string(authProxy->port());
      syncRelay = "http";
    }
    if(syncRelay.empty())
      syncRelay = "http";
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
      bool defaultRun = onlyTests.empty() && updateTests.empty() && !updateAll && syncServer.empty();
      if(serverTests == 2) {
        redraw = 0;
        ScribbleTest test(app->activeDoc(), app->bookmarkArea, app->scribbleMode, outPath);
        test.runServerTests();
        printf("%s\n", test.resultStr.c_str());
        if(test.nFailed > 0) res = 1;
      }
      else if(redraw != 2) {
        ScribbleTest test(testPath, outPath);
        test.onlyTests = onlyTests;
        test.updateTests = updateTests;
        test.updateAll = updateAll;
        test.syncRelay = syncRelay == "port1" ? "" : syncRelay;
        test.syncBasicAuth = syncBasicAuth;
        test.runAll(syncServer);
        printf("%s\n", test.resultStr.c_str());
        res = test.nFailed > 0 || (strict && test.nMinor > 0) ? 1 : 0;
      }
      if(serverTests == 1 && redraw != 2 && defaultRun) {
        ScribbleTest test(app->activeDoc(), app->bookmarkArea, app->scribbleMode, outPath);
        test.runServerTests();
        printf("%s\n", test.resultStr.c_str());
        if(test.nFailed > 0) res = 1;
      }
      if(redraw == 2 || (redraw == 1 && defaultRun)) {
        ScribbleTest test(app->activeDoc(), app->bookmarkArea, app->scribbleMode, outPath);
        test.runRedrawTests();
        printf("%s\n", test.resultStr.c_str());
        saveImagePNG(headlessReadWindow(), outPath + "/window.png");  // last frame, for inspection
        if(test.nFailed > 0) res = 1;
      }
    }
  }

  if(authProxy) {
    printf("auth proxy: %d requests forwarded, %d rejected\n", int(authProxy->nForwarded), int(authProxy->nRejected));
    if(authProxy->nForwarded == 0 || authProxy->nRejected > 0) res = 1;
    authProxy.reset();
  }
  if(localServer) {
    localServer->stop();
    localServerThread.join();
  }
  appShutdown();
  return res;
}
