#include "agent/session.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
  using namespace agent;
  std::string dir = "out/tmp_session_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::string path = dir + "/chat.json";

  // Saving an empty conversation produces a valid file.
  std::vector<Message> none;
  CHECK(saveSession(path, none, "model-x"));
  CHECK(std::filesystem::is_regular_file(path));

  // Round-trip a realistic conversation.
  std::vector<Message> msgs;
  msgs.emplace_back("user", "refactor the search tool", "", std::vector<ToolCall>{});
  msgs.emplace_back("assistant",
                    "",
                    "",
                    std::vector<ToolCall>{{"call_1", "grep", "{\"pattern\":\"TODO\"}"}});
  msgs.emplace_back("tool", "{\"ok\":true,\"matches\":1}", "call_1");
  msgs.emplace_back("assistant", "Done, one TODO found.");
  CHECK(saveSession(path, msgs, "deepseek-v2"));

  std::vector<Message> loaded;
  std::string model;
  CHECK(loadSession(path, loaded, model));
  CHECK(model == "deepseek-v2");
  CHECK(loaded.size() == 4);
  CHECK(loaded[0].role == "user");
  CHECK(loaded[0].content == "refactor the search tool");
  CHECK(loaded[1].role == "assistant");
  CHECK(loaded[1].toolCalls.size() == 1);
  CHECK(loaded[1].toolCalls[0].id == "call_1");
  CHECK(loaded[1].toolCalls[0].name == "grep");
  CHECK(loaded[1].toolCalls[0].arguments == "{\"pattern\":\"TODO\"}");
  CHECK(loaded[2].role == "tool");
  CHECK(loaded[2].toolCallId == "call_1");
  CHECK(loaded[3].role == "assistant");
  CHECK(loaded[3].content == "Done, one TODO found.");
  CHECK(loaded[3].toolCalls.empty());

  // Content survives with special characters intact.
  msgs = std::vector<Message>{Message("user", "line1\n\"quoted\" \\ backslash \u00e9\u4e2d\u6587")};
  CHECK(saveSession(path, msgs, "m"));
  CHECK(loadSession(path, loaded, model));
  CHECK(loaded.size() == 1);
  CHECK(loaded[0].content == "line1\n\"quoted\" \\ backslash \u00e9\u4e2d\u6587");

  // Missing / corrupt files fail cleanly.
  CHECK(loadSession(dir + "/nope.json", loaded, model) == 0);
  {
    std::ofstream f(dir + "/bad.json", std::ios::binary | std::ios::trunc);
    f << "{not json";
  }
  CHECK(loadSession(dir + "/bad.json", loaded, model) == -1);
  CHECK(saveSession(dir + "/nope.json", msgs, "m"));  // parent exists -> ok

  // Multi-session helpers: listing, titles, fresh-id generation.
  {
    std::string sdir = dir + "/sessdir";
    std::filesystem::create_directories(sdir);
    std::vector<Message> one{Message("user", "hello world"), Message("assistant", "hi")};
    std::vector<Message> two{Message("user", "second chat")};
    std::string idA = agent::makeSessionId();
    CHECK(!idA.empty());
    CHECK(idA.find("sess-") == 0);
    std::string idB = agent::makeSessionId();
    CHECK(idA != idB);  // ids are unique
    CHECK(saveSession(sdir + "/" + idA + ".json", one, "m1", "hello world"));
    CHECK(saveSession(sdir + "/" + idB + ".json", two, "m2", "second chat"));
    std::vector<agent::SessionInfo> list;
    CHECK(agent::listSessions(sdir, list));
    CHECK(list.size() == 2);
    bool foundA = false, foundB = false;
    for (auto const& s : list) {
      if (s.id == idA) {
        foundA = true;
        CHECK(s.title == "hello world");
        CHECK(s.model == "m1");
        CHECK(s.msgCount == 2);
        CHECK(s.savedAt > 0);
        CHECK(!s.path.empty());
      } else if (s.id == idB) {
        foundB = true;
        CHECK(s.title == "second chat");
        CHECK(s.msgCount == 1);
      }
    }
    CHECK(foundA && foundB);
    // Newest-first ordering.
    CHECK(list[0].savedAt >= list[1].savedAt);

    // listSessions on a missing dir returns false.
    std::vector<agent::SessionInfo> empty;
    CHECK(!agent::listSessions(dir + "/missing_sessdir", empty));
    std::filesystem::remove_all(sdir);
  }

  std::filesystem::remove_all(dir);
  std::printf("checks=%d failures=%d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
