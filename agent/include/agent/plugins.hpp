#pragma once
#include "agent/tools.hpp"
#include "minijson.hpp"
#include <memory>
#include <string>
#include <vector>

namespace agent {

// One plugin = one child process speaking line-based JSON-RPC 2.0 over
// stdio. The host speaks first:
//
//   -> {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
//   <- {"jsonrpc":"2.0","id":1,"result":{"name":"...","version":"..."}}
//   -> {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
//   <- {"jsonrpc":"2.0","id":2,"result":{"tools":[ToolSpec...]}}
//   -> {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"x","arguments":{...}}}
//   <- {"jsonrpc":"2.0","id":3,"result":{"content":"text result"}}
//
// The plugin must print exactly one JSON document per line on stdout and
// must not write anything else there (stderr is free for logging).
//
// ToolSpec mirrors agent::Tool: {"name","description","parameters","enabled"}.
struct PluginSpec {
  std::string name;
  std::string description;
  mini::Value parameters;
  bool enabled = true;
};

// One MCP stdio server (Model Context Protocol over newline-delimited JSON-RPC
// 2.0, same transport as a native plugin). The client runs the MCP handshake
// (initialize -> notifications/initialized), lists tools via tools/list and
// forwards calls via tools/call. Advertised tools are registered under a
// "<name>_" prefix so they cannot collide with built-ins or other servers.
struct McpConfig {
  std::string name;     // logical name; also the tool prefix source
  std::string command;  // executable, e.g. "npx" or a path to a server binary
  std::vector<std::string> args;
  std::vector<std::pair<std::string, std::string>> env;  // optional extra env
  bool enabled = true;
};

// The pimpl struct is declared inline in the class; its definition lives in
// plugins.cpp so callers only need an incomplete type.

class PluginProcess {
 public:
  PluginProcess();
  ~PluginProcess();
  PluginProcess(PluginProcess const&) = delete;
  PluginProcess& operator=(PluginProcess const&) = delete;
  PluginProcess(PluginProcess&&) noexcept;
  PluginProcess& operator=(PluginProcess&&) noexcept;
  // Starts the child and blocks until initialize() and tools/list() return.
  // On failure returns a non-empty error and the process is stopped.
  std::string start(std::string const& exePath,
                    std::vector<std::string> const& args = {});
  // Starts the child as an MCP stdio server: spawn, initialize (with protocol
  // params), then the notifications/initialized ping. On failure returns a
  // non-empty error and the process is stopped.
  std::string startMcp(McpConfig const& cfg);
  // Registers every advertised tool into `reg`. When `prefix` is non-empty,
  // every tool name becomes "<prefix><name>". Tool runs are forwarded to the
  // child synchronously (tools/call) and return their text content (MCP
  // content arrays and isError are handled). Returns "" on success.
  std::string registerTools(ToolRegistry& reg,
                            std::string const& prefix = "");
  void stop();
  bool running() const;
  std::string const& name() const { return name_; }
  size_t toolCount() const { return toolCount_; }

  // Low-level request; returns the full JSON-RPC response document. Empty on
  // transport error/timeout (see lastError()).
  std::string request(std::string const& method, std::string const& paramsJson,
                      int timeoutMs = 10000);
  std::string const& lastError() const { return lastError_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string name_;
  std::string prefix_;
  std::string lastError_;
  size_t toolCount_ = 0;
};

// Convenience: start every plugin-capable executable in `dir` (Windows:
// *.exe; POSIX: any executable file) and register their tools into `reg`.
// Non-plugin executables are skipped silently. Returns "" on success.
std::string startPluginsFromDir(std::string const& dir, ToolRegistry& reg,
                                std::vector<std::unique_ptr<PluginProcess>>& out);

}  // namespace agent