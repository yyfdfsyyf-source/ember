// Trajectory log tests: append sequencing, JSONL persistence round-trip,
// filter logic, and the per-item-style List extension.
#include "agent/trajectory.hpp"
#include "tui/widgets.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

static std::string tempPath(char const* tag) {
  std::string p = std::getenv("TEMP") ? std::getenv("TEMP") : ".";
  p += "/dsh_traj_" + std::string(tag);
  return p;
}

static void test_append_sequencing() {
  agent::TrajectoryLog log;
  log.open("");

  agent::TrajEvent e;
  e.kind = agent::TrajKind::UserMessage;
  e.text = "hi";
  log.append(e);
  e.kind = agent::TrajKind::ToolCall;
  e.toolName = "fs.read";
  e.toolArgs = "{\"path\":\"a\"}";
  log.append(e);
  e.kind = agent::TrajKind::ToolResult;
  e.toolName = "fs.read";
  e.text = "content";
  log.append(e);

  CHECK(log.size() == 3);
  CHECK(log.at(0).seq == 0);
  CHECK(log.at(1).seq == 1);
  CHECK(log.at(2).seq == 2);
  CHECK(log.at(0).time > 0);  // log stamps the time
  CHECK(log.at(1).toolName == "fs.read");
  CHECK(log.at(2).kind == agent::TrajKind::ToolResult);
  log.close();
}

static void test_jsonl_roundtrip() {
  std::string path = tempPath("roundtrip.jsonl");
  std::remove(path.c_str());

  {
    agent::TrajectoryLog log;
    log.open(path);
    CHECK(log.isOpen());
    agent::TrajEvent e;
    e.kind = agent::TrajKind::TurnStart;
    e.turn = 1;
    e.model = "m1";
    log.append(e);
    e.kind = agent::TrajKind::UserMessage;
    e.turn = 1;
    e.source = "prompt";
    e.text = "hello world";
    log.append(e);
    e.kind = agent::TrajKind::ToolCall;
    e.turn = 1;
    e.toolName = "shell_exec";
    e.toolArgs = "{\"cmd\":\"echo \\\"x\\\"\"}";
    log.append(e);
  }

  // file lines mention the fields
  {
    std::ifstream in(path);
    std::string l1, l2, l3;
    std::getline(in, l1);
    std::getline(in, l2);
    std::getline(in, l3);
    CHECK(l1.find("\"kind\":\"turn-start\"") != std::string::npos);
    CHECK(l2.find("\"source\":\"prompt\"") != std::string::npos);
    CHECK(l2.find("\"text\":\"hello world\"") != std::string::npos);
    CHECK(l3.find("\"toolName\":\"shell_exec\"") != std::string::npos);
  }

  // reload: same events, appended log continues with contiguous seq
  {
    agent::TrajectoryLog log;
    log.open(path);
    CHECK(log.size() == 3);
    CHECK(log.at(0).kind == agent::TrajKind::TurnStart);
    CHECK(log.at(0).model == "m1");
    CHECK(log.at(1).text == "hello world");
    CHECK(log.at(2).toolArgs.find("echo") != std::string::npos);
    agent::TrajEvent e;
    e.kind = agent::TrajKind::TurnEnd;
    e.turn = 1;
    e.rc = 0;
    e.promptTokens = 1234;
    e.completionTokens = 56;
    e.cachedTokens = 700;
    log.append(e);
    CHECK(log.at(3).seq == 3);
  }
  // token ledger fields survive a reload
  {
    agent::TrajectoryLog log;
    log.open(path);
    CHECK(log.size() == 4);
    agent::TrajEvent const& te = log.at(3);
    CHECK(te.kind == agent::TrajKind::TurnEnd);
    CHECK(te.promptTokens == 1234);
    CHECK(te.completionTokens == 56);
    CHECK(te.cachedTokens == 700);
    CHECK(te.rc == 0);
  }
  std::remove(path.c_str());
}

static void test_torn_tail_ignored() {
  std::string path = tempPath("torn.jsonl");
  std::remove(path.c_str());
  { std::ofstream out(path); out << "{\"seq\":0,\"kind\":\"user\",\"time\":1,\"text\":\"ok\"}\n{\"seq\":1,\""; }
  agent::TrajectoryLog log;
  log.open(path);
  CHECK(log.size() == 1);  // torn last line skipped
  log.close();
  std::remove(path.c_str());
}

static void test_filters() {
  using agent::TrajKind;
  using agent::TrajFilter;
  CHECK(agent::trajFilterMatch(TrajFilter::All, TrajKind::ToolCall));
  CHECK(agent::trajFilterMatch(TrajFilter::Conversation, TrajKind::UserMessage));
  CHECK(agent::trajFilterMatch(TrajFilter::Conversation, TrajKind::Assistant));
  CHECK(!agent::trajFilterMatch(TrajFilter::Conversation, TrajKind::ToolCall));
  CHECK(agent::trajFilterMatch(TrajFilter::Reasoning, TrajKind::Reasoning));
  CHECK(!agent::trajFilterMatch(TrajFilter::Tools, TrajKind::Reasoning));
  CHECK(agent::trajFilterMatch(TrajFilter::Tools, TrajKind::ToolResult));
  CHECK(agent::trajFilterMatch(TrajFilter::Audit, TrajKind::TurnStart));
  CHECK(agent::trajFilterMatch(TrajFilter::Audit, TrajKind::ContextNote));
  CHECK(agent::trajFilterMatch(TrajFilter::Audit, TrajKind::Error));
  CHECK(!agent::trajFilterMatch(TrajFilter::Audit, TrajKind::UserMessage));
}

static void test_list_item_styles() {
  tui::Screen sc;
  sc.resize(30, 10);
  tui::List l;
  l.setItems({"a", "b", "c"});
  tui::Style s1 = tui::Style::plain().fg(tui::Color::rgb(1, 2, 3));
  tui::Style s2 = tui::Style::plain().fg(tui::Color::rgb(4, 5, 6));
  l.setItemStyles({s1, s2, tui::Style::plain()});
  l.setSelected(1);

  tui::Rect box(0, 0, 30, 10);
  tui::Style normal = tui::Style::plain().fg(tui::Color::rgb(9, 9, 9));
  tui::Style highlight = tui::Style::plain().fg(tui::Color::rgb(0, 0, 0)).bg(tui::Color::rgb(200, 200, 200));
  l.render(sc, box, normal, highlight);
  // selected row wears the highlight style
  CHECK(sc.cell(0, 1).st.v == highlight.v);
  // non-selected rows use their per-item styles when set
  CHECK(sc.cell(0, 0).st.v == s1.v);
  CHECK(sc.cell(0, 2).st.v == normal.v);  // empty style falls back to normal
}

static void test_kind_names() {
  CHECK(agent::trajKindName(agent::TrajKind::TurnStart) == "turn-start");
  CHECK(agent::trajKindName(agent::TrajKind::ToolResult) == "tool-result");
  CHECK(!agent::trajKindIcon(agent::TrajKind::Assistant).empty());
}

int main() {
  std::fprintf(stderr, "step1\n");
  test_append_sequencing();
  std::fprintf(stderr, "step2\n");
  test_jsonl_roundtrip();
  std::fprintf(stderr, "step3\n");
  test_torn_tail_ignored();
  std::fprintf(stderr, "step4\n");
  test_filters();
  std::fprintf(stderr, "step5\n");
  test_list_item_styles();
  std::fprintf(stderr, "step6\n");
  test_kind_names();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}