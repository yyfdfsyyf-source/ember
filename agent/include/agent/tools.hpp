#pragma once
#include "minijson.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {

class DiskCache;

// A callable tool the model may invoke. `run` receives the JSON-encoded
// arguments and returns a text result (structured tools return JSON).
struct Tool {
  std::string name;
  std::string description;
  mini::Value parameters;  // JSON-schema object describing the arguments
  bool enabled = true;
  // true = side-effect free; results may be served from the local disk cache
  // (file_read/file_list get an mtime-augmented key, web_fetch a short TTL).
  bool pure = false;
  std::function<std::string(std::string const& argsJson)> run;
};

class ToolRegistry {
 public:
  // A pre-execution policy hook. Return Deny to block a call (the message is
  // surfaced to the model as the tool result). Runs on the worker thread and
  // may block for interactive approval. It is enforced for every dispatch
  // through run(), including run_code sub-steps and sub-agent tool calls.
  enum class GuardVerdict { Allow, Deny };
  struct GuardResult {
    GuardVerdict verdict = GuardVerdict::Allow;
    std::string message;
  };
  using Guard = std::function<GuardResult(std::string const& tool,
                                           std::string const& argsJson)>;
  void setGuard(Guard g) { guard_ = std::move(g); }

  // Local tool-result cache (nullptr disables; only `pure` tools use it).
  void setCache(std::shared_ptr<DiskCache> c) { cache_ = std::move(c); }

  void add(Tool t);
  Tool const* find(std::string const& name) const;
  void setEnabled(std::string const& name, bool on);
  std::vector<std::string> names() const;
  std::vector<std::string> enabledNames() const;
  // Build the OpenAI "tools" JSON array. `subset` restricts the tools
  // advertised (empty => all enabled tools).
  std::string toolsJson(std::vector<std::string> const& subset = {}) const;
  // Execute a tool by name; returns its text result. Unknown tools return
  // a JSON error object.
  std::string run(std::string const& name, std::string const& argsJson) const;

 private:
  std::vector<Tool> tools_;
  std::shared_ptr<DiskCache> cache_;
  Guard guard_;
};

// Register the built-in text-first tool set (shell/file/web).
void registerBuiltinTools(ToolRegistry& reg);

// Workspace policy helpers (pure, unit-testable). normalizeWorkspacePath makes
// a path absolute (against the process CWD), collapses `..`/separators to '/',
// and case-folds on Windows. toolPathTargets extracts path-like targets from a
// tool's JSON arguments: values under path/file/dir, plus path-shaped tokens
// inside "command" strings for shell-family tools.
std::string normalizeWorkspacePath(std::string const& p);
bool pathWithinNormalizedRoot(std::string const& rootN, std::string const& pN);
std::vector<std::string> commandPathCandidates(std::string const& cmd);
std::vector<std::string> toolPathTargets(std::string const& tool,
                                         std::string const& argsJson);

// Individual implementations, exposed for tests.
std::string toolShellExec(std::string const& argsJson);
std::string toolVerify(std::string const& argsJson);
std::string toolRemember(std::string const& argsJson);
std::string toolRecall(std::string const& argsJson);
std::string toolFileRead(std::string const& argsJson);
std::string toolFileWrite(std::string const& argsJson);
std::string toolFileList(std::string const& argsJson);
std::string toolWebFetch(std::string const& argsJson);
std::string toolEdit(std::string const& argsJson);
std::string toolPatch(std::string const& argsJson);
std::string toolGrep(std::string const& argsJson);
std::string toolGlob(std::string const& argsJson);
// HTML -> text helpers used by web_fetch/web_search (exposed for tests).
std::string htmlToText(std::string const& html);
std::string htmlBodyToText(std::string const& html);
std::string toolRagIndex(std::string const& argsJson);
std::string toolRagSearch(std::string const& argsJson);

// Git wrappers (model can review/commit its own changes).
std::string toolGitStatus(std::string const& argsJson);
std::string toolGitDiff(std::string const& argsJson);
std::string toolGitLog(std::string const& argsJson);
std::string toolGitCommit(std::string const& argsJson);

// PTC-mode batch runner: executes a JSON program of {tool,arguments,var} steps
// against `reg`, interpolating "{$var}" / "$var" from earlier steps. Exposed for
// tests; registered as the "run_code" tool in registerBuiltinTools.
std::string toolRunCode(ToolRegistry& reg, std::string const& argsJson);

// Interactive confirmation: lets a tool pause and ask the human a question.
// The callback runs synchronously on the calling (worker) thread and returns
// the user's typed answer. Not set => ask_user reports it is unavailable.
void setAskUserCallback(std::function<std::string(std::string const& question)> fn);
std::string toolAskUser(std::string const& argsJson);
// Sub-agent delegation: runs a fresh, isolated agent turn (own message list)
// on the calling thread and returns its final answer. Not set => subagent
// reports it is unavailable.
void setSubagentCallback(
    std::function<std::string(std::string const& description, std::string const& prompt)> fn);
std::string toolSubagent(std::string const& argsJson);
// Parallel delegation: takes a JSON array of {description, prompt} tasks,
// runs up to `max_parallel` subagents on separate threads, returns an array
// of {description, ok, answer}. Not set => reports it is unavailable.
void setSubagentParallelCallback(std::function<std::string(std::string const& tasksJson)> fn);
std::string toolSubagentParallel(std::string const& argsJson);
// Multi-step task plan state, shared across the process.
std::string toolTodoWrite(std::string const& argsJson);
std::string todoListJson();

// Headless-browser tools (share one browser session).
std::string toolBrowserOpen(std::string const& argsJson);
std::string toolBrowserClick(std::string const& argsJson);
std::string toolBrowserType(std::string const& argsJson);
std::string toolBrowserKey(std::string const& argsJson);
std::string toolBrowserScroll(std::string const& argsJson);
std::string toolBrowserSnapshot(std::string const& argsJson);
std::string toolBrowserEval(std::string const& argsJson);
std::string toolBrowserWait(std::string const& argsJson);
std::string toolBrowserBack(std::string const& argsJson);
std::string toolBrowserForward(std::string const& argsJson);
std::string toolBrowserReload(std::string const& argsJson);
std::string toolBrowserScreenshot(std::string const& argsJson);
std::string toolBrowserUpload(std::string const& argsJson);
std::string toolBrowserClose(std::string const& argsJson);

// Windows desktop accessibility (UI Automation) tools: read the a11y tree as
// text and drive the UI by element path/name (click/type/key/scroll).
std::string toolDesktopWindows(std::string const& argsJson);
std::string toolDesktopTree(std::string const& argsJson);
std::string toolDesktopClick(std::string const& argsJson);
std::string toolDesktopType(std::string const& argsJson);
std::string toolDesktopKey(std::string const& argsJson);
std::string toolDesktopScroll(std::string const& argsJson);
std::string toolDesktopWait(std::string const& argsJson);

}  // namespace agent
