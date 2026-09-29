#include "agent/background.hpp"
#include "minijson.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace agent {

namespace {

constexpr size_t kMaxBuffered = 256 * 1024;  // per-task retained output
constexpr size_t kMaxStatusOutput = 6000;    // chars handed out per poll
constexpr size_t kMaxNotifyTail = 2000;      // chars in completion notices

struct Task {
  std::string id;
  std::string command;
  bool running = true;
  bool killed = false;      // manual kill OR timeout
  bool manualKill = false;  // set by kill() only
  bool timedOut = false;
  int exitCode = -1;
  long timeoutMs = 0;
  std::mutex m;
  std::string output;   // retained output (trimmed to kMaxBuffered)
  size_t consumed = 0;  // bytes of `output` already returned to the model
  std::thread reader;
  int64_t startedMs = 0;
#ifdef _WIN32
  HANDLE process = nullptr;
  HANDLE readPipe = nullptr;
#else
  pid_t pid = -1;
  int readFd = -1;
#endif
};

int64_t nowMs() {
#ifdef _WIN32
  return (int64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void appendChunk(Task& t, std::string const& s) {
  t.output += s;
  if (t.output.size() > kMaxBuffered) {
    size_t drop = t.output.size() - kMaxBuffered;
    t.output.erase(0, drop);
    t.consumed = t.consumed >= drop ? t.consumed - drop : 0;
  }
}

std::string joinTail(Task const& t, size_t max) {
  if (t.output.size() <= max) return t.output;
  return t.output.substr(t.output.size() - max);
}

#ifdef _WIN32

bool spawnTask(std::shared_ptr<Task> const& t) {
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;
  HANDLE rp = nullptr, wp = nullptr;
  if (!CreatePipe(&rp, &wp, &sa, 0)) return false;
  if (!SetHandleInformation(rp, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(rp);
    CloseHandle(wp);
    return false;
  }
  HANDLE nul =
      CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                  OPEN_EXISTING, 0, nullptr);
  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = wp;
  si.hStdError = wp;
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  std::string cmdline = "cmd /C " + t->command;  // CreateProcessA wants a writable buffer
  BOOL ok = CreateProcessA(nullptr, &cmdline[0], nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wp);
  if (nul) CloseHandle(nul);
  if (!ok) {
    CloseHandle(rp);
    return false;
  }
  CloseHandle(pi.hThread);
  t->process = pi.hProcess;
  t->readPipe = rp;
  return true;
}

void readerLoop(std::shared_ptr<Task> const& t,
                std::function<void(int, bool, std::string const&)> done) {
  int64_t t0 = nowMs();
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(t->readPipe, nullptr, 0, nullptr, &avail, nullptr) &&
        avail > 0) {
      char buf[4096];
      DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
      DWORD got = 0;
      if (ReadFile(t->readPipe, buf, want, &got, nullptr) && got > 0) {
        std::lock_guard lk(t->m);
        appendChunk(*t, std::string(buf, got));
      }
    }
    DWORD w = WaitForSingleObject(t->process, 100);
    if (w == WAIT_OBJECT_0) break;
    bool timedOut = false;
    {
      std::lock_guard lk(t->m);
      if (!t->killed && t->timeoutMs > 0 &&
          nowMs() - t0 > (int64_t)t->timeoutMs) {
        t->killed = true;
        t->timedOut = true;
        timedOut = true;
        TerminateProcess(t->process, 1);
      }
    }
    if (timedOut) {
      WaitForSingleObject(t->process, 3000);
      break;
    }
  }
  for (;;) {  // drain anything buffered after exit
    DWORD a = 0;
    if (!PeekNamedPipe(t->readPipe, nullptr, 0, nullptr, &a, nullptr) || a == 0)
      break;
    char buf[4096];
    DWORD want = a < sizeof(buf) ? a : sizeof(buf);
    DWORD got = 0;
    if (!ReadFile(t->readPipe, buf, want, &got, nullptr) || got == 0) break;
    std::lock_guard lk(t->m);
    appendChunk(*t, std::string(buf, got));
  }
  DWORD code = 0;
  GetExitCodeProcess(t->process, &code);
  CloseHandle(t->process);
  CloseHandle(t->readPipe);
  t->process = nullptr;
  t->readPipe = nullptr;
  bool manualKill = false, timedOut = false;
  std::string tail;
  {
    std::lock_guard lk(t->m);
    t->exitCode = (int)code;
    t->running = false;
    manualKill = t->manualKill;
    timedOut = t->timedOut;
    tail = joinTail(*t, kMaxNotifyTail);
  }
  if (!manualKill && done) done((int)code, timedOut, tail);
}

#else  // POSIX

bool spawnTask(std::shared_ptr<Task> const& t) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    execl("/bin/sh", "sh", "-c", t->command.c_str(), (char*)nullptr);
    _exit(127);
  }
  close(fds[1]);
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  t->pid = pid;
  t->readFd = fds[0];
  return true;
}

void readerLoop(std::shared_ptr<Task> const& t,
                std::function<void(int, bool, std::string const&)> done) {
  int64_t t0 = nowMs();
  bool finished = false;
  bool reaped = false;
  int reapedStatus = 0;
  while (!finished) {
    char buf[4096];
    ssize_t n = read(t->readFd, buf, sizeof buf);
    if (n > 0) {
      std::lock_guard lk(t->m);
      appendChunk(*t, std::string(buf, (size_t)n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct timespec ts = {0, 100 * 1000000};
      nanosleep(&ts, nullptr);
      int st = 0;
      if (!reaped && waitpid(t->pid, &st, WNOHANG) == t->pid) {
        reaped = true;
        reapedStatus = st;
        // 子进程已经退出，但它写进管道的内容可能还没读完：非阻塞 read
        // 到没数据为止。少这一步就会把最后一段输出（常见就是一行 echo）
        // 连同 readFd 一起 close 掉。
        for (;;) {
          ssize_t m = read(t->readFd, buf, sizeof buf);
          if (m <= 0) break;
          std::lock_guard lk(t->m);
          appendChunk(*t, std::string(buf, (size_t)m));
        }
        finished = true;
      }
      bool timedOut = false;
      {
        std::lock_guard lk(t->m);
        if (!t->killed && t->timeoutMs > 0 &&
            nowMs() - t0 > (int64_t)t->timeoutMs) {
          t->killed = true;
          t->timedOut = true;
          timedOut = true;
          if (!reaped) ::kill(t->pid, SIGKILL);
        }
      }
      if (timedOut) {
        if (!reaped) {
          int st2 = 0;
          if (waitpid(t->pid, &st2, 0) < 0) st2 = 0;
          reaped = true;
          reapedStatus = st2;
        }
        finished = true;
      }
      continue;
    }
    if (n == 0) finished = true;  // EOF
  }
  int st = reapedStatus;
  if (!reaped && waitpid(t->pid, &st, 0) < 0) st = 0;
  int code = WIFEXITED(st) ? WEXITSTATUS(st)
                           : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1);
  close(t->readFd);
  t->readFd = -1;
  bool manualKill = false, timedOut = false;
  std::string tail;
  {
    std::lock_guard lk(t->m);
    t->exitCode = code;
    t->running = false;
    manualKill = t->manualKill;
    timedOut = t->timedOut;
    tail = joinTail(*t, kMaxNotifyTail);
  }
  if (!manualKill && done) done(code, timedOut, tail);
}

#endif

}  // namespace

struct BackgroundRunner::Impl {
  std::mutex mu;
  std::vector<std::shared_ptr<Task>> tasks;
  std::function<void(std::string const&, int, bool, std::string const&)>
      notifier;
  int nextId = 1;

  std::shared_ptr<Task> find(std::string const& id) {
    std::lock_guard lk(mu);
    for (auto const& t : tasks)
      if (t->id == id) return t;
    return nullptr;
  }

  void onFinished(std::string const& id, int exitCode, bool timedOut,
                  std::string const& tail) {
    std::function<void(std::string const&, int, bool, std::string const&)> fn;
    {
      std::lock_guard lk(mu);
      fn = notifier;
    }
    if (fn) fn(id, exitCode, timedOut, tail);
  }
};

BackgroundRunner::BackgroundRunner() : impl_(new Impl) {}
BackgroundRunner::~BackgroundRunner() {
  shutdown();
  delete impl_;
}

std::string BackgroundRunner::start(std::string const& command, long timeoutMs) {
  std::shared_ptr<Task> t = std::make_shared<Task>();
  t->command = command;
  t->timeoutMs = timeoutMs;
  {
    std::lock_guard lk(impl_->mu);
    for (auto it = impl_->tasks.begin(); it != impl_->tasks.end();) {
      if (!(*it)->running) {  // prune finished tasks
        if ((*it)->reader.joinable()) (*it)->reader.join();
        it = impl_->tasks.erase(it);
      } else {
        ++it;
      }
    }
    t->id = "b" + std::to_string(impl_->nextId++);
    impl_->tasks.push_back(t);
  }
  t->startedMs = nowMs();
  if (!spawnTask(t)) {
    std::lock_guard lk(impl_->mu);
    for (auto it = impl_->tasks.begin(); it != impl_->tasks.end(); ++it)
      if (*it == t) {
        impl_->tasks.erase(it);
        break;
      }
    return "{\"error\":\"failed to start background command\"}";
  }
  BackgroundRunner* self = this;
  t->reader = std::thread([self, t]() {
    readerLoop(t, [self, t](int code, bool timedOut, std::string const& tail) {
      self->impl_->onFinished(t->id, code, timedOut, tail);
    });
  });
  mini::Value o = mini::Value::makeObject();
  o.set("id", mini::Value::makeString(t->id));
  o.set("command", mini::Value::makeString(command));
  o.set("running", mini::Value::makeBool(true));
  return mini::dump(o);
}

std::string BackgroundRunner::status(std::string const& id) {
  auto t = impl_->find(id);
  if (!t) return "{\"error\":\"unknown background task '" + id + "'\"}";
  std::string newOut;
  {
    std::lock_guard lk(t->m);
    if (t->consumed < t->output.size()) {
      newOut = t->output.substr(t->consumed);
      t->consumed = t->output.size();
    }
    if (newOut.size() > kMaxStatusOutput) newOut = newOut.substr(newOut.size() - kMaxStatusOutput);
  }
  mini::Value o = mini::Value::makeObject();
  o.set("id", mini::Value::makeString(id));
  o.set("command", mini::Value::makeString(t->command));
  o.set("running", mini::Value::makeBool(t->running));
  o.set("exit_code", mini::Value::makeInt(t->exitCode));
  o.set("killed", mini::Value::makeBool(t->killed));
  o.set("timed_out", mini::Value::makeBool(t->timedOut));
  o.set("output", mini::Value::makeString(newOut));
  return mini::dump(o);
}

std::string BackgroundRunner::kill(std::string const& id) {
  auto t = impl_->find(id);
  if (!t) return "{\"error\":\"unknown background task '" + id + "'\"}";
  bool wasRunning = false;
  {
    std::lock_guard lk(t->m);
    wasRunning = t->running;
    t->killed = true;
    t->manualKill = true;
  }
  if (wasRunning) {
#ifdef _WIN32
    if (t->process) TerminateProcess(t->process, 1);
#else
    if (t->pid > 0) ::kill(t->pid, SIGKILL);
#endif
  }
  mini::Value o = mini::Value::makeObject();
  o.set("id", mini::Value::makeString(id));
  o.set("killed", mini::Value::makeBool(wasRunning));
  return mini::dump(o);
}

std::string BackgroundRunner::list() {
  std::vector<std::shared_ptr<Task>> tasks;
  {
    std::lock_guard lk(impl_->mu);
    tasks = impl_->tasks;
  }
  mini::Value arr = mini::Value::makeArray();
  for (auto const& t : tasks) {
    std::string tail;
    {
      std::lock_guard lk(t->m);
      tail = joinTail(*t, 300);
    }
    mini::Value o = mini::Value::makeObject();
    o.set("id", mini::Value::makeString(t->id));
    o.set("command", mini::Value::makeString(t->command));
    o.set("running", mini::Value::makeBool(t->running));
    o.set("exit_code", mini::Value::makeInt(t->exitCode));
    o.set("timed_out", mini::Value::makeBool(t->timedOut));
    o.set("output_len", mini::Value::makeInt((int64_t)t->output.size()));
    o.set("output_tail", mini::Value::makeString(tail));
    arr.arr.push_back(std::move(o));
  }
  return mini::dump(arr);
}

void BackgroundRunner::setNotifier(
    std::function<void(std::string const&, int, bool, std::string const&)> fn) {
  std::lock_guard lk(impl_->mu);
  impl_->notifier = std::move(fn);
}

void BackgroundRunner::shutdown() {
  std::vector<std::shared_ptr<Task>> tasks;
  {
    std::lock_guard lk(impl_->mu);
    tasks = impl_->tasks;
    impl_->tasks.clear();
    impl_->notifier = nullptr;
  }
  for (auto const& t : tasks) {
    {
      std::lock_guard lk(t->m);
      if (t->running) t->killed = t->manualKill = true;
    }
#ifdef _WIN32
    if (t->process) TerminateProcess(t->process, 1);
#else
    if (t->pid > 0) ::kill(t->pid, SIGKILL);
#endif
    if (t->reader.joinable()) t->reader.join();
  }
}

size_t BackgroundRunner::taskCount() const {
  std::lock_guard lk(impl_->mu);
  return impl_->tasks.size();
}

BackgroundRunner& backgroundRunner() {
  static BackgroundRunner runner;
  return runner;
}

// ---------------------------------------------------------------------------
// Tool entry points
// ---------------------------------------------------------------------------

std::string toolBackgroundStart(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) {
    return "{\"error\":\"arguments must be a JSON object\"}";
  }
  std::string cmd = args.has("command") ? args.get("command")->asString() : "";
  if (cmd.empty()) return "{\"error\":\"missing 'command' string\"}";
  long timeoutMs = args.has("timeout_ms") ? (long)args.get("timeout_ms")->asInt(0) : 0;
  return backgroundRunner().start(cmd, timeoutMs);
}

std::string toolBackgroundStatus(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) {
    return "{\"error\":\"arguments must be a JSON object\"}";
  }
  std::string id = args.has("id") ? args.get("id")->asString() : "";
  if (id.empty()) return "{\"error\":\"missing 'id' string\"}";
  return backgroundRunner().status(id);
}

std::string toolBackgroundKill(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) {
    return "{\"error\":\"arguments must be a JSON object\"}";
  }
  std::string id = args.has("id") ? args.get("id")->asString() : "";
  if (id.empty()) return "{\"error\":\"missing 'id' string\"}";
  return backgroundRunner().kill(id);
}

std::string toolBackgroundList(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) {
    return "{\"error\":\"arguments must be a JSON object\"}";
  }
  return backgroundRunner().list();
}

}  // namespace agent
