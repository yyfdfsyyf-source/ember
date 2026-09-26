// Minimal plugin: line-based JSON-RPC 2.0 over stdio.
//
//   initialize -> {name, version}
//   tools/list  -> echo_text(text), add_numbers(a, b)
//   tools/call  -> dispatches, returns {"content":"..."}
//
// Prints exactly one JSON object per line on stdout. Build standalone:
//   g++ -std=c++20 -O2 -Ithird_party examples/plugin_echo.cpp -o plugin_echo(.exe)
#include "minijson.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

static std::string jstr(mini::Value const& o, char const* key, std::string def = "") {
  return o.has(key) ? o.get(key)->asString() : def;
}

static int jint(mini::Value const& o, char const* key, int def = 0) {
  return o.has(key) ? (int)o.get(key)->asInt(def) : def;
}

static std::string dumpResult(mini::Value const& id, std::string const& content) {
  mini::Value o = mini::Value::makeObject();
  o.set("jsonrpc", mini::Value::makeString("2.0"));
  o.set("id", id);
  mini::Value res = mini::Value::makeObject();
  res.set("content", mini::Value::makeString(content));
  o.set("result", std::move(res));
  return mini::dump(o);
}

static std::string dumpError(mini::Value const& id, std::string const& msg) {
  mini::Value o = mini::Value::makeObject();
  o.set("jsonrpc", mini::Value::makeString("2.0"));
  o.set("id", id);
  mini::Value e = mini::Value::makeObject();
  e.set("code", mini::Value::makeInt(-32000));
  e.set("message", mini::Value::makeString(msg));
  o.set("error", std::move(e));
  return mini::dump(o);
}

int main() {
  std::string line;
  char buf[8192];
  while (fgets(buf, sizeof buf, stdin)) {
    line = buf;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    mini::Value req;
    if (!mini::tryParse(line, req) || req.type != mini::Value::Object) {
      std::puts("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"parse error\"}}");
      std::fflush(stdout);
      continue;
    }
    mini::Value id = req.has("id") ? *req.get("id") : mini::Value::makeNull();
    std::string method = jstr(req, "method");
    mini::Value const* params = req.get("params");
    mini::Value emptyObj = mini::Value::makeObject();
    if (!params) params = &emptyObj;

    std::string out;
    if (method == "initialize") {
      mini::Value r = mini::Value::makeObject();
      r.set("name", mini::Value::makeString("echo-plugin"));
      r.set("version", mini::Value::makeString("1.0"));
      out = mini::dump(r);
      mini::Value o = mini::Value::makeObject();
      o.set("jsonrpc", mini::Value::makeString("2.0"));
      o.set("id", id);
      o.set("result", std::move(r));
      std::puts(mini::dump(o).c_str());
    } else if (method == "tools/list") {
      mini::Value tools = mini::Value::makeArray();
      {
        mini::Value t = mini::Value::makeObject();
        t.set("name", mini::Value::makeString("echo_text"));
        t.set("description", mini::Value::makeString("Echo the given text back verbatim."));
        mini::Value p = mini::Value::makeObject();
        mini::Value props = mini::Value::makeObject();
        mini::Value pt = mini::Value::makeObject();
        pt.set("type", mini::Value::makeString("string"));
        pt.set("description", mini::Value::makeString("Text to echo."));
        props.set("text", std::move(pt));
        p.set("type", mini::Value::makeString("object"));
        p.set("properties", std::move(props));
        t.set("parameters", std::move(p));
        tools.arr.push_back(std::move(t));
      }
      {
        mini::Value t = mini::Value::makeObject();
        t.set("name", mini::Value::makeString("add_numbers"));
        t.set("description", mini::Value::makeString("Add two integers and return the sum."));
        mini::Value p = mini::Value::makeObject();
        mini::Value props = mini::Value::makeObject();
        mini::Value pn = mini::Value::makeObject();
        pn.set("type", mini::Value::makeString("integer"));
        props.set("a", std::move(pn));
        pn = mini::Value::makeObject();
        pn.set("type", mini::Value::makeString("integer"));
        props.set("b", std::move(pn));
        p.set("type", mini::Value::makeString("object"));
        p.set("properties", std::move(props));
        t.set("parameters", std::move(p));
        tools.arr.push_back(std::move(t));
      }
      mini::Value o = mini::Value::makeObject();
      o.set("jsonrpc", mini::Value::makeString("2.0"));
      o.set("id", id);
      mini::Value r = mini::Value::makeObject();
      r.set("tools", std::move(tools));
      o.set("result", std::move(r));
      std::puts(mini::dump(o).c_str());
    } else if (method == "tools/call") {
      std::string tname = jstr(*params, "name");
      mini::Value const* args = params->get("arguments");
      mini::Value no = mini::Value::makeObject();
      if (!args) args = &no;
      if (tname == "echo_text") {
        std::string text = jstr(*args, "text");
        std::puts(dumpResult(id, "echo: " + text).c_str());
      } else if (tname == "add_numbers") {
        int sum = jint(*args, "a") + jint(*args, "b");
        std::puts(dumpResult(id, std::to_string(sum)).c_str());
      } else {
        std::puts(dumpError(id, "unknown tool: " + tname).c_str());
      }
    } else {
      std::puts(dumpError(id, "unknown method: " + method).c_str());
    }
    std::fflush(stdout);
  }
  return 0;
}