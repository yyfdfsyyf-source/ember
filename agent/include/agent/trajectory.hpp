#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace agent {

// Event kinds recorded in the trajectory log. The set mirrors the dsh session
// event vocabulary at a coarser granularity: turn boundaries, surface
// messages, tool calls, context injections, and failures — but not raw stream
// chunks (the assembled text blocks are what the UI replays).
enum class TrajKind : uint8_t {
  TurnStart,   // turn began; carries model
  TurnEnd,     // turn ended; carries rc (0 = ok)
  UserMessage, // user prompt (source: "prompt" | "ask" | "inject")
  Reasoning,   // assembled reasoning block
  Assistant,   // assembled assistant text block
  ToolCall,    // model requested a tool; carries toolName/toolArgs
  ToolResult,  // tool finished; carries toolName + text
  ContextNote, // context compression / injection notice
  Error,       // failed request
};

// Filter groups shown in the trajectory view header ("by source", dsh-style).
enum class TrajFilter : uint8_t {
  All,        // everything
  Conversation, // UserMessage + Assistant
  Reasoning,  // Reasoning blocks
  Tools,      // ToolCall + ToolResult
  Audit,      // TurnStart/TurnEnd + ContextNote + Error
};

// One immutable-ish entry in the append-only trajectory log.
struct TrajEvent {
  int64_t seq = 0;       // monotonic position in the log
  TrajKind kind = TrajKind::UserMessage;
  int64_t time = 0;      // epoch ms, filled by the log on append
  int turn = 0;          // enclosing turn number (0 before the first turn)
  int rc = 0;            // TurnEnd: runTurn result code
  std::string model;     // TurnStart
  std::string source;    // UserMessage: prompt | ask | inject
  std::string text;      // main content (also TurnEnd reason when rc != 0)
  std::string toolName;  // ToolCall / ToolResult
  std::string toolArgs;  // ToolCall: raw arguments JSON as produced
  int64_t promptTokens = -1;      // TurnEnd: usage reported this turn
  int64_t completionTokens = -1;  // TurnEnd: usage reported this turn
  int64_t cachedTokens = -1;      // TurnEnd: cached input tokens this turn
};

// Append-only trajectory log. Kept in memory always; when a path is given,
// every event is also appended to a JSONL file (one event per line, written
// from the UI thread) and previously recorded events are loaded on open().
// open()/append()/events() are called from one thread only (the UI thread).
class TrajectoryLog {
 public:
  ~TrajectoryLog() { close(); }
  // Open for append + replay. An existing file's lines are parsed into the
  // in-memory log (torn tail lines are skipped). path "" keeps it memory-only.
  void open(std::string const& path);
  void close();
  bool isOpen() const { return file_ != nullptr; }
  std::string const& path() const { return path_; }

  // Append one event: assigns seq/time, keeps it in memory, and (when open)
  // writes it to the JSONL file. Returns the stored event.
  TrajEvent const& append(TrajEvent ev);

  size_t size() const { return events_.size(); }
  TrajEvent const& at(size_t i) const { return events_[i]; }
  std::vector<TrajEvent> const& events() const { return events_; }

 private:
  void writeLine(TrajEvent const& ev);
  std::string path_;
  FILE* file_ = nullptr;
  std::vector<TrajEvent> events_;
  // Monotonic seq for events appended after open(). Starts at the file's max
  // seq + 1 (or 0 for a fresh log) so displayed #N stays monotonic even when
  // the in-memory window only holds the most recent events.
  int64_t nextSeq_ = 0;
};

std::string trajKindName(TrajKind k);
std::string trajKindIcon(TrajKind k);
bool trajFilterMatch(TrajFilter f, TrajKind k);
std::string trajFilterName(TrajFilter f);

}  // namespace agent
