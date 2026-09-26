#pragma once
#include "agent/client.hpp"
#include "minijson.hpp"
#include <functional>
#include <string>
#include <vector>

namespace agent {

// Rough token estimate tuned for mixed CJK/Latin text.
// CJK chars count ~1 token each, latin ~4 chars per token.
size_t estimateTokens(std::string const& text);

// Sum over the whole conversation (roles + content + tool payloads).
size_t estimateMessagesTokens(std::vector<Message> const& messages);

// Cap a tool result before it is appended to the conversation. `cap` is the
// byte budget (0 = unlimited). JSON results have every string field truncated
// individually so the JSON stays valid; plain-text results are tail-truncated
// with a marker appended.
std::string capToolResult(std::string result, size_t cap);

// Drop redundancy before a request is sent: consecutive tool-result messages
// that repeat the same tool call id (the model already saw them) are removed,
// runaway user notices are capped, and conversational filler words ("好的",
// "嗯", "um", "well", ...) are stripped from assistant prose. Never splits a
// tool-call chain and never touches code fences, user messages, or tool
// payloads. Runs only when `enabled` is true. Returns whether any messages
// were touched.
bool pruneBeforeSend(std::vector<Message>& messages, bool enabled);

// Strip conversational filler words from one text block: standalone filler
// lines and leading filler tokens ("好的,", "嗯", "um,", "well,"...) are
// removed, code fences (``` blocks) are preserved verbatim. Safe to apply to
// assistant prose before sending: meaningful words are never damaged because
// filler must be bounded by line boundaries or separators.
std::string stripFillerWords(std::string const& text);

// Convert model markdown-ish output into safe plain text for the TUI:
// strips markdown emphasis/fences/links, keeps code and lists readable,
// and removes any ANSI escape sequences.
std::string toPlainText(std::string const& in);

// Cheap, stateless sanitizer for streaming chunks: removes ANSI escape
// sequences and other control bytes (keeps \n and \t). Safe to apply per
// network chunk without breaking markdown markers across chunks.
std::string stripAnsi(std::string const& in);

// If the conversation exceeds `budgetTokens`, summarize the oldest safe prefix
// with the model itself and replace it with a compact system note. Never
// touches a leading system prompt and never splits a tool-call chain.
// `onNote` receives progress messages for the UI. Returns true if it ran.
bool compactConversation(Client& client, std::vector<Message>& messages, ChatOptions const& opts,
                         size_t budgetTokens,
                         std::function<void(std::string const& note)> onNote = {});

// Like runTurn, but forces structured output: the final assistant answer must
// parse as JSON (and pass the optional `validator`). On failure a corrective
// user message is appended and the turn retried, up to `maxRetries`.
// Returns 0 on success, negative on error/limit.
int runTurnStructured(
    Client& client, std::vector<Message>& messages, ChatOptions const& opts,
    std::function<std::string(std::string const& name, std::string const& argsJson)> executor,
    std::function<void(StreamEvent const&)> onEvent,
    std::function<bool(mini::Value const&)> validator = {}, int maxRetries = 2);

// Retry-policy helpers used by runTurn (implemented in api.cpp): true when the
// failure is transient and worth an automatic retry.
bool isTransientChatError(int rc);
bool isTransientToolError(std::string const& result);

}  // namespace agent
