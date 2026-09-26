#pragma once
#include "agent/types.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {

class DiskCache;

// Between tool-loop iterations a turn re-sends the full conversation. Most of
// the payload is unchanged, so chat() accepts a caller-owned cache: it stores
// the serialized header + per-message JSON objects and only re-serializes the
// appended tail. `invalidate()` must be called whenever messages are mutated
// in place (e.g. prune) instead of appended.
struct BodyCache {
  std::string header;             // JSON before "messages":[...]
  std::vector<std::string> msgs;  // one JSON object string per message
  std::string body;               // last full request body
  size_t count = (size_t)-1;      // messages.size() the cache was built for
  std::string lastFp;             // fingerprint of messages[count-1]
  const void* dataPtr = nullptr;  // messages.data() at build time
  bool dirty = true;
  void invalidate() { dirty = true; count = (size_t)-1; }
};

// OpenAI-compatible chat completions client (also speaks Ollama's
// /v1/chat/completions endpoint). Text-first: no image/multimodal payloads.
class Client {
 public:
  // baseUrl e.g. "http://localhost:11434/v1" or "https://api.openai.com/v1".
  bool configure(std::string baseUrl, std::string apiKey);
  std::string const& baseUrl() const { return baseUrl_; }

  // Low-level chat. Appends the final assistant message (with any tool_calls)
  // to `messages`. onEvent receives stream events (deltas, tool calls, errors).
  // Returns 0 on success, negative on transport error, HTTP status on API error.
  // Optional `cache` avoids re-serializing the unchanged history each call
  // (see BodyCache above).
  int chat(std::vector<Message>& messages, ChatOptions const& opts,
           std::function<void(StreamEvent const&)> onEvent, BodyCache* cache = nullptr);

  // GET {base}/models, filling `out` with the available model ids in the
  // server's order. Returns 0 on success, -1 on transport/parse error,
  // HTTP status code on API error.
  int listModels(std::vector<std::string>& out) const;

  // Local LLM response cache (nullptr disables; default). On a hit the cached
  // reply is replayed as stream events without any network request; on a
  // successful miss the reply is stored (errors are never cached).
  void setCache(std::shared_ptr<DiskCache> c) { respCache_ = std::move(c); }
  DiskCache* cache() const { return respCache_.get(); }

 private:
  std::string buildRequestBody(std::vector<Message> const& messages, ChatOptions const& opts) const;
  int handleResponse(bool stream, std::vector<Message>& messages, ChatOptions const& opts,
                     std::function<void(StreamEvent const&)> onEvent, int status,
                     std::function<void(char const*, size_t)> feedChunks) const;

  std::string baseUrl_;
  std::string apiKey_;
  std::shared_ptr<DiskCache> respCache_;
};

// One full agent turn: chat -> execute tool calls via `executor` -> repeat
// until a final text answer. Appends every message to `messages`.
// Returns 0 on success; negative on error/limit.
// `maxToolLoops` caps the tool-call iterations; 0 (or negative) means no cap
// (the turn keeps going until the model stops calling tools on its own).
int runTurn(Client& client, std::vector<Message>& messages, ChatOptions const& opts,
            std::function<std::string(std::string const& name, std::string const& argsJson)> executor,
            std::function<void(StreamEvent const&)> onEvent, int maxToolLoops = 0);

}  // namespace agent
