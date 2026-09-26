#pragma once
#include <cstdint>
#include <string>

namespace agent {

// Session-wide usage counters (thread-safe). The client records LLM call
// timing and token usage, the app records tool timing and turn counts, and
// the UI renders them with formatUsage().
struct SessionUsage {
  int64_t turns = 0;             // user turns started
  int64_t steps = 0;             // tool invocations
  int64_t llmMs = 0;             // wall time in chat API calls
  int64_t toolMs = 0;            // wall time executing tools
  int64_t firstTokenSumMs = 0;   // first-token latencies, summed
  int64_t calls = 0;             // chat API calls
  int64_t promptTokens = 0;      // input tokens
  int64_t completionTokens = 0;  // output tokens
  int64_t cachedTokens = 0;      // prompt tokens served from cache
};

// Thread-safe snapshot of the global session usage.
SessionUsage snapshotUsage();
void recordTurn();
void recordToolRun(int64_t ms);
// llmMs: total wall time of one chat call; firstTokenMs: time to the first
// content/reasoning/tool-call delta (== llmMs when unknown).
void recordChat(int64_t llmMs, int64_t firstTokenMs, int64_t promptTokens,
                int64_t completionTokens, int64_t cachedTokens);
void resetUsage();

// "106m44s" / "12.3s" / "0.8s"
std::string formatDuration(int64_t ms);
// "58.3M" / "270K" / "1234"
std::string formatTokens(int64_t n);
// Estimated USD cost: prompt/1e6*inPerM + completion/1e6*outPerM.
double estimateCostUsd(SessionUsage const& u, double inPerM, double outPerM);
// "$0.42" / "$12.34" / "$1.2k"
std::string formatUsd(double v);
// Cache hit rate in percent (0..100), or -1 when no prompt tokens were seen.
int cacheHitPct(SessionUsage const& u);
// One-line session summary:
// "12 轮 · 236 步 | LLM 106m44s · 工具 3.2s | 首 token 平均 8.9s · 62 tok/s | 缓存命中 100% | 输入 58.3M · 输出 270K"
std::string formatUsage(SessionUsage const& u);

}  // namespace agent
