#include "agent/tools.hpp"
#include "agent/background.hpp"
#include "agent/cache.hpp"
#include "agent/http.hpp"
#include "agent/patch.hpp"
#ifdef _WIN32
#include "agent/browser.hpp"
#endif
#include "agent/search.hpp"
#include "agent/rag.hpp"
#ifdef _WIN32
#include "agent/uia.hpp"
#endif
#include <cctype>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#endif

namespace agent {

namespace {

std::string truncateStr(std::string const& s, size_t max) {
  if (s.size() <= max) return s;
  return s.substr(0, max) + "\n...[truncated]";
}

// Terse JSON-schema builder helpers (string/number/bool + description).
mini::Value strProp(std::string const& desc) {
  mini::Value o = mini::Value::makeObject();
  o.set("type", mini::Value::makeString("string"));
  o.set("description", mini::Value::makeString(desc));
  return o;
}
mini::Value intProp(std::string const& desc) {
  mini::Value o = mini::Value::makeObject();
  o.set("type", mini::Value::makeString("integer"));
  o.set("description", mini::Value::makeString(desc));
  return o;
}
mini::Value boolProp(std::string const& desc) {
  mini::Value o = mini::Value::makeObject();
  o.set("type", mini::Value::makeString("boolean"));
  o.set("description", mini::Value::makeString(desc));
  return o;
}
mini::Value arrProp(std::string const& desc) {
  mini::Value o = mini::Value::makeObject();
  o.set("type", mini::Value::makeString("array"));
  o.set("description", mini::Value::makeString(desc));
  return o;
}
mini::Value objSchema(mini::Value props, std::vector<std::string> const& req) {
  mini::Value o = mini::Value::makeObject();
  o.set("type", mini::Value::makeString("object"));
  o.set("properties", std::move(props));
  mini::Value reqArr = mini::Value::makeArray();
  for (auto const& r : req) reqArr.arr.push_back(mini::Value::makeString(r));
  o.set("required", std::move(reqArr));
  return o;
}

bool readTextFile(std::string const& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool writeTextFile(std::string const& path, std::string const& content) {
  std::error_code ec;
  std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(content.data(), (std::streamsize)content.size());
  return f.good();
}

// Strip `strip` leading path components from a diff header path (`a/foo.c`).
std::string stripPathPrefix(std::string const& name, int strip) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : name) {
    if (c == '/' || c == '\\') {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  if (strip <= 0 || (size_t)strip >= parts.size()) return name;
  std::string out;
  for (size_t i = (size_t)strip; i < parts.size(); i++) {
    if (!out.empty()) out += "/";
    out += parts[i];
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

void ToolRegistry::add(Tool t) {
  if (t.name.empty()) return;
  tools_.push_back(std::move(t));
}

Tool const* ToolRegistry::find(std::string const& name) const {
  for (auto const& t : tools_)
    if (t.name == name) return &t;
  return nullptr;
}

void ToolRegistry::setEnabled(std::string const& name, bool on) {
  for (auto& t : tools_)
    if (t.name == name) t.enabled = on;
}

std::vector<std::string> ToolRegistry::names() const {
  std::vector<std::string> out;
  for (auto const& t : tools_) out.push_back(t.name);
  return out;
}

std::vector<std::string> ToolRegistry::enabledNames() const {
  std::vector<std::string> out;
  for (auto const& t : tools_)
    if (t.enabled) out.push_back(t.name);
  return out;
}

std::string ToolRegistry::toolsJson(std::vector<std::string> const& subset) const {
  mini::Value arr = mini::Value::makeArray();
  for (auto const& t : tools_) {
    if (!t.enabled) continue;
    if (!subset.empty()) {
      bool in = false;
      for (auto const& n : subset)
        if (n == t.name) { in = true; break; }
      if (!in) continue;
    }
    mini::Value jtool = mini::Value::makeObject();
    jtool.set("type", mini::Value::makeString("function"));
    mini::Value fn = mini::Value::makeObject();
    fn.set("name", mini::Value::makeString(t.name));
    fn.set("description", mini::Value::makeString(t.description));
    if (t.parameters.type == mini::Value::Object) fn.set("parameters", t.parameters);
    jtool.set("function", std::move(fn));
    arr.arr.push_back(std::move(jtool));
  }
  return mini::dump(arr);
}

std::string ToolRegistry::run(std::string const& name, std::string const& argsJson) const {
  Tool const* t = find(name);
  if (!t || !t->enabled) {
    mini::Value err = mini::Value::makeObject();
    err.set("error", mini::Value::makeString("unknown or disabled tool: " + name));
    return mini::dump(err);
  }
  if (guard_) {
    GuardResult g = guard_(name, argsJson);
    if (g.verdict == GuardVerdict::Deny) {
      mini::Value err = mini::Value::makeObject();
      err.set("error", mini::Value::makeString(
                           g.message.empty() ? "blocked by workspace guard" : g.message));
      return mini::dump(err);
    }
  }
  try {
    if (!t->run) throw std::runtime_error("tool has no implementation: " + name);
    // Tool-result cache (pure tools only): file_read/file_list fold the target
    // file/dir mtime into the key so an edited file misses naturally;
    // web_fetch gets a short 10-minute TTL.
    if (cache_ && t->pure) {
      std::string keyData = argsJson;
      if (name == "file_read" || name == "file_list") {
        mini::Value args;
        if (mini::tryParse(argsJson, args)) {
          std::string path = args.has("path") ? args.get("path")->asString() : "";
          if (path.empty()) path = ".";
          std::error_code ec;
          auto wt = std::filesystem::last_write_time(path, ec);
          if (!ec) keyData += "\x01mtime:" + std::to_string((long long)wt.time_since_epoch().count());
        }
      }
      std::string key = DiskCache::hashTool(name, keyData);
      std::string hit;
      if (cache_->get(key, hit)) return hit;
      std::string result = t->run(argsJson);
      // Only cache successful results (no "error" field in the JSON).
      mini::Value rv;
      bool ok = !mini::tryParse(result, rv) || !rv.has("error");
      if (ok) cache_->put(key, result, name == "web_fetch" ? 600 : 0);
      return result;
    }
    return t->run(argsJson);
  } catch (std::exception const& ex) {
    mini::Value err = mini::Value::makeObject();
    err.set("error", mini::Value::makeString(std::string("tool exception: ") + ex.what()));
    return mini::dump(err);
  }
}

// ---------------------------------------------------------------------------
// Git helpers: run git in a working directory with a strict argument
// whitelist. Command output is captured via popen.
// ---------------------------------------------------------------------------

namespace {

std::string gitErr(std::string const& m) {
  mini::Value e = mini::Value::makeObject();
  e.set("error", mini::Value::makeString(m));
  return mini::dump(e);
}

std::string gitJstr(mini::Value const& args, char const* key) {
  return args.has(key) ? args.get(key)->asString() : "";
}

bool safeGitArg(std::string const& s) {
  if (s.empty()) return true;
  for (char c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.' || c == '/' || c == '\\' || c == ':' || c == '~';
    if (!ok) return false;
  }
  return true;
}

std::string runGitCmd(std::string const& workDir, std::vector<std::string> const& args,
                      size_t cap) {
  // Use `git -C <dir>` instead of `cd /d <dir> && git ...`: cmd.exe's `cd /d`
  // cannot reliably switch to a forward-slash relative path (e.g. the test
  // fixture "out/tmp_git_repo"), which made git run in the process CWD and
  // leak a git repository there.
  std::string full = "git -C \"" + workDir + "\"";
  for (auto const& a : args) {
    full += " " + a;
  }
  full += " 2>&1";
#ifdef _WIN32
  FILE* p = _popen(full.c_str(), "r");
#else
  FILE* p = popen(full.c_str(), "r");
#endif
  if (!p) return "{\"error\":\"failed to start git\"}";
  std::string out;
  char buf[4096];
  bool truncated = false;
  while (fgets(buf, sizeof buf, p)) {
    out += buf;
    if (out.size() > cap) {
      truncated = true;
      break;
    }
  }
  int rc;
#ifdef _WIN32
  rc = _pclose(p);
#else
  int raw = pclose(p);  // wait status，不是退出码
  rc = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("rc", mini::Value::makeInt(rc));
  o.set("output", mini::Value::makeString(out));
  if (truncated) o.set("truncated", mini::Value::makeBool(true));
  return mini::dump(o);
}

// Writes a commit message to a temp file so it never has to be shell-escaped.
std::string writeTempFile(std::string const& content) {
#ifdef _WIN32
  char tmp[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, tmp);
  std::string path = n ? std::string(tmp) : ".";
  path += "git_msg_" + std::to_string(GetCurrentProcessId()) + ".txt";
#else
  std::string path = "/tmp/git_msg_" + std::to_string((long)getpid()) + ".txt";
#endif
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << content;
  return f.good() ? path : "";
}

}  // namespace

std::string toolGitStatus(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gitErr("arguments must be a JSON object");
  std::string dir = gitJstr(args, "dir");
  if (dir.empty()) dir = ".";
  if (!safeGitArg(dir)) return gitErr("invalid 'dir'");
  return runGitCmd(dir, {"status", "--short", "--branch"}, 4000);
}

std::string toolGitDiff(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gitErr("arguments must be a JSON object");
  std::string dir = gitJstr(args, "dir");
  if (dir.empty()) dir = ".";
  if (!safeGitArg(dir)) return gitErr("invalid 'dir'");
  std::vector<std::string> a = {"diff"};
  if (args.has("staged") && args.get("staged")->asBool(false)) a.push_back("--cached");
  std::string file = gitJstr(args, "file");
  if (!file.empty()) {
    if (!safeGitArg(file)) return gitErr("invalid 'file'");
    a.push_back("--");
    a.push_back(file);
  }
  return runGitCmd(dir, std::move(a), 8000);
}

std::string toolGitLog(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gitErr("arguments must be a JSON object");
  std::string dir = gitJstr(args, "dir");
  if (dir.empty()) dir = ".";
  if (!safeGitArg(dir)) return gitErr("invalid 'dir'");
  int n = args.has("n") ? (int)args.get("n")->asInt(20) : 20;
  if (n < 1) n = 1;
  if (n > 100) n = 100;
  std::vector<std::string> a = {"log", "--oneline", "-n", std::to_string(n)};
  return runGitCmd(dir, std::move(a), 4000);
}

std::string toolGitCommit(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gitErr("arguments must be a JSON object");
  std::string dir = gitJstr(args, "dir");
  if (dir.empty()) dir = ".";
  if (!safeGitArg(dir)) return gitErr("invalid 'dir'");
  std::string msg = gitJstr(args, "message");
  if (msg.empty()) return gitErr("message is required");
  bool addAll = !(args.has("add_all") && !args.get("add_all")->asBool(true));
  if (addAll) {
    std::string ar = runGitCmd(dir, {"add", "-A"}, 2000);
    (void)ar;
  }
  std::string msgPath = writeTempFile(msg);
  if (msgPath.empty()) return gitErr("cannot write commit message file");
  std::string r = runGitCmd(dir, {"commit", "-F", msgPath}, 4000);
#ifdef _WIN32
  DeleteFileA(msgPath.c_str());
#else
  remove(msgPath.c_str());
#endif
  return r;
}

struct CapturedCmd {
  std::string out;
  int exitCode = -1;
  bool timedOut = false;
};

#ifdef _WIN32
CapturedCmd runCommandCaptured(std::string const& fullCmd, size_t cap,
                               DWORD timeoutMs) {
  CapturedCmd res;
  HANDLE rPipe = NULL, wPipe = NULL;
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;
  if (!CreatePipe(&rPipe, &wPipe, &sa, 0)) return res;
  if (!SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(rPipe);
    CloseHandle(wPipe);
    return res;
  }
  HANDLE nul =
      CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                  OPEN_EXISTING, 0, NULL);
  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = wPipe;
  si.hStdError = wPipe;
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  std::string mutableCmd = fullCmd;  // CreateProcessA needs a writable buffer
  BOOL ok = CreateProcessA(NULL, &mutableCmd[0], NULL, NULL, TRUE, 0, NULL,
                           NULL, &si, &pi);
  CloseHandle(wPipe);
  if (nul) CloseHandle(nul);
  if (!ok) {
    CloseHandle(rPipe);
    return res;
  }
  CloseHandle(pi.hThread);

  bool truncated = cap == 0;
  ULONGLONG start = GetTickCount64();
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(rPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
      char tmp[4096];
      // After the cap is reached we keep draining the pipe and drop what we
      // read: stopping early leaves the child blocked on a full pipe buffer,
      // and we would report a timeout for a command that was only waiting
      // for us to catch up.
      DWORD room = sizeof(tmp);
      if (!truncated) {
        size_t left = cap > res.out.size() ? cap - res.out.size() : 0;
        if (left < room) room = (DWORD)left;
      }
      DWORD want = avail < room ? avail : room;
      DWORD got = 0;
      if (ReadFile(rPipe, tmp, want, &got, NULL) && got > 0 && !truncated) {
        res.out.append(tmp, got);
        if (res.out.size() >= cap) truncated = true;
      }
    }
    DWORD w = WaitForSingleObject(pi.hProcess, 30);
    if (w == WAIT_OBJECT_0) {
      for (;;) {  // drain anything left after exit
        DWORD a = 0;
        if (!PeekNamedPipe(rPipe, NULL, 0, NULL, &a, NULL) || a == 0 ||
            truncated)
          break;
        char tmp[4096];
        DWORD want = (DWORD)(cap - res.out.size());
        if (want > sizeof(tmp)) want = sizeof(tmp);
        if (want > a) want = a;
        DWORD got = 0;
        if (!ReadFile(rPipe, tmp, want, &got, NULL) || got == 0) break;
        res.out.append(tmp, got);
        if (res.out.size() >= cap) {
          truncated = true;
          break;
        }
      }
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      res.exitCode = (int)code;
      break;
    }
    if (timeoutMs && GetTickCount64() - start > timeoutMs) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 500);
      res.timedOut = true;
      res.exitCode = -1;
      break;
    }
  }
  CloseHandle(rPipe);
  CloseHandle(pi.hProcess);
  if (truncated) res.out += "\n...[truncated]";
  res.out += res.timedOut ? "\n...[timed out]" : "";
  return res;
}
#else
CapturedCmd runCommandCaptured(std::string const& fullCmd, size_t cap,
                               long timeoutMs) {
  CapturedCmd res;
  int fds[2];
  if (pipe(fds) != 0) return res;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return res;
  }
  if (pid == 0) {
    // Child: stick to async-signal-safe calls between fork and exec.
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(STDOUT_FILENO, STDERR_FILENO);
    close(fds[1]);
    execl("/bin/sh", "sh", "-c", fullCmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  close(fds[1]);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  std::string out;
  char buf[4096];
  bool truncated = false;
  for (;;) {
    auto leftMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - std::chrono::steady_clock::now()).count();
    if (leftMs <= 0) {
      res.timedOut = true;
      break;
    }
    struct pollfd pf;
    pf.fd = fds[0];
    pf.events = POLLIN;
    pf.revents = 0;
    // Poll in <=1s slices so a stalled child is still caught by the deadline
    // even if the machine's clock moves.
    int pr = poll(&pf, 1, (int)std::min<long long>(leftMs, 1000));
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;  // EOF: the command finished
    if (!truncated) {
      out.append(buf, (size_t)n);
      if (out.size() > cap) {
        out.resize(cap);
        truncated = true;
      }
    }
    // Once truncated we keep reading and drop the bytes: stopping early would
    // leave the child blocked on a full pipe, and the waitpid below would hang.
  }
  close(fds[0]);

  if (res.timedOut) ::kill(pid, SIGKILL);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  // wait() 状态的高字节才是退出码，不拆的话 `exit 3` 会读成 768。
  res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (truncated) res.out = out + "\n...[truncated]";
  else res.out = out + (res.timedOut ? "\n...[timed out]" : "");
  return res;
}
#endif

std::string toolShellExec(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) {
    return "{\"error\":\"arguments must be a JSON object\"}";
  }
  std::string cmd = args.has("command") ? args.get("command")->asString() : "";
  if (cmd.empty()) return "{\"error\":\"missing 'command' string\"}";
  size_t cap = args.has("max_chars") ? (size_t)args.get("max_chars")->asInt(4000) : 4000;
  long timeoutMs = args.has("timeout_ms") ? args.get("timeout_ms")->asInt(120000) : 120000;
  if (timeoutMs <= 0) timeoutMs = 120000;

#ifdef _WIN32
  std::string full = "cmd /C " + cmd + " 2>&1";
#else
  std::string full = cmd + " 2>&1";
#endif
#ifdef _WIN32
  CapturedCmd c = runCommandCaptured(full, cap, (DWORD)timeoutMs);
#else
  CapturedCmd c = runCommandCaptured(full, cap, timeoutMs);
#endif
  mini::Value res = mini::Value::makeObject();
  res.set("command", mini::Value::makeString(cmd));
  res.set("output", mini::Value::makeString(c.out));
  res.set("exit_code", mini::Value::makeInt(c.exitCode));
  res.set("timed_out", mini::Value::makeBool(c.timedOut));
  res.set("truncated", mini::Value::makeBool(c.out.size() >= cap));
  return mini::dump(res);
}

// Change -> verify closed loop: run the project's build and test commands and
// return their output so the model can react to failures. Prefer explicit
// `command`; default tries build.ps1 then out/test_*.exe (Windows) or
// `make` then `ctest` (Unix) when the files exist.
std::string toolVerify(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) {
    return "{\"error\":\"arguments must be a JSON object\"}";
  }
  std::string cmd =
      args.has("command") ? args.get("command")->asString() : "";
  size_t cap = args.has("max_chars") ? (size_t)args.get("max_chars")->asInt(16000) : 16000;
  long timeoutMs = args.has("timeout_ms") ? args.get("timeout_ms")->asInt(600000) : 600000;

  std::vector<std::string> cmds;
  if (!cmd.empty()) {
    cmds.push_back(cmd);
  } else {
#ifdef _WIN32
    if (std::ifstream("build.ps1")) cmds.push_back("powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1");
#else
    if (std::ifstream("Makefile")) cmds.push_back("make");
#endif
    // tests: run every test_*.exe in out/ on Windows; ctest on Unix.
    {
      std::vector<std::string> tests;
      std::error_code ec;
      for (auto const& e : std::filesystem::directory_iterator("out", ec)) {
        std::string n = e.path().filename().string();
#ifdef _WIN32
        if (n.rfind("test_", 0) == 0 && n.size() > 4 &&
            n.compare(n.size() - 4, 4, ".exe") == 0)
          tests.push_back(e.path().string());
#else
        if (n.rfind("test_", 0) == 0) tests.push_back(e.path().string());
#endif
      }
      std::sort(tests.begin(), tests.end());
      for (auto const& t : tests)
        cmds.push_back("\"" + t + "\" 2>&1");
    }
  }

  mini::Value results = mini::Value::makeArray();
  bool anyFailed = false;
  for (auto const& c : cmds) {
#ifdef _WIN32
    std::string full = "cmd /C " + c + " 2>&1";
#else
    std::string full = c + " 2>&1";
#endif
    CapturedCmd out = runCommandCaptured(full, cap, timeoutMs);
    if (out.exitCode != 0) anyFailed = true;
    mini::Value one = mini::Value::makeObject();
    one.set("command", mini::Value::makeString(c));
    one.set("exit_code", mini::Value::makeInt(out.exitCode));
    one.set("timed_out", mini::Value::makeBool(out.timedOut));
    one.set("output", mini::Value::makeString(out.out));
    results.arr.push_back(std::move(one));
  }
  mini::Value res = mini::Value::makeObject();
  res.set("ok", mini::Value::makeBool(!anyFailed));
  res.set("steps", std::move(results));
  return mini::dump(res);
}

// Lessons memory: a small append-only markdown file (out/lessons.md) that
// survives sessions. remember() appends a lesson; recall() returns the file.
namespace {
std::string lessonsPath() { return "out/lessons.md"; }

bool appendLessonsFile(std::string const& text) {
  std::error_code ec;
  std::filesystem::create_directories("out", ec);
  std::ofstream f(lessonsPath(), std::ios::app | std::ios::binary);
  if (!f) return false;
  if (f.tellp() > 0 && text.front() != '\n') f << "\n";
  f << text << "\n";
  return f.good();
}

std::string readLessonsFile() {
  std::ifstream in(lessonsPath(), std::ios::binary);
  if (!in) return "";
  std::stringstream ss;
  ss << in.rdbuf();
  std::string t = ss.str();
  if (t.size() > 12000) {
    t = t.substr(t.size() - 12000);
    t = "...(older lessons omitted)\n" + t;
  }
  return t;
}
}  // namespace

std::string toolRemember(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) {
    return "{\"error\":\"arguments must be a JSON object\"}";
  }
  std::string lesson = args.has("lesson") ? args.get("lesson")->asString() : "";
  if (lesson.empty()) return "{\"error\":\"missing 'lesson' string\"}";
  if (lesson.size() > 2000) lesson.resize(2000);
  std::string ts;
  {
    std::time_t now = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", std::localtime(&now));
    ts = b;
  }
  std::string line = "- [" + ts + "] " + lesson;
  if (!appendLessonsFile(line)) return "{\"error\":\"cannot write lessons file\"}";
  mini::Value res = mini::Value::makeObject();
  res.set("ok", mini::Value::makeBool(true));
  res.set("note", mini::Value::makeString(
                      "Lesson saved to " + lessonsPath() +
                      ". It will be injected into future sessions as project "
                      "memory, so state durable facts and failure patterns."));
  return mini::dump(res);
}

std::string toolRecall(std::string const& argsJson) {
  (void)argsJson;
  std::string t = readLessonsFile();
  mini::Value res = mini::Value::makeObject();
  res.set("found", mini::Value::makeBool(!t.empty()));
  res.set("file", mini::Value::makeString(lessonsPath()));
  res.set("lessons", mini::Value::makeString(t.empty() ? "(no lessons yet)" : t));
  return mini::dump(res);
}

std::string toolFileRead(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return "{\"error\":\"arguments must be a JSON object\"}";
  std::string path = args.has("path") ? args.get("path")->asString() : "";
  if (path.empty()) return "{\"error\":\"missing 'path' string\"}";
  int64_t offset = args.has("offset") ? args.get("offset")->asInt(1) : 1;
  if (offset < 1) offset = 1;
  int64_t limit = args.has("limit") ? args.get("limit")->asInt(0) : 0;
  size_t cap = args.has("max_chars") ? (size_t)args.get("max_chars")->asInt(16000) : 16000;

  std::string content;
  if (!readTextFile(path, content)) {
    return "{\"error\":\"cannot open file: " + truncateStr(path, 200) + "\"}";
  }
  std::vector<std::string> lines = splitFileLines(content);
  bool trailingNl = !content.empty() && content.back() == '\n';
  int64_t total = (int64_t)lines.size();

  int64_t start = offset - 1;
  int64_t end = (limit > 0) ? start + limit : total;
  if (start > total) start = total;
  if (end > total) end = total;

  bool truncated = false;
  std::string win;
  for (int64_t i = start; i < end; i++) {
    win += lines[(size_t)i];
    if (i + 1 < end || (trailingNl && end == total)) win += "\n";
  }
  size_t rawLen = win.size();
  if (win.size() > cap) {
    win.resize(cap);
    win += "\n...[truncated]";
    truncated = true;
  }

  mini::Value res = mini::Value::makeObject();
  res.set("path", mini::Value::makeString(path));
  res.set("content", mini::Value::makeString(win));
  res.set("start_line", mini::Value::makeInt(start + 1));
  res.set("end_line", mini::Value::makeInt(std::max((int64_t)start, end)));
  res.set("total_lines", mini::Value::makeInt(total));
  res.set("bytes", mini::Value::makeInt((int64_t)rawLen));
  res.set("truncated", mini::Value::makeBool(truncated));
  return mini::dump(res);
}

std::string toolFileWrite(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return "{\"error\":\"arguments must be a JSON object\"}";
  std::string path = args.has("path") ? args.get("path")->asString() : "";
  std::string content = args.has("content") ? args.get("content")->asString() : "";
  if (path.empty()) return "{\"error\":\"missing 'path' string\"}";

  if (!writeTextFile(path, content)) {
    return "{\"error\":\"cannot write file: " + truncateStr(path, 200) + "\"}";
  }
  mini::Value res = mini::Value::makeObject();
  res.set("ok", mini::Value::makeBool(true));
  res.set("bytes", mini::Value::makeInt((int64_t)content.size()));
  return mini::dump(res);
}

std::string toolEdit(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return "{\"error\":\"arguments must be a JSON object\"}";
  std::string path = args.has("path") ? args.get("path")->asString() : "";
  std::string oldStr = args.has("old_string") ? args.get("old_string")->asString() : "";
  std::string newStr = args.has("new_string") ? args.get("new_string")->asString() : "";
  bool replaceAll = args.has("replace_all") ? args.get("replace_all")->asBool(false) : false;
  if (path.empty()) return "{\"error\":\"missing 'path' string\"}";
  if (oldStr.empty()) return "{\"error\":\"missing 'old_string' string\"}";

  std::string content;
  if (!readTextFile(path, content)) {
    return "{\"error\":\"cannot open file: " + truncateStr(path, 200) + "\"}";
  }
  if (content.find(oldStr) == std::string::npos) {
    return "{\"error\":\"old_string not found in file (" + std::to_string(content.size()) +
           " bytes); re-read the file for the exact current content\"}";
  }
  int64_t reps = 0;
  if (replaceAll) {
    std::string out;
    out.reserve(content.size());
    size_t pos = 0;
    for (;;) {
      size_t at = content.find(oldStr, pos);
      if (at == std::string::npos) { out.append(content, pos, std::string::npos); break; }
      out.append(content, pos, at - pos);
      out += newStr;
      pos = at + oldStr.size();
      reps++;
    }
    content = std::move(out);
  } else {
    content.replace(content.find(oldStr), oldStr.size(), newStr);
    reps = 1;
  }
  if (!writeTextFile(path, content)) {
    return "{\"error\":\"cannot write file: " + truncateStr(path, 200) + "\"}";
  }
  mini::Value res = mini::Value::makeObject();
  res.set("ok", mini::Value::makeBool(true));
  res.set("replacements", mini::Value::makeInt(reps));
  res.set("bytes", mini::Value::makeInt((int64_t)content.size()));
  return mini::dump(res);
}

std::string toolPatch(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return "{\"error\":\"arguments must be a JSON object\"}";
  std::string patchText = args.has("patch") ? args.get("patch")->asString() : "";
  int strip = args.has("strip") ? (int)args.get("strip")->asInt(1) : 1;
  if (patchText.empty()) return "{\"error\":\"missing 'patch' string\"}";

  UnifiedDiff d;
  std::string perr;
  if (!parseUnifiedDiff(patchText, d, perr)) {
    return "{\"error\":\"bad patch: " + truncateStr(perr, 400) + "\"}";
  }

  // Candidate target paths: the normalized header path first, then fall back
  // toward 0 for diffs that didn't carry an a/ b/ prefix and need a -pN strip.
  std::string headerName = d.isCreate ? d.newFile : d.oldFile;
  std::vector<std::string> candidates;
  candidates.push_back(headerName);
  for (int s = strip; s >= 1; s--) {
    std::string t = stripPathPrefix(headerName, s);
    if (t != headerName) candidates.push_back(t);
  }
  if (candidates.empty() || candidates.front().empty()) {
    return "{\"error\":\"cannot determine target file from patch\"}";
  }

  std::string path = candidates[0];
  if (!d.isCreate) {
    bool found = false;
    std::string probe;
    for (auto const& c : candidates) {
      if (readTextFile(c, probe)) { path = c; found = true; break; }
    }
    if (!found) {
      return "{\"error\":\"target file not found: " + truncateStr(path, 200) +
             "\" (tried: " + truncateStr(candidates[0], 200) + ")";
    }
  }

  std::vector<std::string> oldLines;
  std::string oldContent;
  std::string oldTail;  // trailing newline preserved on rewrite
  if (!d.isCreate) {
    if (!readTextFile(path, oldContent)) {
      return "{\"error\":\"cannot open file: " + truncateStr(path, 200) + "\"}";
    }
    oldLines = splitFileLines(oldContent);
    if (!oldContent.empty() && oldContent.back() == '\n') oldTail = "\n";
  }

  std::vector<std::string> outLines;
  std::string aerr;
  if (!applyDiffLines(oldLines, d, outLines, aerr)) {
    mini::Value err = mini::Value::makeObject();
    err.set("error", mini::Value::makeString("patch apply failed\n" + truncateStr(aerr, 1500)));
    return mini::dump(err);
  }

  if (d.isDelete) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    mini::Value res = mini::Value::makeObject();
    res.set("ok", mini::Value::makeBool(true));
    res.set("deleted", mini::Value::makeBool(true));
    return mini::dump(res);
  }

  std::string joined;
  for (size_t i = 0; i < outLines.size(); i++) {
    if (i > 0) joined += "\n";
    joined += outLines[i];
  }
  if (!outLines.empty() && !d.isCreate) joined += oldTail;
  if (d.isCreate && !joined.empty()) joined += "\n";

  if (!writeTextFile(path, joined)) {
    return "{\"error\":\"cannot write file: " + truncateStr(path, 200) + "\"}";
  }
  mini::Value res = mini::Value::makeObject();
  res.set("ok", mini::Value::makeBool(true));
  res.set("created", mini::Value::makeBool(d.isCreate));
  res.set("bytes", mini::Value::makeInt((int64_t)joined.size()));
  return mini::dump(res);
}

std::string toolFileList(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return "{\"error\":\"arguments must be a JSON object\"}";
  std::string path = args.has("path") ? args.get("path")->asString() : ".";
  std::string pattern = args.has("pattern") ? args.get("pattern")->asString() : "";
  size_t cap = args.has("max_entries") ? (size_t)args.get("max_entries")->asInt(500) : 500;

  mini::Value res = mini::Value::makeObject();
  res.set("path", mini::Value::makeString(path));
  mini::Value entries = mini::Value::makeArray();
  std::error_code ec;
  size_t n = 0;
  try {
    for (auto const& de : std::filesystem::directory_iterator(path, ec)) {
      std::string name = de.path().filename().string();
      if (!pattern.empty() && name.find(pattern) == std::string::npos) continue;
      mini::Value je = mini::Value::makeObject();
      je.set("name", mini::Value::makeString(name));
      je.set("dir", mini::Value::makeBool(de.is_directory(ec)));
      entries.arr.push_back(std::move(je));
      if (++n >= cap) break;
    }
  } catch (std::exception const&) {
    return "{\"error\":\"cannot list path: " + truncateStr(path, 200) + "\"}";
  }
  res.set("entries", std::move(entries));
  res.set("count", mini::Value::makeInt((int64_t)n));
  return mini::dump(res);
}

// Turn an HTML body into readable text: drop <script>/<style> blocks, strip
// tags, decode common entities, collapse whitespace.
std::string htmlToText(std::string const& in) {
  std::string out;
  out.reserve(in.size());
  bool tag = false;
  for (char ch : in) {
    if (ch == '<') { tag = true; continue; }
    if (ch == '>') { tag = false; continue; }
    if (!tag) out += ch;
  }
  for (auto const& rep : {"&amp;", "&lt;", "&gt;", "&quot;", "&#39;", "&nbsp;", "&#x27;",
                         "&ensp;", "&#0183;"}) {
    std::string from = rep;
    std::string to;
    if (from == "&amp;") to = "&";
    else if (from == "&lt;") to = "<";
    else if (from == "&gt;") to = ">";
    else if (from == "&quot;") to = "\"";
    else if (from == "&#39;" || from == "&#x27;") to = "'";
    else if (from == "&nbsp;" || from == "&ensp;") to = " ";
    else to = "·";  // &#0183; middle-dot entity
    size_t pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
      out.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
  std::string cleaned;
  cleaned.reserve(out.size());
  bool wsPrev = false;
  for (char ch : out) {
    bool ws = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
    if (ws) {
      if (!wsPrev) cleaned += ' ';
      wsPrev = true;
    } else {
      cleaned += ch;
      wsPrev = false;
    }
  }
  while (!cleaned.empty() && cleaned.front() == ' ') cleaned.erase(cleaned.begin());
  while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
  return cleaned;
}

// Turn an HTML body into readable text: drop <script>/<style> blocks, strip
// tags, decode common entities, collapse whitespace.
std::string htmlBodyToText(std::string const& in) {
  std::string pre;
  pre.reserve(in.size());
  size_t pos = 0;
  while (pos < in.size()) {
    size_t lt = in.find('<', pos);
    if (lt == std::string::npos) { pre += in.substr(pos); break; }
    pre += in.substr(pos, lt - pos);
    size_t gt = in.find('>', lt);
    if (gt == std::string::npos) break;
    std::string tag = in.substr(lt + 1, gt - lt - 1);
    bool script = tag.rfind("script", 0) == 0 || tag.rfind("style", 0) == 0 ||
                  tag.rfind("/script", 0) == 0 || tag.rfind("/style", 0) == 0;
    if (!script) pre += ' ';
    pos = gt + 1;
    if (script) {
      // skip until the matching closing tag
      std::string closer = tag.front() == '/' ? "" : (tag.rfind("script", 0) == 0 ? "</script>" : "</style>");
      if (!closer.empty()) {
        size_t end = in.find(closer, pos);
        if (end != std::string::npos) pos = end + closer.size();
      }
    }
  }
  return htmlToText(pre);
}

std::string toolWebFetch(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return "{\"error\":\"arguments must be a JSON object\"}";
  std::string url = args.has("url") ? args.get("url")->asString() : "";
  if (url.empty()) return "{\"error\":\"missing 'url' string\"}";
  size_t cap = args.has("max_chars") ? (size_t)args.get("max_chars")->asInt(20000) : 20000;

  HttpRequest req;
  req.url = std::move(url);
  req.method = "GET";
  req.timeoutSec = 30;
  std::string body;
  int status = httpRequest(req, [&](char const* p, size_t n) { body.append(p, n); });
  if (status < 0) return "{\"error\":\"network error fetching url\"}";

  std::string content = std::move(body);
  bool raw = args.has("raw") ? args.get("raw")->asBool(false) : false;
  if (!raw) content = htmlBodyToText(content);
  bool truncated = false;
  if (content.size() > cap) {
    content.resize(cap);
    truncated = true;
  }
  mini::Value res = mini::Value::makeObject();
  res.set("url", mini::Value::makeString(req.url));
  res.set("status", mini::Value::makeInt(status));
  res.set("content", mini::Value::makeString(content));
  res.set("truncated", mini::Value::makeBool(truncated));
  return mini::dump(res);
}


// ---------------------------------------------------------------------------
// Web search: rank-and-extract results from reachable engines. Tries Bing
// first, then Baidu; the engines that are actually reachable vary by network
// (WinHTTP direct vs proxied), so run all and keep the first that answers.
// ---------------------------------------------------------------------------

std::string urlEncode(std::string const& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// Extract (href, text) pairs of anchors under the first <h2>/<h3> heading of a
// result block; returns false when the block has no usable link.
bool anchorFromHeading(std::string const& block, std::string& href, std::string& text) {
  size_t h = block.find("<h2"), h3 = block.find("<h3");
  if (h == std::string::npos) h = h3;
  if (h == std::string::npos) return false;
  size_t a = block.find("<a", h);
  if (a == std::string::npos) return false;
  size_t hrefBeg = block.find("href=\"", a);
  if (hrefBeg == std::string::npos) return false;
  hrefBeg += 6;
  size_t hrefEnd = block.find('"', hrefBeg);
  if (hrefEnd == std::string::npos) return false;
  href = block.substr(hrefBeg, hrefEnd - hrefBeg);
  size_t gt = block.find('>', a);
  if (gt == std::string::npos) return false;
  size_t close = block.find("</a", gt);
  if (close == std::string::npos) return false;
  text = htmlToText(block.substr(gt + 1, close - gt - 1));
  return !text.empty();
}

// Longest <p>..</p> text within `region` (Bing/baidu put the actual snippet in
// one of several paragraphs; the first is often a "N 天之前" timestamp line).
std::string snippetAfterAnchor(std::string const& region) {
  std::string best;
  size_t pos = 0;
  while (true) {
    size_t p = region.find("<p", pos);
    if (p == std::string::npos) break;
    // guard: must be a real <p tag (not <pre>/<param>)
    size_t after = p + 2;
    if (after >= region.size()) break;
    if (region[after] != '>' && region[after] != ' ' && region[after] != '\n' &&
        region[after] != '\t' && region[after] != '\r') {
      pos = after;
      continue;
    }
    size_t gt = region.find('>', p);
    if (gt == std::string::npos) break;
    size_t close = region.find("</p", gt);
    if (close == std::string::npos) close = region.size();
    std::string t = htmlToText(region.substr(gt + 1, close - gt - 1));
    if (t.size() > best.size()) best = std::move(t);
    pos = close < region.size() ? close + 3 : region.size();
  }
  if (best.size() > 300) best.resize(300);
  // Strip leading engine timestamp prefixes ("14小时前· ", "2 天之前 · ",
  // "3 days ago · " ...) — anything short starting with digits/space/CJK
  // followed by "·" at the beginning counts as an age stamp.
  {
    size_t dot = best.find("\xC2\xB7");  // UTF-8 "·"
    if (dot != std::string::npos && dot > 0 && dot <= 20) {
      bool stamp = false;
      bool hasDigit = false;
      for (size_t k = 0; k < dot; k++) {
        unsigned char c = (unsigned char)best[k];
        if (c >= '0' && c <= '9') hasDigit = true;
        if (!((c >= '0' && c <= '9') || c == ' ' || c >= 0x80)) { stamp = false; break; }
        stamp = true;
      }
      if (stamp && hasDigit) {
        size_t end = dot + 2;  // skip '·'
        while (end < best.size() && best[end] == ' ') end++;
        best.erase(0, end);
      }
    }
  }
  return best;
}

struct SearchHitS { std::string title, url, snippet; };

bool extractBing(std::string const& html, std::vector<SearchHitS>& out) {
  size_t pos = 0;
  bool any = false;
  while (true) {
    size_t li = html.find("b_algo", pos);
    if (li == std::string::npos) break;
    size_t open = html.rfind('<', li);
    if (open != std::string::npos) li = open;
    size_t close = html.find("</li>", li);
    if (close == std::string::npos) break;
    std::string block = html.substr(li, close - li);
    pos = close + 5;
    // Ad filter: sponsored blocks carry an "Ad" label / b_ad / adSlot marker.
    {
      std::string b = block;
      for (char& c : b) c = (char)std::tolower((unsigned char)c);
      if (b.find("b_ad") != std::string::npos || b.find("adlabel") != std::string::npos ||
          b.find("sponsored") != std::string::npos || b.find(">ad<") != std::string::npos ||
          b.find("adsbygoogle") != std::string::npos) {
        continue;
      }
    }
    std::string href, text;
    if (!anchorFromHeading(block, href, text)) continue;
    SearchHitS h;
    h.url = href;
    h.title = text;
    h.snippet = snippetAfterAnchor(block);
    out.push_back(std::move(h));
    any = true;
  }
  return any;
}

bool extractBaidu(std::string const& html, std::vector<SearchHitS>& out) {
  size_t pos = 0;
  bool any = false;
  while (true) {
    size_t h3 = html.find("<h3", pos);
    if (h3 == std::string::npos) break;
    size_t close = html.find("</h3>", h3);
    if (close == std::string::npos) break;
    pos = close + 5;
    // Ad filter: Baidu sponsored blocks live in ec_ containers with ad
    // markers; check the container window before the heading.
    {
      size_t look = h3 >= 2500 ? h3 - 2500 : 0;
      std::string win = html.substr(look, h3 - look);
      std::string w = win;
      for (char& c : w) c = (char)std::tolower((unsigned char)c);
      if (w.find("ec_") != std::string::npos || w.find("data-adid") != std::string::npos ||
          w.find("adinfo") != std::string::npos || w.find("广告") != std::string::npos ||
          w.find("advert") != std::string::npos) {
        continue;
      }
    }
    std::string block = html.substr(h3, close - h3);
    std::string href, text;
    if (!anchorFromHeading(block, href, text)) continue;
    SearchHitS h;
    h.url = href;
    h.title = text;
    h.snippet = snippetAfterAnchor(html.substr(close + 5, 1800));
    out.push_back(std::move(h));
    any = true;
  }
  return any;
}

std::string toolWebSearch(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return "{\"error\":\"arguments must be a JSON object\"}";
  std::string q = args.has("query") ? args.get("query")->asString() : "";
  if (q.empty()) return "{\"error\":\"missing 'query' string\"}";
  int maxResults = args.has("max_results") ? (int)args.get("max_results")->asInt(8) : 8;
  if (maxResults < 1) maxResults = 1;
  if (maxResults > 20) maxResults = 20;

  auto fetchEngine = [&](const char* name, const char* url,
                         std::vector<SearchHitS>& hits) -> bool {
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.timeoutSec = 20;
    req.headers.push_back({"User-Agent",
                           "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                           "Chrome/126.0.0.0 Safari/537.36"});
    req.headers.push_back({"Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8"});
    std::string body;
    int status = httpRequest(req, [&](char const* p, size_t n) { body.append(p, n); });
    if (status < 0) return false;
    std::string n = name;
    if (n == "bing") return extractBing(body, hits);
    if (n == "baidu") return extractBaidu(body, hits);
    return false;
  };

  auto lower = [](std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  // Query tokens: ASCII words (stopwords removed). A contiguous CJK run is
  // split into overlapping 2-character grams: a page containing 交叉编译 also
  // contains 交叉 and 编译, so coverage becomes measurable. Keeping the whole
  // run made almost every Chinese query report 0% coverage and get flagged
  // "low" even when the results were exactly right.
  std::vector<std::string> tokens;
  {
    std::string cur;
    bool curCjk = false;
    auto flush = [&]() {
      if (cur.empty()) return;
      if (curCjk) {
        size_t n = cur.size();
        if (n < 6) tokens.push_back(cur);  // a single CJK char
        for (size_t k = 0; k + 6 <= n; k += 3) tokens.push_back(cur.substr(k, 6));
      } else {
        tokens.push_back(lower(cur));
      }
      cur.clear();
    };
    for (size_t i = 0; i < q.size();) {
      unsigned char c = (unsigned char)q[i];
      bool asciiWord = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (c >= 0x80) {  // CJK run
        if (!cur.empty() && !curCjk) flush();  // ends an ASCII word
        curCjk = true;
        cur += q[i];
        i++;
        continue;
      }
      if (asciiWord) {
        if (!cur.empty() && curCjk) flush();  // ends a CJK run
        curCjk = false;
        cur += (char)c;
        i++;
        continue;
      }
      flush();
      i++;
    }
    flush();
    static const char* kStop[] = {"the", "a",  "an",  "of",    "to",    "in",
                                  "on",  "for", "and", "or",    "with",  "from",
                                  "by",  "at",  "is",  "it",    "as",    "how",
                                  "what", "when", "who", "https", "http", "www",
                                  "com", "cn",  "org", "about", "your",  "you",
                                  "use", "using"};
    std::vector<std::string> keep;
    keep.reserve(tokens.size());
    for (auto& t : tokens) {
      if (t.size() < 2 || t.size() > 24) continue;
      bool stop = false;
      for (auto* s : kStop)
        if (t == s) { stop = true; break; }
      if (!stop) keep.push_back(t);
    }
    tokens.swap(keep);
  }

  struct ScoredHit {
    SearchHitS h;
    double score;
    int matchedTokens = 0;
    std::string engine;
  };
  auto coverageOf = [&](std::vector<ScoredHit> const& v) {
    if (tokens.empty()) return 1.0;
    double best = 0;
    for (auto const& s : v)
      best = std::max(best, (double)s.matchedTokens / (double)tokens.size());
    return best;
  };
  auto absorb = [&](char const* engineName, std::vector<SearchHitS>& hits,
                    std::vector<ScoredHit>& out) {
    std::string en = engineName;
    for (auto& h : hits) {
      if (h.snippet.empty() && h.title.empty()) continue;
      ScoredHit s;
      s.h = h;
      s.engine = en;
      double score = 0;
      int matched = 0;
      std::string title = lower(h.title), url = lower(h.url), snip = lower(h.snippet);
      if (!tokens.empty()) {
        for (auto const& t : tokens) {
          bool m = false;
          if (title.find(t) != std::string::npos) { score += 3.0; m = true; }
          if (url.find(t) != std::string::npos) { score += 1.5; m = true; }
          if (snip.find(t) != std::string::npos) { score += 1.0; m = true; }
          if (m) matched++;
        }
        score /= (double)(tokens.size() * 3);
      } else {
        score = 1.0;
      }
      s.score = score;
      s.matchedTokens = matched;
      out.push_back(std::move(s));
    }
  };

  struct Round {
    std::vector<ScoredHit> hits;
    bool cnOk = false;
    bool bingOk = false;
    bool baiduOk = false;
  };
  auto fetchRound = [&](std::string const& enc) {
    Round r;
    std::vector<SearchHitS> bing, bingCn, baidu;
    r.bingOk =
        fetchEngine("bing", ("https://www.bing.com/search?q=" + enc + "&count=12").c_str(), bing);
    r.cnOk =
        fetchEngine("bing", ("https://cn.bing.com/search?q=" + enc + "&count=12").c_str(), bingCn);
    r.baiduOk = fetchEngine("baidu", ("https://www.baidu.com/s?wd=" + enc + "&rn=12").c_str(), baidu);
    if (!r.cnOk && !r.bingOk) {
      // Network without reachable Bing at all: try Baidu as the sole source.
      if (r.baiduOk) absorb("baidu", baidu, r.hits);
    } else {
      absorb("cn.bing", bingCn, r.hits);
      absorb("bing", bing, r.hits);
      if (r.baiduOk) absorb("baidu", baidu, r.hits);
    }
    if (std::getenv("SEARCH_DEBUG"))
      std::fprintf(stderr,
                   "[search] reached cn.bing=%d bing=%d baidu=%d parsed %zu/%zu/%zu hits\n",
                   (int)r.cnOk, (int)r.bingOk, (int)r.baiduOk, bingCn.size(), bing.size(),
                   baidu.size());
    return r;
  };

  Round rd = fetchRound(urlEncode(q));
  // No query-rewriting retry: quoting the entity ("钢铁雄心4") was measured to
  // return byte-for-byte the same steel-industry pages as the unquoted query,
  // so it only cost three extra round-trips.
  std::vector<ScoredHit>& all = rd.hits;
  bool bingCnOk = rd.cnOk, bingOk = rd.bingOk, baiduOk = rd.baiduOk;
  if (all.empty()) {
    return "{\"error\":\"all search engines unreachable (cn.bing, bing, baidu)\"}";
  }

  // Zero-scored hits are always dropped: a page that matches no query token is
  // not a result however plausible it looks. The old guard only pruned them
  // when *something else* scored, so a nonsense query still delivered pages.
  std::vector<ScoredHit> keep;
  std::vector<std::string> seenKeys;
  std::map<std::string, int> perHost;
  auto hostOf = [&](std::string const& u) {
    size_t hp = u.find("://");
    size_t hb = hp == std::string::npos ? 0 : hp + 3;
    size_t he = u.find('/', hb);
    std::string host = u.substr(hb, he == std::string::npos ? std::string::npos : he - hb);
    if (host.size() > 4 && host.rfind("www.", 0) == 0) host = host.substr(4);
    // Baidu organic results are /link?url= redirects sharing one host: treat
    // the full redirect URL as the identity so results are not collapsed.
    if (u.find("baidu.com/link?") != std::string::npos) return std::string("link:") + u;
    return host;
  };
  for (auto& s : all) {
    if (!tokens.empty() && s.score <= 0.0) continue;
    std::string host = hostOf(s.h.url);
    // Identity is host+title, not host: two different pages on one site are not
    // clones, and host-only collapsing let one engine take every slot.
    std::string key = host + "|" + lower(s.h.title).substr(0, 24);
    bool dup = false;
    for (auto const& c : seenKeys) {
      if (c == key) { dup = true; break; }
    }
    if (dup) continue;
    if (perHost[host] >= 2) continue;  // bound one site's share of the list
    perHost[host]++;
    seenKeys.push_back(key);
    keep.push_back(std::move(s));
  }
  if (keep.empty()) {
    mini::Value none = mini::Value::makeObject();
    none.set("ok", mini::Value::makeBool(true));
    none.set("query", mini::Value::makeString(q));
    none.set("quality", mini::Value::makeString("none"));
    none.set("hint", mini::Value::makeString(
                         "抓取到的页面没有一个含查询词，说明搜索引擎也没理解这个查询，"
                         "而不是「网上没有」。换说法、补产品全名或年份再搜；已有候选站点就直接 web_fetch。"));
    none.set("results", mini::Value::makeArray());
    return mini::dump(none);
  }
  std::stable_sort(keep.begin(), keep.end(),
                   [](ScoredHit const& a, ScoredHit const& b) { return a.score > b.score; });

  // Coverage = share of query tokens the best hit contains.
  double bestCoverage = coverageOf(keep);
  // Report what each engine *contributed*, not merely what answered: the old
  // field said "cn.bing+bing" while dedupe had already discarded everything but
  // cn.bing, which made single-source results look cross-checked.
  std::map<std::string, int> contrib;
  for (auto const& s : keep) contrib[s.engine]++;
  mini::Value src = mini::Value::makeObject();
  src.set("cn.bing", mini::Value::makeInt(contrib["cn.bing"]));
  src.set("bing", mini::Value::makeInt(contrib["bing"]));
  src.set("baidu", mini::Value::makeInt(contrib["baidu"]));
  std::string contributed, reached;
  if (contrib["cn.bing"]) contributed += "cn.bing,";
  if (contrib["bing"]) contributed += "bing,";
  if (contrib["baidu"]) contributed += "baidu,";
  if (!contributed.empty()) contributed.pop_back();
  if (bingCnOk) reached += "cn.bing,";
  if (bingOk) reached += "bing,";
  if (baiduOk) reached += "baidu,";
  if (!reached.empty()) reached.pop_back();

  char const* quality = bestCoverage >= 0.5   ? "ok"
                        : bestCoverage >= 0.25 ? "partial"
                                               : "low";
  mini::Value res = mini::Value::makeObject();
  res.set("engine", mini::Value::makeString(contributed.empty() ? reached : contributed));
  res.set("engines_reached", mini::Value::makeString(reached));
  res.set("sources", std::move(src));
  res.set("query", mini::Value::makeString(q));
  res.set("quality", mini::Value::makeString(quality));
  res.set("coverage_pct", mini::Value::makeInt((int64_t)(bestCoverage * 100)));
  if (std::string(quality) == "low") {
    res.set("hint", mini::Value::makeString(
                        std::string("最佳结果只覆盖了查询词的 ") +
                        std::to_string((int)(bestCoverage * 100)) +
                        "%。别把它当答案复述；挑出最像含结论的站点用 web_fetch 读正文，"
                        "或把查询词换成更具体的名称/年份再试。"));
  }
  mini::Value arr = mini::Value::makeArray();
  for (size_t i = 0; i < keep.size() && (int)i < maxResults; i++) {
    mini::Value o = mini::Value::makeObject();
    o.set("title", mini::Value::makeString(keep[i].h.title));
    o.set("url", mini::Value::makeString(keep[i].h.url));
    o.set("snippet", mini::Value::makeString(keep[i].h.snippet));
    o.set("source", mini::Value::makeString(keep[i].engine));
    o.set("score_pct", mini::Value::makeInt((int64_t)(keep[i].score * 100 + 0.5)));
    arr.arr.push_back(std::move(o));
  }
  res.set("results", std::move(arr));
  return mini::dump(res);
}

// ---------------------------------------------------------------------------
// Headless browser tools (share one Chrome/Edge session via CDP).

std::string gsErr(std::string const& msg) {
  mini::Value o = mini::Value::makeObject();
  o.set("error", mini::Value::makeString(msg));
  return mini::dump(o);
}

int64_t jint(mini::Value const& args, char const* key, int64_t def) {
  mini::Value const* v = args.get(key);
  return v ? v->asInt(def) : def;
}
double jdbl(mini::Value const& args, char const* key, double def) {
  mini::Value const* v = args.get(key);
  return v ? v->asDouble(def) : def;
}
bool jbool(mini::Value const& args, char const* key, bool def) {
  mini::Value const* v = args.get(key);
  return v ? v->asBool(def) : def;
}
std::string gsJstr(mini::Value const& args, char const* key) {
  mini::Value const* v = args.get(key);
  return v ? v->asString() : std::string();
}
std::string errFrom(mini::Value const& v, char const* prefix) {
  mini::Value const* e = v.get("error");
  return prefix + (e ? e->asString() : "unknown error");
}

namespace {

std::mutex g_askMutex;
std::function<std::string(std::string const&)> g_askUserFn;

std::mutex g_subagentMutex;
std::function<std::string(std::string const&, std::string const&)> g_subagentFn;

std::mutex g_subagentParallelMutex;
std::function<std::string(std::string const&)> g_subagentParallelFn;

struct Todo {
  std::string content;
  std::string status;  // pending / in_progress / completed / cancelled
  std::string priority;
};

std::mutex g_todoMutex;
std::vector<Todo> g_todos;

}  // namespace

void setAskUserCallback(std::function<std::string(std::string const&)> fn) {
  std::lock_guard lk(g_askMutex);
  g_askUserFn = std::move(fn);
}

std::string toolAskUser(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string q = gsJstr(args, "question");
  if (q.empty()) return gsErr("question is required");
  std::function<std::string(std::string const&)> fn;
  {
    std::lock_guard lk(g_askMutex);
    fn = g_askUserFn;
  }
  if (!fn) return gsErr("ask_user is unavailable in non-interactive mode");
  std::string ans = fn(q);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("answer", mini::Value::makeString(ans));
  return mini::dump(o);
}

void setSubagentCallback(
    std::function<std::string(std::string const&, std::string const&)> fn) {
  std::lock_guard lk(g_subagentMutex);
  g_subagentFn = std::move(fn);
}

std::string toolSubagent(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string prompt = gsJstr(args, "prompt");
  if (prompt.empty()) return gsErr("prompt is required");
  std::string description = gsJstr(args, "description");
  std::function<std::string(std::string const&, std::string const&)> fn;
  {
    std::lock_guard lk(g_subagentMutex);
    fn = g_subagentFn;
  }
  if (!fn) return gsErr("subagent is unavailable in non-interactive mode");
  std::string result = fn(description, prompt);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("result", mini::Value::makeString(result));
  return mini::dump(o);
}

void setSubagentParallelCallback(std::function<std::string(std::string const&)> fn) {
  std::lock_guard lk(g_subagentParallelMutex);
  g_subagentParallelFn = std::move(fn);
}

std::string toolSubagentParallel(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  mini::Value const* tasks = args.get("tasks");
  if (!tasks || tasks->type != mini::Value::Array || tasks->arr.empty())
    return gsErr("tasks must be a non-empty array of {description, prompt}");
  int64_t maxPar = jint(args, "max_parallel", 4);
  if (maxPar < 1) maxPar = 1;
  if (maxPar > 12) maxPar = 12;
  std::function<std::string(std::string const&)> fn;
  {
    std::lock_guard lk(g_subagentParallelMutex);
    fn = g_subagentParallelFn;
  }
  if (!fn) return gsErr("subagent_parallel is unavailable in non-interactive mode");
  mini::Value payload = mini::Value::makeObject();
  payload.set("tasks", *tasks);
  payload.set("max_parallel", mini::Value::makeInt(maxPar));
  return fn(mini::dump(payload));
}

namespace {
mini::Value todoToJson(Todo const& t) {
  mini::Value o = mini::Value::makeObject();
  o.set("content", mini::Value::makeString(t.content));
  o.set("status", mini::Value::makeString(t.status));
  if (!t.priority.empty()) o.set("priority", mini::Value::makeString(t.priority));
  return o;
}
}  // namespace

std::string todoListJson() {
  std::lock_guard lk(g_todoMutex);
  mini::Value arr = mini::Value::makeArray();
  for (auto const& t : g_todos) arr.arr.push_back(todoToJson(t));
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("todos", std::move(arr));
  return mini::dump(o);
}

std::string toolTodoWrite(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  if (!args.get("todos")) return todoListJson();
  mini::Value const* items = args.get("todos");
  if (items->type != mini::Value::Array) return gsErr("todos must be an array");
  static const char* kStatuses[] = {"pending", "in_progress", "completed", "cancelled"};
  std::vector<Todo> next;
  next.reserve(items->arr.size());
  for (auto const& it : items->arr) {
    if (it.type != mini::Value::Object) return gsErr("each todo must be an object");
    std::string content = it.has("content") ? it.get("content")->asString() : "";
    if (content.empty()) return gsErr("each todo needs a non-empty content");
    std::string status = it.has("status") ? it.get("status")->asString() : "pending";
    bool known = false;
    for (char const* s : kStatuses)
      if (status == s) { known = true; break; }
    if (!known) return gsErr("invalid status '" + status + "'");
    Todo t;
    t.content = content;
    t.status = status;
    if (it.has("priority")) t.priority = it.get("priority")->asString();
    next.push_back(std::move(t));
  }
  {
    std::lock_guard lk(g_todoMutex);
    g_todos = std::move(next);
  }
  return todoListJson();
}

namespace {

rag::Index g_ragIndex;
std::mutex g_ragMutex;
std::string g_ragIndexFile = "out/rag_index.json";

}  // namespace

std::string toolRagIndex(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string q = gsJstr(args, "path");
  if (q.empty()) q = ".";
  std::string indexFile = gsJstr(args, "index_file");
  if (indexFile.empty()) indexFile = g_ragIndexFile;
  std::lock_guard lk(g_ragMutex);
  std::string e = g_ragIndex.build(q);
  if (!e.empty()) return gsErr("rag_index: " + e);
  std::string saveErr = g_ragIndex.save(indexFile);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("root", mini::Value::makeString(g_ragIndex.root()));
  o.set("files", mini::Value::makeInt((int64_t)g_ragIndex.fileCount()));
  o.set("segments", mini::Value::makeInt((int64_t)g_ragIndex.segmentCount()));
  o.set("index_file", mini::Value::makeString(indexFile));
  if (!saveErr.empty()) o.set("save_error", mini::Value::makeString(saveErr));
  return mini::dump(o);
}

std::string toolRagSearch(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string q = gsJstr(args, "query");
  if (q.empty()) return gsErr("query is required");
  std::string path = gsJstr(args, "path");
  if (path.empty()) path = ".";
  int64_t topK = jint(args, "top_k", 5);
  if (topK < 1) topK = 1;
  if (topK > 20) topK = 20;
  std::string indexFile = gsJstr(args, "index_file");
  if (indexFile.empty()) indexFile = g_ragIndexFile;

  std::lock_guard lk(g_ragMutex);
  // Reuse a persisted index unless it is stale (cheap mtime scan) and keep it
  // fresh with a full rebuild when files changed.
  if (g_ragIndex.empty()) {
    bool haveOnDisk = false;
    {
      std::error_code ec;
      haveOnDisk = std::filesystem::is_regular_file(indexFile, ec);
    }
    if (haveOnDisk) {
      std::string e = g_ragIndex.load(indexFile);
      if (!e.empty()) {
        rag::Index fresh;
        std::string be = fresh.build(path);
        if (!be.empty()) return gsErr("rag_search: " + be);
        g_ragIndex = std::move(fresh);
      }
    } else {
      std::string be = g_ragIndex.build(path);
      if (!be.empty()) return gsErr("rag_search: " + be);
    }
  }
  std::string s = g_ragIndex.buildIfStale(path);
  if (!s.empty() && s != "unchanged") {
    rag::Index fresh;
    std::string be = fresh.build(path);
    if (!be.empty()) return gsErr("rag_search: " + be);
    g_ragIndex = std::move(fresh);
  }

  std::vector<rag::Hit> hits;
  if (!g_ragIndex.search(q, (int)topK, hits)) {
    mini::Value o = mini::Value::makeObject();
    o.set("ok", mini::Value::makeBool(true));
    o.set("results", mini::Value::makeArray());
    o.set("note", mini::Value::makeString("no matching segments (index may be empty)"));
    return mini::dump(o);
  }
  mini::Value arr = mini::Value::makeArray();
  for (auto const& h : hits) {
    mini::Value o = mini::Value::makeObject();
    o.set("path", mini::Value::makeString(h.path));
    o.set("line", mini::Value::makeInt(h.line));
    o.set("score", mini::Value::makeDouble(h.score));
    o.set("text", mini::Value::makeString(h.text));
    arr.arr.push_back(std::move(o));
  }
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("results", std::move(arr));
  return mini::dump(o);
}

std::string toolGrep(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string pattern = gsJstr(args, "pattern");
  if (pattern.empty()) return gsErr("missing 'pattern' regex");
  std::string root = gsJstr(args, "path");
  if (root.empty()) root = ".";
  std::string include = gsJstr(args, "include");
  bool ignoreCase = jbool(args, "ignore_case", false);
  bool fixed = jbool(args, "fixed", false);
  int contextLines = (int)jint(args, "context_lines", 0);
  if (contextLines < 0 || contextLines > 3) contextLines = 0;
  size_t maxResults = (size_t)jint(args, "max_results", 100);
  if (maxResults < 1) maxResults = 1;

  std::regex::flag_type flags = std::regex::ECMAScript;
  if (ignoreCase) flags |= std::regex::icase;
  std::string err;
  GrepResult gr;
  try {
    // fixed=true treats the pattern literally. Without it 'C++' is really the
    // regex 'C+' (matches "CC"), and '(paren' dies as a regex_error.
    std::regex re(fixed ? regexEscape(pattern) : pattern, flags);
    gr = grepFiles(root, re, include, contextLines, maxResults, 64u << 20, &err);
  } catch (std::regex_error const& e) {
    return gsErr("bad regex: " + std::string(e.what()) +
                 " (pass \"fixed\": true to search literally)");
  } catch (std::exception const& e) {
    return gsErr("grep failed: " + std::string(e.what()));
  }
  if (!err.empty()) return gsErr(err);

  mini::Value o = mini::Value::makeObject();
  mini::Value arr = mini::Value::makeArray();
  for (auto const& h : gr.hits) {
    mini::Value it = mini::Value::makeObject();
    it.set("file", mini::Value::makeString(h.file));
    it.set("line", mini::Value::makeInt(h.line));
    it.set("text", mini::Value::makeString(h.text));
    if (h.textTruncated) it.set("text_truncated", mini::Value::makeBool(true));
    if (!h.context.empty()) {
      mini::Value ctx = mini::Value::makeArray();
      for (auto const& c : h.context) ctx.arr.push_back(mini::Value::makeString(c));
      it.set("context", std::move(ctx));
    }
    arr.arr.push_back(std::move(it));
  }
  o.set("ok", mini::Value::makeBool(true));
  o.set("matches", mini::Value::makeInt((int64_t)gr.hits.size()));
  o.set("truncated", mini::Value::makeBool(gr.truncated));
  // Report what the walk left out, so zero hits can be read as "not in the
  // searched set" rather than a definitive "does not exist".
  o.set("scanned_files", mini::Value::makeInt((int64_t)gr.stats.filesScanned));
  if (gr.stats.skippedBinary)
    o.set("skipped_binary", mini::Value::makeInt((int64_t)gr.stats.skippedBinary));
  if (gr.stats.skippedTooBig)
    o.set("skipped_too_big", mini::Value::makeInt((int64_t)gr.stats.skippedTooBig));
  if (gr.stats.linesTruncated)
    o.set("hit_lines_truncated", mini::Value::makeInt((int64_t)gr.stats.linesTruncated));
  if (!gr.stats.skippedDirs.empty()) {
    mini::Value sd = mini::Value::makeArray();
    for (auto const& d : gr.stats.skippedDirs)
      sd.arr.push_back(mini::Value::makeString(d));
    o.set("skipped_dirs", std::move(sd));
  }
  o.set("results", std::move(arr));
  return mini::dump(o);
}

std::string toolGlob(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string pattern = gsJstr(args, "pattern");
  if (pattern.empty()) return gsErr("missing 'pattern' glob");
  std::string root = gsJstr(args, "path");
  if (root.empty()) root = ".";
  // A bare name like "*.cpp" should match at any depth; only patterns with an
  // explicit '/' keep their directory structure ('**') otherwise.
  if (pattern.find('/') == std::string::npos) pattern = "**/" + pattern;
  size_t maxEntries = (size_t)jint(args, "max_entries", 500);
  if (maxEntries < 1) maxEntries = 1;

  std::string err;
  WalkStats stats;
  std::vector<std::string> files = globFiles(root, pattern, maxEntries, stats, &err);
  if (!err.empty()) return gsErr(err);

  mini::Value o = mini::Value::makeObject();
  mini::Value arr = mini::Value::makeArray();
  for (auto const& f : files) arr.arr.push_back(mini::Value::makeString(f));
  o.set("ok", mini::Value::makeBool(true));
  o.set("files", std::move(arr));
  o.set("count", mini::Value::makeInt((int64_t)files.size()));
  o.set("truncated", mini::Value::makeBool(files.size() >= maxEntries));
  o.set("scanned_files", mini::Value::makeInt((int64_t)stats.filesScanned));
  if (!stats.skippedDirs.empty()) {
    mini::Value sd = mini::Value::makeArray();
    for (auto const& d : stats.skippedDirs) sd.arr.push_back(mini::Value::makeString(d));
    o.set("skipped_dirs", std::move(sd));
  }
  return mini::dump(o);
}

std::vector<unsigned char> base64Decode(std::string const& in) {
  static char const* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int tbl[256];
  std::memset(tbl, -1, sizeof tbl);
  for (int i = 0; i < 64; i++) tbl[(unsigned char)T[i]] = i;
  std::vector<unsigned char> out;
  int val = 0, nbits = 0;
  for (unsigned char c : in) {
    int d = tbl[c];
    if (d < 0) continue;  // skip padding/newlines
    val = (val << 6) | d;
    nbits += 6;
    if (nbits >= 8) {
      nbits -= 8;
      out.push_back((unsigned char)((val >> nbits) & 0xFF));
    }
  }
  return out;
}

#ifdef _WIN32
// The CDP/Chrome stack below is Windows-only in this build (see browser.cpp);
// the browser_* tools are not registered elsewhere.
// Mouse press/release sequence at (x, y). `hoverOnly` skips the buttons.
bool mouseClick(BrowserSession& bs, double x, double y, std::string const& button, int clicks,
                bool hoverOnly, std::string* err) {
  auto fire = [&](char const* type, std::string const& btn, int cc) {
    mini::Value p = mini::Value::makeObject();
    p.set("type", mini::Value::makeString(type));
    p.set("x", mini::Value::makeInt((int)x));
    p.set("y", mini::Value::makeInt((int)y));
    p.set("button", mini::Value::makeString(btn));
    p.set("clickCount", mini::Value::makeInt(cc));
    return bs.call("Input.dispatchMouseEvent", p);
  };
  mini::Value r = fire("mouseMoved", "none", 0);
  if (r.has("error")) { *err = errFrom(r, "mouse: "); return false; }
  if (hoverOnly) return true;
  for (int i = 1; i <= clicks; i++) {
    r = fire("mousePressed", button, i);
    if (r.has("error")) { *err = errFrom(r, "mouse: "); return false; }
    r = fire("mouseReleased", button, i);
    if (r.has("error")) { *err = errFrom(r, "mouse: "); return false; }
  }
  return true;
}

// Locate an element by CSS selector, scroll it into view, and return its
// center coordinates plus a short description.
bool findElement(BrowserSession& bs, std::string const& sel, double* x, double* y,
                 std::string* desc, std::string* err) {
  std::string esc;
  mini::escapeInto(esc, sel);
  std::string expr = "(function(){var el=document.querySelector(" + esc + ");if(!el)return null;"
                     "el.scrollIntoView({block:'center',inline:'center'});"
                     "var r=el.getBoundingClientRect();"
                     "return {x:r.left+r.width/2,y:r.top+r.height/2,w:r.width,h:r.height,"
                     "tag:el.tagName.toLowerCase(),id:el.id||'',text:(el.innerText||'').slice(0,120)};})()";
  mini::Value r = bs.evaluate(expr);
  if (r.has("error")) { *err = errFrom(r, "find: "); return false; }
  if (r.type == mini::Value::Null) { *err = "selector not found: " + sel; return false; }
  if (r.has("x")) *x = r.get("x")->asDouble(0);
  if (r.has("y")) *y = r.get("y")->asDouble(0);
  if (desc) {
    std::string d = gsJstr(r, "tag");
    std::string id = gsJstr(r, "id");
    if (!id.empty()) d += "#" + id;
    std::string text = gsJstr(r, "text");
    if (!text.empty()) d += " \"" + text + "\"";
    *desc = d;
  }
  return true;
}

struct KeyInfo {
  std::string name, code, text;
  int vk = 0;
};

bool keyInfo(std::string const& key, KeyInfo& out) {
  struct E { char const* name; char const* code; int vk; char const* text; };
  static const E map[] = {
      {"enter", "Enter", 13, "\r"},          {"tab", "Tab", 9, "\t"},
      {"space", "Space", 32, " "},           {"backspace", "Backspace", 8, ""},
      {"delete", "Delete", 46, ""},          {"escape", "Escape", 27, ""},
      {"home", "Home", 36, ""},              {"end", "End", 35, ""},
      {"pageup", "PageUp", 33, ""},          {"pagedown", "PageDown", 34, ""},
      {"arrowup", "ArrowUp", 38, ""},        {"up", "ArrowUp", 38, ""},
      {"arrowdown", "ArrowDown", 40, ""},    {"down", "ArrowDown", 40, ""},
      {"arrowleft", "ArrowLeft", 37, ""},    {"left", "ArrowLeft", 37, ""},
      {"arrowright", "ArrowRight", 39, ""},  {"right", "ArrowRight", 39, ""},
  };
  for (auto const& e : map) {
    if (key == e.name) {
      out = KeyInfo{key, e.code, e.text, e.vk};
      return true;
    }
  }
  if (key.size() == 1) {
    char c = key[0];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      char up = (char)toupper(c);
      std::string code = (c >= '0') ? std::string("Digit") + c : std::string("Key") + up;
      out = KeyInfo{key, code, key, (int)up};
      return true;
    }
  }
  if (key.size() >= 2 && key[0] == 'f' && key.size() <= 3) {
    int n = std::atoi(key.c_str() + 1);
    if (n >= 1 && n <= 12) {
      out = KeyInfo{key, std::string("F") + std::to_string(n), "", 111 + n};
      return true;
    }
  }
  return false;
}

bool dispatchKey(BrowserSession& bs, KeyInfo const& k, int modifiers, std::string* err) {
  auto ev = [&](char const* type, bool withText) {
    mini::Value p = mini::Value::makeObject();
    p.set("type", mini::Value::makeString(type));
    p.set("modifiers", mini::Value::makeInt(modifiers));
    p.set("key", mini::Value::makeString(k.name));
    p.set("code", mini::Value::makeString(k.code));
    p.set("windowsVirtualKeyCode", mini::Value::makeInt(k.vk));
    p.set("nativeVirtualKeyCode", mini::Value::makeInt(k.vk));
    if (withText && !k.text.empty()) {
      p.set("text", mini::Value::makeString(k.text));
      p.set("unmodifiedText", mini::Value::makeString(k.text));
    } else {
      p.set("text", mini::Value::makeString(""));
      p.set("unmodifiedText", mini::Value::makeString(""));
    }
    return bs.call("Input.dispatchKeyEvent", p);
  };
  mini::Value r = ev("rawKeyDown", true);
  if (r.has("error")) { *err = errFrom(r, "key: "); return false; }
  if (modifiers == 0 && !k.text.empty()) {
    mini::Value ch = mini::Value::makeObject();
    ch.set("type", mini::Value::makeString("char"));
    ch.set("modifiers", mini::Value::makeInt(0));
    ch.set("text", mini::Value::makeString(k.text));
    mini::Value r2 = bs.call("Input.dispatchKeyEvent", ch);
    if (r2.has("error")) { *err = errFrom(r2, "key: "); return false; }
  }
  r = ev("keyUp", false);
  if (r.has("error")) { *err = errFrom(r, "key: "); return false; }
  return true;
}

std::string toolBrowserOpen(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string url = gsJstr(args, "url");
  if (url.empty()) return gsErr("missing 'url' string");
  int waitMs = (int)jint(args, "wait_ms", 30000);
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);
  std::string nerr = bs.navigateAndWait(url, waitMs);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(nerr.empty()));
  o.set("url", mini::Value::makeString(bs.currentUrl()));
  o.set("title", mini::Value::makeString(bs.currentTitle()));
  if (!nerr.empty()) o.set("note", mini::Value::makeString(nerr));
  return mini::dump(o);
}

std::string toolBrowserClick(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string sel = gsJstr(args, "selector");
  bool hasX = args.has("x"), hasY = args.has("y");
  if (sel.empty() && !hasX && !hasY) return gsErr("need 'selector' or x/y coordinates");
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);
  double x = jdbl(args, "x", 0), y = jdbl(args, "y", 0);
  std::string desc;
  if (!sel.empty()) {
    std::string err;
    if (!findElement(bs, sel, &x, &y, &desc, &err)) return gsErr(err);
  }
  std::string button = gsJstr(args, "button");
  if (button.empty()) button = "left";
  if (button != "left" && button != "right" && button != "middle")
    return gsErr("button must be left/right/middle");
  int clicks = (int)jint(args, "click_count", 1);
  if (clicks < 1 || clicks > 2) clicks = 1;
  std::string err;
  if (!mouseClick(bs, x, y, button, clicks, false, &err)) return gsErr(err);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("x", mini::Value::makeInt((int)x));
  o.set("y", mini::Value::makeInt((int)y));
  o.set("button", mini::Value::makeString(button));
  o.set("clicks", mini::Value::makeInt(clicks));
  if (!desc.empty()) o.set("target", mini::Value::makeString(desc));
  return mini::dump(o);
}

std::string toolBrowserType(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string text = gsJstr(args, "text");
  if (text.empty()) return gsErr("missing 'text' string");
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);
  std::string sel = gsJstr(args, "selector");
  if (!sel.empty()) {
    double x, y;
    std::string desc, err;
    if (!findElement(bs, sel, &x, &y, &desc, &err)) return gsErr(err);
    if (!mouseClick(bs, x, y, "left", 1, false, &err)) return gsErr(err);
  }
  mini::Value p = mini::Value::makeObject();
  p.set("text", mini::Value::makeString(text));
  mini::Value r = bs.call("Input.insertText", p);
  if (r.has("error")) return gsErr(errFrom(r, "type: "));
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("chars", mini::Value::makeInt((int64_t)text.size()));
  if (!sel.empty()) o.set("target", mini::Value::makeString(sel));
  return mini::dump(o);
}

std::string toolBrowserKey(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string key = gsJstr(args, "key");
  if (key.empty()) return gsErr("missing 'key' string");
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);

  // Parse "ctrl+shift+t" style combinations.
  int modifiers = 0;
  std::string name;
  std::string cur;
  auto flush = [&]() {
    if (cur.empty()) return;
    if (cur == "ctrl" || cur == "control") modifiers |= 2;
    else if (cur == "alt") modifiers |= 1;
    else if (cur == "shift") modifiers |= 8;
    else if (cur == "meta" || cur == "win" || cur == "command") modifiers |= 4;
    else name = cur;
    cur.clear();
  };
  for (char c : key) {
    if (c == '+') flush();
    else cur += (char)tolower(c);
  }
  flush();
  if (name.empty() && modifiers != 0) return gsErr("key needs a key name besides modifiers");
  if (name.empty()) name = cur;
  KeyInfo k;
  if (!keyInfo(name, k))
    return gsErr("unsupported key: '" + name +
                   "' (try enter, tab, space, backspace, delete, escape, arrows, letters, "
                   "digits, F1-F12, or 'ctrl+...' combinations)");
  std::string err;
  if (!dispatchKey(bs, k, modifiers, &err)) return gsErr(err);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("key", mini::Value::makeString(key));
  o.set("modifiers", mini::Value::makeInt(modifiers));
  return mini::dump(o);
}

std::string toolBrowserScroll(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string dir = gsJstr(args, "direction");
  if (dir.empty()) return gsErr("missing 'direction' (up/down/left/right/top/bottom)");
  double amount = jdbl(args, "amount", 300);
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);

  mini::Value o = mini::Value::makeObject();
  if (dir == "top" || dir == "bottom") {
    std::string js = dir == "top"
                         ? "window.scrollTo(0,0)"
                         : "window.scrollTo(0,Math.max(document.documentElement.scrollHeight,"
                           "document.body.scrollHeight))";
    mini::Value r = bs.evaluate(js, false);
    if (r.has("error")) return gsErr(errFrom(r, "scroll: "));
    o.set("ok", mini::Value::makeBool(true));
    o.set("position", mini::Value::makeString(dir));
    return mini::dump(o);
  }
  double dx = 0, dy = 0;
  if (dir == "up") dy = -amount;
  else if (dir == "down") dy = amount;
  else if (dir == "left") dx = -amount;
  else if (dir == "right") dx = amount;
  else return gsErr("direction must be up/down/left/right/top/bottom");

  mini::Value c = bs.evaluate("[Math.round(innerWidth/2),Math.round(innerHeight/2)]", false);
  double x = 683, y = 384;  // default: 1366x768 viewport center
  if (!c.has("error") && c.type == mini::Value::Array && c.arr.size() == 2) {
    x = c.arr[0].asDouble(x);
    y = c.arr[1].asDouble(y);
  }
  mini::Value p = mini::Value::makeObject();
  p.set("type", mini::Value::makeString("mouseWheel"));
  p.set("x", mini::Value::makeInt((int)x));
  p.set("y", mini::Value::makeInt((int)y));
  p.set("deltaX", mini::Value::makeInt((int)dx));
  p.set("deltaY", mini::Value::makeInt((int)dy));
  mini::Value r = bs.call("Input.dispatchMouseEvent", p);
  if (r.has("error")) return gsErr(errFrom(r, "scroll: "));
  o.set("ok", mini::Value::makeBool(true));
  o.set("direction", mini::Value::makeString(dir));
  o.set("delta_x", mini::Value::makeInt((int)dx));
  o.set("delta_y", mini::Value::makeInt((int)dy));
  return mini::dump(o);
}

std::string toolBrowserSnapshot(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  int maxChars = (int)jint(args, "max_chars", 12000);
  int maxLines = (int)jint(args, "max_lines", 400);
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);
  mini::Value s = bs.snapshotOutline(maxChars, maxLines);
  if (s.has("error")) return gsErr(errFrom(s, "snapshot: "));
  s.set("ok", mini::Value::makeBool(true));
  s.set("url", mini::Value::makeString(bs.currentUrl()));
  s.set("title", mini::Value::makeString(bs.currentTitle()));
  return mini::dump(s);
}

std::string toolBrowserEval(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string expr = gsJstr(args, "expression");
  if (expr.empty()) return gsErr("missing 'expression' string");
  bool awaitP = jbool(args, "await_promise", true);
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);
  mini::Value r = bs.evaluate(expr, awaitP);
  if (r.has("error")) return gsErr(errFrom(r, "eval: "));
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  switch (r.type) {
    case mini::Value::String:
      o.set("type", mini::Value::makeString("string"));
      o.set("value", mini::Value::makeString(truncateStr(r.s, 4000)));
      break;
    case mini::Value::Null:
      o.set("type", mini::Value::makeString("null"));
      o.set("value", mini::Value::makeString("null"));
      break;
    default:
      o.set("type", mini::Value::makeString(
                        r.type == mini::Value::Object ? "object"
                        : r.type == mini::Value::Array ? "array"
                        : r.type == mini::Value::Bool   ? "boolean"
                                                        : "number"));
      o.set("value", mini::Value::makeString(truncateStr(mini::dump(r), 8000)));
      break;
  }
  return mini::dump(o);
}

std::string toolBrowserWait(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  int ms = (int)jint(args, "ms", 800);
  if (ms < 0 || ms > 300000) ms = 800;
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("waited_ms", mini::Value::makeInt(ms));
  return mini::dump(o);
}

std::string toolBrowserHistory(std::string const& argsJson, int step) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);
  mini::Value nav = bs.call("Page.getNavigationHistory", mini::Value::makeObject());
  if (nav.has("error")) return gsErr(errFrom(nav, "history: "));
  mini::Value const* entries = nav.get("entries");
  int64_t idx = jint(nav, "currentIndex", -1);
  if (!entries || idx < 0 || idx + step < 0 || idx + step >= (int64_t)entries->arr.size())
    return gsErr(step < 0 ? "no previous page in history" : "no next page in history");
  int64_t target = idx + step;
  mini::Value const* entry = &entries->arr[(size_t)target];
  int64_t entryId = jint(*entry, "id", 0);
  if (bs.hasAttachedFiles()) {
    // Chromium's renderer can crash navigating a page with uploaded files:
    // relaunch the session and navigate to the target entry instead.
    std::string targetUrl = gsJstr(*entry, "url");
    if (targetUrl.empty()) return gsErr("history entry has no url");
    bs.stop();
    std::string e2 = bs.ensureStarted();
    if (!e2.empty()) return gsErr("browser: " + e2);
    std::string nerr = bs.navigateAndWait(targetUrl, 30000);
    if (!nerr.empty()) return gsErr("history: " + nerr);
    mini::Value o = mini::Value::makeObject();
    o.set("ok", mini::Value::makeBool(true));
    o.set("url", mini::Value::makeString(bs.currentUrl()));
    o.set("title", mini::Value::makeString(bs.currentTitle()));
    return mini::dump(o);
  }
  bs.clearFileInputs();  // avoid the Chromium file-input reload crash
  mini::Value p = mini::Value::makeObject();
  p.set("entryId", mini::Value::makeInt(entryId));
  mini::Value r = bs.call("Page.navigateToHistoryEntry", p);
  if (r.has("error")) return gsErr(errFrom(r, "history: "));
  std::string w = bs.waitReady();
  if (!w.empty()) return gsErr("history: " + w);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("url", mini::Value::makeString(bs.currentUrl()));
  o.set("title", mini::Value::makeString(bs.currentTitle()));
  return mini::dump(o);
}

std::string toolBrowserBack(std::string const& argsJson) {
  return toolBrowserHistory(argsJson, -1);
}
std::string toolBrowserForward(std::string const& argsJson) {
  return toolBrowserHistory(argsJson, +1);
}

std::string toolBrowserReload(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);
  if (bs.hasAttachedFiles()) {
    // Chromium's renderer can crash reloading a page with uploaded files:
    // relaunch the session and re-open the page instead.
    std::string url = bs.currentUrl();
    bs.stop();
    std::string e2 = bs.ensureStarted();
    if (!e2.empty()) return gsErr("browser: " + e2);
    std::string nerr = bs.navigateAndWait(url, 30000);
    if (!nerr.empty()) return gsErr("reload: " + nerr);
    mini::Value o = mini::Value::makeObject();
    o.set("ok", mini::Value::makeBool(true));
    o.set("url", mini::Value::makeString(bs.currentUrl()));
    o.set("title", mini::Value::makeString(bs.currentTitle()));
    return mini::dump(o);
  }
  bs.clearFileInputs();  // avoid the Chromium file-input reload crash
  mini::Value r = bs.call("Page.reload", mini::Value::makeObject());
  if (r.has("error")) return gsErr(errFrom(r, "reload: "));
  std::string w = bs.waitReady();
  if (!w.empty()) return gsErr("reload: " + w);
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("url", mini::Value::makeString(bs.currentUrl()));
  o.set("title", mini::Value::makeString(bs.currentTitle()));
  return mini::dump(o);
}

std::string toolBrowserScreenshot(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  bool full = jbool(args, "full_page", false);
  BrowserSession& bs = browserSession();
  std::string path = gsJstr(args, "path");
  if (path.empty()) {
    char ts[64];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(ts, sizeof ts, "shot_%Y%m%d_%H%M%S.png", &tm);
    path = std::string("browser_screenshots/") + ts;
  }
  std::error_code ec;
  std::filesystem::path fp(path);
  if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);

  // Capturing a page that still holds uploaded files can kill the renderer;
  // clear them first, and retry once through crash recovery if it dies anyway.
  for (int attempt = 0; attempt < 2; attempt++) {
    std::string e = bs.ensureStarted();
    if (!e.empty()) return gsErr("browser: " + e);
    bs.clearFileInputs();
    mini::Value p = mini::Value::makeObject();
    p.set("format", mini::Value::makeString("png"));
    p.set("captureBeyondViewport", mini::Value::makeBool(full));
    mini::Value r = bs.call("Page.captureScreenshot", p);
    if (r.has("error")) {
      if (bs.crashed()) continue;  // next attempt relaunches and re-navigates
      return gsErr(errFrom(r, "screenshot: "));
    }
    std::string data = gsJstr(r, "data");
    std::vector<unsigned char> png = base64Decode(data);
    if (png.empty()) return gsErr("screenshot returned no image data");
    {
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (!f) return gsErr("cannot write screenshot file: " + path);
      f.write((char const*)png.data(), (std::streamsize)png.size());
    }
    mini::Value o = mini::Value::makeObject();
    o.set("ok", mini::Value::makeBool(true));
    o.set("path", mini::Value::makeString(path));
    o.set("bytes", mini::Value::makeInt((int64_t)png.size()));
    o.set("full_page", mini::Value::makeBool(full));
    return mini::dump(o);
  }
  return gsErr("screenshot failed after crash recovery");
}

std::string toolBrowserUpload(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string sel = gsJstr(args, "selector");
  std::string file = gsJstr(args, "file");
  if (sel.empty()) return gsErr("missing 'selector' for the file input");
  if (file.empty()) return gsErr("missing 'file' path");
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec))
    return gsErr("file not found: " + file);
  BrowserSession& bs = browserSession();
  std::string e = bs.ensureStarted();
  if (!e.empty()) return gsErr("browser: " + e);

  mini::Value r = bs.call("DOM.enable", mini::Value::makeObject());
  if (r.has("error")) return gsErr(errFrom(r, "upload: "));
  r = bs.call("DOM.getDocument", mini::Value::makeObject());
  if (r.has("error")) return gsErr(errFrom(r, "upload: "));
  mini::Value const* root = r.get("root");
  int64_t docNode = root ? jint(*root, "nodeId", 0) : 0;
  if (!docNode) return gsErr("upload: cannot get document root");
  mini::Value qp = mini::Value::makeObject();
  qp.set("nodeId", mini::Value::makeInt(docNode));
  qp.set("selector", mini::Value::makeString(sel));
  r = bs.call("DOM.querySelector", qp);
  if (r.has("error")) return gsErr(errFrom(r, "upload: "));
  int64_t nodeId = jint(r, "nodeId", 0);
  if (!nodeId) return gsErr("upload: selector not found: " + sel);
  mini::Value sp = mini::Value::makeObject();
  mini::Value files = mini::Value::makeArray();
  files.arr.push_back(mini::Value::makeString(file));
  sp.set("files", std::move(files));
  sp.set("nodeId", mini::Value::makeInt(nodeId));
  r = bs.call("DOM.setFileInputFiles", sp);
  if (r.has("error")) return gsErr(errFrom(r, "upload: "));
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("file", mini::Value::makeString(file));
  o.set("selector", mini::Value::makeString(sel));
  return mini::dump(o);
}

std::string toolBrowserClose(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  browserSession().stop();
  mini::Value o = mini::Value::makeObject();
  o.set("ok", mini::Value::makeBool(true));
  o.set("closed", mini::Value::makeBool(true));
  return mini::dump(o);
}
#endif  // _WIN32: CDP browser tools

// ---------------------------------------------------------------------------
// run_code: PTC-mode batch tool runner.
//   {"steps":[{"tool","arguments","var"?,"return"?}...],
//    "parallel"?,"max_parallel"?,"result_cap"?}
// Sequential (default): steps run in order; a string argument equal to "$var"
// or containing "{$var}" is replaced with the value stored under `var` by an
// earlier step. Parallel: all steps run concurrently (no $var references).
// Returns the last step's result (or the step flagged "return": true) plus a
// compact per-step summary.
// ---------------------------------------------------------------------------

namespace {

bool runCodeIsErr(std::string const& s) {
  mini::Value t;
  return mini::tryParse(s, t) && t.type == mini::Value::Object && t.has("error");
}

std::string runCodeErr(std::string const& m) {
  mini::Value e = mini::Value::makeObject();
  e.set("error", mini::Value::makeString(m));
  return mini::dump(e);
}

}  // namespace

std::string toolRunCode(ToolRegistry& reg, std::string const& argsJson) {
  mini::Value v;
  if (!mini::tryParse(argsJson, v) || v.type != mini::Value::Object) {
    return runCodeErr("run_code: program must be a JSON object with a \"steps\" array");
  }
  mini::Value const* steps = v.get("steps");
  if (!steps || steps->type != mini::Value::Array || steps->arr.empty()) {
    return runCodeErr("run_code: \"steps\" array required");
  }
  bool parallel = v.get("parallel") ? v.get("parallel")->asBool(false) : false;
  size_t maxPar = 4;
  if (v.get("max_parallel")) maxPar = (size_t)v.get("max_parallel")->asInt(4);
  if (maxPar < 1) maxPar = 1;
  size_t cap = 8192;
  if (v.get("result_cap")) cap = (size_t)v.get("result_cap")->asInt(8192);
  if (cap < 256) cap = 256;

  // Interpolate "{$var}" / "$var" string arguments from prior steps.
  std::map<std::string, std::string> vars;
  std::function<void(mini::Value&)> interp = [&](mini::Value& node) {
    switch (node.type) {
      case mini::Value::String: {
        std::string s = node.s;
        if (s.size() > 1 && s[0] == '$') {
          std::string nm = s.substr(1);
          auto it = vars.find(nm);
          if (it != vars.end()) { node.s = it->second; return; }
        }
        std::string out;
        for (size_t i = 0; i < s.size();) {
          if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '$') {
            size_t j = i + 2;
            std::string nm;
            while (j < s.size() && s[j] != '}') { nm += s[j]; j++; }
            if (j < s.size() && s[j] == '}') {
              auto it = vars.find(nm);
              if (it != vars.end()) { out += it->second; i = j + 1; continue; }
            }
          }
          out += s[i];
          i++;
        }
        node.s = out;
        break;
      }
      case mini::Value::Array:
        for (auto& el : node.arr) interp(el);
        break;
      case mini::Value::Object:
        for (auto& kv : node.obj) interp(kv.second);
        break;
      default:
        break;
    }
  };

  auto runOne = [&](mini::Value const& step, std::string& out) {
    if (step.type != mini::Value::Object) {
      out = runCodeErr("run_code: step must be an object with tool + arguments");
      return;
    }
    std::string tool = step.has("tool") ? step.get("tool")->asString() : "";
    if (tool.empty()) {
      out = runCodeErr("run_code: step.tool required");
      return;
    }
    mini::Value const* argsPtr = step.get("arguments");
    mini::Value args =
        (argsPtr && argsPtr->type == mini::Value::Object) ? *argsPtr : mini::Value::makeObject();
    interp(args);
    out = reg.run(tool, mini::dump(args));
  };

  mini::Value detail = mini::Value::makeArray();
  std::string output;
  bool anyErr = false;
  size_t n = steps->arr.size();

  if (!parallel) {
    for (size_t i = 0; i < n; i++) {
      mini::Value const& step = steps->arr[i];
      std::string r;
      runOne(step, r);
      mini::Value item = mini::Value::makeObject();
      std::string t = step.type == mini::Value::Object && step.has("tool")
                          ? step.get("tool")->asString()
                          : "?";
      item.set("tool", mini::Value::makeString(t));
      bool ok = !runCodeIsErr(r);
      item.set("ok", mini::Value::makeBool(ok));
      item.set("bytes", mini::Value::makeInt((int64_t)r.size()));
      detail.arr.push_back(std::move(item));
      if (!ok) anyErr = true;
      std::string stored = truncateStr(r, cap);
      output = stored;
      if (step.type == mini::Value::Object && step.has("var")) {
        std::string nm = step.get("var")->asString();
        if (!nm.empty()) vars[nm] = stored;
      }
    }
  } else {
    std::vector<std::string> results(n);
    std::vector<std::thread> ths;
    std::atomic<size_t> next{0};
    size_t workers = std::min(maxPar, n);
    for (size_t w = 0; w < workers; w++) {
      ths.emplace_back([&]() {
        for (;;) {
          size_t i = next.fetch_add(1);
          if (i >= n) break;
          runOne(steps->arr[i], results[i]);
        }
      });
    }
    for (auto& th : ths) th.join();
    for (size_t i = 0; i < n; i++) {
      mini::Value item = mini::Value::makeObject();
      std::string t = steps->arr[i].type == mini::Value::Object &&
                              steps->arr[i].has("tool")
                          ? steps->arr[i].get("tool")->asString()
                          : "?";
      item.set("tool", mini::Value::makeString(t));
      bool ok = !runCodeIsErr(results[i]);
      item.set("ok", mini::Value::makeBool(ok));
      item.set("bytes", mini::Value::makeInt((int64_t)results[i].size()));
      detail.arr.push_back(std::move(item));
      if (!ok) anyErr = true;
    }
    output = n ? truncateStr(results[n - 1], cap) : "";
  }

  mini::Value res = mini::Value::makeObject();
  res.set("ok", mini::Value::makeBool(!anyErr));
  res.set("steps", mini::Value::makeInt((int64_t)detail.arr.size()));
  res.set("output", mini::Value::makeString(output));
  res.set("detail", std::move(detail));
  return mini::dump(res);
}

// ---------------------------------------------------------------------------
// Workspace policy helpers (pure) — see tools.hpp.
// ---------------------------------------------------------------------------
namespace {
std::string caseFold(std::string s) {
#ifdef _WIN32
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
  return s;
}
}  // namespace

std::string normalizeWorkspacePath(std::string const& p) {
  std::filesystem::path in(p);
  std::error_code ec;
  std::filesystem::path abs = in.is_absolute() ? in : std::filesystem::absolute(in, ec);
  if (ec) abs = in;  // absolute() can fail on a vanished CWD; fall back to lexical
  std::filesystem::path norm = abs.lexically_normal();
  std::string s = norm.generic_string();  // '/' separators on all platforms
  // strip a single trailing slash (except a bare root "/").
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return caseFold(s);
}

bool pathWithinNormalizedRoot(std::string const& rootN, std::string const& pN) {
  if (rootN.empty()) return false;
  if (pN.size() < rootN.size()) return false;
  if (pN.compare(0, rootN.size(), rootN) != 0) return false;
  // the whole root matches, or the next char is a path separator
  return pN.size() == rootN.size() || pN[rootN.size()] == '/';
}

std::vector<std::string> commandPathCandidates(std::string const& cmd) {
  std::vector<std::string> out;
  std::string tok;
  bool started = false;  // a token has begun (even if still empty: "")
  char quote = 0;
  auto eval = [&]() {
    if (!started) return;
    // a path-like token: contains a separator, or starts with drive letter, or
    // is a relative dot-path ("." ".." "./x" "../x").
    bool pathLike = tok.find('/') != std::string::npos ||
                    tok.find('\\') != std::string::npos ||
                    tok.rfind("./", 0) == 0 || tok.rfind("../", 0) == 0 ||
                    tok == "." || tok == ".." ||
                    (tok.size() >= 2 && isalpha((unsigned char)tok[0]) && tok[1] == ':');
    if (pathLike) out.push_back(tok);
    tok.clear();
    started = false;
  };
  for (char c : cmd) {
    if (quote) {
      if (c == quote) quote = 0;
      else tok += c;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; started = true; continue; }
    if ((unsigned char)c <= ' ') { eval(); continue; }
    tok += c;
    started = true;
  }
  eval();
  return out;
}

std::vector<std::string> toolPathTargets(std::string const& tool,
                                         std::string const& argsJson) {
  std::vector<std::string> out;
  mini::Value a;
  if (!mini::tryParse(argsJson, a) || a.type != mini::Value::Object) return out;
  static char const* kPathKeys[] = {"path", "file", "dir", "root", "index_path"};
  for (auto const& kv : a.obj) {
    for (auto const* key : kPathKeys) {
      if (kv.first == key && kv.second.type == mini::Value::String &&
          !kv.second.s.empty()) {
        out.push_back(kv.second.s);
        break;
      }
    }
  }
  if (tool == "shell_exec" || tool == "verify") {
    mini::Value const* c = a.get("command");
    if (c && c->type == mini::Value::String)
      for (auto const& t : commandPathCandidates(c->s)) out.push_back(t);
  }
  return out;
}

void registerBuiltinTools(ToolRegistry& reg) {
  mini::Value shell = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("command", strProp("The shell command to run (Windows: cmd /C semantics)."));
    p.set("timeout_ms", intProp("Optional timeout in milliseconds (default 120000)."));
    return p;
  })(), {"command"});
  reg.add({.name = "shell_exec",
           .description = "Run a command in the local shell and capture its output. Use for "
                          "system tasks, builds, git, inspection, etc. Prefer dedicated file "
                          "search/edit tools over shell for file work.",
           .parameters = shell,
           .run = toolShellExec});

  mini::Value vf = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("command",
          strProp("Optional single command to run. When omitted, runs the project's "
                  "build+test cycle (build.ps1 + out/test_*.exe on Windows, make + "
                  "ctest on Unix)."));
    p.set("timeout_ms", intProp("Timeout per step in ms (default 600000)."));
    p.set("max_chars", intProp("Max output bytes per step (default 16000)."));
    return p;
  })(), {});
  reg.add({.name = "verify",
           .description = "Run the project's build and tests (or a custom command) and "
                          "report results. Use after every code change: if any step "
                          "fails, read the failure, fix, and verify again until ok. "
                          "Prefer this over ad-hoc shell commands for the final "
                          "green check.",
           .parameters = vf,
           .run = toolVerify});

  mini::Value rm = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("lesson", strProp("A durable fact, gotcha or failure pattern worth "
                            "remembering across sessions (max 2000 chars)."));
    return p;
  })(), {"lesson"});
  reg.add({.name = "remember",
           .description = "Save a lesson to persistent project memory "
                          "(out/lessons.md). It is injected into future "
                          "sessions, so record durable facts: API quirks, "
                          "build gotchas, naming rules learned the hard way.",
           .parameters = rm,
           .run = toolRemember});
  reg.add({.name = "recall",
           .description = "Return the accumulated lessons from previous "
                          "sessions (out/lessons.md). Use it mid-task to "
                          "check for known pitfalls before repeating a "
                          "mistake.",
           .parameters = mini::Value::makeObject(),
           .run = toolRecall});

  mini::Value bgs = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("command",
          strProp("The command to run in the background (Windows: cmd /C semantics). "));
    p.set("timeout_ms",
          intProp("Optional auto-kill after this many milliseconds (0/absent = run until "
                  "killed)."));
    return p;
  })(), {"command"});
  reg.add({.name = "background_start",
           .description = "Launch a command in the background and return immediately with a "
                          "task id. Use for long-running work (builds, servers, downloads, "
                          "watchers) that should not block the conversation. The task keeps "
                          "running while you do other things; poll with background_status for "
                          "new output. You will automatically be notified when the task "
                          "finishes. Terminate it with background_kill whenever you decide.",
           .parameters = bgs,
           .run = toolBackgroundStart});
  mini::Value bgs_id = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("id", strProp("Task id returned by background_start."));
    return p;
  })(), {"id"});
  reg.add({.name = "background_status",
           .description = "Return the state of a background task and any new output since the "
                          "last poll. Poll sparingly: you are notified when a task completes.",
           .parameters = bgs_id,
           .run = toolBackgroundStatus});
  reg.add({.name = "background_kill",
           .description = "Force-terminate a background task you started and no longer need. "
                          "Returns whether it was still running.",
           .parameters = bgs_id,
           .run = toolBackgroundKill});
  reg.add({.name = "background_list",
           .description = "List every background task (id, command, running state, recent "
                          "output).",
           .parameters = mini::Value::makeObject(),
           .run = toolBackgroundList});

  mini::Value rp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("path", strProp("File path to read. For large files, combine offset/limit to page."));
    p.set("offset", intProp("1-based first line to return (default 1)."));
    p.set("limit", intProp("Max number of lines to return (0 = to end of file)."));
    p.set("max_chars", intProp("Max characters of returned content (default 16000)."));
    return p;
  })(), {"path"});
  reg.add({.name = "file_read",
           .description = "Read a text file from disk, with optional line window "
                          "(offset/limit) for large files.",
           .parameters = rp,
           .pure = true,
           .run = toolFileRead});

  mini::Value wp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("path", strProp("File path to write (parent directories are created)."));
    p.set("content", strProp("Full new file content."));
    return p;
  })(), {"path", "content"});
  reg.add({.name = "file_write",
           .description = "Overwrite a whole file with new content (parent dirs auto-created). "
                          "For targeted changes, prefer `edit` (small) or `patch` (multi-line).",
           .parameters = wp,
           .run = toolFileWrite});

  mini::Value ep = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("path", strProp("File path to edit."));
    p.set("old_string", strProp("Exact text to replace (must exist verbatim)."));
    p.set("new_string", strProp("Replacement text (empty removes)."));
    p.set("replace_all", boolProp("Replace every occurrence (default false)."));
    return p;
  })(), {"path", "old_string", "new_string"});
  reg.add({.name = "edit",
           .description = "Precise string-replacement edit for small, localized changes. "
                          "Read the file first to get exact context. For larger or multiple "
                          "changes use `patch`.",
           .parameters = ep,
           .run = toolEdit});

  mini::Value pp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("patch", strProp(
        "Single-file unified diff. Headers --- a/... and +++ b/... with @@ -l,c +l,c @@ hunks; "
        "context/removed/added lines prefixed by space/minus/plus. Use /dev/null as the old "
        "header to create a file, as the new header to delete it."));
    p.set("strip", intProp("Path-prefix components to drop from the file name (patch -p). "
                           "Default 1 (drops a/ b/)."));
    return p;
  })(), {"patch"});
  reg.add({.name = "patch",
           .description = "Apply a single-file unified diff for precise multi-line edits. "
                          "Preferred over file_write when a change touches a few regions.",
           .parameters = pp,
           .run = toolPatch});

  mini::Value lp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("path", strProp("Directory to list (default current dir)."));
    p.set("pattern", strProp("Optional substring to filter entry names."));
    p.set("max_entries", intProp("Maximum entries to return (default 500)."));
    return p;
  })(), {});
  reg.add({.name = "file_list",
           .description = "List directory entries (file names and dir flags).",
           .parameters = lp,
           .pure = true,
           .run = toolFileList});

  mini::Value fp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("url", strProp("URL to fetch over HTTP(S)."));
    p.set("max_chars", intProp("Max characters of returned body (default 20000)."));
    p.set("raw", boolProp("Return the raw body instead of stripped text (default false)."));
    return p;
  })(), {"url"});
  reg.add({.name = "web_fetch",
           .description = "Fetch a URL over HTTP(S) and return the page text (HTML tags, "
                          "scripts and styles stripped by default; set raw=true for the "
                          "raw body).",
           .parameters = fp,
           .pure = true,
           .run = toolWebFetch});

  mini::Value sp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("query", strProp("Search query (words or phrase)."));
    p.set("max_results", intProp("Max results to return (default 8, max 20)."));
    return p;
  })(), {"query"});
  reg.add({.name = "web_search",
           .description = "Search the web by scraping Bing (cn.bing.com and www.bing.com) "
                          "and Baidu. The reply states which engines were reachable and how "
                          "many results each actually contributed, plus a quality of "
                          "ok/partial/low/none - read it before trusting the list. Returns an "
                          "empty results array rather than unrelated pages. Use web_fetch to "
                          "read a chosen page.",
           .parameters = sp,
           .run = toolWebSearch});

  mini::Value gp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("pattern", strProp("Regex (ECMAScript) to search for, e.g. \"tool[A-Z]\\w+\". Set "
                             "fixed=true to search for this text literally."));
    p.set("path", strProp("Directory to search (default current dir)."));
    p.set("include", strProp("Optional glob restricting files, e.g. \"**/*.cpp\" or \"*.h\"."));
    p.set("fixed", boolProp("Treat pattern as a literal string, not a regex (default false). "
                            "Use whenever the text contains + . ( ) [ ] or similar."));
    p.set("ignore_case", boolProp("Case-insensitive match (default false)."));
    p.set("context_lines", intProp("Context lines around each hit, 0-3 (default 0)."));
    p.set("max_results", intProp("Max hits to return (default 100)."));
    return p;
  })(), {"pattern"});
  reg.add({.name = "grep",
           .description = "Search file contents inside a directory tree. Long lines are "
                          "matched in full but reported truncated (text_truncated). Skips "
                          "binaries, hidden dirs (.git, .idea...) and build outputs; every "
                          "exclusion it applied is reported as scanned_files / skipped_* so "
                          "that zero hits is never mistaken for 'does not exist'. Use glob "
                          "first to find files, then grep to zero in.",
           .parameters = gp,
           .run = toolGrep});

  mini::Value lbp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("pattern", strProp("Glob pattern, e.g. \"src/**/*.cpp\" or \"*.md\" (matches at any "
                             "depth). Supports * ? and **."));
    p.set("path", strProp("Directory to search (default current dir)."));
    p.set("max_entries", intProp("Max files to return (default 500)."));
    return p;
  })(), {"pattern"});
  reg.add({.name = "glob",
           .description = "Find files by path pattern inside a directory tree (same "
                          "directory exclusions as grep).",
           .parameters = lbp,
           .run = toolGlob});

  mini::Value ri = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("path", strProp("Directory to index (default current dir)."));
    p.set("index_file", strProp("Where to persist the index (default out/rag_index.json)."));
    return p;
  })(), {});
  reg.add({.name = "rag_index",
           .description = "Build a BM25 inverted index over a directory tree (same "
                          "exclusions as grep) and persist it. The agent can then "
                          "semantic-search its own codebase/docs with rag_search.",
           .parameters = ri,
           .run = toolRagIndex});

  mini::Value rq = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("query", strProp("Search words; English tokens match word-stem runs, CJK "
                           "characters match individually."));
    p.set("path", strProp("Directory to index if none is loaded (default current dir). "
                          "Already-indexed trees are auto-refreshed when files change."));
    p.set("top_k", intProp("Max segments to return (1-20, default 5)."));
    p.set("index_file", strProp("Index file to load/refresh (default out/rag_index.json)."));
    return p;
  })(), {"query"});
  reg.add({.name = "rag_search",
           .description = "Ranked full-text search (BM25) over the indexed directories. "
                          "Use it for knowledge lookup across the whole project: which "
                          "file mentions a class/API/behavior, prior fixes, docs. Use "
                          "grep for exact pattern hits, rag_search for broad recall.",
           .parameters = rq,
           .run = toolRagSearch});

  mini::Value aq = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("question", strProp("The question to ask the user; shown in the CLI."));
    return p;
  })(), {"question"});
  reg.add({.name = "ask_user",
           .description = "Ask the human a question and wait for their reply. Use it for "
                          "decisions, confirmations or facts only the user can provide.",
           .parameters = aq,
           .run = toolAskUser});

  mini::Value sa = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("description", strProp("Short task label (3-5 words)."));
    p.set("prompt",
          strProp("Detailed task description. State the goal, constraints and what to "
                  "return. The subagent runs with a fresh context and its own tools."));
    return p;
  })(), {"prompt"});
  reg.add({.name = "subagent",
           .description = "Delegate a task to a sub-agent with an isolated context. Use it "
                          "for complex multi-step work (research, refactors) whose details "
                          "would clutter the main thread. Returns the subagent's final "
                          "answer. Not available inside a subagent.",
           .parameters = sa,
           .run = toolSubagent});

  mini::Value spa = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("tasks",
          arrProp("Array of {description, prompt} work items to run in parallel "
                  "subagents, each with an isolated context."));
    p.set("max_parallel",
          intProp("Max concurrent subagents, 1-12 (default 4)."));
    return p;
  })(), {"tasks"});
  reg.add({.name = "subagent_parallel",
           .description = "Run up to max_parallel independent subagents "
                          "concurrently, each with an isolated context, and "
                          "return their answers together. Use it to split "
                          "independent research/refactor work across several "
                          "contexts at once.",
           .parameters = spa,
           .run = toolSubagentParallel});

  // --- git ---------------------------------------------------------------
  mini::Value gdir = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("dir", strProp("Working tree/directory to run git in (default: current dir)."));
    return p;
  })(), {});
  reg.add({.name = "git_status",
           .description = "Show the working tree status (short + branch). Use it before and "
                          "after changes to review what was modified.",
           .parameters = gdir,
           .run = toolGitStatus});
  mini::Value gdiff = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("dir", strProp("Working directory (default: current dir)."));
    p.set("staged", boolProp("Show staged changes instead of unstaged (default false)."));
    p.set("file", strProp("Restrict to one path (optional)."));
    return p;
  })(), {});
  reg.add({.name = "git_diff",
           .description = "Show line-level changes: unstaged by default, or staged with "
                          "staged=true, or a single file with file=.",
           .parameters = gdiff,
           .run = toolGitDiff});
  mini::Value glog = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("dir", strProp("Working directory (default: current dir)."));
    p.set("n", intProp("Number of recent commits to show (1-100, default 20)."));
    return p;
  })(), {});
  reg.add({.name = "git_log",
           .description = "List recent commits oneline (hash + subject) for review.",
           .parameters = glog,
           .run = toolGitLog});
  mini::Value gcm = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("dir", strProp("Working directory (default: current dir)."));
    p.set("message", strProp("Commit message (required)."));
    p.set("add_all", boolProp("git add -A before committing (default true)."));
    return p;
  })(), {"message"});
  reg.add({.name = "git_commit",
           .description = "Stage all changes (unless add_all=false) and create a commit with "
                          "the given message. Revert/skip if there is nothing to commit.",
           .parameters = gcm,
           .run = toolGitCommit});

  mini::Value tw = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    mini::Value arr = mini::Value::makeObject();
    arr.set("type", mini::Value::makeString("array"));
    mini::Value items = mini::Value::makeObject();
    items.set("type", mini::Value::makeString("object"));
    mini::Value ip = mini::Value::makeObject();
    ip.set("content", strProp("What to do."));
    ip.set("status", strProp("pending, in_progress, completed or cancelled (default pending)."));
    ip.set("priority", strProp("high, medium or low (optional)."));
    items.set("properties", std::move(ip));
    mini::Value reqArr = mini::Value::makeArray();
    reqArr.arr.push_back(mini::Value::makeString("content"));
    items.set("required", std::move(reqArr));
    arr.set("items", std::move(items));
    p.set("todos", std::move(arr));
    return p;
  })(), {});
  reg.add({.name = "todowrite",
           .description = "Maintain the multi-step task plan (shown to the user). Pass the "
                          "entire updated list; it replaces the previous one. Call it when "
                          "starting or progressing a multi-step task. Empty array clears.",
           .parameters = tw,
           .run = toolTodoWrite});

#ifdef _WIN32
  // --- headless browser ---------------------------------------------------
  mini::Value bopen = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("url", strProp("URL to navigate to (http/https/file/data...)."));
    p.set("wait_ms", intProp("Max ms to wait for page load (default 30000)."));
    return p;
  })(), {"url"});
  reg.add({.name = "browser_open",
           .description = "Open a URL in the headless browser (starts Chrome/Edge on first "
                          "use) and wait for it to load. Use browser_snapshot to inspect.",
           .parameters = bopen,
           .run = toolBrowserOpen});

  mini::Value bclick = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("selector", strProp("CSS selector of the element to click (preferred)."));
    p.set("x", intProp("Optional viewport x coordinate instead of a selector."));
    p.set("y", intProp("Optional viewport y coordinate instead of a selector."));
    p.set("button", strProp("left (default), right or middle."));
    p.set("click_count", intProp("1 (default) or 2 for a double click."));
    return p;
  })(), {});
  reg.add({.name = "browser_click",
           .description = "Click an element (by CSS selector) or at x/y viewport "
                          "coordinates; the element is scrolled into view first.",
           .parameters = bclick,
           .run = toolBrowserClick});

  mini::Value btype = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("text", strProp("Text to type (Unicode safe, typed as a human would)."));
    p.set("selector", strProp("Optional CSS selector; the element is clicked first to focus."));
    return p;
  })(), {"text"});
  reg.add({.name = "browser_type",
           .description = "Type text into the focused element (or into a selector).",
           .parameters = btype,
           .run = toolBrowserType});

  mini::Value bkey = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("key", strProp("Key to press: enter, tab, space, backspace, delete, escape, arrows, "
                         "home/end/pageup/pagedown, F1-F12, letters/digits, or combos like "
                         "'ctrl+a', 'ctrl+shift+t'."));
    return p;
  })(), {"key"});
  reg.add({.name = "browser_key",
           .description = "Press a keyboard key (with optional ctrl/alt/shift modifiers).",
           .parameters = bkey,
           .run = toolBrowserKey});

  mini::Value bscroll = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("direction", strProp("up, down, left, right (wheel), or top/bottom (jump)."));
    p.set("amount", intProp("Wheel delta in pixels (default 300)."));
    return p;
  })(), {"direction"});
  reg.add({.name = "browser_scroll",
           .description = "Scroll the page with the mouse wheel or jump to top/bottom.",
           .parameters = bscroll,
           .run = toolBrowserScroll});

  mini::Value bsnap = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("max_chars", intProp("Max snapshot characters (default 12000)."));
    p.set("max_lines", intProp("Max outline lines (default 400)."));
    return p;
  })(), {});
  reg.add({.name = "browser_snapshot",
           .description = "Compact text outline of the current page (elements, links, inputs "
                          "with coordinates and values). Read this to see the page.",
           .parameters = bsnap,
           .run = toolBrowserSnapshot});

  mini::Value beval = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("expression", strProp("JavaScript expression to run in the page (returnByValue)."));
    p.set("await_promise", boolProp("Await promises before returning (default true)."));
    return p;
  })(), {"expression"});
  reg.add({.name = "browser_eval",
           .description = "Run arbitrary JavaScript in the page and return its value.",
           .parameters = beval,
           .run = toolBrowserEval});

  mini::Value bwait = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("ms", intProp("Milliseconds to wait (default 800, max 300000)."));
    return p;
  })(), {});
  reg.add({.name = "browser_wait",
           .description = "Pause briefly (e.g. for animations or network settle).",
           .parameters = bwait,
           .run = toolBrowserWait});

  mini::Value bhist = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    return p;
  })(), {});
  reg.add({.name = "browser_back",
           .description = "Go to the previous page in history.",
           .parameters = bhist,
           .run = toolBrowserBack});
  reg.add({.name = "browser_forward",
           .description = "Go to the next page in history.",
           .parameters = bhist,
           .run = toolBrowserForward});

  mini::Value brel = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    return p;
  })(), {});
  reg.add({.name = "browser_reload",
           .description = "Reload the current page.",
           .parameters = brel,
           .run = toolBrowserReload});

  mini::Value bshot = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("path", strProp("Where to save the PNG (default browser_screenshots/shot_<ts>.png)."));
    p.set("full_page", boolProp("Capture the full scrollable page (default false)."));
    return p;
  })(), {});
  reg.add({.name = "browser_screenshot",
           .description = "Save a PNG screenshot of the current viewport (or full page).",
           .parameters = bshot,
           .run = toolBrowserScreenshot});

  mini::Value bup = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("selector", strProp("CSS selector of the <input type=file> element."));
    p.set("file", strProp("Path of the local file to attach."));
    return p;
  })(), {"selector", "file"});
  reg.add({.name = "browser_upload",
           .description = "Attach a local file to a file input (like a human picker).",
           .parameters = bup,
           .run = toolBrowserUpload});

  mini::Value bclose = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    return p;
  })(), {});
  reg.add({.name = "browser_close",
           .description = "Shut down the headless browser and delete its profile.",
           .parameters = bclose,
           .run = toolBrowserClose});

  // --- desktop accessibility (Windows UI Automation) ----------------------
  // The model reads the a11y tree as text (no vision) and drives the UI by
  // element path/name. UI Automation has no portable backend, so these tools
  // are not registered at all on Linux/macOS.
  mini::Value dw = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("filter", strProp("Optional title substring to narrow the list."));
    return p;
  })(), {});
  reg.add({.name = "desktop_windows",
           .description = "List the open desktop windows with their hwnd, title, role and "
                          "rect. Use this to find the window you want to operate.",
           .parameters = dw,
           .run = toolDesktopWindows});

  mini::Value dt = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("window", strProp("Target window: a hwnd, a title substring, or 'focused'/'root' "
                            "(default 'focused')."));
    p.set("path", strProp("Optional element path from a previous desktop_tree ('0/2/1'); "
                          "dumps just that subtree."));
    p.set("name", strProp("Optional element name substring; dumps the matching subtree."));
    p.set("role", strProp("Optional control role (button/edit/window/...)."));
    p.set("automation_id", strProp("Optional automation id substring."));
    p.set("max_depth", intProp("Optional max tree depth (default 12)."));
    p.set("max_nodes", intProp("Optional max elements (default 300)."));
    p.set("max_chars", intProp("Optional max characters (default 12000)."));
    return p;
  })(), {});
  reg.add({.name = "desktop_tree",
           .description = "Dump the accessibility tree of a window as text: one line per "
                          "element with an index path like '0/2/1', role, name, value and "
                          "rect. Read this to see what is on screen without vision.",
           .parameters = dt,
           .run = toolDesktopTree});

  mini::Value dc = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("window", strProp("Target window (hwnd/title/focused/root). Default 'focused'."));
    p.set("path", strProp("Element path from desktop_tree, e.g. '0/2/1'."));
    p.set("name", strProp("Element name substring to click."));
    p.set("role", strProp("Element role to disambiguate the name search."));
    p.set("automation_id", strProp("Automation id substring to click."));
    p.set("x", intProp("Optional absolute screen x to click (raw coordinate click)."));
    p.set("y", intProp("Optional absolute screen y to click (raw coordinate click)."));
    p.set("button", strProp("left (default) or right."));
    p.set("click_count", intProp("1 (default) or 2 for double-click."));
    return p;
  })(), {});
  reg.add({.name = "desktop_click",
           .description = "Click an element found by path/name/role/automation_id, or click "
                          "at raw screen coordinates. Uses InvokePattern/SelectionItemPattern "
                          "when available, else a real mouse click at the element center.",
           .parameters = dc,
           .run = toolDesktopClick});

  mini::Value ty = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("text", strProp("Text to type (Unicode-safe)."));
    p.set("window", strProp("Target window (hwnd/title/focused/root). Default 'focused'."));
    p.set("path", strProp("Element path from desktop_tree, e.g. '0/2/1'."));
    p.set("name", strProp("Element name substring to type into."));
    p.set("clear", boolProp("Optional. Clear existing text first via the value pattern."));
    return p;
  })(), {"text"});
  reg.add({.name = "desktop_type",
           .description = "Type text into an element (value pattern when supported, else "
                          "focused element via simulated keystrokes).",
           .parameters = ty,
           .run = toolDesktopType});

  mini::Value kk = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("keys", strProp("Key or combo to send: 'enter', 'tab', 'ctrl+s', 'alt+f4', "
                          "'ctrl+shift+t', 'f5', arrows, letters, digits."));
    p.set("window", strProp("Optional window to focus first (hwnd/title/focused/root). "
                            "Default: keys go to the current focus."));
    return p;
  })(), {"keys"});
  reg.add({.name = "desktop_key",
           .description = "Send keyboard keys/chords to the desktop (modifiers + main key).",
           .parameters = kk,
           .run = toolDesktopKey});

  mini::Value sc = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("window", strProp("Target window (hwnd/title/focused/root). Default 'focused'."));
    p.set("path", strProp("Element path from desktop_tree, e.g. '0/2/1'."));
    p.set("name", strProp("Element name substring to scroll."));
    p.set("direction", strProp("up, down, left or right (default down)."));
    p.set("amount", intProp("Number of scroll units (default 1)."));
    return p;
  })(), {});
  reg.add({.name = "desktop_scroll",
           .description = "Scroll a scrollable element (scroll pattern when supported).",
           .parameters = sc,
           .run = toolDesktopScroll});

  mini::Value wt = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("window", strProp("Target window (hwnd/title/focused/root). Default 'focused'."));
    p.set("name", strProp("Element name substring to wait for."));
    p.set("role", strProp("Optional role to disambiguate."));
    p.set("timeout_ms", intProp("Optional timeout in ms (default 10000)."));
    return p;
  })(), {});
  reg.add({.name = "desktop_wait",
           .description = "Poll the window until an element matching name/role appears. "
                          "Use after clicking to wait for UI changes.",
           .parameters = wt,
           .run = toolDesktopWait});
#endif  // _WIN32: desktop_* need UI Automation

  mini::Value rcp = objSchema(([] {
    mini::Value p = mini::Value::makeObject();
    p.set("steps",
          arrProp("Array of steps: {\"tool\",\"arguments\",\"var\"?,\"return\"?}. A string "
                  "argument equal to \"$var\" or containing \"{$var}\" is replaced with the "
                  "value stored under that var by an earlier step."));
    p.set("parallel",
          boolProp("Optional. true = run all steps concurrently ($var references are "
                   "unavailable then)."));
    p.set("max_parallel", intProp("Optional max threads for parallel mode (default 4)."));
    p.set("result_cap", intProp("Optional per-step result byte cap (default 8192)."));
    return p;
  })(), {"steps"});
  reg.add({.name = "run_code",
           .description = "Execute a short JSON program that batches multiple tool calls "
                          "into ONE invocation. Program: {\"steps\":[{tool,arguments,var,"
                          "return}...], \"parallel\"?, \"max_parallel\"?, \"result_cap\"?}. "
                          "Steps run in order; a string argument equal to \"$var\" or "
                          "containing \"{$var}\" is replaced by the value saved under that "
                          "var by an earlier step. Returns the last step's result (or the "
                          "step flagged return:true) plus a compact per-step summary. Use "
                          "this instead of issuing many separate tool calls.",
           .parameters = rcp,
           .run = [&reg](std::string const& a) { return toolRunCode(reg, a); }});
}

}  // namespace agent
