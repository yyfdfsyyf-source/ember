// Headless browser support: launches a real Chrome/Edge in headless mode and
// drives it through the Chrome DevTools Protocol over a WebSocket. The WS
// client is a small local implementation on raw sockets: WinHTTP refuses to
// pass "Connection: Upgrade" through, so we do the handshake ourselves.
#include "agent/browser.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#endif

#include "agent/http.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#error "headless browser support is Windows-only in this build"
#endif

namespace agent {

namespace {

using Clock = std::chrono::steady_clock;

mini::Value errVal(std::string const& msg) {
  mini::Value o = mini::Value::makeObject();
  o.set("error", mini::Value::makeString(msg));
  return o;
}

#ifdef _WIN32
std::wstring s2w(std::string const& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}
std::string w2s(std::wstring const& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], n, nullptr, nullptr);
  return out;
}
std::string winErr(DWORD err) {
  wchar_t wbuf[256] = {0};
  DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                           err, 0, wbuf, (DWORD)256, nullptr);
  while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n')) wbuf[--n] = 0;
  return w2s(std::wstring(wbuf, n));
}
#endif

bool fileExists(std::string const& p) {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec);
}

std::vector<std::string> browserCandidates() {
  std::vector<std::string> out;
  if (char const* c = std::getenv("CHROME_PATH")) {
    if (*c) out.push_back(c);
  }
  auto addEnv = [&](char const* var, char const* rel) {
    if (char const* base = std::getenv(var)) {
      if (*base) {
        std::string p = std::string(base) + "\\" + rel;
        if (fileExists(p)) out.push_back(p);
      }
    }
  };
  addEnv("LOCALAPPDATA", "Google\\Chrome\\Application\\chrome.exe");
  addEnv("PROGRAMFILES", "Google\\Chrome\\Application\\chrome.exe");
  addEnv("ProgramFiles(x86)", "Google\\Chrome\\Application\\chrome.exe");
  addEnv("PROGRAMFILES", "Microsoft\\Edge\\Application\\msedge.exe");
  addEnv("ProgramFiles(x86)", "Microsoft\\Edge\\Application\\msedge.exe");
  addEnv("LOCALAPPDATA", "Microsoft\\Edge\\Application\\msedge.exe");
  return out;
}

std::string findBrowserExe() {
  for (auto const& c : browserCandidates()) {
    if (fileExists(c)) return c;
  }
  return "";
}

#ifdef _WIN32
std::string b64Encode(unsigned char const* data, size_t n) {
  static char const* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)data[i] << 16;
    if (i + 1 < n) v |= (unsigned)data[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned)data[i + 2];
    out += T[(v >> 18) & 63];
    out += T[(v >> 12) & 63];
    out += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
    out += (i + 2 < n) ? T[v & 63] : '=';
  }
  return out;
}

bool sendAll(SOCKET fd, char const* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    int w = ::send(fd, data + off, (int)(n - off), 0);
    if (w <= 0) return false;
    off += (size_t)w;
  }
  return true;
}

bool recvAll(SOCKET fd, char* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    int r = ::recv(fd, buf + got, (int)(n - got), 0);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

// Whole httpSuccess handshake: GET /devtools/... with the WebSocket upgrade
// headers; returns the connected socket on 101.
SOCKET wsHandshake(std::string const& host, int port, std::string const& path,
                   std::string* err) {
  SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == INVALID_SOCKET) {
    *err = "socket() failed: " + std::to_string(WSAGetLastError());
    return INVALID_SOCKET;
  }
  // Bound the blocking reads/sends. Crash recovery relies on this firing
  // promptly when the renderer dies mid-command; 30s is far beyond any
  // real CDP response (screenshots of huge pages are the slow end).
  DWORD tv = 30000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char const*)&tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char const*)&tv, sizeof tv);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  addr.sin_addr.s_addr = ::inet_addr(host.c_str());
  if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
    *err = "connect to " + host + ":" + std::to_string(port) +
           " failed: " + std::to_string(WSAGetLastError());
    ::closesocket(fd);
    return INVALID_SOCKET;
  }
  static bool seeded = false;
  if (!seeded) { std::srand((unsigned)GetTickCount64()); seeded = true; }
  unsigned char rnd[16];
  for (int i = 0; i < 16; i++) rnd[i] = (unsigned char)(std::rand() & 0xFF);
  std::string key = b64Encode(rnd, 16);
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                    std::to_string(port) +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                    key + "\r\n\r\n";
  if (!sendAll(fd, req.data(), req.size())) {
    *err = "handshake write failed: " + std::to_string(WSAGetLastError());
    ::closesocket(fd);
    return INVALID_SOCKET;
  }
  // Read the response headers (bounded by the receive timeout).
  std::string resp;
  char buf[1024];
  while (resp.find("\r\n\r\n") == std::string::npos) {
    int r = ::recv(fd, buf, sizeof buf, 0);
    if (r <= 0) {
      *err = "handshake read failed: " + std::to_string(WSAGetLastError());
      ::closesocket(fd);
      return INVALID_SOCKET;
    }
    resp.append(buf, (size_t)r);
    if (resp.size() > 65536) break;
  }
  if (resp.compare(0, 12, "HTTP/1.1 101") != 0 && resp.compare(0, 12, "HTTP/1.0 101") != 0) {
    std::string line = resp.substr(0, resp.find('\r'));
    *err = "WebSocket upgrade rejected (" + line + ")";
    ::closesocket(fd);
    return INVALID_SOCKET;
  }
  return fd;
}

// Client-side WebSocket frames (RFC 6455), always masked.
bool wsSendFrame(SOCKET fd, std::string const& data) {
  std::string frame;
  frame.reserve(data.size() + 16);
  frame.push_back((char)0x81);  // FIN | text
  uint64_t len = data.size();
  if (len < 126) {
    frame.push_back((char)(0x80 | len));
  } else if (len <= 0xFFFF) {
    frame.push_back((char)(0x80 | 126));
    frame.push_back((char)(len >> 8));
    frame.push_back((char)(len & 0xFF));
  } else {
    frame.push_back((char)(0x80 | 127));
    for (int i = 7; i >= 0; i--) frame.push_back((char)((len >> (8 * i)) & 0xFF));
  }
  static uint32_t maskCounter = 0;
  maskCounter += 0x9E3779B9u;
  unsigned char mask[4] = {(unsigned char)(maskCounter >> 24), (unsigned char)(maskCounter >> 16),
                           (unsigned char)(maskCounter >> 8), (unsigned char)maskCounter};
  frame.append((char const*)mask, 4);
  size_t base = frame.size();
  frame.resize(base + (size_t)len);
  for (size_t i = 0; i < (size_t)len; i++) frame[base + i] = data[i] ^ mask[i & 3];
  return sendAll(fd, frame.data(), frame.size());
}

// Reply to pings so the peer keeps the connection alive.
bool wsSendPong(SOCKET fd, std::string const& payload) {
  std::string frame;
  frame.push_back((char)0x8A);
  uint64_t len = payload.size();
  if (len < 126) {
    frame.push_back((char)(0x80 | len));
  } else {
    frame.push_back((char)(0x80 | 126));
    frame.push_back((char)(len >> 8));
    frame.push_back((char)(len & 0xFF));
  }
  static uint32_t maskCounter = 0;
  maskCounter += 0x517CC1B7u;
  unsigned char mask[4] = {(unsigned char)(maskCounter >> 24), (unsigned char)(maskCounter >> 16),
                           (unsigned char)(maskCounter >> 8), (unsigned char)maskCounter};
  frame.append((char const*)mask, 4);
  size_t base = frame.size();
  frame.resize(base + (size_t)len);
  for (size_t i = 0; i < (size_t)len; i++) frame[base + i] = payload[i] ^ mask[i & 3];
  return sendAll(fd, frame.data(), frame.size());
}

// Receive one complete text message, reassembling fragments and answering
// pings. Returns false on close frame or transport error.
bool wsRecvMessage(SOCKET fd, std::string& out) {
  out.clear();
  for (;;) {
    unsigned char hdr[2];
    if (!recvAll(fd, (char*)hdr, 2)) return false;
    bool fin = (hdr[0] & 0x80) != 0;
    int op = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;
    unsigned char ext[8];
    if (len == 126) {
      if (!recvAll(fd, (char*)ext, 2)) return false;
      len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
      if (!recvAll(fd, (char*)ext, 8)) return false;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && !recvAll(fd, (char*)mask, 4)) return false;
    std::string payload;
    if (len > 0) {
      payload.resize((size_t)len);
      if (!recvAll(fd, &payload[0], (size_t)len)) return false;
      if (masked)
        for (size_t i = 0; i < (size_t)len; i++) payload[i] ^= mask[i & 3];
    }
    if (op == 0x8) return false;           // close frame
    if (op == 0x9) {                       // ping
      wsSendPong(fd, payload);
      continue;
    }
    if (op == 0xA) continue;               // pong
    out += payload;
    if (fin) {
      if (std::getenv("BROWSER_DEBUG"))
        std::fprintf(stderr, "WS << op=%d len=%zu fin=1\n", op, out.size());
      return true;
    }
    // continuation frames re-enter the loop with op 0.
  }
}
#endif

// Compact page outline produced by Runtime.evaluate with returnByValue:
// one line per meaningful element, e.g.
//   button#go[role=button] "Go" @640,120
//   a ->https://example.com "Docs"
//   input#q[type="text"] label="Search"
std::string const kSnapshotJs = R"JS((() => {
  const skip = new Set(['SCRIPT','STYLE','NOSCRIPT','TEMPLATE','SVG','HEAD','LINK','META','TITLE']);
  const interactiveRoles = new Set(['button','link','tab','menuitem','checkbox','radio','combobox','option','switch','textbox','searchbox','slider','listbox']);
  const interactiveTags = new Set(['A','BUTTON','INPUT','SELECT','TEXTAREA','SUMMARY','DETAILS']);
  const lines = [];
  const walk = (el, depth) => {
    if (!el || !el.tagName || skip.has(el.tagName) || depth > 14 || lines.length >= 500) return;
    const vis = el.getClientRects ? el.getClientRects().length > 0 : true;
    const tag = el.tagName.toLowerCase();
    const role = el.getAttribute ? el.getAttribute('role') : null;
    const inter = interactiveTags.has(el.tagName) || (role && interactiveRoles.has(role));
    let ownText = '';
    for (const n of el.childNodes) if (n.nodeType === 3) ownText += n.textContent;
    ownText = ownText.replace(/\s+/g, ' ').trim();
    const label = (el.getAttribute ? (el.getAttribute('aria-label') || el.getAttribute('title') || el.getAttribute('placeholder')) : '') || '';
    if (!vis && !inter) { for (const c of el.children) walk(c, depth + 1); return; }
    if (!inter && !ownText && !label && !el.id) { for (const c of el.children) walk(c, depth + 1); return; }
    let l = tag;
    if (el.id) l += '#' + el.id;
    try {
      const cls = (typeof el.className === 'string' ? el.className : '').split(/\s+/).filter(Boolean).slice(0, 3).join('.');
      if (cls) l += '.' + cls;
    } catch (e) {}
    if (role) l += '[role=' + role + ']';
    if (el.tagName === 'A' && el.getAttribute('href')) l += ' ->' + el.getAttribute('href').slice(0, 140);
    if (el.tagName === 'IMG' && el.src) l += ' <' + el.src.slice(0, 100) + '>';
    if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.tagName === 'SELECT') {
      l += '[type="' + (el.type || tag) + '"]';
      const val = (el.value || '').slice(0, 60);
      if (val) l += ' value="' + val + '"';
    }
    if (label && !(el.tagName === 'INPUT' && el.placeholder === label)) l += ' label="' + label.slice(0, 60) + '"';
    if (ownText) l += ' "' + ownText.slice(0, 80) + '"';
    if (inter) {
      const r = el.getBoundingClientRect();
      if (r.width > 0 && r.height > 0 && r.bottom > 0 && r.top < innerHeight) {
        l += ' @' + Math.round(r.left + r.width / 2) + ',' + Math.round(r.top + r.height / 2);
      }
    }
    lines.push(l);
    for (const c of el.children) walk(c, depth + 1);
  };
  walk(document.body, 0);
  return lines;
})()
)JS";

}  // namespace

struct BrowserSession::Impl {
  PROCESS_INFORMATION pi{};
  SOCKET sock = INVALID_SOCKET;
  std::string userDataDir;
  int64_t nextId = 1;
  bool crashed = false;
};

BrowserSession::BrowserSession() = default;
BrowserSession::~BrowserSession() { stop(); }

bool BrowserSession::running() const { return state_ != nullptr; }

bool BrowserSession::crashed() const { return state_ && state_->crashed; }

std::string BrowserSession::ensureStarted() {
  if (state_) {
    if (state_->crashed) {
      stop();  // last target crashed; relaunch fresh
    } else {
      return "";
    }
  }
#ifndef _WIN32
  return "headless browser is only supported on Windows in this build";
#endif
  Impl* s = new Impl;
  state_ = s;  // from here on, stop() can clean up after any failure

  std::string exe = findBrowserExe();
  if (exe.empty()) {
    stop();
    return "no Chrome/Edge binary found (install Chrome/Edge or set CHROME_PATH)";
  }
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    stop();
    return "WSAStartup failed";
  }
  s->userDataDir = (std::filesystem::temp_directory_path() / "tui-agent-browser").string() + "-" +
                   std::to_string(GetCurrentProcessId()) + "-" +
                   std::to_string(GetTickCount64());
  std::error_code ec;
  std::filesystem::create_directories(s->userDataDir, ec);
  if (ec) {
    stop();
    return "cannot create browser profile dir: " + ec.message();
  }

  std::wstring wExe = s2w(exe);
  std::wstring cmd =
      L"\"" + wExe + L"\" --headless --disable-gpu --disable-extensions --no-first-run "
      L"--no-default-browser-check --mute-audio --hide-scrollbars --disable-background-networking "
      L"--disable-component-update --disable-sync --remote-allow-origins=* "
      L"--window-size=1366,768 --remote-debugging-port=0 --user-data-dir=\"" +
      s2w(s->userDataDir) + L"\" about:blank";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back(0);
  if (!CreateProcessW(wExe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si,
                      &s->pi)) {
    std::string msg = "failed to launch '" + exe + "': " + winErr(GetLastError());
    s->userDataDir.clear();
    stop();
    return msg;
  }

  // Wait for the debug port to appear.
  std::string portFile = s->userDataDir + "/DevToolsActivePort";
  auto deadline = Clock::now() + std::chrono::seconds(15);
  int port = 0;
  while (Clock::now() < deadline) {
    std::ifstream f(portFile);
    if (f) {
      std::string line;
      if (std::getline(f, line)) {
        int p = std::atoi(line.c_str());
        if (p > 0) { port = p; break; }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!port) {
    stop();
    return "Chrome started but no DevTools port appeared within 15s";
  }

  // Discover the page target's WebSocket URL.
  std::string body;
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/json/list";
  int st = httpRequest({.url = url,
                        .method = "GET",
                        .body = std::string(),
                        .headers = {},
                        .timeoutSec = 5},
                       [&](char const* p, size_t n) { body.append(p, n); });
  std::string wsUrl;
  if (st == 200) {
    mini::Value list;
    if (mini::tryParse(body, list) && list.type == mini::Value::Array) {
      for (auto const& t : list.arr) {
        if (!(t.has("type") && t.get("type")->asString() == "page")) continue;
        mini::Value const* w = t.get("webSocketDebuggerUrl");
        if (w && w->type == mini::Value::String) { wsUrl = w->s; break; }
      }
    }
  }
  if (wsUrl.empty()) {
    stop();
    return "DevTools did not expose a page target";
  }

  // Parse ws://host:port/path
  std::string rest = wsUrl.find("://") != std::string::npos ? wsUrl.substr(wsUrl.find("://") + 3)
                                                            : wsUrl;
  size_t slash = rest.find('/');
  std::string auth = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = auth.rfind(':');
  std::string host = colon == std::string::npos ? auth : auth.substr(0, colon);
  int wsPort = colon == std::string::npos ? 80 : std::atoi(auth.substr(colon + 1).c_str());
  if (host.empty() || wsPort <= 0) {
    stop();
    return "bad DevTools WebSocket URL: " + wsUrl;
  }

  std::string wsErr;
  s->sock = wsHandshake(host, wsPort, path, &wsErr);
  if (s->sock == INVALID_SOCKET) {
    stop();
    return wsErr;
  }

  // Wake the page target so commands are accepted (harmless no-op).
  mini::Value hello = mini::Value::makeObject();
  mini::Value r = call("Runtime.enable", hello);
  if (r.has("error")) {
    std::string msg = r.get("error")->asString();
    stop();
    return "CDP init failed: " + msg;
  }

  // The previous renderer crashed: bring the last page back so tools keep
  // working without the caller noticing (other than a brief pause). Crash
  // recovery also loses history, so only the last URL is restored.
  if (!lastUrl_.empty()) {
    mini::Value p = mini::Value::makeObject();
    p.set("url", mini::Value::makeString(lastUrl_));
    call("Page.navigate", p);
    auto until = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < until) {
      mini::Value rs = evaluate("document.readyState", false);
      if (!rs.has("error") && rs.type == mini::Value::String && rs.s == "complete") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
  return "";
}

bool BrowserSession::hasAttachedFiles() {
  if (!state_) return false;
  // True when any file input on the page currently holds an uploaded file.
  mini::Value v = evaluate(
      "(()=>{let f=document.querySelectorAll('input[type=file]');"
      "for(let i=0;i<f.length;i++){if(f[i].files.length>0)return true}return false})()",
      false);
  return v.type == mini::Value::Bool && v.b;
}

void BrowserSession::clearFileInputs() {
  if (!state_) return;
  mini::Value g = call("DOM.getDocument", mini::Value());
  if (g.has("error")) return;
  mini::Value const* root = g.get("root");
  if (!root || !root->get("nodeId")) return;
  mini::Value qp = mini::Value::makeObject();
  qp.set("nodeId", mini::Value::makeInt(root->get("nodeId")->asInt(0)));
  qp.set("selector", mini::Value::makeString("input[type=file]"));
  mini::Value q = call("DOM.querySelectorAll", qp);
  if (q.has("error")) return;
  mini::Value const* ids = q.get("nodeIds");
  if (!ids || ids->type != mini::Value::Array) return;
  for (auto const& idv : ids->arr) {
    mini::Value fp = mini::Value::makeObject();
    fp.set("nodeId", mini::Value::makeInt(idv.asInt(0)));
    fp.set("files", mini::Value::makeArray());  // empty list detaches any file
    call("DOM.setFileInputFiles", fp);
  }
}

void BrowserSession::stop() {
  if (!state_) return;
  Impl* s = state_;
  state_ = nullptr;
#ifdef _WIN32
  if (s->sock != INVALID_SOCKET) ::closesocket(s->sock);
  if (s->pi.hProcess) {
    TerminateProcess(s->pi.hProcess, 1);
    WaitForSingleObject(s->pi.hProcess, 3000);
    CloseHandle(s->pi.hProcess);
    if (s->pi.hThread) CloseHandle(s->pi.hThread);
  }
#endif
  if (!s->userDataDir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(s->userDataDir, ec);
  }
  delete s;
}

namespace {
mini::Value jsonOf(std::string const& text) {
  mini::Value v;
  if (mini::tryParse(text, v)) return v;
  return mini::Value();
}
}  // namespace

mini::Value BrowserSession::call(std::string const& method, mini::Value params) {
#ifdef _WIN32
  if (!state_) return errVal("browser is not running");
  Impl& s = *state_;
  std::string raw = "{\"id\":" + std::to_string(s.nextId) + ",\"method\":\"" + method +
                    "\",\"params\":" + mini::dump(params) + "}";
  if (std::getenv("BROWSER_DEBUG"))
    std::fprintf(stderr, "WS >> id=%lld %s len=%zu\n", (long long)s.nextId, method.c_str(),
                 raw.size());
  if (!wsSendFrame(s.sock, raw)) {
    s.crashed = true;
    return errVal("websocket send failed; browser session will restart");
  }
  int64_t want = s.nextId++;

  auto deadline = Clock::now() + std::chrono::seconds(180);
  while (Clock::now() < deadline) {
    auto t0 = Clock::now();
    std::string msg;
    if (!wsRecvMessage(s.sock, msg)) {
      s.crashed = true;
      return errVal("websocket receive failed; browser session will restart");
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    if (std::getenv("BROWSER_DEBUG"))
      std::fprintf(stderr, "WS >> wait %lldms got %.80s\n", (long long)ms, msg.c_str());
    mini::Value v = jsonOf(msg);
    if (!v.has("id")) {
      if (v.has("method") && v.get("method")->type == mini::Value::String &&
          v.get("method")->s.find("targetCrashed") != std::string::npos) {
        s.crashed = true;
        return errVal("browser page crashed; session will restart");
      }
      continue;  // unsolicited event, ignore
    }
    mini::Value const* idv = v.get("id");
    if (idv->type != mini::Value::Int || idv->i != want) continue;
    if (auto* e = v.get("error")) {
      std::string m = e->has("message") ? e->get("message")->asString() : "CDP error";
      return errVal(m);
    }
    if (mini::Value const* r = v.get("result")) return *r;
    return errVal("CDP reply without result");
  }
  return errVal("timed out waiting for CDP response to " + method);
#else
  (void)method;
  (void)params;
  return errVal("headless browser is not supported on this platform");
#endif
}

mini::Value BrowserSession::evaluate(std::string const& expression, bool awaitPromise) {
  mini::Value p = mini::Value::makeObject();
  p.set("expression", mini::Value::makeString(expression));
  p.set("returnByValue", mini::Value::makeBool(true));
  p.set("awaitPromise", mini::Value::makeBool(awaitPromise));
  mini::Value r = call("Runtime.evaluate", p);
  if (r.has("error")) return r;
  if (mini::Value const* ex = r.get("exceptionDetails")) {
    std::string m = ex->has("text") ? ex->get("text")->asString() : "JS exception";
    if (ex->has("exception")) {
      mini::Value const* e = ex->get("exception");
      if (e->has("description")) m += ": " + e->get("description")->asString();
    }
    return errVal(m);
  }
  // ReturnByValue responses wrap the value as RemoteObject; unwrap it.
  if (mini::Value const* res = r.get("result")) {
    if (res->has("value")) return *res->get("value");
    return mini::Value();  // undefined / thrown-in-promise etc.: null
  }
  return errVal("evaluate returned no result");
}

std::string BrowserSession::waitReady(int waitMs) {
  if (!state_) return "browser is not running";
  auto deadline = Clock::now() + std::chrono::milliseconds(waitMs);
  std::string state;
  while (Clock::now() < deadline) {
    mini::Value rs = evaluate("document.readyState", false);
    if (!rs.has("error") && rs.type == mini::Value::String) {
      state = rs.s;
      if (state == "complete") return "";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return "page did not finish loading within " + std::to_string(waitMs) + "ms (readyState=" +
         state + ")";
}

std::string BrowserSession::navigateAndWait(std::string const& url, int waitMs) {
  if (!state_) return "browser is not running";
  clearFileInputs();  // avoid the Chromium file-input reload crash
  mini::Value p = mini::Value::makeObject();
  p.set("url", mini::Value::makeString(url));
  mini::Value r = call("Page.navigate", p);
  if (!r.has("error")) lastUrl_ = url;
  if (r.has("error")) {
    // The renderer may have died just now; relaunch once and retry.
    if (state_ && state_->crashed) {
      stop();
      std::string e = ensureStarted();
      if (!e.empty()) return "restart failed: " + e;
      r = call("Page.navigate", p);
    }
    if (r.has("error")) return "navigate failed: " + r.get("error")->asString();
  }
  return waitReady(waitMs);
}

std::string BrowserSession::currentUrl() {
  mini::Value v = evaluate("location.href", false);
  return v.type == mini::Value::String ? v.s : "";
}

std::string BrowserSession::currentTitle() {
  mini::Value v = evaluate("document.title", false);
  return v.type == mini::Value::String ? v.s : "";
}

mini::Value BrowserSession::snapshotOutline(int maxChars, int maxLines) {
  mini::Value r = evaluate(kSnapshotJs);
  if (r.has("error")) return r;
  std::string joined;
  int count = 0;
  bool truncated = false;
  if (r.type == mini::Value::Array) {
    for (auto const& line : r.arr) {
      if (count >= maxLines) { truncated = true; break; }
      std::string s = line.type == mini::Value::String ? line.s : "";
      if (!joined.empty()) joined += "\n";
      joined += s;
      count++;
    }
  }
  bool tooLong = joined.size() > (size_t)maxChars;
  if (tooLong) {
    joined = joined.substr(0, (size_t)maxChars) + "\n...[snapshot truncated]";
    truncated = true;
  }
  mini::Value o = mini::Value::makeObject();
  o.set("lines", mini::Value::makeInt(count));
  o.set("snapshot", mini::Value::makeString(joined));
  o.set("truncated", mini::Value::makeBool(truncated));
  return o;
}

BrowserSession& browserSession() {
  static BrowserSession s;
  return s;
}

}  // namespace agent