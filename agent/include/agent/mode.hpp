#pragma once
#include <string>
#include <vector>

namespace agent {

// Run-mode presets ported from DeepSeek Harness (dsh):
//   standard: full tool set (default, current behavior)
//   minimal : only shell_exec + edit + ask_user (no web/browser/subagent/...)
//   ptc     : standard + run_code (one invocation batches many tool calls)
//   creator : standard + list_tools/get_config runtime introspection
enum class Mode : int { Standard = 0, Minimal, Ptc, Creator };

inline Mode modeFromString(std::string const& s) {
  if (s == "minimal") return Mode::Minimal;
  if (s == "ptc") return Mode::Ptc;
  if (s == "creator") return Mode::Creator;
  return Mode::Standard;
}

inline std::string modeToString(Mode m) {
  switch (m) {
    case Mode::Minimal: return "minimal";
    case Mode::Ptc: return "ptc";
    case Mode::Creator: return "creator";
    default: return "standard";
  }
}

inline std::vector<std::string> modeNames() { return {"standard", "minimal", "ptc", "creator"}; }

// Tools a mode adds beyond the standard set (advertised + runnable).
inline std::vector<std::string> modeExtraTools(Mode m) {
  if (m == Mode::Ptc) return {"run_code"};
  if (m == Mode::Creator) return {"list_tools", "get_config"};
  return {};
}

// Hard gate: a tool must be allowed by the mode before it may run. Only
// minimal restricts; everything else accepts the full registry.
inline bool modeAllowsTool(Mode m, std::string const& tool) {
  if (m == Mode::Minimal)
    return tool == "shell_exec" || tool == "edit" || tool == "ask_user";
  return true;
}

// Extra system-prompt instructions injected at turn start for the mode.
inline std::string modeInstructions(Mode m) {
  switch (m) {
    case Mode::Minimal:
      return "MODE: minimal. You have only shell_exec, edit and ask_user - no web, no "
             "browser, no subagents, no background or RAG tools. Keep work direct and "
             "small-step; prefer edit for file changes and shell for builds/tests/git. "
             "Ask the user (ask_user) before anything destructive.";
    case Mode::Ptc:
      return "MODE: ptc (programmatic tool calling). Whenever a task needs several tool "
             "calls, write ONE run_code program (a JSON object with a \"steps\" array) that "
             "runs them all in a single invocation instead of issuing many separate tool "
             "calls. Use a string argument \"$var\" (or \"{$var}\" inside a longer string) "
             "to feed an earlier step's stored result (its \"var\") into the next step. "
             "This collapses round-trips and keeps intermediate data out of context.";
    case Mode::Creator:
      return "MODE: creator. You may inspect the runtime before acting: use list_tools to "
             "see the full registered tool surface and get_config to see the current "
             "configuration, then plan how the agent is composed.";
    default:
      return "";
  }
}

}  // namespace agent