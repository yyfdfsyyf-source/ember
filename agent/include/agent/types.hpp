#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace agent {

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;  // JSON string
};

// role: system | user | assistant | tool
struct Message {
  std::string role;
  std::string content;
  std::string toolCallId;            // role == "tool"
  std::vector<ToolCall> toolCalls;   // role == "assistant"
  // Multimodal: base64 data URIs ("data:image/png;base64,....") attached to a
  // user message so vision-capable models can actually see images (e.g. a
  // browser_screenshot result). Emitted as an OpenAI image_url content block.
  std::vector<std::string> images;

  Message() = default;
  Message(std::string r, std::string c, std::string tcid = "", std::vector<ToolCall> tcs = {})
      : role(std::move(r)), content(std::move(c)), toolCallId(std::move(tcid)), toolCalls(std::move(tcs)) {}
};

struct ChatOptions {
  std::string model;
  double temperature = 0.7;
  int maxTokens = 0;
  bool stream = true;
  bool jsonMode = false;                  // response_format = {type:"json_object"}
  std::string thinking;                   // "auto" = send nothing; else see thinking.hpp
  std::string thinkingStyle;              // "" = "effort"; the provider's wire style
  std::vector<std::string> toolNames;     // subset of registered tools to advertise
  std::string toolsJson;                  // pre-built "tools" JSON array (from ToolRegistry)
  std::string systemPrompt;               // optional system message prepended
  size_t toolResultCap = 0;               // bytes; 0 = unlimited (tool-result cap)
  bool prune = true;                      // drop redundant tool output before sending
};

enum class StreamKind : uint8_t {
  Delta,
  Reasoning,   // model "thinking" text (reasoning_content / reasoning)
  ToolCall,
  ToolResult,
  Done,
  Error,
  TurnComplete,  // terminal marker pushed by the app worker (not the client)
  Background,    // a background task finished on its own (or timed out)
  Models,        // async /model fetch done; turnRc=0 ok, text=joined model ids
};

struct StreamEvent {
  StreamKind kind = StreamKind::Delta;
  std::string text;        // Delta: appended text
  std::string toolName;    // ToolCall
  std::string toolArgs;    // ToolCall: (partial) arguments JSON
  std::string toolResult;  // ToolResult: text returned to the model
  std::string finishReason;
  std::string error;
  int turnRc = 0;                       // TurnComplete: result code from runTurn
  std::vector<Message> turnMessages;    // TurnComplete: final conversation state
};

}  // namespace agent
