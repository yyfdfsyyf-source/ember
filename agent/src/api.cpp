#include "agent/client.hpp"
#include "agent/cache.hpp"
#include "agent/context.hpp"
#include "agent/thinking.hpp"
#include "agent/usage.hpp"
#include "agent/http.hpp"
#include "minijson.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace agent {

// Retry policy for transient failures.
//  - Chat level: API timeouts / 408 / 429 / 5xx are retried with backoff
//    inside runTurn before the turn is abandoned.
//  - Tool level: results whose "error" field looks like a transient network
//    failure are auto-retried a couple of times; deterministic errors (bad
//    args, missing files, compile failures) are passed straight through.

constexpr int kMaxChatRetries = 5;
constexpr int kMaxToolRetries = 2;

bool isTransientChatError(int rc) {
  return rc == -1 || rc == 408 || rc == 429 || rc == 500 || rc == 502 || rc == 503 || rc == 504;
}

bool isTransientToolError(std::string const& result) {
  mini::Value v;
  if (!mini::tryParse(result, v) || v.type != mini::Value::Object) return false;
  mini::Value const* e = v.get("error");
  if (!e || e->type != mini::Value::String) return false;
  std::string const& msg = e->s;
  for (char const* kw : {"network", "timeout", "timed out", "timed_out", "connection",
                         "reset", "too many", "temporarily unavailable", "slow network"}) {
    if (msg.find(kw) != std::string::npos) return true;
  }
  // HTTP status codes embedded in the message: 429 and anything >= 500 are
  // transient; deterministic 4xx like 400/404 are not.
  for (size_t i = 0; i + 2 < msg.size(); i++) {
    if (msg[i] < '0' || msg[i] > '9') continue;
    if (i > 0 && ((msg[i - 1] >= '0' && msg[i - 1] <= '9') || (msg[i - 1] >= 'a' && msg[i - 1] <= 'z') ||
                  (msg[i - 1] >= 'A' && msg[i - 1] <= 'Z')))
      continue;  // part of a longer number/word
    if (!(msg[i + 1] >= '0' && msg[i + 1] <= '9') || !(msg[i + 2] >= '0' && msg[i + 2] <= '9'))
      continue;
    int code = (msg[i] - '0') * 100 + (msg[i + 1] - '0') * 10 + (msg[i + 2] - '0');
    if (code >= 500 && code <= 599) return true;
    if (code == 429) return true;
    break;  // first standalone 3-digit number decided it
  }
  return false;
}

bool Client::configure(std::string baseUrl, std::string apiKey) {
  baseUrl_ = std::move(baseUrl);
  apiKey_ = std::move(apiKey);
  return !baseUrl_.empty();
}

int Client::listModels(std::vector<std::string>& out) const {
  std::string url = baseUrl_;
  if (url.empty()) return -1;
  if (url.back() != '/') url += '/';
  url += "models";
  HttpRequest req;
  req.url = std::move(url);
  req.method = "GET";
  req.timeoutSec = 15;
  if (!apiKey_.empty())
    req.headers.push_back({"Authorization", "Bearer " + apiKey_});
  std::string body;
  int status = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    body.clear();
    status = httpRequest(req, [&](char const* d, size_t n) { body.append(d, n); });
    if (status == 200 || !isTransientChatError(status)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(600 * (attempt + 1)));
  }
  if (status != 200) return status;
  mini::Value v;
  if (!mini::tryParse(body, v) || v.type != mini::Value::Object) return -1;
  mini::Value const* data = v.get("data");
  if (!data || data->type != mini::Value::Array) return -1;
  for (auto const& m : data->arr) {
    if (m.type != mini::Value::Object) continue;
    mini::Value const* id = m.get("id");
    if (id && id->type == mini::Value::String && !id->s.empty()) out.push_back(id->s);
  }
  return out.empty() ? -1 : 0;
}

namespace {

std::string base64Encode(std::string const& in) {
  static char const* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    uint32_t v = 0;
    int n = 0;
    for (size_t j = 0; j < 3 && i + j < in.size(); ++j, ++n) {
      v = (v << 8) | (unsigned char)in[i + j];
    }
    v <<= 8 * (3 - n);
    for (int k = 0; k < 4; ++k) {
      out.push_back(T[(v >> (6 * (3 - k))) & 0x3F]);
    }
    if (n == 1) { out[out.size() - 2] = '='; out[out.size() - 1] = '='; }
    else if (n == 2) { out[out.size() - 1] = '='; }
  }
  return out;
}

std::string serializeMessage(Message const& m) {
  mini::Value jm = mini::Value::makeObject();
  jm.set("role", mini::Value::makeString(m.role));
  if (!m.images.empty() && m.role == "user") {
    mini::Value content = mini::Value::makeArray();
    mini::Value textBlock = mini::Value::makeObject();
    textBlock.set("type", mini::Value::makeString("text"));
    textBlock.set("text", mini::Value::makeString(m.content));
    content.arr.push_back(std::move(textBlock));
    for (auto const& img : m.images) {
      mini::Value imageBlock = mini::Value::makeObject();
      imageBlock.set("type", mini::Value::makeString("image_url"));
      mini::Value imageUrl = mini::Value::makeObject();
      imageUrl.set("url", mini::Value::makeString(img));
      imageBlock.set("image_url", std::move(imageUrl));
      content.arr.push_back(std::move(imageBlock));
    }
    jm.set("content", std::move(content));
  } else {
    jm.set("content", mini::Value::makeString(m.content));
  }
  if (m.role == "tool") jm.set("tool_call_id", mini::Value::makeString(m.toolCallId));
  if (!m.toolCalls.empty()) {
    mini::Value tcs = mini::Value::makeArray();
    for (auto const& tc : m.toolCalls) {
      mini::Value jtc = mini::Value::makeObject();
      jtc.set("id", mini::Value::makeString(tc.id));
      jtc.set("type", mini::Value::makeString("function"));
      mini::Value fn = mini::Value::makeObject();
      fn.set("name", mini::Value::makeString(tc.name));
      fn.set("arguments", mini::Value::makeString(tc.arguments));
      jtc.set("function", fn);
      tcs.arr.push_back(std::move(jtc));
    }
    jm.set("tool_calls", tcs);
  }
  return mini::dump(jm);
}

// Fingerprint that detects in-place mutation (prune) vs plain appends: the
// last message's cheap identity. Good enough: prune changes the tail.
std::string msgFingerprint(Message const& m) {
  std::string fp = m.role;
  fp += '|';
  fp += m.content.size() > 64 ? m.content.substr(0, 64) : m.content;
  fp += '|';
  fp += m.toolCallId;
  size_t n = 0;
  for (auto const& tc : m.toolCalls)
    n += tc.id.size() + tc.name.size() + tc.arguments.size();
  fp += '|';
  fp += std::to_string(n);
  return fp;
}

// JSON for everything except "messages", without the closing '}' so the
// messages array can be spliced in place.
std::string bodyHeader(ChatOptions const& opts) {
  mini::Value root = mini::Value::makeObject();
  root.set("model", mini::Value::makeString(opts.model));
  if (opts.temperature != 0.0) root.set("temperature", mini::Value::makeDouble(opts.temperature));
  if (opts.maxTokens > 0) root.set("max_tokens", mini::Value::makeInt(opts.maxTokens));
  root.set("stream", mini::Value::makeBool(opts.stream));
  if (opts.jsonMode) {
    mini::Value fmt = mini::Value::makeObject();
    fmt.set("type", mini::Value::makeString("json_object"));
    root.set("response_format", fmt);
  }
  // Thinking strength. Level "auto" (the default) emits nothing, so requests
  // stay byte-identical for anyone who has not opted in.
  if (std::string lvl = thinkingWire(opts.thinking); !lvl.empty()) {
    std::string style = thinkingStyleFromString(opts.thinkingStyle);
    if (style == "effort") {
      root.set("reasoning_effort", mini::Value::makeString(lvl));
    } else if (style == "thinking") {
      mini::Value t = mini::Value::makeObject();
      t.set("type", mini::Value::makeString(lvl == "none" ? "disabled" : "enabled"));
      root.set("thinking", std::move(t));
    } else if (style == "enable_thinking") {
      root.set("enable_thinking", mini::Value::makeBool(lvl != "none"));
    } else if (style == "chat_template_kwargs") {
      mini::Value tk = mini::Value::makeObject();
      tk.set("enable_thinking", mini::Value::makeBool(lvl != "none"));
      root.set("chat_template_kwargs", std::move(tk));
    }
  }
  if (!opts.toolsJson.empty()) {
    mini::Value tv;
    if (mini::tryParse(opts.toolsJson, tv) && tv.type == mini::Value::Array) {
      root.set("tools", std::move(tv));
    }
  }
  std::string h = mini::dump(root);
  if (!h.empty() && h.back() == '}') h.pop_back();
  return h;
}

std::string assembleBody(std::string const& header, std::vector<std::string> const& msgs) {
  std::string out = header;
  out += ",\"messages\":[";
  for (size_t i = 0; i < msgs.size(); i++) {
    if (i) out += ',';
    out += msgs[i];
  }
  out += "]}";
  return out;
}

}  // namespace

std::string Client::buildRequestBody(std::vector<Message> const& messages, ChatOptions const& opts) const {
  std::vector<std::string> ser;
  ser.reserve(messages.size() + (opts.systemPrompt.empty() ? 0 : 1));
  if (!opts.systemPrompt.empty()) ser.push_back(serializeMessage({"system", opts.systemPrompt}));
  for (auto const& m : messages) ser.push_back(serializeMessage(m));
  return assembleBody(bodyHeader(opts), ser);
}

// Build the request body, reusing `cache` when the message tail was only
// appended since the last call (the common case inside a tool loop).
std::string buildCachedBody(std::vector<Message> const& messages, ChatOptions const& opts,
                            BodyCache& cache) {
  std::string header;
  bool headerOk = !cache.dirty && cache.header == (header = bodyHeader(opts));
  if (headerOk && cache.dataPtr == messages.data() && cache.count <= messages.size() &&
      (cache.count == messages.size() ||
       (cache.count < messages.size() && msgFingerprint(messages[cache.count - 1]) == cache.lastFp))) {
    // Tail append: serialize only the new messages.
    cache.msgs.reserve(messages.size());
    for (size_t i = cache.count; i < messages.size(); i++) cache.msgs.push_back(serializeMessage(messages[i]));
    cache.body = assembleBody(cache.header, cache.msgs);
    cache.count = messages.size();
  } else {
    // Full rebuild (mutation, restore, first use).
    std::string h = bodyHeader(opts);
    std::vector<std::string> ser;
    ser.reserve(messages.size() + (opts.systemPrompt.empty() ? 0 : 1));
    if (!opts.systemPrompt.empty()) ser.push_back(serializeMessage({"system", opts.systemPrompt}));
    for (auto const& m : messages) ser.push_back(serializeMessage(m));
    cache.header = h;
    cache.msgs = std::move(ser);
    cache.body = assembleBody(cache.header, cache.msgs);
    cache.count = messages.size();
    cache.dirty = false;
  }
  if (cache.count > 0)
    cache.lastFp = msgFingerprint(messages[cache.count - 1]);
  else
    cache.lastFp.clear();
  cache.dataPtr = messages.data();
  return cache.body;
}

namespace {

struct AccTool {
  std::string id;
  std::string name;
  std::string args;
};

// Streaming SSE line parser.
struct Sse {
  std::string lineBuf;
  std::string data;
  std::function<void(std::string const&)> onEvent;

  void feed(char const* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
      char c = p[i];
      if (c == '\n') {
        handleLine();
        lineBuf.clear();
      } else if (c != '\r') {
        lineBuf += c;
      }
    }
  }
  void handleLine() {
    if (lineBuf.empty()) {
      if (!data.empty() && onEvent) onEvent(data);
      data.clear();
      return;
    }
    if (lineBuf.compare(0, 6, "data: ") == 0) {
      if (!data.empty()) data += '\n';
      data += lineBuf.substr(6);
    }
  }
};

void emitError(std::function<void(StreamEvent const&)> onEvent, std::string msg) {
  StreamEvent ev;
  ev.kind = StreamKind::Error;
  ev.error = std::move(msg);
  if (onEvent) onEvent(ev);
}

// Pull token usage out of an OpenAI/Ollama "usage" object.
void extractUsage(mini::Value const* u, int64_t& pt, int64_t& ct, int64_t& cd) {
  if (!u) return;
  if (auto* p = u->get("prompt_tokens")) pt = p->asInt();
  if (auto* c = u->get("completion_tokens")) ct = c->asInt();
  if (auto* d = u->get("prompt_tokens_details")) {
    if (auto* cv = d->get("cached_tokens")) cd = cv->asInt();
  }
  if (!cd) {
    if (auto* oh = u->get("prompt_cache_hit_tokens")) cd = oh->asInt();
  }
}

// Serialize the final assistant reply for the LLM response cache (V2-E:
// reasoning is stored too, so a hit saves the whole thinking round).
std::string serializeReply(std::string const& content, std::string const& reasoning,
                           std::string const& finishReason, std::vector<AccTool> const& acc) {
  mini::Value v = mini::Value::makeObject();
  v.set("content", mini::Value::makeString(content));
  v.set("reasoning", mini::Value::makeString(reasoning));
  v.set("finish_reason", mini::Value::makeString(finishReason));
  mini::Value tcs = mini::Value::makeArray();
  for (auto const& a : acc) {
    if (a.id.empty() && a.name.empty()) continue;
    mini::Value t = mini::Value::makeObject();
    t.set("id", mini::Value::makeString(a.id));
    t.set("name", mini::Value::makeString(a.name));
    t.set("arguments", mini::Value::makeString(a.args));
    tcs.arr.push_back(std::move(t));
  }
  v.set("tool_calls", std::move(tcs));
  return mini::dump(v);
}

// Replay a cached reply as the same event sequence a live call would emit:
// Reasoning -> Delta (chunked) -> ToolCall(s) -> push assistant message -> Done.
int replayCachedReply(std::string const& json, std::vector<Message>& messages,
                      std::function<void(StreamEvent const&)> onEvent) {
  mini::Value v;
  if (!mini::tryParse(json, v) || v.type != mini::Value::Object) return -100;

  if (mini::Value const* rc = v.get("reasoning")) {
    std::string t = rc->asString();
    if (!t.empty()) {
      StreamEvent ev;
      ev.kind = StreamKind::Reasoning;
      ev.text = t;
      if (onEvent) onEvent(ev);
    }
  }
  std::string content;
  if (mini::Value const* c = v.get("content")) content = c->asString();
  // Chunk the delta for a streaming look (~60 chars per frame feel).
  for (size_t i = 0; i < content.size(); i += 512) {
    StreamEvent ev;
    ev.kind = StreamKind::Delta;
    ev.text = content.substr(i, 512);
    if (onEvent) onEvent(ev);
  }

  Message am;
  am.role = "assistant";
  am.content = content;
  if (mini::Value const* tcs = v.get("tool_calls"); tcs && tcs->type == mini::Value::Array) {
    for (auto const& tc : tcs->arr) {
      ToolCall t;
      if (auto* id = tc.get("id")) t.id = id->asString();
      if (auto* nm = tc.get("name")) t.name = nm->asString();
      if (auto* ag = tc.get("arguments")) t.arguments = ag->asString();
      if (!t.id.empty() || !t.name.empty()) {
        StreamEvent ev;
        ev.kind = StreamKind::ToolCall;
        ev.toolName = t.name;
        ev.toolArgs = t.arguments;
        if (onEvent) onEvent(ev);
        am.toolCalls.push_back(std::move(t));
      }
    }
  }
  if (!content.empty() || !am.toolCalls.empty()) messages.push_back(std::move(am));

  StreamEvent done;
  done.kind = StreamKind::Done;
  if (auto* fr = v.get("finish_reason")) done.finishReason = fr->asString();
  if (onEvent) onEvent(done);
  return 0;
}

}  // namespace

int Client::chat(std::vector<Message>& messages, ChatOptions const& opts,
                 std::function<void(StreamEvent const&)> onEvent, BodyCache* cache) {
  // Local response cache: identical request (model + opts + full history)
  // replays the stored reply without touching the network.
  std::string cacheKey;
  if (respCache_) {
    cacheKey = DiskCache::hashRequest(messages, opts);
    std::string hit;
    if (respCache_->get(cacheKey, hit)) {
      int rc = replayCachedReply(hit, messages, onEvent);
      if (rc == 0) return 0;  // corrupt entry: fall through to a live call
    }
  }
  std::string body = cache ? buildCachedBody(messages, opts, *cache) : buildRequestBody(messages, opts);
  std::string url = baseUrl_;
  if (!url.empty() && url.back() != '/') url += '/';
  url += "chat/completions";

  HttpRequest req;
  req.url = std::move(url);
  req.method = "POST";
  req.body = std::move(body);
  req.headers.emplace_back("Content-Type", "application/json");
  if (!apiKey_.empty()) req.headers.emplace_back("Authorization", "Bearer " + apiKey_);

  std::vector<AccTool> acc;
  std::string content;
  std::string reasoning;  // accumulated for the response cache (V2-E)
  std::string finishReason;
  bool gotError = false;
  int status = 0;

  auto t0 = std::chrono::steady_clock::now();
  int64_t firstTokMs = -1;
  auto markFirst = [&]() {
    if (firstTokMs < 0)
      firstTokMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
  };
  int64_t uPrompt = 0, uCompletion = 0, uCached = 0;

  auto recordUsage = [&](mini::Value const* u) {
    if (u) {
      int64_t pt = 0, ct = 0, cd = 0;
      extractUsage(u, pt, ct, cd);
      uPrompt = pt;
      uCompletion = ct;
      uCached = cd;
    }
  };

  if (opts.stream) {
    Sse sse;
    sse.onEvent = [&](std::string const& data) {
      if (data == "[DONE]") return;
      mini::Value v;
      if (!mini::tryParse(data, v)) return;
      if (v.has("error")) {
        gotError = true;
        emitError(onEvent, mini::dump(*v.get("error")));
        return;
      }
      recordUsage(v.get("usage"));
      mini::Value const* choices = v.get("choices");
      if (!choices || choices->arr.empty()) return;
      mini::Value const& choice = choices->arr[0];
      if (auto* fr = choice.get("finish_reason")) finishReason = fr->asString();
      mini::Value const* delta = choice.get("delta");
      if (!delta) return;
      mini::Value const* rc = delta->get("reasoning_content");
      if (!rc) rc = delta->get("reasoning");
      if (rc) {
        std::string t = rc->asString();
        if (!t.empty()) {
          if (respCache_) reasoning += t;
          markFirst();
          StreamEvent ev;
          ev.kind = StreamKind::Reasoning;
          ev.text = t;
          if (onEvent) onEvent(ev);
        }
      }
      if (auto* c = delta->get("content")) {
        std::string t = c->asString();
        content += t;
        if (!t.empty()) {
          markFirst();
          StreamEvent ev;
          ev.kind = StreamKind::Delta;
          ev.text = t;
          if (onEvent) onEvent(ev);
        }
      }
      mini::Value const* tcs = delta->get("tool_calls");
      if (tcs && tcs->type == mini::Value::Array) {
        markFirst();
        for (auto const& tc : tcs->arr) {
          int idx = tc.get("index") ? (int)tc.get("index")->asInt() : 0;
          if (idx < 0) idx = 0;
          if ((int)acc.size() <= idx) acc.resize(idx + 1);
          if (auto* id = tc.get("id")) {
            std::string idv = id->asString();
            if (!idv.empty()) acc[idx].id = idv;
          }
          if (auto* fn = tc.get("function")) {
            if (auto* nm = fn->get("name")) {
              std::string nmv = nm->asString();
              if (!nmv.empty()) acc[idx].name = nmv;
            }
            if (auto* args = fn->get("arguments")) acc[idx].args += args->asString();
          }
        }
      }
    };
    status = httpRequest(req, [&](char const* p, size_t n) { sse.feed(p, n); });
    sse.feed("\n", 1);  // flush a trailing, possibly unterminated data line
  } else {
    // non-streaming
    std::string chunkBuffer;
    status = httpRequest(req, [&](char const* p, size_t n) { chunkBuffer.append(p, n); });
    mini::Value v;
    if (mini::tryParse(chunkBuffer, v)) {
      if (v.has("error")) {
        gotError = true;
        emitError(onEvent, mini::dump(*v.get("error")));
      } else if (auto* choices = v.get("choices"); choices && !choices->arr.empty()) {
        recordUsage(v.get("usage"));
        mini::Value const& choice = choices->arr[0];
        if (auto* fr = choice.get("finish_reason")) finishReason = fr->asString();
        if (auto* msg = choice.get("message")) {
          mini::Value const* rc = msg->get("reasoning_content");
          if (!rc) rc = msg->get("reasoning");
          if (rc) {
            std::string t = rc->asString();
            if (!t.empty()) {
              if (respCache_) reasoning += t;
              StreamEvent ev;
              ev.kind = StreamKind::Reasoning;
              ev.text = t;
              if (onEvent) onEvent(ev);
            }
          }
          if (auto* c = msg->get("content")) content = c->asString();
          if (auto* tcs = msg->get("tool_calls"); tcs && tcs->type == mini::Value::Array) {
            for (auto const& tc : tcs->arr) {
              AccTool a;
              if (auto* id = tc.get("id")) a.id = id->asString();
              if (auto* fn = tc.get("function")) {
                if (auto* nm = fn->get("name")) a.name = nm->asString();
                if (auto* args = fn->get("arguments")) a.args = args->asString();
              }
              acc.push_back(a);
            }
          }
        }
      }
    } else {
      gotError = true;
      std::string snippet = chunkBuffer.size() > 200 ? chunkBuffer.substr(0, 200) : chunkBuffer;
      emitError(onEvent, "invalid JSON response: " + snippet);
    }
  }

  if (status < 0) {
    emitError(onEvent, "network error: " + url + "  " + httpLastErrorDetail());
    recordChat(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count(),
               firstTokMs < 0 ? 0 : firstTokMs, 0, 0, 0);
    return -1;
  }

  // Emit each tool call once, with its fully accumulated arguments (SSE delivers
  // function.arguments in many small chunks).
  for (auto const& a : acc) {
    if (a.id.empty() && a.name.empty()) continue;
    StreamEvent ev;
    ev.kind = StreamKind::ToolCall;
    ev.toolName = a.name;
    ev.toolArgs = a.args;
    if (onEvent) onEvent(ev);
  }

  if (gotError) return status == 200 ? 0 : status;

  // Store only successful, complete replies; errors never enter the cache.
  if (respCache_ && !cacheKey.empty() && status == 200 && finishReason != "cancelled" &&
      (!content.empty() || !acc.empty())) {
    respCache_->put(cacheKey, serializeReply(content, reasoning, finishReason, acc));
  }

  Message am;
  am.role = "assistant";
  am.content = content;
  for (auto& a : acc) {
    if (!a.id.empty() || !a.name.empty()) am.toolCalls.push_back({a.id, a.name, a.args});
  }
  if (!content.empty() || !am.toolCalls.empty()) messages.push_back(std::move(am));

  StreamEvent done;
  done.kind = StreamKind::Done;
  done.finishReason = finishReason;
  if (onEvent) onEvent(done);
  int64_t llmMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  recordChat(llmMs, firstTokMs < 0 ? llmMs : firstTokMs, uPrompt, uCompletion,
             uCached);
  return status == 200 ? 0 : status;
}

int runTurn(Client& client, std::vector<Message>& messages, ChatOptions const& opts,
            std::function<std::string(std::string const&, std::string const&)> executor,
            std::function<void(StreamEvent const&)> onEvent, int maxToolLoops) {
  BodyCache bodyCache;
  static constexpr int kConcludes = 3;  // concluding nudges after the budget is out
  int loop = 0;
  int concludes = 0;
  // Non-fatal process notices (dup calls, retries, auto-conclude) are status
  // lines, not errors: only real failures should ever show in red.
  auto note = [&](std::string const& text) {
    StreamEvent ev;
    ev.kind = StreamKind::ToolResult;
    ev.toolName = "context";
    ev.toolResult = text;
    if (onEvent) onEvent(ev);
  };
  for (; ; loop++) {
    if (maxToolLoops > 0) {
      if (loop >= maxToolLoops) {
        if (concludes >= kConcludes) break;
        concludes++;
        messages.push_back({"user",
                            "工具调用次数已达上限。请立即停止调用任何工具,直接给出最终回答;"
                            "若确实还差关键信息,在回答末尾用一句\"补充:\"说明缺什么、下一步怎么做。"});
        note("工具循环上限 " + std::to_string(maxToolLoops) + " 已到,自动收尾 (" +
             std::to_string(concludes) + "/" + std::to_string(kConcludes) + "):要求模型直接作答");
      }
    }
    if (opts.prune && pruneBeforeSend(messages, true)) bodyCache.invalidate();
    int rc = client.chat(messages, opts, onEvent, &bodyCache);
    if (rc != 0 && isTransientChatError(rc)) {
      // Transient API/network failure: back off and retry a few times before
      // giving up on the whole turn.
      int attempts = 0;
      while (rc != 0 && isTransientChatError(rc) && attempts < kMaxChatRetries) {
        attempts++;
        std::this_thread::sleep_for(std::chrono::milliseconds(600 * attempts));
        note("网络/服务瞬时故障(rc " + std::to_string(rc) + "),重试 (" +
             std::to_string(attempts) + "/" + std::to_string(kMaxChatRetries) + ")");
        rc = client.chat(messages, opts, onEvent, &bodyCache);
      }
    }
    if (rc != 0) return rc;
    if (messages.empty() || messages.back().role != "assistant" || messages.back().toolCalls.empty()) {
      return 0;  // final answer received
    }
    Message& last = messages.back();
    std::unordered_map<std::string, std::string> seenCalls;
    for (auto const& tc : last.toolCalls) {
      std::string result;
      std::string callKey = tc.name;
      callKey += '\x01';
      callKey += tc.arguments;
      auto seen = seenCalls.find(callKey);
      if (seen != seenCalls.end()) {
        // The model repeated the exact same tool call within this batch: reuse
        // the previous outcome and tell it, so it stops burning loop budget.
        result = seen->second;
        if (result.size() >= 2 && result[0] == '{') {
          result.insert(1, "\"_dup\":true,");
        } else {
          result = "{\"_dup\":true,\"note\":\"identical call previously executed in this "
                   "batch; result reused above\",\"result\":" + result + "}";
        }
        if (onEvent) {
          StreamEvent ev;
          ev.kind = StreamKind::ToolResult;
          ev.toolName = "context";
          ev.toolResult = "duplicate call " + tc.name + " (same args) skipped, reusing prior result";
          onEvent(ev);
        }
      } else if (executor) {
        for (int attempt = 0;; attempt++) {
          result = executor(tc.name, tc.arguments);
          if (attempt >= kMaxToolRetries || !isTransientToolError(result)) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(700 * (attempt + 1)));
          if (onEvent) {
            StreamEvent ev;
            ev.kind = StreamKind::ToolResult;
            ev.toolName = "context";
            ev.toolResult = "transient tool error, retrying " + tc.name + " (" +
                            std::to_string(attempt + 1) + "/" + std::to_string(kMaxToolRetries) + ")";
            onEvent(ev);
          }
        }
        seenCalls.emplace(callKey, result);
      } else {
        result = "{\"error\":\"no executor registered\"}";
      }
      std::string screenshotPath;
      {
        mini::Value rv;
        if (mini::tryParse(result, rv) && rv.type == mini::Value::Object) {
          if (mini::Value const* p = rv.get("path")) screenshotPath = p->asString();
        }
      }
      if (opts.toolResultCap > 0) result = capToolResult(std::move(result), opts.toolResultCap);
      Message tm;
      tm.role = "tool";
      tm.toolCallId = tc.id;
      tm.content = std::move(result);
      StreamEvent ev;
      ev.kind = StreamKind::ToolResult;
      ev.toolName = tc.name;
      ev.toolResult = tm.content;
      if (onEvent) onEvent(ev);
      messages.push_back(std::move(tm));
      // Multimodal: if the model just took a screenshot, inject a synthetic
      // user message carrying the image as a data URI so vision-capable models
      // can actually see it on their next turn.
      if (tc.name == "browser_screenshot" && !screenshotPath.empty()) {
        std::ifstream in(screenshotPath, std::ios::binary);
        if (in) {
          std::stringstream ss;
          ss << in.rdbuf();
          Message im;
          im.role = "user";
          im.content = "Screenshot saved: " + screenshotPath;
          im.images.push_back("data:image/png;base64," + base64Encode(ss.str()));
          messages.push_back(std::move(im));
        }
      }
    }
  }
  emitError(onEvent, "tool loop limit reached");
  return -2;
}

}  // namespace agent
