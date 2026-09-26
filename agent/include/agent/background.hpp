#pragma once
#include <functional>
#include <string>

namespace agent {

// Background command runner. Tasks launched here keep running while the
// conversation does other things; output is buffered per task and handed to
// the model on demand. When a task exits on its own (or times out) the
// notifier fires from an internal thread. Manual kills do not notify.
class BackgroundRunner {
 public:
  BackgroundRunner();
  ~BackgroundRunner();
  BackgroundRunner(BackgroundRunner const&) = delete;
  BackgroundRunner& operator=(BackgroundRunner const&) = delete;

  // Start `command` in the background; optional auto-kill after `timeoutMs`
  // (0 = run until the model kills it). Returns a JSON object:
  //   {"id","command","running":true}
  // or {"error":...} on spawn failure.
  std::string start(std::string const& command, long timeoutMs);
  // Running state plus any NEW output since the last status/kill call.
  // Returns a JSON object:
  //   {"id","command","running","exit_code","killed","timed_out","output"}
  // or {"error":"unknown background task '<id>'"}.
  std::string status(std::string const& id);
  // Force-terminate a running task. JSON: {"id","killed":bool}.
  std::string kill(std::string const& id);
  // JSON array with one entry per task:
  //   {"id","command","running","exit_code","timed_out","output_len","output_tail"}
  std::string list();
  // Called from an internal thread when a task exits on its own or times out.
  // `timedOut` distinguishes the two. `tail` holds the latest output.
  void setNotifier(std::function<void(std::string const& id, int exitCode,
                                      bool timedOut, std::string const& tail)>
                       fn);
  // Kill everything and join reader threads (no notifications).
  void shutdown();
  size_t taskCount() const;

 private:
  struct Impl;
  Impl* impl_;
};

// Process-wide instance (tools and the app share it).
BackgroundRunner& backgroundRunner();

// Tool entry points bound to the global runner.
std::string toolBackgroundStart(std::string const& argsJson);
std::string toolBackgroundStatus(std::string const& argsJson);
std::string toolBackgroundKill(std::string const& argsJson);
std::string toolBackgroundList(std::string const& argsJson);

}  // namespace agent
