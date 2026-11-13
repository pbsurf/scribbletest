#pragma once

// Minimal reverse proxy w/ HTTP Basic auth for tests - like nginx w/ auth_basic and
//  `proxy_set_header X-Remote-User $remote_user` (see scribbleres/swbserver-nginx.conf)
// - one request per connection (responds w/ Connection: close); one thread per connection
// - requests w/o valid credentials get 401; otherwise request is forwarded to upstream w/ X-Remote-User set (any
//  X-Remote-User from client is removed)

#include "ulib/unet.h"
#include "ulib/stringutil.h"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>

class AuthProxy
{
public:
  typedef std::function<bool(const std::string& user, const std::string& pass)> Validator;
  std::atomic<int> nForwarded{0}, nRejected{0};

  AuthProxy(int upstreamPort, Validator v) : upstream(std::to_string(upstreamPort)), validate(std::move(v))
  {
    listenfd = unet_socket(UNET_TCP, UNET_BIND, UNET_NOBLOCK, "127.0.0.1", "0");
    if(listenfd < 0 || unet_listen(listenfd, 64) != 0) return;
    struct unet_addr addr;
    if(unet_address(listenfd, &addr) == 0)
      proxyPort = unet_address_port(&addr);
    acceptThread = std::thread([this](){ acceptLoop(); });
  }

  ~AuthProxy()
  {
    stopFlag = true;
    if(acceptThread.joinable()) acceptThread.join();
    std::lock_guard<std::mutex> lock(mutex);
    for(std::thread& t : handlers) t.join();
    if(listenfd >= 0) unet_close(listenfd);
  }

  int port() const { return proxyPort; }

private:
  void acceptLoop()
  {
    while(!stopFlag) {
      if(unet_select(listenfd, -1, 0.1) <= 0) continue;
      int fd = unet_accept(listenfd, NULL);
      if(fd < 0) continue;
      unet_set_flags(fd, UNET_DEFAULT);
      std::lock_guard<std::mutex> lock(mutex);
      handlers.emplace_back([this, fd](){ handle(fd); unet_close(fd); });
    }
  }

  // read until pred(buf) or timeout/stop; returns false on close or timeout
  bool readUntil(int fd, std::string& buf, std::function<bool()> pred, double timeout)
  {
    auto t0 = std::chrono::steady_clock::now();
    char tmp[65536];
    while(!pred()) {
      if(stopFlag || std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout)
        return false;
      if(unet_select(fd, -1, 0.1) <= 0) continue;
      int n = unet_recv(fd, tmp, sizeof(tmp));
      if(n <= 0) return false;
      buf.append(tmp, n);
    }
    return true;
  }

  static void sendAll(int fd, const std::string& s)
  {
    size_t pos = 0;
    while(pos < s.size()) {
      int n = unet_send(fd, s.data() + pos, int(s.size() - pos));
      if(n <= 0) return;
      pos += n;
    }
  }

  void handle(int fd)
  {
    std::string in;
    if(!readUntil(fd, in, [&](){ return in.find("\r\n\r\n") != std::string::npos; }, 10)) return;
    size_t hdrend = in.find("\r\n\r\n");
    size_t eol = in.find("\r\n");
    std::string reqline = in.substr(0, eol);
    std::string fwd = reqline + "\r\n";
    std::string auth;
    long long contentLength = 0;
    size_t pos = eol + 2;
    while(pos < hdrend) {
      size_t e = in.find("\r\n", pos);
      std::string line = in.substr(pos, e - pos);
      pos = e + 2;
      size_t colon = line.find(':');
      if(colon == std::string::npos) continue;
      std::string name = toLower(line.substr(0, colon));
      std::string value = line.substr(line.find_first_not_of(' ', colon + 1) == std::string::npos ? line.size()
          : line.find_first_not_of(' ', colon + 1));
      if(name == "authorization") { auth = value; continue; }
      if(name == "x-remote-user" || name == "connection") continue;
      if(name == "content-length") contentLength = atoll(value.c_str());
      fwd += line + "\r\n";
    }
    size_t need = hdrend + 4 + size_t(contentLength);
    if(!readUntil(fd, in, [&](){ return in.size() >= need; }, 10)) return;

    std::string user, pass;
    if(auth.compare(0, 6, "Basic ") == 0) {
      std::vector<unsigned char> dec = base64_decode(auth.substr(6));
      std::string cred(dec.begin(), dec.end());
      size_t colon = cred.find(':');
      if(colon != std::string::npos) {
        user = cred.substr(0, colon);
        pass = cred.substr(colon + 1);
      }
    }
    if(user.empty() || !validate(user, pass)) {
      ++nRejected;
      sendAll(fd, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"test\"\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    ++nForwarded;
    fwd += "X-Remote-User: " + user + "\r\nConnection: close\r\n\r\n";
    fwd.append(in, hdrend + 4, size_t(contentLength));
    int up = unet_socket(UNET_TCP, UNET_CONNECT, UNET_DEFAULT, "127.0.0.1", upstream.c_str());
    if(up < 0) {
      sendAll(fd, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }
    sendAll(up, fwd);
    // relay response until upstream closes
    std::string resp;
    readUntil(up, resp, [](){ return false; }, 120);
    unet_close(up);
    sendAll(fd, resp);
  }

  std::string upstream;
  Validator validate;
  int listenfd = -1;
  int proxyPort = 0;
  std::atomic_bool stopFlag{false};
  std::thread acceptThread;
  std::mutex mutex;
  std::vector<std::thread> handlers;
};
