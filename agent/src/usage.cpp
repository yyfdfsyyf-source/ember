#include "agent/usage.hpp"
#include <cstdio>
#include <mutex>

namespace agent {

namespace {
std::mutex gMu;
SessionUsage gU;
}  // namespace

SessionUsage snapshotUsage() {
  std::lock_guard lk(gMu);
  return gU;
}

void recordTurn() {
  std::lock_guard lk(gMu);
  gU.turns++;
}

void recordToolRun(int64_t ms) {
  std::lock_guard lk(gMu);
  gU.steps++;
  gU.toolMs += ms;
}

void recordChat(int64_t llmMs, int64_t firstTokenMs, int64_t promptTokens,
                int64_t completionTokens, int64_t cachedTokens) {
  std::lock_guard lk(gMu);
  gU.llmMs += llmMs;
  gU.calls++;
  gU.firstTokenSumMs += firstTokenMs;
  gU.promptTokens += promptTokens;
  gU.completionTokens += completionTokens;
  gU.cachedTokens += cachedTokens;
}

void resetUsage() {
  std::lock_guard lk(gMu);
  gU = SessionUsage();
}

std::string formatDuration(int64_t ms) {
  if (ms < 0) ms = 0;
  int64_t sec = ms / 1000;
  if (sec >= 60) {
    char b[32];
    snprintf(b, sizeof b, "%lldm%02llds", (long long)(sec / 60),
             (long long)(sec % 60));
    return b;
  }
  char b[32];
  snprintf(b, sizeof b, "%.1fs", ms / 1000.0);
  return b;
}

std::string formatTokens(int64_t n) {
  if (n < 0) n = 0;
  char b[32];
  if (n >= 1000000) {
    snprintf(b, sizeof b, "%.1fM", n / 1000000.0);
  } else if (n >= 10000) {
    snprintf(b, sizeof b, "%.0fK", n / 1000.0);
  } else if (n >= 1000) {
    snprintf(b, sizeof b, "%.1fK", n / 1000.0);
  } else {
    snprintf(b, sizeof b, "%lld", (long long)n);
  }
  return b;
}

double estimateCostUsd(SessionUsage const& u, double inPerM, double outPerM) {
  return (double)u.promptTokens / 1e6 * inPerM +
         (double)u.completionTokens / 1e6 * outPerM;
}

std::string formatUsd(double v) {
  if (v < 0) v = 0;
  char b[32];
  if (v >= 1000) {
    snprintf(b, sizeof b, "$%.0f", v);
  } else {
    snprintf(b, sizeof b, "$%.2f", v);
  }
  return b;
}

int cacheHitPct(SessionUsage const& u) {
  if (u.promptTokens <= 0) return -1;
  return (int)(u.cachedTokens * 100 / u.promptTokens);
}

std::string formatUsage(SessionUsage const& u) {
  int64_t avgFirst = u.calls > 0 ? u.firstTokenSumMs / u.calls : 0;
  double toksPerSec =
      u.llmMs > 0 ? (double)u.completionTokens / ((double)u.llmMs / 1000.0) : 0.0;
  int cachePct = cacheHitPct(u);
  std::string s = std::to_string(u.turns) + " 轮 · " +
                  std::to_string(u.steps) + " 步 | LLM " +
                  formatDuration(u.llmMs) + " · 工具 " +
                  formatDuration(u.toolMs) + " | 首 token 平均 " +
                  formatDuration(avgFirst) + " · " +
                  std::to_string((long long)(toksPerSec + 0.5)) + " tok/s | 缓存命中 " +
                  (cachePct < 0 ? "--" : std::to_string(cachePct) + "%") +
                  " | 输入 " + formatTokens(u.promptTokens) + " · 输出 " +
                  formatTokens(u.completionTokens);
  return s;
}

}  // namespace agent