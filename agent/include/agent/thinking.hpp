#pragma once
#include <string>
#include <vector>

namespace agent {

// Model thinking-strength control. Two independent knobs:
//   level - how much the model should think: auto | none | minimal | low |
//           medium | high. "auto" sends no field at all, so an unconfigured
//           model gets byte-identical requests to the pre-feature behavior.
//   style - how a provider spells it on the wire (see thinkingStyles()).

inline std::vector<std::string> thinkingLevels() {
  return {"auto", "none", "minimal", "low", "medium", "high"};
}

// "" (unset) and an unrecognized value both normalize to "auto".
inline std::string thinkingLevelFromString(std::string const& s) {
  for (auto const& l : thinkingLevels())
    if (l == s) return s;
  return "auto";
}

// Wire value for the "effort" style; empty = omit the field.
inline std::string thinkingWire(std::string const& level) {
  std::string l = thinkingLevelFromString(level);
  return l == "auto" ? std::string() : l;
}

// Providers that accept none of these get no thinking field whatsoever.
//   effort          reasoning_effort: "low"            (OpenAI, DeepSeek, OpenRouter)
//   thinking        thinking: {type: enabled|disabled} (Zhipu GLM, Moonshot Kimi)
//   enable_thinking enable_thinking: true              (Qwen / DashScope / vLLM)
//   chat_template_kwargs  chat_template_kwargs: {enable_thinking: true}
//                                                  (Agnes AI, some vLLM deployments)
//   none            -                                  (endpoint cannot control it)
inline std::vector<std::string> thinkingStyles() {
  return {"effort", "thinking", "enable_thinking", "chat_template_kwargs", "none"};
}

// "" (unset) and an unrecognized value both normalize to "effort".
inline std::string thinkingStyleFromString(std::string const& s) {
  for (auto const& s2 : thinkingStyles())
    if (s2 == s) return s;
  return "effort";
}

// The binary styles cannot express a strength; they only know on/off.
inline bool thinkingStyleIsBinary(std::string const& style) {
  std::string s = thinkingStyleFromString(style);
  return s == "thinking" || s == "enable_thinking" || s == "chat_template_kwargs";
}

}  // namespace agent
