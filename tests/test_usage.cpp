// Session usage stats: recording, formatting, and the one-line summary.
#include "agent/usage.hpp"
#include <cstdio>
#include <string>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                     \
  } while (0)

static void test_format_duration() {
  CHECK(agent::formatDuration(0) == "0.0s");
  CHECK(agent::formatDuration(800) == "0.8s");
  CHECK(agent::formatDuration(3200) == "3.2s");
  CHECK(agent::formatDuration(6404000) == "106m44s");
  CHECK(agent::formatDuration(60000) == "1m00s");
  CHECK(agent::formatDuration(8900) == "8.9s");
  CHECK(agent::formatDuration(-5) == "0.0s");
}

static void test_format_tokens() {
  CHECK(agent::formatTokens(0) == "0");
  CHECK(agent::formatTokens(1234) == "1.2K");
  CHECK(agent::formatTokens(270000) == "270K");
  CHECK(agent::formatTokens(58300000) == "58.3M");
  CHECK(agent::formatTokens(999) == "999");
}

static void test_format_usage() {
  agent::SessionUsage u;
  u.turns = 12;
  u.steps = 236;
  u.llmMs = 6404000;
  u.toolMs = 3200;
  u.firstTokenSumMs = 8900;
  u.calls = 1;
  u.promptTokens = 58300000;
  u.completionTokens = 270000;
  u.cachedTokens = 58300000;
  std::string s = agent::formatUsage(u);
  CHECK(s.find("12 轮") != std::string::npos);
  CHECK(s.find("236 步") != std::string::npos);
  CHECK(s.find("LLM 106m44s") != std::string::npos);
  CHECK(s.find("工具 3.2s") != std::string::npos);
  CHECK(s.find("首 token 平均 8.9s") != std::string::npos);
  CHECK(s.find("42 tok/s") != std::string::npos);  // 270000 / 6404s = 42.2
}

static void test_record_accumulate() {
  agent::resetUsage();
  agent::recordTurn();
  agent::recordTurn();
  agent::recordTurn();
  agent::recordToolRun(100);
  agent::recordToolRun(250);
  agent::recordChat(1000, 100, 5000, 200, 500);
  agent::recordChat(2000, 200, 3000, 300, 0);
  agent::SessionUsage u = agent::snapshotUsage();
  CHECK(u.turns == 3);
  CHECK(u.steps == 2);
  CHECK(u.toolMs == 350);
  CHECK(u.llmMs == 3000);
  CHECK(u.calls == 2);
  CHECK(u.firstTokenSumMs == 300);
  CHECK(u.promptTokens == 8000);
  CHECK(u.completionTokens == 500);
  CHECK(u.cachedTokens == 500);
  // tok/s = 500 / 3.0s = 166.7 -> 167
  std::string s = agent::formatUsage(u);
  CHECK(s.find("167 tok/s") != std::string::npos);
  CHECK(s.find("缓存命中 6%") != std::string::npos);  // 500/8000
  agent::resetUsage();
}

static void test_cost_and_cache() {
  agent::SessionUsage u;
  u.promptTokens = 1000000;
  u.completionTokens = 100000;
  u.cachedTokens = 800000;
  double c = agent::estimateCostUsd(u, 0.15, 0.60);
  CHECK(c > 0.20 && c < 0.22);  // 0.15 + 0.06 = 0.21
  CHECK(agent::formatUsd(0.21) == "$0.21");
  CHECK(agent::formatUsd(12.345) == "$12.35");
  CHECK(agent::formatUsd(2500) == "$2500");
  CHECK(agent::cacheHitPct(u) == 80);
  agent::SessionUsage empty;
  CHECK(agent::cacheHitPct(empty) == -1);
  CHECK(agent::estimateCostUsd(empty, 1, 1) == 0.0);
}

int main() {
  std::fprintf(stderr, "step1: duration\n");
  test_format_duration();
  std::fprintf(stderr, "step2: tokens\n");
  test_format_tokens();
  std::fprintf(stderr, "step3: usage line\n");
  test_format_usage();
  std::fprintf(stderr, "step4: accumulate\n");
  test_record_accumulate();
  std::fprintf(stderr, "step5: cost + cache\n");
  test_cost_and_cache();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}