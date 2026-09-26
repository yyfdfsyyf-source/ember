#include "agent/trajectory.hpp"
#include "minijson.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agent {

namespace {

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string kindName(TrajKind k) {
  switch (k) {
    case TrajKind::TurnStart: return "turn-start";
    case TrajKind::TurnEnd: return "turn-end";
    case TrajKind::UserMessage: return "user";
    case TrajKind::Reasoning: return "reasoning";
    case TrajKind::Assistant: return "assistant";
    case TrajKind::ToolCall: return "tool-call";
    case TrajKind::ToolResult: return "tool-result";
    case TrajKind::ContextNote: return "context";
    case TrajKind::Error: return "error";
  }
  return "unknown";
}

TrajKind kindFromName(std::string const& n) {
  if (n == "turn-start") return TrajKind::TurnStart;
  if (n == "turn-end") return TrajKind::TurnEnd;
  if (n == "user") return TrajKind::UserMessage;
  if (n == "reasoning") return TrajKind::Reasoning;
  if (n == "assistant") return TrajKind::Assistant;
  if (n == "tool-call") return TrajKind::ToolCall;
  if (n == "tool-result") return TrajKind::ToolResult;
  if (n == "context") return TrajKind::ContextNote;
  if (n == "error") return TrajKind::Error;
  return TrajKind::UserMessage;
}

}  // namespace

std::string trajKindName(TrajKind k) { return kindName(k); }

std::string trajKindIcon(TrajKind k) {
  switch (k) {
    case TrajKind::TurnStart: return "\xE2\x97\x87";        // ◇
    case TrajKind::TurnEnd: return "\xE2\x97\x89";          // ◉
    case TrajKind::UserMessage: return "\xE2\x96\xB6";      // ▶
    case TrajKind::Reasoning: return "\xE2\x9C\xA7";        // ✧
    case TrajKind::Assistant: return "\xE2\x96\xB8";        // ▸
    case TrajKind::ToolCall: return "\xE2\x9A\x99";         // ⚙
    case TrajKind::ToolResult: return "\xE2\x86\xB3";       // ↳
    case TrajKind::ContextNote: return "\xE2\xA7\x89";      // ⧉
    case TrajKind::Error: return "\xE2\x9C\x95";            // ✕
  }
  return "?";
}

bool trajFilterMatch(TrajFilter f, TrajKind k) {
  switch (f) {
    case TrajFilter::All:
      return true;
    case TrajFilter::Conversation:
      return k == TrajKind::UserMessage || k == TrajKind::Assistant;
    case TrajFilter::Reasoning:
      return k == TrajKind::Reasoning;
    case TrajFilter::Tools:
      return k == TrajKind::ToolCall || k == TrajKind::ToolResult;
    case TrajFilter::Audit:
      return k == TrajKind::TurnStart || k == TrajKind::TurnEnd ||
             k == TrajKind::ContextNote || k == TrajKind::Error;
  }
  return true;
}

std::string trajFilterName(TrajFilter f) {
  switch (f) {
    case TrajFilter::All: return "all";
    case TrajFilter::Conversation: return "chat";
    case TrajFilter::Reasoning: return "think";
    case TrajFilter::Tools: return "tools";
    case TrajFilter::Audit: return "audit";
  }
  return "all";
}

void TrajectoryLog::open(std::string const& path) {
  close();
  events_.clear();
  nextSeq_ = 0;
  if (path.empty()) return;
  // Rolling archive: if the existing trajectory is too big, archive it before
  // appending so a long session never grows the file without bound.
  constexpr int64_t kMaxTrajBytes = 10 * 1024 * 1024;  // 10 MiB
  {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (!ec && sz > kMaxTrajBytes) {
      auto archiveOf = [&](int n) { return path + "." + std::to_string(n); };
      std::error_code ignore;
      std::filesystem::remove(archiveOf(3), ignore);
      std::filesystem::rename(archiveOf(2), archiveOf(3), ignore);
      std::filesystem::rename(archiveOf(1), archiveOf(2), ignore);
      std::filesystem::rename(path, archiveOf(1), ignore);
    }
  }
  // Bound how much history is kept in memory: only the most recent events are
  // replayed, so a long-lived 10 MiB log does not balloon memory on startup.
  // Older events remain on disk (rolled into archives) and are not shown in the
  // trajectory list, which only displays recent activity anyway.
  constexpr size_t kMaxLoaded = 4000;
  std::vector<TrajEvent> tail;
  tail.reserve(kMaxLoaded);
  std::ifstream in(path);
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      mini::Value v;
      if (!mini::tryParse(line, v) || v.get("seq") == nullptr || v.get("kind") == nullptr) {
        continue;
      }
      TrajEvent ev;
      auto const* sq = v.get("seq");
      ev.seq = sq ? sq->asInt(0) : 0;
      ev.kind = kindFromName(v.get("kind")->asString());
      auto const* tv = v.get("time");
      ev.time = tv ? tv->asInt(0) : 0;
      auto const* tn = v.get("turn");
      ev.turn = tn ? (int)tn->asInt(0) : 0;
      auto const* rc = v.get("rc");
      ev.rc = rc ? (int)rc->asInt(0) : 0;
      auto const* pt = v.get("promptTokens");
      if (pt) ev.promptTokens = pt->asInt(-1);
      auto const* ct = v.get("completionTokens");
      if (ct) ev.completionTokens = ct->asInt(-1);
      auto const* cd = v.get("cachedTokens");
      if (cd) ev.cachedTokens = cd->asInt(-1);
      auto const* s = v.get("model");
      if (s) ev.model = s->asString();
      s = v.get("source");
      if (s) ev.source = s->asString();
      s = v.get("text");
      if (s) ev.text = s->asString();
      s = v.get("toolName");
      if (s) ev.toolName = s->asString();
      s = v.get("toolArgs");
      if (s) ev.toolArgs = s->asString();
      if (ev.seq >= nextSeq_) nextSeq_ = ev.seq + 1;
      tail.push_back(std::move(ev));
    }
  }
  if (tail.size() > kMaxLoaded) {
    tail.erase(tail.begin(), tail.begin() + (tail.size() - kMaxLoaded));
  }
  events_ = std::move(tail);
  path_ = path;
  file_ = std::fopen(path.c_str(), "ab");
}

void TrajectoryLog::close() {
  if (file_) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
  path_.clear();
}

TrajEvent const& TrajectoryLog::append(TrajEvent ev) {
  ev.seq = nextSeq_++;
  ev.time = nowMs();
  events_.push_back(ev);
  if (file_) writeLine(events_.back());
  return events_.back();
}

void TrajectoryLog::writeLine(TrajEvent const& ev) {
  mini::Value v = mini::Value::makeObject();
  v.set("seq", mini::Value::makeInt(ev.seq));
  v.set("kind", mini::Value::makeString(kindName(ev.kind)));
  v.set("time", mini::Value::makeInt(ev.time));
  v.set("turn", mini::Value::makeInt(ev.turn));
  v.set("rc", mini::Value::makeInt(ev.rc));
  if (ev.promptTokens >= 0) v.set("promptTokens", mini::Value::makeInt(ev.promptTokens));
  if (ev.completionTokens >= 0) v.set("completionTokens", mini::Value::makeInt(ev.completionTokens));
  if (ev.cachedTokens >= 0) v.set("cachedTokens", mini::Value::makeInt(ev.cachedTokens));
  if (!ev.model.empty()) v.set("model", mini::Value::makeString(ev.model));
  if (!ev.source.empty()) v.set("source", mini::Value::makeString(ev.source));
  if (!ev.text.empty()) v.set("text", mini::Value::makeString(ev.text));
  if (!ev.toolName.empty()) v.set("toolName", mini::Value::makeString(ev.toolName));
  if (!ev.toolArgs.empty()) v.set("toolArgs", mini::Value::makeString(ev.toolArgs));
  std::string line = mini::dump(v);
  std::fputs(line.c_str(), file_);
  std::fputc('\n', file_);
  std::fflush(file_);
}

}  // namespace agent