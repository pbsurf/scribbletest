// Tests for document saving and handling of external changes to the document file: Document::save(),
//  ScribbleDoc::saveDocument() (used directly by autosave), ScribbleApp::doSave() / checkExtModified()
// Invariants checked (implementation independent where possible, so they should keep passing after fixes):
// - a save that succeeds without any prompt leaves the file at the document's path equal to the document in memory
// - external changes to the file are never ignored or silently overwritten: if the document is unmodified, it is
//  reloaded; if modified, saving prompts (Save conflict); saves that can't prompt (autosave, app suspend) keep both
//  versions; this must work regardless of modification time: sync clients (Google Drive, Syncthing, rsync -t) and
//  `cp -p` + `mv` replace the file (new inode) w/ the original mtime, and changes within the same second as our save
//  aren't visible w/ 1 second mtime resolution
// - a failed save is reported to the user, the document remains modified, and a later save succeeds
// - known limitation (reported as NOTE, not counted as failure): since saves are done in place (partial saves of
//  .svgz only rewrite pages from the first modified page on, which is important for large documents), a write
//  error or interrupted save (crash, power loss) can leave a corrupt file; regular saves or autosave limit the loss
// Files are left in <out>/filetest for inspection; Linux/macOS only (POSIX file APIs)
// Unless noted, each document is created, saved, then reopened, so pages are loaded lazily as for any opened document

#include "scribbletest.h"
#include "scribbleapp.h"
#include "scribbledoc.h"
#include "document.h"
#include "page.h"
#include "element.h"
#include "scribblemode.h"
#include "documentlist.h"
#include "application.h"
#include "ugui/uidriver.h"
#include "ulib/fileutil.h"
#include "ulib/platformutil.h"
#include "ulib/stringutil.h"

#if PLATFORM_WIN
void ScribbleTest::runFileTests() { resultStr = "file tests: not supported on Windows"; }
#else

#include <functional>
#include <algorithm>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>

#ifdef __APPLE__
#define st_mtim st_mtimespec
#endif

namespace {

typedef std::vector<std::vector<std::string>> DocIds;  // ids of strokes on each page

uint32_t rngState = 1;
float frand()
{
  rngState = rngState*1664525u + 1013904223u;
  return (rngState >> 8)/float(1 << 24);
}

// random walk, so content doesn't compress well (realistic file sizes)
Element* makeStroke(const std::string& id, int npts = 40)
{
  Path2D path;
  Point pt(50 + 600*frand(), 50 + 900*frand());
  path.moveTo(pt);
  for(int ii = 1; ii < npts; ++ii) {
    pt += Point(20*frand() - 10, 20*frand() - 10);
    path.lineTo(pt);
  }
  SvgPath* node = new SvgPath(path);
  node->setAttr<color_t>("fill", Color::NONE);
  setSvgStrokeColor(node, Color::BLACK);
  node->setAttr<float>("stroke-width", 1.5f);
  node->setXmlId(id.c_str());
  return new Element(node);
}

DocIds pageIds(Document* doc, std::string* err)
{
  DocIds ids;
  for(size_t ii = 0; ii < doc->pages.size(); ++ii) {
    Page* page = doc->pages[ii];
    if(!page->ensureLoaded(false))
      *err += fstring("page %d failed to load; ", int(ii));
    ids.emplace_back();
    for(SvgNode* n : page->contentNode->children())
      ids.back().push_back(n->xmlId());
  }
  return ids;
}

// load file w/ a separate Document, as a fresh instance of Write would
DocIds diskIds(const std::string& path, std::string* err)
{
  err->clear();
  FileStream* strm = new FileStream(path.c_str(), "rb");
  if(!strm->is_open()) {
    delete strm;
    *err = "file missing";
    return {};
  }
  Document doc;
  Document::loadresult_t res = doc.load(strm, true);
  if(res != Document::LOAD_OK)
    *err = fstring("load result %d; ", int(res));
  return pageIds(&doc, err);
}

DocIds diskIds(const std::string& path)
{
  std::string err;
  DocIds ids = diskIds(path, &err);
  return err.empty() ? ids : DocIds();
}

// filler strokes have ids starting w/ "f": show counts for these and ids of others
std::string idsStr(const DocIds& ids)
{
  std::string s = fstring("%d pages:", int(ids.size()));
  for(const auto& pg : ids) {
    int nfill = 0;
    std::string marks;
    for(const std::string& id : pg) {
      if(id.size() > 0 && id[0] == 'f') ++nfill;
      else marks += " " + id;
    }
    s += fstring(" [%d%s]", nfill, marks.c_str());
  }
  return s;
}

bool writeFileStr(const std::string& path, const std::string& data)
{
  FILE* f = fopen(path.c_str(), "wb");
  if(!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  return fclose(f) == 0 && ok;
}

bool setMTime(const std::string& path, const timespec& t)
{
  timespec ts[2] = {{0, UTIME_OMIT}, t};
  return utimensat(AT_FDCWD, path.c_str(), ts, 0) == 0;
}

struct stat fileStat(const std::string& path)
{
  struct stat st;
  if(stat(path.c_str(), &st) != 0)
    memset(&st, 0, sizeof(st));
  return st;
}

std::vector<std::string> listDir(const std::string& dir)
{
  std::vector<std::string> files;
  DIR* d = opendir(dir.c_str());
  if(!d) return files;
  while(struct dirent* e = readdir(d)) {
    std::string path = dir + "/" + e->d_name;
    if(e->d_name[0] != '.' && S_ISREG(fileStat(path).st_mode))
      files.push_back(path);
  }
  closedir(d);
  return files;
}

// how the file is changed externally; REPLACE: new file renamed over original (new inode), as by sync clients and
//  many editors; INPLACE: file truncated and rewritten (same inode)
enum ExtOp { REPLACE_NEW_MTIME, REPLACE_SAME_MTIME, INPLACE_SAME_MTIME, INPLACE_SAME_SECOND };
const char* extOpName(ExtOp op)
{
  static const char* names[] = {"replaced w/ new mtime", "replaced w/ original mtime",
      "rewritten in place w/ original mtime", "rewritten in place in same second"};
  return names[op];
}

// replace or rewrite file at path w/ content of src (or its current content if src is empty)
bool extModify(const std::string& path, const std::string& src, ExtOp op)
{
  struct stat st0;
  if(stat(path.c_str(), &st0) != 0) return false;
  std::string data = readFile((src.empty() ? path : src).c_str());
  timespec mt = st0.st_mtim;
  if(op == REPLACE_NEW_MTIME)
    mt.tv_sec += 100;
  else if(op == INPLACE_SAME_SECOND)
    mt.tv_nsec = (mt.tv_nsec + 500000000) % 1000000000;  // different mtime, but same st_mtime (seconds)
  if(op == REPLACE_NEW_MTIME || op == REPLACE_SAME_MTIME) {
    std::string tmp = path + ".exttmp";
    return writeFileStr(tmp, data) && setMTime(tmp, mt) && rename(tmp.c_str(), path.c_str()) == 0;
  }
  return writeFileStr(path, data) && setMTime(path, mt);
}

// write "their" version of file at src to dest: stroke w/ id added to page pg, saved by separate Document
bool makeTheirs(const std::string& src, const std::string& dest, int pg, const std::string& id)
{
  Document doc;
  if(doc.load(new FileStream(src.c_str(), "rb"), false) != Document::LOAD_OK || !doc.ensurePagesLoaded())
    return false;
  doc.pages[pg]->contentNode->addChild(makeStroke(id)->node);
  FileStream* out = new FileStream(dest.c_str(), "wb+");
  if(doc.save(out, NULL, Document::SAVE_FORCE))
    return true;  // doc owns out
  delete out;
  return false;
}

bool hasDialog(const std::vector<std::string>& seen, const char* title)
{
  for(const std::string& s : seen) {
    if(s.find(title) != std::string::npos)
      return true;
  }
  return false;
}

}  // namespace

void ScribbleTest::runFileTests()
{
  nFailed = 0;
  int nchecks = 0;
  std::string details;
  std::string section;
  ScribbleApp* app = ScribbleApp::app;
  UiDriver ui(ScribbleApp::gui);
  Application::uiDriver = &ui;
  Timestamp t0 = mSecSinceEpoch();
  const std::string dir = outPath + "/filetest";
  const std::string theirsDir = dir + "/theirs";
  removeDir(dir);
  createPath(theirsDir);

  auto check = [&](bool ok, const std::string& what) {
    ++nchecks;
    if(!ok) {
      ++nFailed;
      details += "files FAIL: " + section + ": " + what + "\n";
    }
    return ok;
  };
  int nnotes = 0;
  auto note = [&](bool ok, const std::string& what) {
    if(!ok) {
      ++nnotes;
      details += "files NOTE: " + section + ": " + what + "\n";
    }
  };
  auto d = [&](){ return app->activeDoc(); };
  auto memIds = [&](std::string* err) { return pageIds(d()->document, err); };
  // run fn, answering any dialogs shown w/ the first of buttons present; returns titles of dialogs shown
  auto withDialogs = [&](std::vector<std::string> buttons, std::function<void()> fn) {
    std::vector<std::string> seen;
    for(int ii = 0; ii < 4; ++ii) {
      ui.onModal("", [&seen, buttons, app](UiDriver& u, Window* w){
        seen.push_back(UiDriver::widgetText(w));
        for(const std::string& b : buttons) {
          if(Widget* btn = u.find("button:" + b, w)) {
            u.click(btn);
            return;
          }
        }
        // document chooser (Save As, e.g. after "Read-only file" message): cancel
        if(app->documentList && w == app->documentList)
          app->documentList->finish(DocumentList::REJECTED);
        else
          u.errors.push_back("no button to click in dialog " + UiDriver::widgetText(w));
      });
    }
    fn();
    ui.clearModalHandlers();
    if(!ui.errors.empty())
      check(false, "UI errors: " + joinStr(ui.errors, "\n"));
    ui.errors.clear();
    return seen;
  };
  auto dialogsStr = [](const std::vector<std::string>& seen) {
    return seen.empty() ? std::string("no dialogs") : "dialogs: " + joinStr(seen, ", ");
  };

  int nextFillId = 0;
  auto mark = [&](int pg, const std::string& id) {
    Page* page = d()->document->pages[pg];
    page->ensureLoaded();
    d()->startAction(pg);
    page->addStroke(makeStroke(id));
    d()->endAction();
  };
  auto removeStrokes = [&](int pg, int n) {
    Page* page = d()->document->pages[pg];
    page->ensureLoaded();
    d()->startAction(pg);
    for(int ii = 0; ii < n && page->strokeCount() > 0; ++ii)
      page->removeStroke(static_cast<Element*>(page->contentNode->children().front()->ext()));
    d()->endAction();
  };
  // create document w/ npages of nstrokes each, save to path, and reopen
  auto makeDoc = [&](const std::string& path, int npages, int nstrokes) {
    d()->newDocument();
    for(int pg = 0; pg < npages; ++pg) {
      if(pg > 0)
        d()->newPage();
      d()->startAction(pg);
      for(int ii = 0; ii < nstrokes; ++ii)
        d()->document->pages[pg]->addStroke(makeStroke(fstring("f%d", nextFillId++)));
      d()->endAction();
    }
    // as for Save As
    if(!check(d()->saveDocument(path.c_str(), Document::SAVE_FORCE), "create " + path))
      return false;
    bool opened = false;
    auto seen = withDialogs({}, [&](){ opened = app->openDocument(path); });
    return check(opened && seen.empty() && !d()->isModified() && path == d()->fileName(), "open " + path);
  };
  // check that file at path matches document in memory (and both load w/o errors)
  auto diskMatchesMem = [&](const std::string& path, const std::string& what) {
    std::string err, memerr;
    DocIds disk = diskIds(path, &err);
    DocIds mem = memIds(&memerr);
    return check(err.empty() && memerr.empty() && disk == mem, what + ": file: " + (err.empty() ? idsStr(disk) : err)
        + "; memory: " + idsStr(mem) + memerr);
  };
  auto diskMatches = [&](const std::string& path, const DocIds& expect, const std::string& what) {
    std::string err;
    DocIds disk = diskIds(path, &err);
    return check(err.empty() && disk == expect, what + ": file: " + (err.empty() ? idsStr(disk) : err)
        + "; expected: " + idsStr(expect));
  };
  // regular save (Save button), no dialogs expected
  auto save = [&](const std::string& what) {
    bool ok = false;
    auto seen = withDialogs({"OK"}, [&](){ ok = app->doSave(d()); });
    return check(ok && seen.empty(), what + ": save (" + dialogsStr(seen) + ")");
  };
  // files in dir w/ content matching ids
  auto findContent = [&](const DocIds& ids) {
    std::vector<std::string> found;
    for(const std::string& f : listDir(dir)) {
      if(diskIds(f) == ids)
        found.push_back(f);
    }
    return found;
  };

  // Round trip: sequence of edits, each followed by a save; file must match memory after each save, and document
  //  after reopening must match document before
  for(const char* ext : {"svgz", "html", "svg"}) {
    section = fstring("round trip .%s", ext);
    std::string path = dir + "/roundtrip." + ext;
    if(!makeDoc(path, 8, 20))
      continue;
    diskMatchesMem(path, "after open");
    struct Step { const char* name; std::function<void()> fn; };
    std::vector<Step> steps = {
      {"mark last page", [&](){ mark(7, "m1"); }},
      {"mark middle page", [&](){ mark(4, "m2"); }},
      {"mark first page", [&](){ mark(0, "m3"); }},
      {"append page", [&](){ d()->newPage(); mark(8, "m4"); }},
      {"insert page at start", [&](){ d()->newPage(0); mark(0, "m5"); }},
      {"delete page", [&](){ d()->deletePage(3); }},
      {"move page", [&](){ d()->movePage(1, 5); }},
      {"remove strokes", [&](){ removeStrokes(1, 15); removeStrokes(2, 20); }},
      {"no changes", [&](){}},
      {"add stroke and undo", [&](){ mark(1, "m6"); d()->doCommand(ID_UNDO); }},
      {"mark every other page", [&](){
        for(int pg = 0; pg < d()->document->numPages(); pg += 2) mark(pg, fstring("m7_%d", pg)); }},
    };
    for(const Step& step : steps) {
      off_t size0 = fileStat(path).st_size;
      step.fn();
      if(save(step.name))
        diskMatchesMem(path, step.name);
      if(strcmp(step.name, "remove strokes") == 0)
        check(fileStat(path).st_size < size0, "remove strokes: file smaller");
    }
    std::string err;
    DocIds before = memIds(&err);
    app->openDocument(path);
    DocIds after = memIds(&err);
    check(err.empty() && before == after, "reopened: " + idsStr(after) + err + "; before: " + idsStr(before));
  }

  // Save As: new file gets later edits, original is unchanged
  {
    section = "save as";
    std::string path = dir + "/saveas_a.svgz", path2 = dir + "/saveas_b.svgz";
    if(makeDoc(path, 4, 10)) {
      DocIds orig = diskIds(path);
      mark(1, "m1");
      check(d()->saveDocument(path2.c_str(), Document::SAVE_FORCE), "save as");  // as done by ScribbleApp::doSaveAs()
      mark(2, "m2");
      if(save("after save as"))
        diskMatchesMem(path2, "new file");
      diskMatches(path, orig, "original file unchanged");
    }
  }

  // block index is omitted for > 1024 pages (MAX_BLOCK_INFO_COUNT in Document::saveBgz())
  {
    section = "1030 pages";
    std::string path = dir + "/manypages.svgz";
    if(makeDoc(path, 1030, 1)) {
      mark(1029, "m1");
      mark(0, "m2");
      if(save("first and last pages"))
        diskMatchesMem(path, "first and last pages");
      mark(500, "m3");
      if(save("middle page"))
        diskMatchesMem(path, "middle page");
    }
  }

  // file replaced by identical copy w/ original mtime (Google Drive update, `cp -p` + `mv`): later edits must be
  //  saved to the file at path, not to the unlinked file still held open (data loss reported by user)
  for(bool editFirst : {false, true}) {
    section = std::string("replaced by identical copy, doc ") + (editFirst ? "modified" : "unmodified");
    std::string path = dir + fstring("/identical%d.svgz", int(editFirst));
    if(!makeDoc(path, 6, 20))
      continue;
    if(editFirst)
      mark(2, "ours1");
    ino_t ino0 = fileStat(path).st_ino;
    if(!check(extModify(path, "", REPLACE_SAME_MTIME) && fileStat(path).st_ino != ino0, "replace file"))
      continue;
    // window activated; any reasonable response is OK (nothing, reload, prompt)
    withDialogs({"OK", "Discard Other"}, [&](){ app->checkExtModified(); });
    mark(4, "ours2");
    bool ok = false;
    auto seen = withDialogs({"Discard Other", "OK"}, [&](){ ok = app->doSave(d()); });
    check(ok, "save (" + dialogsStr(seen) + ")");
    diskMatchesMem(path, "after save (" + dialogsStr(seen) + ")");
    // shrinking save: Document::saveBgz() truncates by path (FileStream::truncate()), so would truncate the other file
    removeStrokes(0, 20);
    removeStrokes(1, 20);
    seen = withDialogs({"Discard Other", "OK"}, [&](){ ok = app->doSave(d()); });
    check(ok, "shrinking save (" + dialogsStr(seen) + ")");
    diskMatchesMem(path, "after shrinking save (" + dialogsStr(seen) + ")");
  }

  // shrinking autosave after replacement by identical copy: must not corrupt file (FileStream::truncate() by path)
  {
    section = "replaced by identical copy, shrinking autosave";
    std::string path = dir + "/identical_shrink.svgz";
    if(makeDoc(path, 8, 30) && check(extModify(path, "", REPLACE_SAME_MTIME), "replace file")) {
      removeStrokes(0, 25);
      removeStrokes(3, 25);
      auto seen = withDialogs({"OK"}, [&](){ d()->saveDocument(); });
      std::string err;
      diskIds(path, &err);
      check(err.empty(), "file not corrupted: " + err);
      diskMatchesMem(path, "after autosave (" + dialogsStr(seen) + ")");
    }
  }

  // external modification of unmodified doc: doc must be reloaded (when window is activated or by periodic check)
  for(ExtOp op : {REPLACE_NEW_MTIME, REPLACE_SAME_MTIME, INPLACE_SAME_MTIME, INPLACE_SAME_SECOND}) {
    section = std::string("unmodified doc, file ") + extOpName(op);
    std::string path = dir + fstring("/reload%d.svgz", int(op));
    std::string theirsPath = theirsDir + fstring("/reload%d.svgz", int(op));
    if(!makeDoc(path, 6, 20))
      continue;
    // change on page 4, which hasn't been loaded (in-place change w/o reload would break lazy loading)
    if(!check(makeTheirs(path, theirsPath, 4, "theirs") && extModify(path, theirsPath, op), "modify file"))
      continue;
    DocIds theirs = diskIds(theirsPath);
    auto seen = withDialogs({"OK"}, [&](){ app->checkExtModified(); });
    std::string err;
    DocIds mem = memIds(&err);
    check(err.empty() && mem == theirs, "doc reloaded (" + dialogsStr(seen) + "): memory: " + idsStr(mem) + err
        + "; file: " + idsStr(theirs));
    check(!d()->isModified(), "doc unmodified after reload");
  }

  // external modification of modified doc: saving must prompt; check both choices
  for(const char* ext : {"svgz", "html"}) {
    for(ExtOp op : {REPLACE_NEW_MTIME, REPLACE_SAME_MTIME, INPLACE_SAME_MTIME, INPLACE_SAME_SECOND}) {
      for(const char* choice : {"Discard Other", "Discard Current"}) {
        section = fstring("modified .%s doc, file %s, choose %s", ext, extOpName(op), choice);
        std::string name = fstring("conflict%d%c.%s", int(op), choice[8], ext);
        std::string path = dir + "/" + name, theirsPath = theirsDir + "/" + name;
        if(!makeDoc(path, 6, 20))
          continue;
        mark(1, "ours");
        if(!check(makeTheirs(path, theirsPath, 4, "theirs") && extModify(path, theirsPath, op), "modify file"))
          continue;
        DocIds theirs = diskIds(theirsPath);
        bool ok = false;
        auto seen = withDialogs({choice, "OK"}, [&](){ ok = app->doSave(d()); });
        check(hasDialog(seen, "conflict"), "save conflict prompt (" + dialogsStr(seen) + ")");
        bool inplace = op == INPLACE_SAME_MTIME || op == INPLACE_SAME_SECOND;
        if(choice[8] == 'O' && inplace && diskIds(path) == theirs) {
          // pages we hadn't loaded were overwritten, so we can't save our version (Save As is offered); other
          //  version must not be destroyed
          check(hasDialog(seen, "Documents"), "Save As offered (" + dialogsStr(seen) + ")");
          note(false, "our version lost (unloaded pages overwritten in place)");
        }
        else if(choice[8] == 'O') {
          diskMatchesMem(path, "file overwritten w/ ours");
          std::string err;
          DocIds mem = memIds(&err);
          check(mem.size() > 1 && std::find(mem[1].begin(), mem[1].end(), "ours") != mem[1].end(), "memory has our version: " + idsStr(mem));
        }
        else {
          std::string err;
          DocIds mem = memIds(&err);
          check(err.empty() && mem == theirs, "doc reloaded: memory: " + idsStr(mem) + err + "; theirs: " + idsStr(theirs));
          diskMatches(path, theirs, "file unchanged");
        }
      }
    }
  }

  // file deleted or renamed while doc is open and modified: save must write to path (after prompt or not)
  for(bool renamed : {false, true}) {
    section = renamed ? "file renamed, doc modified" : "file deleted, doc modified";
    std::string path = dir + (renamed ? "/renamed.svgz" : "/deleted.svgz");
    std::string path2 = dir + "/renamed_other.svgz";
    if(!makeDoc(path, 6, 20))
      continue;
    DocIds orig = diskIds(path);
    mark(2, "ours");
    if(!check(renamed ? rename(path.c_str(), path2.c_str()) == 0 : unlink(path.c_str()) == 0, "rename/delete"))
      continue;
    bool ok = false;
    auto seen = withDialogs({"Discard Other", "OK"}, [&](){ ok = app->doSave(d()); });
    check(ok, "save (" + dialogsStr(seen) + ")");
    diskMatchesMem(path, "file at original path (" + dialogsStr(seen) + ")");
    if(renamed)
      diskMatches(path2, orig, "renamed file unchanged");
  }

  // saves that can't prompt (autosave, app suspend): after external change, neither version may be lost; for an
  //  identical copy, our version must be at the original path; other versions must be in the doc's folder
  for(int how : {0, 1}) {
    for(int kind : {0, 1, 2}) {
      const char* kindstr[] = {"identical copy w/ original mtime", "different file w/ original mtime",
          "different file w/ new mtime"};
      section = fstring("%s after file replaced by %s", how ? "app suspend" : "autosave", kindstr[kind]);
      std::string name = fstring("silent%d%d.svgz", how, kind);
      std::string path = dir + "/" + name, theirsPath = theirsDir + "/" + name;
      if(!makeDoc(path, 6, 20))
        continue;
      mark(1, "ours");
      if(kind > 0 && !check(makeTheirs(path, theirsPath, 4, "theirs"), "create other version"))
        continue;
      if(!check(extModify(path, kind > 0 ? theirsPath : "", kind == 2 ? REPLACE_NEW_MTIME : REPLACE_SAME_MTIME),
          "replace file"))
        continue;
      std::string err;
      DocIds ours = memIds(&err);
      // as done by autosave timer in ScribbleApp::loadConfig() and ScribbleDoc::doRefresh()
      auto seen = withDialogs({"OK"}, [&](){ if(how) app->appSuspending(); else d()->saveDocument(); });
      if(kind == 0)
        diskMatches(path, ours, "our version at path (" + dialogsStr(seen) + ")");
      else {
        DocIds theirs = diskIds(theirsPath);
        check(!findContent(ours).empty(), "our version saved somewhere in " + dir + " (" + dialogsStr(seen) + ")");
        check(!findContent(theirs).empty(), "other version kept in " + dir);
      }
    }
  }

  // interrupted save (crash, power loss): run save in child process killed by SIGXFSZ when file size limit (half of
  //  current file size) is exceeded; file holding old or new version would require temporary file + rename, which
  //  we don't do (see above), so only a NOTE
  if(!Application::glRender) {  // child process can't safely use parent's GL context
    for(const char* ext : {"svgz", "html"}) {
      section = fstring("interrupted save .%s", ext);
      std::string path = dir + "/interrupted." + ext;
      if(!makeDoc(path, 8, 30))
        continue;
      DocIds orig = diskIds(path);
      mark(0, "ours");  // first page, so .svgz partial save rewrites (almost) whole file
      std::string err;
      DocIds ours = memIds(&err);
      off_t size = fileStat(path).st_size;
      fflush(stdout);
      fflush(stderr);
      pid_t pid = fork();
      if(pid == 0) {
        struct rlimit nocore = {0, 0};
        setrlimit(RLIMIT_CORE, &nocore);
        signal(SIGXFSZ, SIG_DFL);
        struct rlimit fsize;
        getrlimit(RLIMIT_FSIZE, &fsize);
        fsize.rlim_cur = size/2;
        setrlimit(RLIMIT_FSIZE, &fsize);
        d()->saveDocument();
        _exit(0);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      if(!check(WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ, "save interrupted (test setup)"))
        continue;
      DocIds disk = diskIds(path, &err);
      note(err.empty() && (disk == orig || disk == ours), "file doesn't hold old or new version: file: "
          + (err.empty() ? idsStr(disk) : err) + "; old: " + idsStr(orig) + "; new: " + idsStr(ours));
    }
  }

  // write error (EFBIG via file size limit, w/ SIGXFSZ ignored; same as disk full): failure must be reported, doc
  //  must remain modified, and a later save must succeed; file may be corrupt (NOTE only)
  // - note that any other file writes (e.g. stdout redirected to a log file) may fail while limit is set
  for(const char* ext : {"svgz", "html"}) {
    section = fstring("write error .%s", ext);
    std::string path = dir + "/writeerror." + ext;
    if(!makeDoc(path, 8, 30))
      continue;
    DocIds orig = diskIds(path);
    mark(0, "ours");
    std::string err;
    DocIds ours = memIds(&err);
    struct rlimit fsize0, fsize;
    getrlimit(RLIMIT_FSIZE, &fsize0);
    fsize = fsize0;
    fsize.rlim_cur = fileStat(path).st_size/2;
    bool ok = false;
    void (*prevsig)(int) = signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &fsize);
    auto seen = withDialogs({"OK"}, [&](){ ok = app->doSave(d()); });
    setrlimit(RLIMIT_FSIZE, &fsize0);
    signal(SIGXFSZ, prevsig);
    DocIds disk = diskIds(path, &err);
    note(err.empty() && (disk == orig || disk == ours), "file doesn't hold old or new version: file: "
        + (err.empty() ? idsStr(disk) : err) + "; old: " + idsStr(orig) + "; new: " + idsStr(ours));
    if(disk != ours) {
      check(!ok && hasDialog(seen, "error"), "failure reported (" + dialogsStr(seen) + ")");
      check(d()->isModified(), "doc still modified");
    }
    if(save("save after error cleared"))
      diskMatchesMem(path, "after error cleared");
  }

  // read-only file: failure must be reported and file unchanged
  if(geteuid() != 0) {
    section = "read-only file";
    std::string path = dir + "/readonly.svgz";
    if(makeDoc(path, 4, 10)) {
      DocIds orig = diskIds(path);
      d()->newDocument();
      chmod(path.c_str(), 0444);
      // currently, Write warns when opening a read-only file and clears doc's filename (so Save does Save As)
      auto seen = withDialogs({"OK"}, [&](){ app->openDocument(path); });
      mark(1, "ours");
      bool ok = false;
      auto seen2 = withDialogs({"OK"}, [&](){ ok = app->doSave(d()); });
      seen.insert(seen.end(), seen2.begin(), seen2.end());
      check(!ok && (hasDialog(seen, "error") || hasDialog(seen, "Read-only")), "failure reported (" + dialogsStr(seen) + ")");
      check(d()->isModified(), "doc still modified");
      diskMatches(path, orig, "file unchanged");
      check((fileStat(path).st_mode & 0777) == 0444, "file still read-only");
      chmod(path.c_str(), 0644);
    }
  }

  // file properties preserved (these would be lost by a naive temporary file + rename save)
  {
    section = "symlink";
    std::string path = dir + "/symlink_target.svgz", link = dir + "/symlink.svgz";
    if(makeDoc(path, 4, 10) && check(symlink("symlink_target.svgz", link.c_str()) == 0, "create symlink")) {
      d()->newDocument();
      app->openDocument(link);
      mark(1, "ours");
      if(save("save via symlink"))
        diskMatchesMem(path, "target file");
      struct stat st;
      check(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode), "symlink preserved");
    }
  }
  {
    section = "file mode";
    std::string path = dir + "/mode.svgz";
    if(makeDoc(path, 4, 10)) {
      chmod(path.c_str(), 0640);
      mark(1, "ours");
      if(save("save"))
        diskMatchesMem(path, "after save");
      check((fileStat(path).st_mode & 0777) == 0640, fstring("file mode preserved: %o", fileStat(path).st_mode & 0777));
    }
  }

  d()->newDocument();
  Application::uiDriver = NULL;
  resultStr = details + fstring("file tests: %d checks completed in %d ms: %d failed, %d notes", nchecks,
      int(mSecSinceEpoch() - t0), nFailed, nnotes);
}

#endif
