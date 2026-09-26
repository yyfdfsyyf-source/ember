#pragma once
#include "tui/style.hpp"
#include <string>
#include <vector>

namespace tui {

// ---------------------------------------------------------------------------
// Markdown rendering support.
//
// The parser is deliberately incremental-friendly: it processes a buffer of
// (possibly partial) markdown text and produces display lines, each composed
// of style runs. Re-parsing the accumulated buffer on every stream tick is
// cheap for typical chat message sizes (few KB), which keeps the streaming
// path simple and correct: text that has not been fully typed yet stays
// unstyled until it resolves.
// ---------------------------------------------------------------------------

// One styled text slice within a display line.
struct MdRun {
  std::string text;
  Style st;
};

// A wrapped display line = a sequence of style runs.
using MdLine = std::vector<MdRun>;

// Color theme used while rendering markdown.
struct MdTheme {
  Style text;
  Style heading;
  Style bold;
  Style italic;
  Style code;        // inline code
  Style codeBlock;   // fenced code blocks (whole block / plain tokens)
  Style codeFence;   // ``` fence markers + the body gutter
  Style codeKeyword; // keywords
  Style codeType;    // language primitive / library type names
  Style codeString;  // string & character literals
  Style codeComment; // comments
  Style codeNumber;  // numeric literals
  Style codeFunc;    // function-call identifiers
  Style codePreproc; // preprocessor directives (#include, ...)
  Style quote;       // blockquote
  Style quoteMark;   // '> ' prefix
  Style bullet;      // list markers (-, *, 1.)
  Style bulletText;  // list item body
  Style link;        // [text](url) label
  Style linkUrl;     // the (url) part (dimmed)
  Style hr;          // thematic break
  Style tableHead;   // a table header row cell
  Style table;       // a table data cell
  Style strike;      // ~~deleted text~~
  Style math;        // inline $...$ math
  Style mathBlock;   // block-level $$...$$ math
};

// A fully styled line: runs + a per-line fallback style used for padding.
struct MdStyledLine {
  MdLine runs;
  Style base;  // applied when styling a run yields no explicit color
};

// Parse `text` into wrapped, styled display lines. `width` is the wrap width
// in terminal columns. Lines do not carry trailing '\n'.
std::vector<MdStyledLine> renderMarkdown(std::string const& text, int width, MdTheme const& theme);

}  // namespace tui