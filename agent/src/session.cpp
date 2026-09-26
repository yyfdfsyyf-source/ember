#include "agent/session.hpp"
#include "minijson.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace agent {

namespace {

std::string fmtIdTime(std::time_t t, int ms, int seq) {
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[64];
  std::snprintf(buf, sizeof buf, "sess-%04d%02d%02d-%02d%02d%02d-%03d-%02d",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
                tmv.tm_sec, ms, seq);
  return buf;
}

}  // namespace

std::string makeSessionId() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  static int seq = 0;  // guard against two ids issued in the same millisecond
  int n = seq++;
  return fmtIdTime(system_clock::to_time_t(now), (int)ms.count(), n);
}

bool listSessions(std::string const& dir, std::vector<SessionInfo>& out) {
  std::error_code ec;
  std::filesystem::path d(dir);
  if (!std::filesystem::is_directory(d, ec)) return false;
  for (auto const& entry : std::filesystem::directory_iterator(d, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    std::filesystem::path p = entry.path();
    if (p.extension() != ".json") continue;
    SessionInfo info;
    info.id = p.stem().string();
    info.path = p.string();
    std::ifstream f(p, std::ios::binary);
    if (!f) continue;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    mini::Value root;
    if (!mini::tryParse(text, root) || root.type != mini::Value::Object) continue;
    mini::Value const* model = root.get("model");
    if (model && model->type == mini::Value::String) info.model = model->s;
    mini::Value const* title = root.get("title");
    if (title && title->type == mini::Value::String) info.title = title->s;
    mini::Value const* savedAt = root.get("saved_at");
    if (savedAt && savedAt->type == mini::Value::Int) info.savedAt = (int64_t)savedAt->i;
    mini::Value const* msgs = root.get("messages");
    if (msgs && msgs->type == mini::Value::Array) info.msgCount = (int)msgs->arr.size();
    out.push_back(std::move(info));
  }
  std::stable_sort(out.begin(), out.end(), [](SessionInfo const& a, SessionInfo const& b) {
    return a.savedAt > b.savedAt;
  });
  return true;
}

namespace {

mini::Value msgToJson(Message const& m) {
  mini::Value o = mini::Value::makeObject();
  o.set("role", mini::Value::makeString(m.role));
  if (!m.content.empty()) o.set("content", mini::Value::makeString(m.content));
  if (!m.toolCallId.empty()) o.set("tool_call_id", mini::Value::makeString(m.toolCallId));
  if (!m.toolCalls.empty()) {
    mini::Value arr = mini::Value::makeArray();
    for (auto const& tc : m.toolCalls) {
      mini::Value t = mini::Value::makeObject();
      t.set("id", mini::Value::makeString(tc.id));
      t.set("name", mini::Value::makeString(tc.name));
      if (!tc.arguments.empty()) t.set("arguments", mini::Value::makeString(tc.arguments));
      arr.arr.push_back(std::move(t));
    }
    o.set("tool_calls", std::move(arr));
  }
  return o;
}

bool msgFromJson(mini::Value const& o, Message& out) {
  if (o.type != mini::Value::Object) return false;
  mini::Value const* role = o.get("role");
  if (!role || role->type != mini::Value::String) return false;
  out.role = role->s;
  mini::Value const* content = o.get("content");
  if (content) {
    if (content->type == mini::Value::String) {
      out.content = content->s;
    } else if (content->type == mini::Value::Array) {
      // OpenAI-style content segments: join the text parts.
      for (auto const& seg : content->arr) {
        if (seg.type == mini::Value::String) out.content += seg.s;
      }
    }
  }
  mini::Value const* tcid = o.get("tool_call_id");
  if (tcid && tcid->type == mini::Value::String) out.toolCallId = tcid->s;
  mini::Value const* tcs = o.get("tool_calls");
  if (tcs && tcs->type == mini::Value::Array) {
    for (auto const& tv : tcs->arr) {
      if (tv.type != mini::Value::Object) continue;  // tolerate one bad entry
      ToolCall tc;
      mini::Value const* id = tv.get("id");
      if (id && id->type == mini::Value::String) tc.id = id->s;
      mini::Value const* name = tv.get("name");
      if (name && name->type == mini::Value::String) tc.name = name->s;
      mini::Value const* args = tv.get("arguments");
      if (args) {
        if (args->type == mini::Value::String) tc.arguments = args->s;
        else tc.arguments = mini::dump(*args);  // tolerate object-form args
      }
      out.toolCalls.push_back(std::move(tc));
    }
  }
  return true;
}

}  // namespace

bool saveSession(std::string const& path, std::vector<Message> const& messages,
                 std::string const& model, std::string const& title) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  mini::Value root = mini::Value::makeObject();
  root.set("version", mini::Value::makeInt(1));
  root.set("model", mini::Value::makeString(model));
  if (!title.empty()) root.set("title", mini::Value::makeString(title));
  std::time_t now = std::time(nullptr);
  root.set("saved_at", mini::Value::makeInt(static_cast<int64_t>(now)));
  mini::Value arr = mini::Value::makeArray();
  for (auto const& m : messages) arr.arr.push_back(msgToJson(m));
  root.set("messages", std::move(arr));
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << mini::dump(root);
  return f.good();
}

// Returns 1 on success, 0 if the file cannot be opened, -1 on JSON parse
// failure, -2 if the "messages" field is missing. Bad individual messages are
// skipped (tolerated) rather than failing the whole session file.
int loadSession(std::string const& path, std::vector<Message>& messages, std::string& model) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return 0;
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  mini::Value root;
  if (!mini::tryParse(text, root) || root.type != mini::Value::Object) return -1;
  mini::Value const* m = root.get("model");
  if (m && m->type == mini::Value::String) model = m->s;
  mini::Value const* msgs = root.get("messages");
  if (!msgs || msgs->type != mini::Value::Array) return -2;
  std::vector<Message> out;
  out.reserve(msgs->arr.size());
  for (auto const& mv : msgs->arr) {
    Message msg;
    if (msgFromJson(mv, msg)) out.push_back(std::move(msg));
  }
  messages = std::move(out);
  return 1;
}

}  // namespace agent