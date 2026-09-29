// Background task runner tests: start/poll/status output, kill, timeout,
// unknown ids, list, and no-notify-on-manual-kill semantics.
#include "agent/background.hpp"
#include "minijson.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                     \
  } while (0)

static mini::Value parse(std::string const& json) {
  mini::Value v;
  mini::tryParse(json, v);
  return v;
}

static mini::Value const* field(std::string const& json, char const* key) {
  return parse(json).get(key);
}

static std::string getStr(std::string const& json, char const* key) {
  auto* f = field(json, key);
  return f ? f->asString() : "";
}

static bool getBool(std::string const& json, char const* key) {
  auto* f = field(json, key);
  return f && f->asBool();
}

static void waitDone(std::string const& id, std::string* lastStatus) {
  for (int i = 0; i < 400; i++) {  // up to 10s
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    std::string s = agent::backgroundRunner().status(id);
    if (lastStatus) *lastStatus = s;
    if (!getBool(s, "running")) return;
  }
}

// 拖时间的命令必须按平台选：POSIX 的 ping 只接受一个主机参数，
// `ping -n 30 127.0.0.1` 会当成两个主机名而立刻报错退出，于是
// kill/timeout 两条断言测的是"进程早没了"，跟实现无关。
#if defined(_WIN32)
static char const* kLongRunning = "ping -n 30 127.0.0.1 >nul 2>&1";
#else
static char const* kLongRunning = "sleep 30";
#endif

static void test_quick_finish() {
  std::string r = agent::backgroundRunner().start("echo hello-bg", 0);
  std::string id = getStr(r, "id");
  CHECK(!id.empty());
  CHECK(getBool(r, "running"));
  std::string s, allOut;
  for (int i = 0; i < 400; i++) {  // up to 10s
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    s = agent::backgroundRunner().status(id);
    allOut += getStr(s, "output");  // each poll returns only NEW output
    if (!getBool(s, "running")) break;
  }
  CHECK(!getBool(s, "running"));
  CHECK(allOut.find("hello-bg") != std::string::npos);
}

static void test_kill() {
  std::string r = agent::backgroundRunner().start(
      kLongRunning, 0);  // ~30s lifetime
  std::string id = getStr(r, "id");
  CHECK(!id.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string k = agent::backgroundRunner().kill(id);
  CHECK(getBool(k, "killed"));
  std::string s;
  waitDone(id, &s);
  CHECK(!getBool(s, "running"));
  CHECK(getBool(s, "killed"));
}

static void test_timeout() {
  std::string r = agent::backgroundRunner().start(
      kLongRunning, 800);
  std::string id = getStr(r, "id");
  CHECK(!id.empty());
  std::string s;
  waitDone(id, &s);
  CHECK(!getBool(s, "running"));
  CHECK(getBool(s, "timed_out"));
}

static void test_unknown_and_list() {
  std::string e = agent::backgroundRunner().status("bogus-id");
  CHECK(e.find("error") != std::string::npos);
  std::string l = agent::backgroundRunner().list();
  mini::Value v = parse(l);
  CHECK(v.type == mini::Value::Array);
  CHECK(!v.arr.empty());
}

static void test_no_notify_on_manual_kill() {
  bool notified = false;
  agent::backgroundRunner().setNotifier(
      [&](std::string const&, int, bool, std::string const&) { notified = true; });
  std::string r = agent::backgroundRunner().start(
      kLongRunning, 0);
  std::string id = getStr(r, "id");
  CHECK(!id.empty());
  agent::backgroundRunner().kill(id);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  CHECK(!notified);
  agent::backgroundRunner().setNotifier(nullptr);
}

int main() {
  std::fprintf(stderr, "step1: quick finish\n");
  test_quick_finish();
  std::fprintf(stderr, "step2: kill\n");
  test_kill();
  std::fprintf(stderr, "step3: timeout\n");
  test_timeout();
  std::fprintf(stderr, "step4: unknown + list\n");
  test_unknown_and_list();
  std::fprintf(stderr, "step5: no notify on manual kill\n");
  test_no_notify_on_manual_kill();
  agent::backgroundRunner().shutdown();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}