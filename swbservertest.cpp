// Tests for whiteboard server (syncscribble/swbserver.cpp): HTTP API, auth backends, relay protocol, history replay
// - each test starts its own server on a free port on a separate thread; we talk to it w/ plain blocking sockets,
//  except for the client sign-in test, which uses Write's own code (ScribbleApp::syncSignIn, syncAPICall)
// - sync tests (runAll() w/ --sync local) exercise the server w/ real clients but skip the HTTP API (test token)

#include "scribbletest.h"
#include "scribbleapp.h"
#include "scribbleconfig.h"
#include "swbserver.h"
#include "ulib/unet.h"
#include "ulib/md5.h"
#include "ulib/fileutil.h"
#include "ulib/platformutil.h"
#include "ulib/stringutil.h"
#include <thread>
#include <memory>
#include <functional>

namespace {

std::string md5str(const std::string& s)
{
  uint8_t digest[MD5_DIGEST_SIZE];
  MD5(s.data(), int(s.size()), digest);
  std::string hex;
  for(uint8_t d : digest) hex += fstring("%02x", d);
  return hex;
}

// server running on its own thread
struct TestServer
{
  std::unique_ptr<SWBServer> server;
  std::thread thread;
  bool ok = false;

  TestServer(SWBServerOptions opts)
  {
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.logLevel = 0;  // errors only - tests intentionally trigger warnings
    server.reset(new SWBServer(opts));
    ok = server->start();
    if(ok)
      thread = std::thread([this](){ server->run(); });
  }
  ~TestServer()
  {
    if(ok) {
      server->stop();
      thread.join();
    }
  }
  int port() const { return server->apiPort(); }
};

struct TestSock
{
  int fd = -1;
  std::string buf;  // received data not yet consumed
  bool eof = false;

  TestSock(int port) { fd = unet_socket(UNET_TCP, UNET_CONNECT, UNET_DEFAULT, "127.0.0.1", std::to_string(port).c_str()); }
  ~TestSock() { if(fd != -1) unet_close(fd); }

  void send(const std::string& s)
  {
    size_t pos = 0;
    while(pos < s.size()) {
      int n = unet_send(fd, s.data() + pos, int(s.size() - pos));
      if(n <= 0) return;
      pos += n;
    }
  }

  // read for up to timeout secs; returns true if data was received
  bool poll(double timeout)
  {
    char tmp[65536];
    if(eof || unet_select(fd, -1, timeout) <= 0) return false;
    int n = unet_recv(fd, tmp, sizeof(tmp));
    if(n <= 0) { eof = true; return false; }
    buf.append(tmp, n);
    return true;
  }

  // wait until buffer contains s (or up to 3 sec); returns buffer up to and including s, consuming it
  std::string readUntil(const std::string& s, double timeout = 3)
  {
    Timestamp t0 = mSecSinceEpoch();
    size_t pos;
    while((pos = buf.find(s)) == std::string::npos && !eof && (mSecSinceEpoch() - t0)/1000.0 < timeout)
      poll(0.05);
    if(pos == std::string::npos) return "";
    std::string res = buf.substr(0, pos + s.size());
    buf.erase(0, pos + s.size());
    return res;
  }

  size_t readBytes(size_t n, double timeout = 5)
  {
    Timestamp t0 = mSecSinceEpoch();
    while(buf.size() < n && !eof && (mSecSinceEpoch() - t0)/1000.0 < timeout)
      poll(0.05);
    return buf.size();
  }

  // returns true if peer closes connection within timeout
  bool waitClosed(double timeout = 3)
  {
    Timestamp t0 = mSecSinceEpoch();
    while(!eof && (mSecSinceEpoch() - t0)/1000.0 < timeout)
      poll(0.05);
    return eof;
  }

  // collect anything arriving within a short time (for checking that nothing unexpected is sent)
  std::string drain(double t = 0.1) { while(poll(t)) {} std::string res; res.swap(buf); return res; }
};

std::string httpGet(int port, const std::string& path, const std::string& cookie = "")
{
  TestSock s(port);
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
  if(!cookie.empty()) req += "Cookie: session=" + cookie + "\r\n";
  s.send(req + "\r\n");
  s.waitClosed();
  return s.buf;
}

std::string httpBody(const std::string& resp)
{
  size_t pos = resp.find("\r\n\r\n");
  return pos == std::string::npos ? "" : resp.substr(pos + 4);
}

std::string between(const std::string& s, const std::string& a, const std::string& b)
{
  size_t p0 = s.find(a);
  if(p0 == std::string::npos) return "";
  p0 += a.size();
  size_t p1 = s.find_first_of(b, p0);
  return s.substr(p0, p1 == std::string::npos ? std::string::npos : p1 - p0);
}

std::string authPath(const std::string& user, const std::string& pw, const std::string& ts = "")
{
  std::string t = ts.empty() ? std::to_string(mSecSinceEpoch()) : ts;
  return "/v1/auth?user=" + user + "&timestamp=" + t + "&signature=" + md5str(md5str("styluslabs" + pw) + t);
}

std::string startCmd(const std::string& user, const std::string& doc, const std::string& token, const char* uuid, int offset = 0)
{
  return "/start?version=1&user=" + user + "&document=" + doc + "&token=" + token
      + "&uuid=" + uuid + "&offset=" + std::to_string(offset) + "\n";
}

std::string dataCmd(const std::string& data) { return "/data?length=" + std::to_string(data.size()) + "\n" + data; }

std::string connectMsg(const char* name, const char* uuid)
{
  return std::string("<undo><connect name='") + name + "' uuid='" + uuid + "'/></undo>\n";
}

std::string disconnectMsg(const char* name) { return std::string("<undo><disconnect name='") + name + "'/></undo>\n"; }

}  // namespace

void ScribbleTest::runServerTests()
{
  nFailed = 0;
  int nchecks = 0;
  std::string details;
  std::string section;
  auto check = [&](bool ok, const std::string& what) {
    ++nchecks;
    if(!ok) {
      ++nFailed;
      details += "server FAIL: " + section + ": " + what + "\n";
    }
    return ok;
  };
  Timestamp t0 = mSecSinceEpoch();

  // HTTP API in anonymous mode
  {
    section = "API";
    TestServer srv(SWBServerOptions{});
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    std::string resp = httpGet(port, "/v1/auth?user=alice&timestamp=1&signature=x");
    std::string cookie = between(resp, "Set-Cookie: session=", "\r;");
    check(resp.compare(0, 12, "HTTP/1.1 200") == 0 && !cookie.empty(), "anonymous auth");
    check(httpGet(port, "/v1/createswb?name=wb1").compare(0, 12, "HTTP/1.1 403") == 0, "no session -> 403");
    check(httpGet(port, "/v1/createswb?name=wb1", "bogus").compare(0, 12, "HTTP/1.1 403") == 0, "bad session -> 403");
    check(httpGet(port, "/v1/openswb?name=wb1", cookie).compare(0, 12, "HTTP/1.1 404") == 0, "open nonexistent -> 404");
    resp = httpGet(port, "/v1/createswb?name=wb1&title=it's%20%3Ca%3E&rxonly=1&bad%20key=1&user=mallory", cookie);
    std::string body = httpBody(resp);
    const std::string expectswb = "<swb name='wb1' title='it&apos;s &lt;a&gt;' rxonly='1' user='alice' token='";
    check(body.compare(0, expectswb.size(), expectswb) == 0,
        "create reply (attribute escaping, invalid/reserved keys dropped): " + body);
    std::string token = between(body, "token='", "'");
    check(token.size() == 32, "token");
    check(httpGet(port, "/v1/createswb?name=wb1", cookie).compare(0, 12, "HTTP/1.1 404") == 0, "duplicate create -> 404");
    std::string cookie2 = between(httpGet(port, "/v1/auth?user=bob&timestamp=1&signature=x"), "Set-Cookie: session=", "\r;");
    body = httpBody(httpGet(port, "/v1/openswb?name=wb1", cookie2));
    std::string token2 = between(body, "token='", "'");
    check(body.find("title='it&apos;s &lt;a&gt;' rxonly='1' user='bob'") != std::string::npos, "open reply: " + body);
    check(token2.size() == 32 && token2 != token, "open token");
    check(httpGet(port, "/v1/nosuch", cookie).compare(0, 12, "HTTP/1.1 404") == 0, "unknown path -> 404");
    check(httpGet(port, "/v1/debug").compare(0, 12, "HTTP/1.1 403") == 0, "/v1/debug requires enableTest");
    TestSock bad(port);
    bad.send("garbage\r\n\r\n");
    check(bad.waitClosed() && bad.buf.compare(0, 12, "HTTP/1.1 400") == 0, "malformed request -> 400");

    // relay
    section = "relay";
    int rport = port + 1;
    TestSock a(rport);
    a.send(startCmd("alice", "wb1", token, "11"));
    check(a.readUntil(connectMsg("alice", "11")) == connectMsg("alice", "11"), "alice connect");
    // data split across sends and multiple messages in one send
    a.send("/data?length=5\nhel");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    a.send("lo" + dataCmd("abc"));
    check(a.readUntil("abc") == "helloabc", "echo to sender");
    TestSock b(rport);
    b.send(startCmd("bob", "wb1", token2, "22"));
    std::string hist = connectMsg("alice", "11") + "helloabc" + connectMsg("bob", "22");
    check(b.readUntil(connectMsg("bob", "22")) == hist, "history for new client");
    check(a.readUntil(connectMsg("bob", "22")) == connectMsg("bob", "22"), "connect broadcast");
    TestSock eve(rport);
    eve.send(startCmd("eve", "wb1", token, "33"));  // alice's token
    check(!eve.readUntil("accessdenied").empty() && eve.waitClosed(), "invalid token rejected");
    TestSock info(rport);
    info.send("/info?document=wb1\n");
    check(info.readUntil("bob") == "alice,bob", "/info");
    TestSock nostart(rport);
    nostart.send(dataCmd("xyz"));
    check(nostart.waitClosed() && nostart.buf.empty(), "/data before /start rejected");
    TestSock invalid(rport);
    invalid.send("hello\n");
    check(invalid.waitClosed(), "invalid command closes connection");
    // bob reconnects w/ offset (as after network interruption) -> gets rest of history; old connection kicked
    TestSock b2(rport);
    int offset = int(connectMsg("alice", "11").size() + 5);
    b2.send(startCmd("bob", "wb1", token2, "44", offset));
    std::string expect = hist.substr(offset) + disconnectMsg("bob") + connectMsg("bob", "44");
    check(b2.readUntil(connectMsg("bob", "44")) == expect, "reconnect w/ offset");
    check(!b.readUntil("User logged in from another location").empty() && b.waitClosed(), "old connection kicked");
    check(a.readUntil(connectMsg("bob", "44")) == disconnectMsg("bob") + connectMsg("bob", "44"), "kick broadcast");
    // large message (much larger than socket buffers, so sends are queued)
    std::string big(3 << 20, 'x');
    for(size_t ii = 0; ii < big.size(); ii += 997) big[ii] = char('a' + ii % 26);
    a.send(dataCmd(big));
    check(b2.readBytes(big.size()) >= big.size() && b2.buf.compare(0, big.size(), big) == 0, "3 MB message");
    b2.buf.erase(0, big.size());
    a.drain();
    // many small messages, then history replay from offset 0 and from middle (history chunks are merged)
    std::string small;
    for(int ii = 0; ii < 300; ++ii) {
      std::string m = fstring("<msg %d/>", ii);
      small += m;
      a.send(dataCmd(m));
    }
    check(b2.readUntil("<msg 299/>") == small, "300 small messages");
    a.drain();
    std::string fullhist = hist + disconnectMsg("bob") + connectMsg("bob", "44") + big + small;
    TestSock c(rport);
    c.send(startCmd("carol", "wb1", "SCRIBBLE_SYNC_TEST", "55"));
    check(!c.readUntil("accessdenied").empty(), "test token rejected w/o enableTest");
    TestSock c2(rport);
    int c2off = int(fullhist.size() - small.size()/2 - 3);
    c2.send(startCmd("bob", "wb1", token2, "66", c2off));
    check(c2.readUntil(connectMsg("bob", "66")) == fullhist.substr(c2off) + disconnectMsg("bob") + connectMsg("bob", "66"),
        "history replay from offset in merged chunks");
    b2.drain();
    a.drain();
    // /end
    a.send("/end\n");
    check(a.waitClosed(), "/end closes connection");
    check(c2.readUntil(disconnectMsg("alice")) == disconnectMsg("alice"), "disconnect broadcast");
    c2.send("/end\n");
    c2.waitClosed();
    // whiteboard is deleted after last client disconnects (delDelay = 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(httpGet(port, "/v1/openswb?name=wb1", cookie).compare(0, 12, "HTTP/1.1 404") == 0, "whiteboard deleted");
  }

  // test token and debug page w/ enableTest
  {
    section = "enableTest";
    SWBServerOptions opts;
    opts.enableTest = true;
    TestServer srv(opts);
    if(!check(srv.ok, "server start")) goto done;
    TestSock a(srv.port() + 1);
    a.send(startCmd("t1", "testdoc", "SCRIBBLE_SYNC_TEST", "1"));
    check(a.readUntil(connectMsg("t1", "1")) == connectMsg("t1", "1"), "test token");
    check(httpBody(httpGet(srv.port(), "/v1/debug")).find("testdoc") != std::string::npos, "/v1/debug");
  }

  // users file
  {
    section = "users file";
    std::string usersfile = outPath + "/swbusers.txt";
    removeFile(usersfile);
    SWBServerOptions opts;
    opts.usersFile = usersfile;
    std::string err;
    check(swbAddUser(opts, "bob", "old", &err), "add user to new file: " + err);
    check(swbAddUser(opts, "alice", "pw1", &err), "add user: " + err);
    check(swbAddUser(opts, "bob", "pw2", &err), "update user: " + err);
    check(!swbAddUser(opts, "a:b", "pw", &err), "invalid username rejected");
    std::string contents = readFile(usersfile.c_str());
    check(contents.find("bob:" + md5str("styluslabspw2") + "\nalice:" + md5str("styluslabspw1") + "\n") != std::string::npos,
        "users file contents: " + contents);
    opts.maxClockSkew = 600;
    TestServer srv(opts);
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    check(httpGet(port, authPath("alice", "pw1")).compare(0, 12, "HTTP/1.1 200") == 0, "valid login");
    check(httpGet(port, authPath("bob", "pw2")).compare(0, 12, "HTTP/1.1 200") == 0, "valid login (updated)");
    check(httpGet(port, authPath("bob", "old")).compare(0, 12, "HTTP/1.1 401") == 0, "old password");
    // file is reloaded when modified (mtime has 1 sec resolution on some filesystems)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(swbAddUser(opts, "carol", "pw3", &err), "add user while running");
    check(httpGet(port, authPath("carol", "pw3")).compare(0, 12, "HTTP/1.1 200") == 0, "users file reloaded");
    check(httpGet(port, authPath("alice", "pw2")).compare(0, 12, "HTTP/1.1 401") == 0, "wrong password");
    check(httpGet(port, authPath("dave", "pw3")).compare(0, 12, "HTTP/1.1 401") == 0, "unknown user");
    check(httpGet(port, authPath("alice", "pw1", "1000")).compare(0, 12, "HTTP/1.1 401") == 0, "clock skew");
    check(httpGet(port, "/v1/auth?user=alice").compare(0, 12, "HTTP/1.1 401") == 0, "missing signature");

    // Write's client code: sign in and create whiteboard
    section = "client";
    std::string hostport = "127.0.0.1:" + std::to_string(port);
    ScribbleApp* app = ScribbleApp::app;
    ScribbleConfig* cfg = ScribbleApp::cfg;
    std::string prevServer = cfg->String("syncServer", ""), prevUser = cfg->String("syncUser", "");
    std::string prevPass = cfg->String("syncPass", "");
    cfg->set("syncServer", hostport.c_str());
    cfg->set("syncUser", "alice");
    cfg->set("syncPass", md5str("styluslabspw1").c_str());
    Url url;
    url.host = hostport;
    url.path = "clientwb";
    if(check(app->syncSignIn(url), "syncSignIn")) {
      std::string reply = app->syncAPICall(url, "/v1/createswb?name=clientwb&title=Test");
      check(reply.compare(0, 12, "HTTP/1.1 200") == 0 && reply.find("<swb name='clientwb' title='Test' user='alice' token='")
          != std::string::npos, "syncAPICall createswb: " + httpBody(reply));
    }
    cfg->set("syncServer", prevServer.c_str());
    cfg->set("syncUser", prevUser.c_str());
    cfg->set("syncPass", prevPass.c_str());
    app->syncSession.clear();
  }

  // sqlite (skipped if sqlite3 library isn't available)
  {
    section = "sqlite";
    std::string dbfile = outPath + "/swbusers.sqlite";
    removeFile(dbfile);
    SWBServerOptions opts;
    opts.dbFile = dbfile;
    std::string err;
    if(!swbAddUser(opts, "alice", "pw1", &err))
      details += "server: sqlite tests skipped (" + err + ")\n";
    else {
      check(swbAddUser(opts, "bob", "pw2", &err) && swbAddUser(opts, "bob", "pw3", &err), "add/update user: " + err);
      TestServer srv(opts);
      if(check(srv.ok, "server start")) {
        int port = srv.port();
        check(httpGet(port, authPath("alice", "pw1")).compare(0, 12, "HTTP/1.1 200") == 0, "valid login");
        check(httpGet(port, authPath("bob", "pw3")).compare(0, 12, "HTTP/1.1 200") == 0, "valid login (updated)");
        check(httpGet(port, authPath("bob", "pw2")).compare(0, 12, "HTTP/1.1 401") == 0, "old password");
        check(httpGet(port, authPath("carol", "pw1")).compare(0, 12, "HTTP/1.1 401") == 0, "unknown user");
        // users added while running are seen immediately
        check(swbAddUser(opts, "carol", "pw1", &err), "add user while running");
        check(httpGet(port, authPath("carol", "pw1")).compare(0, 12, "HTTP/1.1 200") == 0, "user added while running");
      }
    }
    removeFile(dbfile);
  }

done:
  resultStr = details + fstring("server tests: %d checks completed in %d ms: %d failed", nchecks,
      int(mSecSinceEpoch() - t0), nFailed);
}
