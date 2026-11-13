// Tests for whiteboard server (syncscribble/swbserver.cpp): HTTP API, relay protocol, history replay
// - each test starts its own server on a free port on a separate thread; we talk to it w/ plain blocking sockets,
//  except for the client sign-in test, which uses Write's own code (ScribbleApp::syncSignIn, syncAPICall)
// - sync tests (runAll() w/ --sync local) exercise the server w/ real clients but skip the HTTP API (test token)

#include "scribbletest.h"
#include "scribbleapp.h"
#include "scribbleconfig.h"
#include "swbserver.h"
#include "scribblesync.h"
#include "authproxy.h"
#include "ulib/unet.h"
#include "ugui/platforms/platformhttp.h"
#include "ulib/md5.h"
#include "ulib/fileutil.h"
#include "ulib/platformutil.h"
#include "ulib/stringutil.h"
#include <thread>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>

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

// read one response w/ Content-Length body
std::string readResponse(TestSock& s, double timeout = 3)
{
  std::string hdr = s.readUntil("\r\n\r\n", timeout);
  if(hdr.empty()) return "";
  size_t cl = hdr.find("Content-Length: ");
  size_t len = cl == std::string::npos ? 0 : size_t(atoll(hdr.c_str() + cl + 16));
  if(s.readBytes(len) < len) return "";
  std::string body = s.buf.substr(0, len);
  s.buf.erase(0, len);
  return hdr + body;
}

// headers: extra headers, each terminated by \r\n
std::string httpGet(int port, const std::string& path, const std::string& cookie = "", const std::string& headers = "")
{
  TestSock s(port);
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + headers;
  if(!cookie.empty()) req += "Cookie: session=" + cookie + "\r\n";
  s.send(req + "\r\n");
  s.waitClosed();
  return s.buf;
}

// request on persistent connection; returns complete response (header and body) or empty string on timeout
std::string httpRequest(TestSock& s, const std::string& method, const std::string& path, const std::string& body = "",
    double timeout = 3)
{
  s.send(method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n"
      + (method == "POST" ? "Content-Length: " + std::to_string(body.size()) + "\r\n" : "") + "\r\n" + body);
  return readResponse(s, timeout);
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

// blocking request w/ HttpClient; state is shared so a late callback (after timeout) is harmless
struct HttpWait
{
  std::mutex mutex;
  std::condition_variable cond;
  bool done = false;
  HttpResponse resp;
  Timestamp doneTime = 0;

  bool wait(double timeout = 15)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return cond.wait_for(lock, std::chrono::milliseconds(int(timeout*1000)), [this](){ return done; });
  }
};

std::shared_ptr<HttpWait> httpStart(HttpClient* client, HttpRequest req, uint64_t* id = NULL)
{
  auto w = std::make_shared<HttpWait>();
  uint64_t reqid = client->request(std::move(req), [w](HttpResponse&& resp){
    std::lock_guard<std::mutex> lock(w->mutex);
    w->resp = std::move(resp);
    w->done = true;
    w->doneTime = mSecSinceEpoch();
    w->cond.notify_all();
  });
  if(id) *id = reqid;
  return w;
}

HttpResponse httpSync(HttpClient* client, HttpRequest req)
{
  auto w = httpStart(client, std::move(req));
  if(!w->wait()) { HttpResponse r; r.error = "test timeout"; return r; }
  return std::move(w->resp);
}

std::string bodyStr(const HttpResponse& r) { return std::string(r.body.begin(), r.body.end()); }

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

  // HTTP API (no authentication: any user accepted)
  {
    section = "API";
    SWBServerOptions apiopts;
    apiopts.delDelay = 0;
    TestServer srv(apiopts);
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    std::string resp = httpGet(port, "/v1/auth?user=alice&timestamp=1&signature=x");
    std::string cookie = between(resp, "Set-Cookie: session=", "\r;");
    check(resp.compare(0, 12, "HTTP/1.1 200") == 0 && !cookie.empty(), "anonymous auth");
    check(httpGet(port, "/v1/createswb?name=wb1").compare(0, 12, "HTTP/1.1 403") == 0, "no session -> 403");
    check(httpGet(port, "/v1/createswb?name=wb1", "bogus").compare(0, 12, "HTTP/1.1 403") == 0, "bad session -> 403");
    check(httpGet(port, "/v1/openswb?name=wb1", cookie).compare(0, 12, "HTTP/1.1 404") == 0, "open nonexistent -> 404");
    resp = httpGet(port, "/v1/createswb?name=wb1&title=it's%20%3Ca%3E&rxonly=1&bad%20key=1&user=mallory&relay=x", cookie);
    std::string body = httpBody(resp);
    const std::string expectswb = "<swb name='wb1' title='it&apos;s &lt;a&gt;' rxonly='1' user='alice' token='";
    check(body.compare(0, expectswb.size(), expectswb) == 0,
        "create reply (attribute escaping, invalid/reserved keys dropped): " + body);
    std::string token = between(body, "token='", "'");
    check(token.size() == 32, "token");
    const std::string relayAttr = " relay='http'/>";
    check(body.size() > relayAttr.size() && body.compare(body.size() - relayAttr.size(), relayAttr.size(), relayAttr) == 0
        && body.find("relay=") == body.rfind("relay="), "relay advertised (and not overridable): " + body);
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

  // default delDelay: whiteboard is kept after last client leaves, but can be replaced by createswb
  {
    section = "delDelay";
    TestServer srv(SWBServerOptions{});
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    std::string cookie = between(httpGet(port, "/v1/auth?user=alice&timestamp=1&signature=x"), "Set-Cookie: session=", "\r;");
    std::string token = between(httpBody(httpGet(port, "/v1/createswb?name=wb1", cookie)), "token='", "'");
    TestSock a(port + 1);
    a.send(startCmd("alice", "wb1", token, "11"));
    a.readUntil(connectMsg("alice", "11"));
    a.send(dataCmd("abc"));
    a.readUntil("abc");
    a.send("/end\n");
    a.waitClosed();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string body = httpBody(httpGet(port, "/v1/openswb?name=wb1", cookie));
    check(between(body, "token='", "'") == token, "whiteboard kept after last client leaves: " + body);
    TestSock b(port + 1);
    b.send(startCmd("alice", "wb1", token, "12"));
    check(b.readUntil(connectMsg("alice", "12")) == connectMsg("alice", "11") + "abc" + connectMsg("alice", "12"), "reconnect to kept whiteboard gets history (no disconnect msg for last client)");
    b.send("/end\n");
    b.waitClosed();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    body = httpBody(httpGet(port, "/v1/createswb?name=wb1", cookie));
    std::string token2 = between(body, "token='", "'");
    check(token2.size() == 32 && token2 != token, "createswb replaces unused whiteboard: " + body);
    TestSock c(port + 1);
    c.send(startCmd("alice", "wb1", token2, "13"));
    check(c.readUntil(connectMsg("alice", "13")) == connectMsg("alice", "13"), "replaced whiteboard has empty history");
  }

  // user names w/ special characters: URL encoded in requests, XML escaped in messages
  {
    section = "names";
    SWBServerOptions opts;
    opts.delDelay = 0;
    TestServer srv(opts);
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    const std::string name = "O'Brien & <Co> \xC3\xA9";
    std::string ename = urlEncode(name.c_str());
    std::string cookie = between(httpGet(port, "/v1/auth?user=" + ename + "&timestamp=1&signature=x"),
        "Set-Cookie: session=", "\r;");
    std::string body = httpBody(httpGet(port, "/v1/createswb?name=names", cookie));
    std::string token = between(body, "token='", "'");
    check(body.find("user='O&apos;Brien &amp; &lt;Co&gt; \xC3\xA9'") != std::string::npos, "user in reply: " + body);
    TestSock a(port + 1);
    a.send(startCmd(ename, "names", token, "1%262"));  // uuid "1&2"
    std::string esc = "O&apos;Brien &amp; &lt;Co&gt; \xC3\xA9";
    std::string expect = "<undo><connect name='" + esc + "' uuid='1&amp;2'/></undo>\n";
    std::string msg = a.readUntil("</undo>\n");
    check(msg == expect, "connect message escaped: " + msg);
    pugi::xml_document doc;
    check(doc.load_string(expect.c_str()) && doc.child("undo").child("connect").attribute("name").as_string() == name,
        "connect message parses to original name");
  }

  // whiteboard server on its own thread (as hosted by Write)
  {
    section = "SWBServerThread";
    SWBServerOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.logLevel = 0;
    std::shared_ptr<SWBServerThread> st1 = SWBServerThread::start(opts);
    if(!check(st1 && st1->apiPort() > 0, "start")) goto done;
    int port1 = st1->apiPort();
    check(httpGet(port1, "/v1/auth?user=a&timestamp=1&signature=x").compare(0, 12, "HTTP/1.1 200") == 0, "server running");
    opts.port = port1;  // in use
    std::shared_ptr<SWBServerThread> st2 = SWBServerThread::start(opts);
    check(st2 && st2->apiPort() > 0 && st2->apiPort() != port1, "falls back to free port if port in use");
    st1.reset();
    TestSock s(port1);
    check(s.fd < 0 || (s.send("GET / HTTP/1.1\r\n\r\n"), s.waitClosed() && s.buf.empty()), "stopped when destroyed");
    std::string lan = swbLanAddress();
    details += "server: LAN address: " + (lan.empty() ? std::string("(unknown)") : lan) + "\n";
    check(lan.empty() || (lan.find_first_not_of("0123456789.") == std::string::npos && lan.compare(0, 4, "127.") != 0),
        "LAN address: " + lan);
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

  // HTTP relay (long polling)
  {
    section = "HTTP relay";
    SWBServerOptions opts;
    opts.pollTimeout = 1;
    opts.pollClientTimeout = 1.5;
    opts.delDelay = 0;
    TestServer srv(opts);
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    auto session = [&](const std::string& user) {
      std::string resp = httpGet(port, "/v1/auth?user=" + user);
      return between(resp, "Set-Cookie: session=", "\r;");
    };
    std::string acookie = session("alice"), bcookie = session("bob"), ccookie = session("carol");
    std::string atoken = between(httpBody(httpGet(port, "/v1/createswb?name=hw", acookie)), "token='", "'");
    std::string btoken = between(httpBody(httpGet(port, "/v1/openswb?name=hw", bcookie)), "token='", "'");
    std::string ctoken = between(httpBody(httpGet(port, "/v1/openswb?name=hw", ccookie)), "token='", "'");
    auto status = [](const std::string& resp) { return resp.size() >= 12 ? resp.substr(9, 3) : std::string("---"); };

    TestSock h(port);  // alice - single persistent connection for all requests
    std::string resp = httpRequest(h, "POST", "/v1/swb/start?document=hw&user=alice&token=" + atoken + "&uuid=1");
    std::string id = httpBody(resp);
    check(status(resp) == "200" && id.size() == 20, "start: " + resp);
    check(status(httpRequest(h, "POST", "/v1/swb/start?document=hw&user=alice&token=bad&uuid=1")) == "403"
        && !h.eof, "start w/ bad token -> 403 (connection kept)");
    check(status(httpRequest(h, "GET", "/v1/swb/start?document=hw&user=alice&token=" + atoken)) == "405", "GET start -> 405");
    std::string hist = connectMsg("alice", "1");
    check(httpBody(httpRequest(h, "GET", "/v1/swb/poll?id=" + id + "&offset=0")) == hist, "poll returns history");
    check(status(httpRequest(h, "POST", "/v1/swb/send?id=" + id + "&seq=1", "hello")) == "200", "send");
    hist += "hello";
    check(httpBody(httpRequest(h, "GET", "/v1/swb/poll?id=" + id + "&offset=" + std::to_string(hist.size() - 5))) == "hello",
        "poll from offset (echo)");
    // raw relay client sees HTTP client's data and vice versa
    TestSock b(port + 1);
    b.send(startCmd("bob", "hw", btoken, "2"));
    hist += connectMsg("bob", "2");
    check(b.readUntil(connectMsg("bob", "2")) == hist, "raw client gets history incl. HTTP client data");
    // pending poll is answered when data arrives
    h.send("GET /v1/swb/poll?id=" + id + "&offset=" + std::to_string(hist.size()) + " HTTP/1.1\r\nHost: x\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(h.buf.empty(), "poll waits for data");
    b.send(dataCmd("frombob"));
    hist += "frombob";
    check(httpBody(readResponse(h)) == "frombob", "pending poll answered on new data");
    check(b.readUntil("frombob") == "frombob", "raw echo");
    // poll timeout -> empty response
    Timestamp tp = mSecSinceEpoch();
    resp = httpRequest(h, "GET", "/v1/swb/poll?id=" + id + "&offset=" + std::to_string(hist.size()));
    check(status(resp) == "200" && httpBody(resp).empty() && mSecSinceEpoch() - tp > 800, "poll timeout -> empty 200");
    // duplicate seq ignored
    httpRequest(h, "POST", "/v1/swb/send?id=" + id + "&seq=1", "dup");
    httpRequest(h, "POST", "/v1/swb/send?id=" + id + "&seq=2", "after");
    hist += "after";
    check(b.readUntil("after") == "after", "duplicate seq ignored");
    check(status(httpRequest(h, "GET", "/v1/swb/poll?id=" + id + "&offset=" + std::to_string(hist.size() + 1))) == "400",
        "offset beyond history -> 400");
    check(status(httpRequest(h, "GET", "/v1/swb/poll?id=nosuch&offset=0")) == "404", "unknown id -> 404");
    // pipelined requests, Expect: 100-continue
    h.send("POST /v1/swb/send?id=" + id + "&seq=3 HTTP/1.1\r\nContent-Length: 2\r\n\r\np1"
        "GET /v1/swb/poll?id=" + id + "&offset=" + std::to_string(hist.size()) + " HTTP/1.1\r\n\r\n");
    hist += "p1";
    check(status(readResponse(h)) == "200" && httpBody(readResponse(h)) == "p1", "pipelined requests");
    h.send("POST /v1/swb/send?id=" + id + "&seq=4 HTTP/1.1\r\nContent-Length: 2\r\nExpect: 100-continue\r\n\r\n");
    check(h.readUntil("\r\n\r\n") == "HTTP/1.1 100 Continue\r\n\r\n", "100 Continue");
    h.send("p2");
    hist += "p2";
    check(status(readResponse(h)) == "200" && b.readUntil("p2") == "p1p2", "body after 100 Continue");
    // /info lists HTTP clients
    TestSock info(port + 1);
    info.send("/info?document=hw\n");
    check(info.readUntil("alice").find("bob") != std::string::npos, "/info includes HTTP client");
    // same user connecting via raw relay kicks HTTP client (w/ pending poll)
    h.send("GET /v1/swb/poll?id=" + id + "&offset=" + std::to_string(hist.size()) + " HTTP/1.1\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TestSock a2(port + 1);
    a2.send(startCmd("alice", "hw", atoken, "4"));
    resp = readResponse(h);
    check(status(resp) == "403" && httpBody(resp).find("logged in from another location") != std::string::npos,
        "HTTP client kicked: " + resp);
    // kicked client's disconnect is broadcast before new client's connect (same as for raw relay)
    check(b.readUntil(connectMsg("alice", "4")) == disconnectMsg("alice") + connectMsg("alice", "4"), "kick broadcast");
    // HTTP client kicks raw client
    std::string id2 = httpBody(httpRequest(h, "POST", "/v1/swb/start?document=hw&user=alice&token=" + atoken + "&uuid=5"));
    check(!a2.readUntil("logged in from another location").empty() && a2.waitClosed(), "raw client kicked by HTTP client");
    check(b.readUntil(connectMsg("alice", "5")) == disconnectMsg("alice") + connectMsg("alice", "5"), "kick by HTTP client broadcast");
    // end
    check(status(httpRequest(h, "POST", "/v1/swb/end?id=" + id2)) == "200", "end");
    check(b.readUntil(disconnectMsg("alice")) == disconnectMsg("alice"), "end -> disconnect broadcast");
    check(status(httpRequest(h, "GET", "/v1/swb/poll?id=" + id2 + "&offset=0")) == "404", "poll after end -> 404");
    // HTTP client stops polling -> disconnected
    TestSock hc(port);
    std::string cid = httpBody(httpRequest(hc, "POST", "/v1/swb/start?document=hw&user=carol&token=" + ctoken + "&uuid=3"));
    check(b.readUntil(connectMsg("carol", "3")) == connectMsg("carol", "3"), "HTTP client connect broadcast");
    check(b.readUntil(disconnectMsg("carol"), 4) == disconnectMsg("carol"), "HTTP client timed out");
    check(status(httpRequest(hc, "GET", "/v1/swb/poll?id=" + cid + "&offset=0")) == "404", "expired session -> 404");
    // large history is returned in multiple polls (max 4 MB per response)
    {
      std::string big5(5 << 20, 'y');
      for(size_t ii = 0; ii < big5.size(); ii += 1009) big5[ii] = char('a' + ii % 26);
      b.send(dataCmd(big5));
      b.readBytes(big5.size());
      b.buf.clear();
      TestSock hd(port);
      std::string dtoken = between(httpBody(httpGet(port, "/v1/openswb?name=hw", session("dave"))), "token='", "'");
      std::string did = httpBody(httpRequest(hd, "POST", "/v1/swb/start?document=hw&user=dave&token=" + dtoken));
      std::string full = httpBody(httpRequest(hd, "GET", "/v1/swb/poll?id=" + did + "&offset=0", "", 10));
      check(full.size() == (4 << 20), fstring("first poll limited to 4 MB (%d)", int(full.size())));
      full += httpBody(httpRequest(hd, "GET", "/v1/swb/poll?id=" + did + "&offset=" + std::to_string(full.size()), "", 10));
      size_t p5 = full.find(big5.substr(0, 64));
      check(p5 != std::string::npos && full.compare(p5, big5.size(), big5) == 0, "large history across polls");
      httpRequest(hd, "POST", "/v1/swb/end?id=" + did);
      b.readUntil(disconnectMsg("dave"));
    }
    // request limits; Connection: close honored
    TestSock big(port);
    big.send("POST /v1/swb/send?id=x HTTP/1.1\r\nContent-Length: 999999999999\r\n\r\n");
    check(big.waitClosed() && status(big.buf) == "413", "body too large -> 413");
    TestSock chunked(port);
    chunked.send("POST /v1/swb/send?id=x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    check(chunked.waitClosed() && status(chunked.buf) == "411", "chunked body -> 411");
    TestSock cl(port);
    cl.send("GET /v1/swb/poll?id=x HTTP/1.1\r\nConnection: close\r\n\r\n");
    check(cl.waitClosed() && status(cl.buf) == "404", "Connection: close");
    // whiteboard deleted after last (HTTP) client leaves
    b.send("/end\n");
    b.waitClosed();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(httpGet(port, "/v1/openswb?name=hw", acookie).compare(0, 12, "HTTP/1.1 404") == 0, "whiteboard deleted");
  }

  // HTTP client (ugui/platforms/platformhttp.h) backends
  for(const char* backend : {"curl", "unet"}) {
    section = std::string("HttpClient ") + backend;
    HttpClient::Options hopts;
    hopts.userAgent = "WriteTest";
    std::unique_ptr<HttpClient> client = backend[0] == 'c' ? HttpClient::create(hopts) : createUnetHttpClient(hopts);
    if(!client) {
      details += "server: " + section + " not available - skipped\n";
      continue;
    }
    SWBServerOptions opts;
    opts.pollTimeout = 1;
    opts.trustProxy = true;  // so we can check that request headers are sent
    TestServer srv(opts);
    if(!check(srv.ok, "server start")) goto done;
    std::string base = "http://127.0.0.1:" + std::to_string(srv.port());
    HttpClient* hc = client.get();

    HttpResponse r = httpSync(hc, HttpRequest(base + "/v1/auth?user=bob", "X-Remote-User: alice"));
    std::string cookie = r.header("set-cookie");
    check(r.status == 200 && r.error.empty() && cookie.compare(0, 8, "session=") == 0, "GET w/ response header: "
        + r.error + r.headers);
    r = httpSync(hc, HttpRequest(base + "/v1/createswb?name=hc", "Cookie: " + cookie + "\r\nX-Remote-User: alice"));
    std::string token = between(bodyStr(r), "token='", "'");
    check(r.status == 200 && bodyStr(r).find("user='alice'") != std::string::npos, "request headers: " + bodyStr(r));
    r = httpSync(hc, HttpRequest(base + "/v1/swb/poll?id=nosuch"));
    check(r.status == 404 && r.error.empty(), "404 status");
    HttpRequest startreq(base + "/v1/swb/start?document=hc&user=alice&uuid=1&token=bad");
    startreq.method = "POST";
    r = httpSync(hc, startreq);
    check(r.status == 403 && bodyStr(r).find("accessdenied") != std::string::npos, "403 w/ body (empty POST)");
    startreq.url = base + "/v1/swb/start?document=hc&user=alice&uuid=1&token=" + token;
    std::string id = bodyStr(httpSync(hc, startreq));
    // binary POST body (gzip data is sent by Write)
    std::string bin(200000, '\0');
    for(size_t ii = 0; ii < bin.size(); ++ii) bin[ii] = char((ii*7919) >> 3);
    r = httpSync(hc, HttpRequest(base + "/v1/swb/send?id=" + id + "&seq=1", "Content-Type: application/octet-stream", bin));
    check(r.status == 200, "binary POST: " + r.error);
    r = httpSync(hc, HttpRequest(base + "/v1/swb/poll?id=" + id + "&offset=0"));
    std::string hist = connectMsg("alice", "1") + bin;
    check(r.status == 200 && bodyStr(r) == hist, fstring("binary response (%d bytes)", int(r.body.size())));
    std::string pollEnd = base + "/v1/swb/poll?id=" + id + "&offset=" + std::to_string(hist.size());
    // long poll: server responds after pollTimeout (1 s) w/ empty body
    Timestamp tq = mSecSinceEpoch();
    r = httpSync(hc, HttpRequest(pollEnd));
    check(r.status == 200 && r.body.empty() && mSecSinceEpoch() - tq > 800, "long poll");
    // request timeout
    HttpRequest shortreq(pollEnd);
    shortreq.timeout = 0.3;
    tq = mSecSinceEpoch();
    r = httpSync(hc, shortreq);
    check(r.status == 0 && !r.error.empty() && mSecSinceEpoch() - tq < 900, "request timeout: " + r.error);
    // cancel pending request
    uint64_t reqid = 0;
    auto w = httpStart(hc, HttpRequest(pollEnd), &reqid);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    tq = mSecSinceEpoch();
    hc->cancel(reqid);
    check(w->wait(2) && w->resp.status == 0 && w->resp.error == HttpClient::CANCELLED && w->doneTime - tq < 500,
        "cancel: " + w->resp.error);
    hc->cancel(reqid);  // no effect
    // more simultaneous requests than maxActive
    std::vector<std::shared_ptr<HttpWait>> waits;
    for(int ii = 0; ii < 20; ++ii)
      waits.push_back(httpStart(hc, HttpRequest(base + "/v1/swb/poll?id=nosuch&n=" + std::to_string(ii))));
    int nok = 0;
    for(auto& wt : waits) nok += wt->wait(5) && wt->resp.status == 404 ? 1 : 0;
    check(nok == 20, fstring("simultaneous requests (%d/20)", nok));
    // request from callback
    auto w2 = std::make_shared<HttpWait>();
    hc->request(HttpRequest(base + "/v1/swb/poll?id=nosuch"), [hc, w2, base](HttpResponse&& resp){
      hc->request(HttpRequest(base + "/v1/swb/poll?id=nosuch"), [w2](HttpResponse&& resp2){
        std::lock_guard<std::mutex> lock(w2->mutex);
        w2->resp = std::move(resp2);
        w2->done = true;
        w2->cond.notify_all();
      });
    });
    check(w2->wait(5) && w2->resp.status == 404, "request from callback");
    // connection refused (port of closed listening socket)
    int lsock = unet_socket(UNET_TCP, UNET_BIND, UNET_DEFAULT, "127.0.0.1", "0");
    struct unet_addr laddr;
    unet_address(lsock, &laddr);
    int closedPort = unet_address_port(&laddr);
    unet_close(lsock);
    r = httpSync(hc, HttpRequest("http://127.0.0.1:" + std::to_string(closedPort) + "/"));
    check(r.status == 0 && !r.error.empty(), "connection refused: " + r.error);
    if(backend[0] == 'u') {
      r = httpSync(hc, HttpRequest("https://127.0.0.1:" + std::to_string(srv.port()) + "/"));
      check(r.status == 0 && r.error.find("HTTPS") != std::string::npos, "https not supported: " + r.error);
    }
    // destroying client cancels pending requests
    w = httpStart(hc, HttpRequest(pollEnd));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.reset();
    check(w->done && w->resp.error == HttpClient::CANCELLED, "pending request cancelled by destructor");
  }

  // reverse proxy headers
  {
    section = "trustProxy";
    // headers are ignored w/o trustProxy
    {
      TestServer srv(SWBServerOptions{});
      if(!check(srv.ok, "server start")) goto done;
      std::string resp = httpGet(srv.port(), "/v1/auth?user=bob", "", "X-Remote-User: alice\r\n");
      std::string cookie = between(resp, "Set-Cookie: session=", "\r;");
      check(httpBody(httpGet(srv.port(), "/v1/createswb?name=p0", cookie)).find("user='bob'") != std::string::npos,
          "X-Remote-User ignored w/o trustProxy");
    }
    SWBServerOptions opts;
    opts.trustProxy = true;
    opts.enableTest = true;
    TestServer srv(opts);
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    const std::string alice = "X-Remote-User: alice\r\n";
    std::string resp = httpGet(port, "/v1/auth?user=mallory", "", alice);
    std::string cookie = between(resp, "Set-Cookie: session=", "\r;");
    std::string body = httpBody(httpGet(port, "/v1/createswb?name=p1", cookie, alice));
    check(body.find("user='alice'") != std::string::npos, "X-Remote-User overrides user arg: " + body);
    std::string token = between(body, "token='", "'");
    check(httpGet(port, "/v1/openswb?name=p1", cookie, "X-Remote-User: bob\r\n").compare(0, 12, "HTTP/1.1 403") == 0,
        "session user != proxy user -> 403");
    check(httpGet(port, "/v1/openswb?name=p1", cookie).compare(0, 12, "HTTP/1.1 200") == 0, "no proxy user -> session user");
    // HTTP relay: start user must match proxy user; client address from proxy headers
    auto post = [&](const std::string& path, const std::string& headers) {
      TestSock ps(port);
      ps.send("POST " + path + " HTTP/1.1\r\nContent-Length: 0\r\nConnection: close\r\n" + headers + "\r\n");
      ps.waitClosed();
      return ps.buf;
    };
    std::string resp1 = post("/v1/swb/start?document=p1&user=alice&uuid=1&token=" + token,
        alice + "X-Forwarded-For: 10.9.9.9, 203.0.113.7\r\n");
    std::string aid = httpBody(resp1);
    check(resp1.compare(0, 12, "HTTP/1.1 200") == 0, "start w/ matching proxy user");
    std::string resp2 = post("/v1/swb/start?document=testdoc&user=t1&uuid=2&token=SCRIBBLE_SYNC_TEST",
        "X-Remote-User: bob\r\nX-Real-IP: 198.51.100.2\r\n");
    check(resp2.compare(0, 12, "HTTP/1.1 403") == 0 && httpBody(resp2).find("accessdenied") != std::string::npos,
        "start user != proxy user rejected (even w/ test token)");
    check(post("/v1/swb/start?document=testdoc&user=t3&uuid=3&token=SCRIBBLE_SYNC_TEST",
        "X-Real-IP: 198.51.100.3\r\nX-Forwarded-For: 203.0.113.9\r\n").compare(0, 12, "HTTP/1.1 200") == 0,
        "start w/o proxy user");
    check(post("/v1/swb/end?id=" + aid, "X-Remote-User: bob\r\n").compare(0, 12, "HTTP/1.1 403") == 0,
        "request for other user's session rejected");
    std::string dbg = httpBody(httpGet(port, "/v1/debug"));
    check(dbg.find("alice@203.0.113.7 (HTTP)") != std::string::npos && dbg.find("t3@198.51.100.3 (HTTP)") != std::string::npos,
        "client address from X-Forwarded-For (last entry) and X-Real-IP: " + dbg);
  }

  // whiteboard links
  {
    section = "links";
    auto linkStr = [](const Url& u) { return u.user + "|" + u.pass + "|" + u.host + "|" + u.path + "|" + u.query; };
    struct { const char* link; const char* expect; } cases[] = {
      {"swb://u:p@host:7000/wb1", "u|p|host:7000|wb1|"},
      {"u@host/wb1?rxonly=1", "u||host|wb1|rxonly=1"},
      {"[::1]:7000/wb1", "||[::1]:7000|wb1|"},
      {"https://example.com/wb1", "||https://example.com|wb1|"},
      {"HTTPS://u:p@example.com:8443/swb/wb1?a=b#frag", "u|p|https://example.com:8443/swb|wb1|a=b"},
      {"http://example.com/a@b/wb1", "||http://example.com/a@b|wb1|"},
      {"https://example.com/", "||https://example.com||"},
      {"https://example.com", "||https://example.com||"},
    };
    for(auto& c : cases) {
      std::string res = linkStr(ScribbleApp::parseWhiteboard(c.link));
      check(res == c.expect, std::string("parseWhiteboard ") + c.link + ": " + res);
    }
    int p;
    std::string h = ScribbleSync::splitHostPort("https://example.com/swb", &p);
    check(h == "example.com" && p == 443, "splitHostPort https: " + h + fstring(":%d", p));
    h = ScribbleSync::splitHostPort("http://[::1]:8000/swb", &p);
    check(h == "::1" && p == 8000, "splitHostPort http: " + h + fstring(":%d", p));
    h = ScribbleSync::splitHostPort("host", &p);
    check(h == "host" && p == 7000, "splitHostPort host: " + h + fstring(":%d", p));
    check(ScribbleSync::httpBaseUrl("https://example.com/swb/") == "https://example.com/swb", "httpBaseUrl https");
    check(ScribbleSync::httpBaseUrl("host") == "http://host:7000", "httpBaseUrl host");
    check(ScribbleSync::basicAuthAllowed("https://example.com/swb") && ScribbleSync::basicAuthAllowed("HTTPS://x")
        && ScribbleSync::basicAuthAllowed("127.0.0.1:7000") && ScribbleSync::basicAuthAllowed("http://localhost:8080")
        && ScribbleSync::basicAuthAllowed("[::1]:7000") && !ScribbleSync::basicAuthAllowed("example.com")
        && !ScribbleSync::basicAuthAllowed("http://example.com") && !ScribbleSync::basicAuthAllowed("10.0.0.1:7000"),
        "Basic auth only over https or to loopback");
    // reference values from `openssl passwd -apr1 -salt <salt> <pw>`
    check(apr1Crypt("password", "xY0./") == "$apr1$xY0./$WnNEczpLd/xsjzVq1coY3/"
        && apr1Crypt("", "saltsalt") == "$apr1$saltsalt$a8ml/vK5HEjiZ5oypDWA7/", "apr1Crypt: " + apr1Crypt("password", "xY0./"));
    check(swbAuthSecret("pw") == "3601f707b2d8d206ab19a8329c69c7d9"
        && swbHtpasswdLine("bob", "pw", "abcdefgh") == "bob:$apr1$abcdefgh$mOzFSYvUPuxwQWa/j1UN4.", "swbHtpasswdLine");
    std::string randline = swbHtpasswdLine("bob", "pw");
    check(randline.size() == 4 + 37 && randline != swbHtpasswdLine("bob", "pw"), "htpasswd line w/ random salt: " + randline);
  }

  // server behind reverse proxy w/ HTTP Basic auth (AuthProxy, like nginx auth_basic)
  {
    section = "Basic auth";
    SWBServerOptions opts;
    opts.trustProxy = true;
    opts.delDelay = 0;
    TestServer srv(opts);
    if(!check(srv.ok, "server start")) goto done;
    AuthProxy proxy(srv.port(), [](const std::string& u, const std::string& p){
      return u == "alice" && p == swbAuthSecret("pw"); });
    if(!check(proxy.port() > 0, "proxy start")) goto done;
    std::string base = "http://127.0.0.1:" + std::to_string(proxy.port());
    for(const char* backend : {"curl", "unet"}) {
      HttpClient::Options hopts;
      std::unique_ptr<HttpClient> client = backend[0] == 'c' ? HttpClient::create(hopts) : createUnetHttpClient(hopts);
      if(!client) continue;
      std::string b = std::string(" (") + backend + ")";
      HttpRequest req(base + "/v1/auth?user=mallory&timestamp=1&signature=x", "X-Remote-User: mallory");
      HttpResponse r = httpSync(client.get(), req);
      check(r.status == 401 && r.header("WWW-Authenticate").find("Basic") == 0, "no credentials -> 401" + b);
      req.user = "alice";
      req.password = "wrong";
      check(httpSync(client.get(), req).status == 401, "wrong password -> 401" + b);
      req.password = swbAuthSecret("pw");
      r = httpSync(client.get(), req);
      std::string cookie = r.header("set-cookie");
      check(r.status == 200 && cookie.compare(0, 8, "session=") == 0, "credentials accepted" + b + ": " + r.error);
      HttpRequest req2(base + "/v1/createswb?name=ba" + backend, "Cookie: " + cookie.substr(0, cookie.find(';')));
      req2.user = "alice";
      req2.password = swbAuthSecret("pw");
      r = httpSync(client.get(), req2);
      check(r.status == 200 && bodyStr(r).find("user='alice'") != std::string::npos,
          "user from proxy, not from request" + b + ": " + bodyStr(r));
    }
  }

  // Write's client code: sign in and create whiteboard
  {
    section = "client";
    TestServer srv(SWBServerOptions{});
    if(!check(srv.ok, "server start")) goto done;
    int port = srv.port();
    std::string hostport = "127.0.0.1:" + std::to_string(port);
    ScribbleApp* app = ScribbleApp::app;
    ScribbleConfig* cfg = ScribbleApp::cfg;
    std::string prevServer = cfg->String("syncServer", ""), prevUser = cfg->String("syncUser", "");
    std::string prevPass = cfg->String("syncPass", "");
    cfg->set("syncServer", hostport.c_str());
    cfg->set("syncUser", "alice");
    cfg->set("syncPass", md5str("styluslabsanything").c_str());  // ignored by server
    Url url;
    url.host = hostport;
    url.path = "clientwb";
    if(check(app->syncSignIn(url), "syncSignIn")) {
      HttpResponse reply = app->syncAPICall(url, "/v1/createswb?name=clientwb&title=Test");
      std::string body = bodyStr(reply);
      check(reply.status == 200 && body.find("<swb name='clientwb' title='Test' user='alice' token='") != std::string::npos
          && body.find("relay='http'") != std::string::npos, "syncAPICall createswb: " + body);
    }
    // whiteboard link as URL (http://) w/ credentials in link
    url = ScribbleApp::parseWhiteboard("http://bob:pw@" + hostport + "/clientwb");
    check(url.host == "http://" + hostport && url.path == "clientwb" && url.user == "bob" && url.pass == "pw",
        "parseWhiteboard http URL: " + url.host + " " + url.path + " " + url.user + " " + url.pass);
    if(check(app->syncSignIn(url), "syncSignIn w/ http:// link")) {
      HttpResponse reply = app->syncAPICall(url, "/v1/openswb?name=clientwb");
      std::string body = bodyStr(reply);
      check(reply.status == 200 && body.find("<swb name='clientwb' title='Test' user='bob' token='") != std::string::npos,
          "syncAPICall openswb w/ http:// link: " + body);
    }
    // through proxy w/ HTTP Basic auth: credentials from link sent w/ all requests (http allowed for loopback)
    {
      SWBServerOptions opts;
      opts.trustProxy = true;
      TestServer psrv(opts);
      AuthProxy proxy(psrv.port(), [](const std::string& u, const std::string& p){
        return u == "carol" && p == swbAuthSecret("secret"); });
      url = ScribbleApp::parseWhiteboard("http://carol:secret@127.0.0.1:" + std::to_string(proxy.port()) + "/authwb");
      if(check(psrv.ok && proxy.port() > 0 && app->syncSignIn(url), "syncSignIn through auth proxy")) {
        check(app->syncAuthUser == "carol" && app->syncAuthPass == swbAuthSecret("secret"), "Basic auth credentials");
        HttpResponse reply = app->syncAPICall(url, "/v1/createswb?name=authwb");
        check(reply.status == 200 && bodyStr(reply).find("user='carol'") != std::string::npos,
            "syncAPICall through auth proxy: " + bodyStr(reply));
        app->syncAuthPass = "wrong";
        check(app->syncAPICall(url, "/v1/openswb?name=authwb").status == 401, "syncAPICall w/ wrong password -> 401");
      }
      app->syncAuthUser.clear();
      app->syncAuthPass.clear();
    }
    // user name w/ special characters
    url.host = hostport;
    url.user = "O'Brien & Co";
    url.pass = "x";
    if(check(app->syncSignIn(url), "syncSignIn w/ special characters in name")) {
      HttpResponse reply = app->syncAPICall(url, "/v1/openswb?name=clientwb");
      check(reply.status == 200 && bodyStr(reply).find("user='O&apos;Brien &amp; Co'") != std::string::npos,
          "user name URL encoded: " + bodyStr(reply));
    }
    // server for whiteboard hosted on this device: shared while in use, stopped when no longer used
    {
      std::shared_ptr<SWBServerThread> h1 = app->hostWhiteboardServer();
      std::shared_ptr<SWBServerThread> h2 = app->hostWhiteboardServer();
      if(check(h1 && h1 == h2, "hosted server shared")) {
        int hport = h1->apiPort();
        check(httpGet(hport, "/v1/auth?user=a&timestamp=1&signature=x").compare(0, 12, "HTTP/1.1 200") == 0,
            "hosted server running on port " + std::to_string(hport));
        h1.reset();
        h2.reset();
        check(app->hostedServer.expired(), "hosted server stopped when unused");
      }
    }
    cfg->set("syncServer", prevServer.c_str());
    cfg->set("syncUser", prevUser.c_str());
    cfg->set("syncPass", prevPass.c_str());
    app->syncSession.clear();
  }

done:
  resultStr = details + fstring("server tests: %d checks completed in %d ms: %d failed", nchecks,
      int(mSecSinceEpoch() - t0), nFailed);
}
