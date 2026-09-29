#include "agent/plugins.hpp"
#include "minijson.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#endif

namespace agent {

namespace fs = std::filesystem;

namespace {

std::string errStr(std::string const& m) {
  mini::Value e = mini::Value::makeObject();
  e.set("error", mini::Value::makeString(m));
  return mini::dump(e);
}

std::string miniStr(mini::Value const* v) { return v ? v->asString() : ""; }

std::string lowerAscii(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

}  // namespace

struct PluginProcess::Impl {
#ifdef _WIN32
  HANDLE hChild = INVALID_HANDLE_VALUE;
  HANDLE hInWrite = INVALID_HANDLE_VALUE;   // we write -> child stdin
  HANDLE hOutRead = INVALID_HANDLE_VALUE;   // we read  <- child stdout
#else
  pid_t pid = -1;
  int inFd = -1;
  int outFd = -1;
#endif
  std::mutex mu;
  std::atomic<bool> alive{false};
  uint64_t nextId = 1;

  bool writeLine(std::string const& line) {
#ifdef _WIN32
    std::string data = line + "\n";
    DWORD written = 0;
    return WriteFile(hInWrite, data.data(), (DWORD)data.size(), &written, nullptr) &&
           written == data.size();
#else
    std::string data = line + "\n";
    ssize_t n = ::write(inFd, data.data(), data.size());
    return n == (ssize_t)data.size();
#endif
  }

  // Reads one line from the child, up to timeoutMs. Returns false on EOF.
  bool readLine(std::string& out, int timeoutMs) {
#ifdef _WIN32
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string acc;
    char b;
    DWORD n = 0;
    while (true) {
      if (!PeekNamedPipe(hOutRead, nullptr, 0, nullptr, &n, nullptr)) return false;
      if (n > 0) {
        if (!ReadFile(hOutRead, &b, 1, &n, nullptr) || n != 1) return false;
        if (b == '\n') { out = acc; return true; }
        acc += b;
        continue;
      }
      if (std::chrono::steady_clock::now() >= deadline) { out.clear(); return false; }
      if (!alive) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
#else
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string acc;
    char b;
    while (true) {
      ssize_t n = ::read(outFd, &b, 1);
      if (n == 1) {
        if (b == '\n') { out = acc; return true; }
        acc += b;
        continue;
      }
      if (n == 0) { alive = false; return false; }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (std::chrono::steady_clock::now() >= deadline) { out.clear(); return false; }
        if (!alive) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }
      return false;  // 其它错误别再空转到 deadline
    }
#endif
  }

  // Spawns the child with optional extra environment variables. On Windows,
  // .cmd/.bat wrappers (e.g. npx) are run through cmd.exe; the extra env is
  // applied to the child only and restored afterwards.
  bool spawn(std::string const& exePath, std::vector<std::string> const& args,
             std::vector<std::pair<std::string, std::string>> const& env) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE inRead, inWrite, outRead, outWrite;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0))
      return false;
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    std::vector<std::pair<std::string, std::string>> backup;
    for (auto const& kv : env) {
      char const* old = getenv(kv.first.c_str());
      backup.emplace_back(kv.first, old ? std::string(old) : std::string());
      _putenv_s(kv.first.c_str(), kv.second.c_str());
    }

    std::string base = lowerAscii(fs::path(exePath).filename().string());
    bool needsCmd =
        (base.size() > 4 &&
         (base.substr(base.size() - 4) == ".cmd" || base.substr(base.size() - 4) == ".bat")) ||
        base == "npx" || base == "npx.exe" || base == "npx.cmd" || base == "npx.bat";
    std::string cmd;
    if (needsCmd) {
      cmd = "/C \"" + exePath + "\"";
      for (auto const& a : args) cmd += " \"" + a + "\"";
    } else {
      cmd = "\"" + exePath + "\"";
      for (auto const& a : args) cmd += " \"" + a + "\"";
    }
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    std::memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(needsCmd ? "cmd.exe" : nullptr, cmdBuf.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    for (auto const& kv : backup) _putenv_s(kv.first.c_str(), kv.second.c_str());
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
      CloseHandle(inWrite);
      CloseHandle(outRead);
      return false;
    }
    CloseHandle(pi.hThread);
    hChild = pi.hProcess;
    hInWrite = inWrite;
    hOutRead = outRead;
    alive = true;
    return true;
#else
    int inP[2], outP[2];
    if (pipe(inP) != 0 || pipe(outP) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
      dup2(inP[0], 0);
      dup2(outP[1], 1);
      close(inP[0]); close(inP[1]); close(outP[0]); close(outP[1]);
      for (auto const& kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
      std::vector<std::string> argv = {exePath};
      argv.insert(argv.end(), args.begin(), args.end());
      std::vector<char*> cargv;
      for (auto const& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
      cargv.push_back(nullptr);
      execvp(exePath.c_str(), cargv.data());
      _exit(127);
    }
    close(inP[0]); close(outP[1]);
    this->pid = pid;
    inFd = inP[1];
    outFd = outP[0];
    // 读端设为非阻塞：readLine 的 timeoutMs 是靠"读到 EAGAIN 就查截止时间"
    // 实现的，阻塞 fd 会让 read() 直接睡死，超时形同不存在（子进程沉默时
    // 整个 agent 跟着挂）。
    fcntl(outFd, F_SETFL, O_NONBLOCK);
    alive = true;
    return true;
#endif
  }
};

PluginProcess::PluginProcess() = default;
PluginProcess::PluginProcess(PluginProcess&&) noexcept = default;
PluginProcess& PluginProcess::operator=(PluginProcess&&) noexcept = default;

PluginProcess::~PluginProcess() { stop(); }

std::string PluginProcess::start(std::string const& exePath,
                                 std::vector<std::string> const& args) {
  stop();
  impl_ = std::make_unique<Impl>();
  name_ = fs::path(exePath).filename().string();
  if (!impl_->spawn(exePath, args, {})) {
    impl_.reset();
    return "plugin: cannot start '" + exePath + "'";
  }

  // Plugin handshake (empty params; the plugin names itself in the result).
  std::string initReq = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{}}";
  std::string resp;
  bool writeOk = false, readOk = false;
  {
    std::lock_guard lk(impl_->mu);
    writeOk = impl_->writeLine(initReq);
    readOk = writeOk && impl_->readLine(resp, 10000);
  }
  // stop() 自己会拿 impl_->mu，必须在上面的锁释放之后再调用：非递归锁二次
  // 上锁就是永久 futex 等待。
  if (!writeOk) { stop(); return "plugin: write failed"; }
  if (!readOk) { stop(); return "plugin: no response from '" + name_ + "'"; }
  mini::Value v;
  if (!mini::tryParse(resp, v) || v.type != mini::Value::Object || v.has("error")) {
    stop();
    return "plugin: bad initialize response from '" + name_ + "'";
  }
  if (mini::Value const* r = v.get("result"))
    if (mini::Value const* nm = r->get("name")) name_ = nm->asString();
  return "";
}

std::string PluginProcess::startMcp(McpConfig const& cfg) {
  stop();
  impl_ = std::make_unique<Impl>();
  name_ = cfg.name;
  prefix_ = cfg.name + "_";
  if (!impl_->spawn(cfg.command, cfg.args, cfg.env)) {
    impl_.reset();
    return "mcp: cannot start '" + cfg.command + "'";
  }

  // MCP initialize: unlike plugins, the server requires protocol params and
  // answers with protocolVersion/capabilities/serverInfo.
  mini::Value params = mini::Value::makeObject();
  params.set("protocolVersion", mini::Value::makeString("2025-06-18"));
  params.set("capabilities", mini::Value::makeObject());
  mini::Value ci = mini::Value::makeObject();
  ci.set("name", mini::Value::makeString("ember"));
  ci.set("version", mini::Value::makeString("1.0"));
  params.set("clientInfo", std::move(ci));
  std::string resp = request("initialize", mini::dump(params), 10000);
  if (resp.empty()) {
    stop();
    return "mcp: initialize failed: " + lastError();
  }
  mini::Value v;
  if (!mini::tryParse(resp, v) || v.type != mini::Value::Object || v.has("error")) {
    stop();
    return "mcp: bad initialize response from '" + name_ + "'";
  }
  // notifications/initialized -> the server may then accept tools/list.
  bool notifyOk = false;
  {
    std::lock_guard lk(impl_->mu);
    notifyOk = impl_->writeLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
  }
  if (!notifyOk) {
    stop();  // stop() 里会把 alive 置假
    return "mcp: write failed";
  }
  return "";
}

std::string PluginProcess::registerTools(ToolRegistry& reg, std::string const& prefix) {
  mini::Value resp;
  std::string raw = request("tools/list", "{}", 10000);
  if (!mini::tryParse(raw, resp) || resp.type != mini::Value::Object || !resp.has("result")) {
    return "plugin: bad tools/list from '" + name_ + "'";
  }
  mini::Value const* r = resp.get("result");
  mini::Value const* tools = r ? r->get("tools") : nullptr;
  if (!tools || tools->type != mini::Value::Array) {
    return "plugin: tools/list without tools array from '" + name_ + "'";
  }
  int added = 0;
  for (auto const& t : tools->arr) {
    if (t.type != mini::Value::Object) continue;
    PluginSpec spec;
    spec.name = miniStr(t.get("name"));
    if (spec.name.empty()) continue;
    spec.description = miniStr(t.get("description"));
    // Plugins use "parameters"; MCP servers use "inputSchema" (JSON Schema).
    mini::Value const* params = t.get("parameters");
    if (!params || params->type != mini::Value::Object) params = t.get("inputSchema");
    if (params && params->type == mini::Value::Object) spec.parameters = *params;
    if (mini::Value const* e = t.get("enabled")) spec.enabled = e->asBool(true);
    Tool tool;
    tool.name = prefix + spec.name;
    tool.description = spec.description;
    tool.parameters = std::move(spec.parameters);
    tool.enabled = spec.enabled;
    tool.run = [this, spec](std::string const& argsJson) {
      mini::Value params = mini::Value::makeObject();
      params.set("name", mini::Value::makeString(spec.name));
      mini::Value am = mini::Value::makeObject();
      if (!mini::tryParse(argsJson, am) || am.type != mini::Value::Object) {
        return errStr("plugin tool arguments must be a JSON object");
      }
      params.set("arguments", std::move(am));
      std::string out = request("tools/call", mini::dump(params));
      mini::Value rv;
      if (!mini::tryParse(out, rv) || rv.type != mini::Value::Object) {
        return errStr("plugin call failed: " + lastError());
      }
      if (rv.has("error")) {
        mini::Value const* e = rv.get("error");
        return errStr("plugin error: " + miniStr(e ? e->get("message") : nullptr));
      }
      // Result content is either a plain string (plugin protocol) or an MCP
      // content array; MCP marks failures with isError.
      if (mini::Value const* res = rv.get("result")) {
        mini::Value const* c = res->get("content");
        bool isErr = false;
        if (mini::Value const* ie = res->get("isError")) isErr = ie->asBool(false);
        std::string text;
        if (c && c->type == mini::Value::String) {
          text = c->s;
        } else if (c && c->type == mini::Value::Array) {
          for (auto const& item : c->arr) {
            if (item.type == mini::Value::String) {
              if (!text.empty()) text += "\n";
              text += item.s;
            } else if (item.type == mini::Value::Object) {
              std::string ct = miniStr(item.get("type"));
              if (ct == "text") {
                if (!text.empty()) text += "\n";
                text += miniStr(item.get("text"));
              } else if (ct == "image") {
                std::string mt = miniStr(item.get("mimeType"));
                if (!text.empty()) text += "\n";
                text += "[image" + (mt.empty() ? "]" : " " + mt + "]");
              } else {
                if (!text.empty()) text += "\n";
                text += mini::dump(item);
              }
            }
          }
        }
        if (isErr) return errStr(text.empty() ? "mcp tool failed" : text);
        return text;
      }
      return errStr("plugin call returned no result");
    };
    reg.add(std::move(tool));
    added++;
  }
  toolCount_ = (size_t)added;
  return added == 0 ? "plugin '" + name_ + "' advertised no tools" : "";
}

std::string PluginProcess::request(std::string const& method, std::string const& paramsJson,
                                   int timeoutMs) {
  lastError_.clear();
  if (!impl_ || !impl_->alive) {
    lastError_ = "plugin process is not running";
    return "";
  }
  mini::Value req = mini::Value::makeObject();
  req.set("jsonrpc", mini::Value::makeString("2.0"));
  req.set("method", mini::Value::makeString(method));
  mini::Value id;
  id.type = mini::Value::Int;
  id.i = (int64_t)(impl_->nextId++);
  req.set("id", std::move(id));
  mini::Value pv;
  if (!mini::tryParse(paramsJson, pv) || pv.type != mini::Value::Object) {
    pv = mini::Value::makeObject();
  }
  req.set("params", std::move(pv));
  std::string line = mini::dump(req);

  std::string resp;
  {
    std::lock_guard lk(impl_->mu);
    if (!impl_->writeLine(line)) {
      impl_->alive = false;
      lastError_ = "plugin write failed";
      return "";
    }
    // Responses carry an "id"; a line with a method but no id is a server
    // notification (e.g. MCP logging) and is skipped.
    while (true) {
      std::string one;
      if (!impl_->readLine(one, timeoutMs)) {
        if (impl_->alive) lastError_ = "plugin request timed out";
        else lastError_ = "plugin process exited";
        return "";
      }
      mini::Value v;
      if (!mini::tryParse(one, v) || v.type != mini::Value::Object) {
        lastError_ = "plugin returned non-JSON";
        return "";
      }
      if (v.has("method") && !v.has("id")) continue;  // notification
      resp = one;
      break;
    }
  }
  return resp;
}

void PluginProcess::stop() {
  if (!impl_) return;
  std::lock_guard lk(impl_->mu);
  impl_->alive = false;
#ifdef _WIN32
  if (impl_->hChild != INVALID_HANDLE_VALUE) {
    TerminateProcess(impl_->hChild, 1);
    WaitForSingleObject(impl_->hChild, 2000);
    CloseHandle(impl_->hChild);
  }
  if (impl_->hInWrite != INVALID_HANDLE_VALUE) CloseHandle(impl_->hInWrite);
  if (impl_->hOutRead != INVALID_HANDLE_VALUE) CloseHandle(impl_->hOutRead);
#else
  if (impl_->pid > 0) {
    kill(impl_->pid, SIGTERM);
    int st = 0;
    waitpid(impl_->pid, &st, 0);
  }
  if (impl_->inFd >= 0) close(impl_->inFd);
  if (impl_->outFd >= 0) close(impl_->outFd);
#endif
}

bool PluginProcess::running() const { return impl_ && impl_->alive; }

std::string startPluginsFromDir(std::string const& dir, ToolRegistry& reg,
                                std::vector<std::unique_ptr<PluginProcess>>& out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return "plugin dir not found: " + dir;
  std::vector<fs::path> exes;
  for (auto const& e : fs::directory_iterator(dir, ec)) {
#ifdef _WIN32
    if (e.is_regular_file(ec) &&
        e.path().extension().string() == ".exe")
      exes.push_back(e.path());
#else
    if (e.is_regular_file(ec) && (e.path().filename().string()[0] != '.')) {
      std::error_code xec;
      if ((fs::status(e.path(), xec).permissions() & fs::perms::owner_exec) != fs::perms::none)
        exes.push_back(e.path());
    }
#endif
  }
  std::sort(exes.begin(), exes.end());
  for (auto const& exe : exes) {
    auto p = std::make_unique<PluginProcess>();
    std::string e = p->start(exe.string());
    if (!e.empty()) continue;  // not a plugin; skip silently
    e = p->registerTools(reg);
    if (e.empty()) out.push_back(std::move(p));
  }
  return "";
}

}  // namespace agent