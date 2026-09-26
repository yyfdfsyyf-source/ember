// Tests for token estimation and plain-text rendering (no network needed).
#include "agent/context.hpp"
#include <cstdio>

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

int main() {
  // token estimation
  CHECK(agent::estimateTokens("") == 1);
  CHECK(agent::estimateTokens("hello world this is a test") < 10);
  CHECK(agent::estimateTokens("hello world this is a test") > 4);
  CHECK(agent::estimateTokens("中文测试") >= 4);  // CJK ~1 token/char
  CHECK(agent::estimateTokens("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") > 30);

  // empty conversation
  std::vector<agent::Message> msgs;
  CHECK(agent::estimateMessagesTokens(msgs) == 0);

  msgs.push_back({"user", "hello world"});
  msgs.push_back({"assistant", "hello back"});
  CHECK(agent::estimateMessagesTokens(msgs) > 0);

  // plain-text rendering
  std::string p = agent::toPlainText("Hello **bold** and *italic* world");
  CHECK(p.find("**") == std::string::npos);
  CHECK(p.find("*") == std::string::npos);
  CHECK(p.find("Hello bold and italic world") != std::string::npos);

  p = agent::toPlainText("# Big Title\n\nsome text");
  CHECK(p.find("[#] Big Title") != std::string::npos);

  p = agent::toPlainText("Use `code()` inline");
  CHECK(p.find("code()") != std::string::npos);
  CHECK(p.find("`") == std::string::npos);

  p = agent::toPlainText("[click me](https://example.com)");
  CHECK(p.find("click me") != std::string::npos);
  CHECK(p.find("https://example.com") != std::string::npos);
  CHECK(p.find("[") == std::string::npos);

  p = agent::toPlainText("```python\nprint('hi')\n```\nend");
  CHECK(p.find("[code:python]") != std::string::npos);
  CHECK(p.find("print('hi')") != std::string::npos);

  p = agent::toPlainText("- item one\n- item two");
  CHECK(p.find("- item one") != std::string::npos);

  // ANSI stripping
  p = agent::toPlainText(std::string("a\x1b[31mred\x1b[0m b"));
  CHECK(p.find("red") != std::string::npos);
  CHECK(p.find("\x1b") == std::string::npos);

  // links that are not links keep their label text
  p = agent::toPlainText("plain [text] here");
  CHECK(p.find("plain text here") != std::string::npos);

  // stripAnsi: removes escapes + control bytes, keeps newline/tab
  p = agent::stripAnsi(std::string("a\x1b[1;31m red\x1b[0m b\nc"));
  CHECK(p == "a red b\nc");
  p = agent::stripAnsi(std::string("x\x01y\tz\x7fw"));
  CHECK(p == "xy\tzw");
  p = agent::stripAnsi("no escapes here");
  CHECK(p == "no escapes here");

  // capToolResult: plain text tail-truncated with marker
  std::string big(10000, 'x');
  std::string capped = agent::capToolResult(big, 1000);
  CHECK(capped.size() <= 1000 + 64);
  CHECK(capped.find("[truncated") != std::string::npos);
  CHECK(agent::capToolResult(big, 0) == big);          // 0 = unlimited
  CHECK(agent::capToolResult("short", 1000) == "short");

  // capToolResult: JSON stays parseable, long fields truncated
  std::string jbig(8000, 'j');
  std::string js = "{\"kind\":\"ok\",\"data\":\"" + jbig + "\",\"n\":42}";
  std::string jc = agent::capToolResult(js, 400);
  mini::Value jv;
  CHECK(mini::tryParse(jc, jv));                        // still valid JSON
  CHECK(jv.type == mini::Value::Object);
  auto* data = jv.get("data");
  CHECK(data && data->s.size() < 8000);
  CHECK(data && data->s.find("[truncated]") != std::string::npos);
  auto* n = jv.get("n");
  CHECK(n && n->asInt() == 42);                         // fields preserved

  // pruneBeforeSend: empty assistant frames removed
  {
    std::vector<agent::Message> msgs = {
        {"assistant", "", {}},
        {"user", "hello"},
        {"assistant", "", {}},
    };
    CHECK(agent::pruneBeforeSend(msgs, true));
    CHECK(msgs.size() == 1);
    CHECK(msgs[0].role == "user");
  }
  // pruneBeforeSend: consecutive identical tool results collapsed
  {
    std::vector<agent::Message> msgs = {
        {"assistant", "calling", "", {{"t1", "read", "{}"}}},
        {"tool", "{\"ok\":true}", "t1"},
        {"tool", "{\"ok\":true}", "t1"},
        {"assistant", "done", ""},
    };
    CHECK(agent::pruneBeforeSend(msgs, true));
    bool seen = false;
    for (auto const& m : msgs)
      if (m.role == "tool" && m.toolCallId == "t1") {
        CHECK(!seen);  // exactly one t1 result remains
        seen = true;
      }
    CHECK(seen);
  }
  // pruneBeforeSend: different tool results are never merged
  {
    std::vector<agent::Message> msgs = {
        {"tool", "{\"ok\":true}", "t1"},
        {"tool", "{\"ok\":false}", "t1"},
    };
    CHECK(!agent::pruneBeforeSend(msgs, true));
    CHECK(msgs.size() == 2);
  }
  // pruneBeforeSend: disabled -> untouched
  {
    std::vector<agent::Message> msgs = {{"assistant", "", {}}, {"user", "hi"}};
    CHECK(!agent::pruneBeforeSend(msgs, false));
    CHECK(msgs.size() == 2);
  }
  // pruneBeforeSend: runaway user / tool payloads capped
  {
    std::vector<agent::Message> msgs = {
        {"user", std::string(20000, 'u')},
        {"tool", std::string(200000, 't'), "t9"},
    };
    agent::pruneBeforeSend(msgs, true);
    CHECK(msgs[0].content.size() <= 8192 + 32);
    CHECK(msgs[1].content.size() <= 65536 + 32);
  }
  // pruneBeforeSend: conversational filler stripped from assistant prose
  {
    std::vector<agent::Message> msgs = {
        {"user", "好的,帮我看看这个问题"},  // user text is sacred: never touched
        {"assistant", "好的,先读一下代码。\n嗯嗯,然后分析。\n其实就是这样。"},
        {"assistant", "```cpp\n好的 int x = 1;  // comment\n```"},
    };
    CHECK(agent::pruneBeforeSend(msgs, true));
    CHECK(msgs[0].content == "好的,帮我看看这个问题");  // user untouched
    CHECK(msgs[1].content.find("好的") == std::string::npos);
    CHECK(msgs[1].content.find("嗯") == std::string::npos);
    CHECK(msgs[1].content.find("先读一下代码") != std::string::npos);
    CHECK(msgs[1].content.find("然后分析") != std::string::npos);
    CHECK(msgs[2].content.find("好的 int x = 1;") != std::string::npos);  // fence preserved
  }
  // stripFillerWords: boundary-safe (meaningful words survive)
  {
    CHECK(agent::stripFillerWords("好的做法是重试") == "好的做法是重试");  // "好的做法" kept
    CHECK(agent::stripFillerWords("好的,开始吧") == "开始吧");
    CHECK(agent::stripFillerWords("Well-known issue") == "Well-known issue");
    CHECK(agent::stripFillerWords("um, let me check") == "let me check");
    CHECK(agent::stripFillerWords("Okay. Now proceed") == "Now proceed");
  }

  // retry-policy helpers (transient vs deterministic failures)
  CHECK(agent::isTransientChatError(-1));
  CHECK(agent::isTransientChatError(408));
  CHECK(agent::isTransientChatError(429));
  CHECK(agent::isTransientChatError(500));
  CHECK(agent::isTransientChatError(502));
  CHECK(agent::isTransientChatError(503));
  CHECK(agent::isTransientChatError(504));
  CHECK(!agent::isTransientChatError(0));
  CHECK(!agent::isTransientChatError(200));
  CHECK(!agent::isTransientChatError(400));
  CHECK(!agent::isTransientChatError(404));

  CHECK(agent::isTransientToolError("{\"error\":\"network error fetching url\"}"));
  CHECK(agent::isTransientToolError("{\"error\":\"timed out after 30s\"}"));
  CHECK(agent::isTransientToolError("{\"error\":\"connection reset by peer\"}"));
  CHECK(agent::isTransientToolError("{\"error\":\"502 bad gateway\"}"));
  CHECK(!agent::isTransientToolError("{\"error\":\"file not found: x.cpp\"}"));
  CHECK(!agent::isTransientToolError("{\"error\":\"arguments must be a JSON object\"}"));
  CHECK(!agent::isTransientToolError("{\"ok\":true,\"output\":\"timeout in log\"}"));
  CHECK(!agent::isTransientToolError("not json at all"));
  CHECK(!agent::isTransientToolError(""));

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
