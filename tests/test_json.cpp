#include "minijson.hpp"
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

int main() {
  // basic parse
  auto v = mini::parse(R"({"a": 1, "b": "hi", "c": [1,2,3], "d": true, "e": null})");
  CHECK(v.type == mini::Value::Object);
  CHECK(v.get("a")->asInt() == 1);
  CHECK(v.get("b")->asString() == "hi");
  CHECK(v.get("c")->size() == 3);
  CHECK(v.get("d")->asBool() == true);
  CHECK(v.get("e")->type == mini::Value::Null);

  // numbers
  CHECK(mini::parse("3.14").asDouble() > 3.13);
  CHECK(mini::parse("-7").asInt() == -7);
  CHECK(mini::parse("1e3").asDouble() == 1000.0);

  // strings with escapes + unicode surrogate pair
  auto sv = mini::parse(R"("\u4e2d\u6587 \n\t\"x\\")");
  CHECK(sv.asString() == "\xe4\xb8\xad\xe6\x96\x87 \n\t\"x\\");
  auto sv2 = mini::parse(R"("\ud83d\ude00")");  // U+1F600
  CHECK(sv2.asString() == "\xf0\x9f\x98\x80");

  // serialize round trip
  auto obj = mini::Value::makeObject();
  obj.set("n", mini::Value::makeInt(42));
  obj.set("s", mini::Value::makeString("a\"b\n"));
  obj.set("arr", mini::Value::makeArray());
  obj.get("arr")->arr.push_back(mini::Value::makeBool(true));
  obj.get("arr")->arr.push_back(mini::Value::makeInt(1));
  std::string dumped = mini::dump(obj);
  auto rt = mini::parse(dumped);
  CHECK(rt.get("n")->asInt() == 42);
  CHECK(rt.get("s")->asString() == "a\"b\n");
  CHECK(rt.get("arr")->size() == 2);

  // error handling
  bool threw = false;
  try { mini::parse("{invalid}"); } catch (std::exception const&) { threw = true; }
  CHECK(threw);

  mini::Value bad;
  CHECK(!mini::tryParse("[1,2", bad));

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
