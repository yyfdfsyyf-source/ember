// Windows desktop automation via UI Automation (UIA). No vision needed: the
// model reads the accessibility tree as text (desktop_tree), then drives the
// UI by element path/name (desktop_click/type/key/scroll). UIA is a COM API
// (uiautomationcore.dll, present on all supported Windows versions).
#include "agent/uia.hpp"
#include "minijson.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <uiautomation.h>
// MinGW's uiautomationcore import lib does not export the class GUIDs, so
// define them explicitly (values from uiautomationclient.h).
extern "C" const GUID CLSID_CUIAutomation = {0xff48dba4, 0x60ef, 0x4201,
                                             {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
extern "C" const GUID CLSID_CUIAutomation8 = {0xe22ad333, 0xb25f, 0x460c,
                                              {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#error "desktop automation (uia) is Windows-only"
#endif

namespace agent {

namespace {

// JSON helpers mirroring tools.cpp conventions (mini::Value + JSON escaping).
mini::Value jObj() { return mini::Value::makeObject(); }
mini::Value jArr() { return mini::Value::makeArray(); }
std::string gsErr(std::string const& m) {
  mini::Value o = jObj();
  o.set("error", mini::Value::makeString(m));
  return mini::dump(o);
}
std::string gsJstr(mini::Value const& a, char const* key) {
  auto const* v = a.get(key);
  if (!v) return "";
  return v->asString();
}
int64_t jint(mini::Value const& a, char const* key, int64_t def) {
  auto const* v = a.get(key);
  if (!v) return def;
  return v->asInt(def);
}
double jdbl(mini::Value const& a, char const* key, double def) {
  auto const* v = a.get(key);
  if (!v) return def;
  return v->asDouble(def);
}

std::wstring s2w(std::string const& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}
std::string w2s(std::wstring const& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], n, nullptr, nullptr);
  return out;
}
std::string bstrToString(BSTR b) { return b ? w2s(std::wstring(b, SysStringLen(b))) : ""; }

// UIA COM objects are tied to the thread that created them, and the provider
// tree is not safe to touch concurrently, so serialize all UIA calls.
std::mutex g_uiaMutex;

// One-shot COM init for the calling thread; tools run on worker threads.
struct ComInit {
  HRESULT hr;
  bool init() {
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
  }
  ~ComInit() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

template <typename T>
void releasePtr(T*& p) {
  if (p) { p->Release(); p = nullptr; }
}

// --- element property helpers ---------------------------------------------

std::string controlTypeName(CONTROLTYPEID id) {
  switch (id) {
    case UIA_ButtonControlTypeId: return "button";
    case UIA_EditControlTypeId: return "edit";
    case UIA_DocumentControlTypeId: return "document";
    case UIA_ListItemControlTypeId: return "listitem";
    case UIA_ListControlTypeId: return "list";
    case UIA_MenuControlTypeId: return "menu";
    case UIA_MenuItemControlTypeId: return "menuitem";
    case UIA_TabControlTypeId: return "tab";
    case UIA_TabItemControlTypeId: return "tabitem";
    case UIA_TreeControlTypeId: return "tree";
    case UIA_TreeItemControlTypeId: return "treeitem";
    case UIA_CheckBoxControlTypeId: return "checkbox";
    case UIA_RadioButtonControlTypeId: return "radio";
    case UIA_ComboBoxControlTypeId: return "combobox";
    case UIA_TextControlTypeId: return "text";
    case UIA_HeaderControlTypeId: return "header";
    case UIA_HeaderItemControlTypeId: return "headeritem";
    case UIA_TableControlTypeId: return "table";
    case UIA_ToolBarControlTypeId: return "toolbar";
    case UIA_StatusBarControlTypeId: return "statusbar";
    case UIA_PaneControlTypeId: return "pane";
    case UIA_GroupControlTypeId: return "group";
    case UIA_ImageControlTypeId: return "image";
    case UIA_HyperlinkControlTypeId: return "link";
    case UIA_ScrollBarControlTypeId: return "scrollbar";
    case UIA_SliderControlTypeId: return "slider";
    case UIA_ProgressBarControlTypeId: return "progressbar";
    case UIA_TitleBarControlTypeId: return "titlebar";
    case UIA_WindowControlTypeId: return "window";
    case UIA_SpinnerControlTypeId: return "spinner";
    case UIA_SplitButtonControlTypeId: return "splitbutton";
    case UIA_CustomControlTypeId: return "custom";
    case UIA_DataGridControlTypeId: return "datagrid";
    case UIA_DataItemControlTypeId: return "dataitem";
    case UIA_SemanticZoomControlTypeId: return "semanticzoom";
    default: return "element";
  }
}

struct ElemInfo {
  std::wstring name;
  std::wstring value;
  std::wstring autoId;
  std::string ctype;
  RECT rect{0, 0, 0, 0};
  bool enabled = false;
  bool offscreen = false;
  bool hasRect = false;
};

bool getElemInfo(IUIAutomationElement* el, ElemInfo& out) {
  if (!el) return false;
  {
    BSTR b = nullptr;
    if (SUCCEEDED(el->get_CurrentName(&b)) && b) { out.name = b; SysFreeString(b); }
  }
  {
    BSTR b = nullptr;
    if (SUCCEEDED(el->get_CurrentAutomationId(&b)) && b) { out.autoId = b; SysFreeString(b); }
  }
  {
    CONTROLTYPEID id = 0;
    if (SUCCEEDED(el->get_CurrentControlType(&id))) out.ctype = controlTypeName(id);
  }
  {
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &v)) &&
        v.vt == VT_BSTR && v.bstrVal) {
      out.value = v.bstrVal;
    }
    VariantClear(&v);
  }
  BOOL b = FALSE;
  if (SUCCEEDED(el->get_CurrentIsEnabled(&b))) out.enabled = !!b;
  if (SUCCEEDED(el->get_CurrentIsOffscreen(&b))) out.offscreen = !!b;
  if (SUCCEEDED(el->get_CurrentBoundingRectangle(&out.rect))) out.hasRect = true;
  return true;
}

// --- tree walking ---------------------------------------------------------

struct TreeLimits {
  int maxDepth = 12;
  int maxNodes = 300;
  int maxChars = 12000;
};

// Dump a subtree as indented text lines. Returns false if limits hit.
bool walkTree(IUIAutomation* uia, IUIAutomationElement* el, int depth,
              std::vector<std::string>& lines, int& nodeCount, size_t& usedChars,
              TreeLimits const& lim, std::string const& path) {
  if (nodeCount >= lim.maxNodes) return false;
  if (depth > lim.maxDepth) return false;
  nodeCount++;
  ElemInfo info;
  getElemInfo(el, info);

  std::ostringstream line;
  line << std::string((size_t)depth * 2, ' ') << "[" << path << "] " << info.ctype;
  if (!info.name.empty()) line << " \"" << w2s(info.name) << "\"";
  if (!info.value.empty()) line << " value=\"" << w2s(info.value) << "\"";
  if (!info.autoId.empty()) line << " id=" << w2s(info.autoId);
  if (!info.enabled) line << " disabled";
  if (info.offscreen) line << " offscreen";
  if (info.hasRect)
    line << " (" << info.rect.left << "," << info.rect.top << " "
         << (info.rect.right - info.rect.left) << "x" << (info.rect.bottom - info.rect.top)
         << ")";
  std::string s = line.str();
  usedChars += s.size() + 1;
  if (usedChars > (size_t)lim.maxChars) return false;
  lines.push_back(std::move(s));

  // Descend into the raw/control view. ControlViewWalker hides non-interactive
  // chrome; we use it to keep the dump compact and actionable.
  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(uia->get_ControlViewWalker(&walker))) return true;
  IUIAutomationElement* child = nullptr;
  if (FAILED(walker->GetFirstChildElement(el, &child))) {
    releasePtr(walker);
    return true;
  }
  int idx = 0;
  IUIAutomationElement* cur = child;
  while (cur) {
    std::string childPath = path + "/" + std::to_string(idx);
    if (!walkTree(uia, cur, depth + 1, lines, nodeCount, usedChars, lim, childPath)) {
      releasePtr(cur);
      releasePtr(walker);
      return false;
    }
    IUIAutomationElement* next = nullptr;
    if (FAILED(walker->GetNextSiblingElement(cur, &next))) {
      releasePtr(cur);
      cur = nullptr;
      break;
    }
    releasePtr(cur);
    cur = next;
    idx++;
  }
  releasePtr(walker);
  return true;
}

// Resolve an element by a slash-separated path relative to a root element
// ("0/2/1"). Each segment is a zero-based child index in the control view.
bool resolvePath(IUIAutomation* uia, IUIAutomationElement* root, std::string const& path,
                 IUIAutomationElement** out) {
  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(uia->get_ControlViewWalker(&walker))) return false;
  IUIAutomationElement* el = root;
  el->AddRef();
  std::string seg;
  std::istringstream ss(path);
  while (std::getline(ss, seg, '/')) {
    if (seg.empty()) continue;
    int idx = atoi(seg.c_str());
    if (idx < 0) { releasePtr(el); releasePtr(walker); return false; }
    IUIAutomationElement* child = nullptr;
    if (FAILED(walker->GetFirstChildElement(el, &child))) {
      releasePtr(el);
      releasePtr(walker);
      return false;
    }
    int i = 0;
    IUIAutomationElement* curEl = child;
    while (curEl && i < idx) {
      IUIAutomationElement* next = nullptr;
      if (FAILED(walker->GetNextSiblingElement(curEl, &next))) {
        releasePtr(curEl);
        curEl = nullptr;
        break;
      }
      releasePtr(curEl);
      curEl = next;
      i++;
    }
    releasePtr(el);
    if (i != idx || !curEl) {
      releasePtr(curEl);
      releasePtr(walker);
      return false;
    }
    el = curEl;  // ownership transferred
  }
  releasePtr(walker);
  *out = el;
  return true;
}

// --- find elements by name/role/automationId ------------------------------

struct FindSpec {
  std::wstring name;
  std::string role;
  std::wstring autoId;
  int matchIndex = 0;  // which match to pick (0-based)
};

void normalizeLower(std::string& s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
}

bool elemMatches(ElemInfo const& info, FindSpec const& spec) {
  if (!spec.name.empty()) {
    std::wstring lhs = info.name;
    std::wstring rhs = spec.name;
    // case-insensitive substring match on names
    std::string a = w2s(lhs), b = w2s(rhs);
    normalizeLower(a);
    normalizeLower(b);
    if (a.find(b) == std::string::npos) return false;
  }
  if (!spec.role.empty()) {
    std::string a = info.ctype, b = spec.role;
    normalizeLower(a);
    normalizeLower(b);
    if (a != b) return false;
  }
  if (!spec.autoId.empty()) {
    std::string a = w2s(info.autoId), b = w2s(spec.autoId);
    normalizeLower(a);
    normalizeLower(b);
    if (a.find(b) == std::string::npos) return false;
  }
  return true;
}

// Depth-first search over the control view for the first (or nth) match.
bool findElementDFS(IUIAutomation* uia, IUIAutomationElement* root, FindSpec const& spec,
                    IUIAutomationElement** out, std::string* outPath, int* outIndex, int depth) {
  if (depth > 40) return false;
  ElemInfo info;
  getElemInfo(root, info);
  if (elemMatches(info, spec)) {
    if (*outIndex == spec.matchIndex) {
      root->AddRef();
      *out = root;
      return true;
    }
    (*outIndex)++;
  }
  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(uia->get_ControlViewWalker(&walker))) return false;
  IUIAutomationElement* child = nullptr;
  if (FAILED(walker->GetFirstChildElement(root, &child))) {
    releasePtr(walker);
    return false;
  }
  int idx = 0;
  IUIAutomationElement* cur = child;
  while (cur) {
    if (findElementDFS(uia, cur, spec, out, outPath, outIndex, depth + 1)) {
      if (outPath) {
        std::string childIdx = std::to_string(idx);
        *outPath = childIdx + (outPath->empty() ? "" : "/" + *outPath);
      }
      releasePtr(cur);
      releasePtr(walker);
      return true;
    }
    IUIAutomationElement* next = nullptr;
    if (FAILED(walker->GetNextSiblingElement(cur, &next))) {
      releasePtr(cur);
      cur = nullptr;
      break;
    }
    releasePtr(cur);
    cur = next;
    idx++;
  }
  releasePtr(walker);
  return false;
}

// Resolve an element inside a window element by either a path ("0/2/1") or a
// name/role/automationId spec. Returns true and sets `out` (refcounted).
bool resolveElement(IUIAutomation* uia, IUIAutomationElement* win, std::string const& path,
                    FindSpec const& spec, IUIAutomationElement** out, std::string* outPath) {
  if (!path.empty()) {
    if (resolvePath(uia, win, path, out)) {
      if (outPath) *outPath = path;
      return true;
    }
    return false;
  }
  if (spec.name.empty() && spec.role.empty() && spec.autoId.empty()) return false;
  int idx = 0;
  bool found = findElementDFS(uia, win, spec, out, outPath, &idx, 0);
  return found;
}

// --- pattern helpers ------------------------------------------------------

// Click an element: try InvokePattern, then SelectionItemPattern, then a real
// mouse click at the element's center. Returns error string on failure.
std::string clickElement(IUIAutomation* uia, IUIAutomationElement* el) {
  // InvokePattern
  {
    IUnknown* unk = nullptr;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_InvokePatternId, &unk)) && unk) {
      IUIAutomationInvokePattern* inv = nullptr;
      if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&inv)))) {
        HRESULT hr = inv->Invoke();
        inv->Release();
        unk->Release();
        if (SUCCEEDED(hr)) return "";
      }
      unk->Release();
    }
  }
  // SelectionItemPattern
  {
    IUnknown* unk = nullptr;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_SelectionItemPatternId, &unk)) && unk) {
      IUIAutomationSelectionItemPattern* sel = nullptr;
      if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&sel)))) {
        HRESULT hr = sel->Select();
        sel->Release();
        unk->Release();
        if (SUCCEEDED(hr)) return "";
      }
      unk->Release();
    }
  }
  // Fallback: real click at element center
  ElemInfo info;
  getElemInfo(el, info);
  if (!info.hasRect) return "element has no bounding rectangle to click";
  long cx = info.rect.left + (info.rect.right - info.rect.left) / 2;
  long cy = info.rect.top + (info.rect.bottom - info.rect.top) / 2;
  SetCursorPos(cx, cy);
  mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
  mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
  return "";
}

std::string setValueText(IUIAutomationElement* el, std::wstring const& text) {
  IUnknown* unk = nullptr;
  if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, &unk)) || !unk)
    return "element does not support the value pattern";
  IUIAutomationValuePattern* vp = nullptr;
  if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&vp)))) {
    unk->Release();
    return "element does not support the value pattern";
  }
  BSTR b = SysAllocStringLen(text.data(), (UINT)text.size());
  HRESULT hr = vp->SetValue(b);
  SysFreeString(b);
  vp->Release();
  unk->Release();
  if (FAILED(hr)) return "value pattern SetValue failed";
  return "";
}

std::string scrollElement(IUIAutomationElement* el, int horizontal, int vertical) {
  IUnknown* unk = nullptr;
  if (FAILED(el->GetCurrentPattern(UIA_ScrollPatternId, &unk)) || !unk)
    return "element does not support the scroll pattern";
  IUIAutomationScrollPattern* sp = nullptr;
  if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&sp)))) {
    unk->Release();
    return "element does not support the scroll pattern";
  }
  HRESULT hr = sp->Scroll((ScrollAmount)(vertical < 0 ? ScrollAmount_LargeDecrement
                                                     : ScrollAmount_LargeIncrement),
                          (ScrollAmount)(horizontal < 0 ? ScrollAmount_LargeDecrement
                                                        : ScrollAmount_LargeIncrement));
  sp->Release();
  unk->Release();
  if (FAILED(hr)) return "scroll failed";
  return "";
}

// --- root / window resolution --------------------------------------------

IUIAutomation* createUia() {
  IUIAutomation* uia = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&uia));
  if (FAILED(hr)) return nullptr;
  return uia;
}

// Resolve a window target argument: empty/"focused"/"root" => desktop root or
// focused element; a decimal number => top-level window by HWND; else search
// top-level windows by title substring.
// Returns the element in *out (refcounted) plus its display name in *name.
bool resolveWindow(IUIAutomation* uia, std::string const& target, std::string const& titleFilter,
                   IUIAutomationElement** out, std::string* name, std::string* err) {
  if (target.empty() || target == "root" || target == "desktop" || target == "focused") {
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->GetRootElement(&root))) {
      *err = "UIA root element unavailable";
      return false;
    }
    if (target == "focused") {
      IUIAutomationElement* f = nullptr;
      HRESULT hr = uia->GetFocusedElement(&f);
      if (SUCCEEDED(hr) && f) {
        // Walk up to the window (top-level) ancestor. Keep the last non-null
        // element we find: GetParentElement may succeed with a null result at
        // the top of the tree.
        IUIAutomationTreeWalker* walker = nullptr;
        if (SUCCEEDED(uia->get_ControlViewWalker(&walker))) {
          IUIAutomationElement* cur = f;
          IUIAutomationElement* top = f;
          IUIAutomationElement* toFree = nullptr;
          for (;;) {
            IUIAutomationElement* parent = nullptr;
            HRESULT phr = walker->GetParentElement(cur, &parent);
            if (FAILED(phr) || !parent) break;
            if (toFree) toFree->Release();
            toFree = cur;
            cur = parent;
            top = parent;
          }
          if (toFree) toFree->Release();
          releasePtr(walker);
          if (top != f) f->Release();
          f = top;
        }
        root->Release();
        *out = f;
        return true;
      }
      if (f) f->Release();
    }
    *out = root;
    return true;
  }

  // By HWND
  bool isNum = !target.empty() && target.find_first_not_of("0123456789") == std::string::npos;
  if (isNum) {
    HWND hwnd = (HWND)(uintptr_t)strtoull(target.c_str(), nullptr, 10);
    if (hwnd && IsWindow(hwnd)) {
      IUIAutomationElement* el = nullptr;
      if (SUCCEEDED(uia->ElementFromHandle(hwnd, &el))) {
        ElemInfo info;
        getElemInfo(el, info);
        if (name) *name = w2s(info.name);
        *out = el;
        return true;
      }
      *err = "UIA cannot see that window";
      return false;
    }
    *err = "invalid or non-existent window handle";
    return false;
  }

  // By title substring among top-level windows
  std::wstring filter = s2w(titleFilter.empty() ? target : titleFilter);
  std::string f8 = titleFilter.empty() ? target : titleFilter;
  normalizeLower(f8);
  IUIAutomationElement* root = nullptr;
  if (FAILED(uia->GetRootElement(&root))) {
    *err = "UIA root element unavailable";
    return false;
  }
  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(uia->get_ControlViewWalker(&walker))) {
    root->Release();
    *err = "control view walker unavailable";
    return false;
  }
  IUIAutomationElement* child = nullptr;
  if (FAILED(walker->GetFirstChildElement(root, &child))) {
    releasePtr(walker);
    root->Release();
    *err = "no top-level windows";
    return false;
  }
  IUIAutomationElement* cur = child;
  bool found = false;
  while (cur) {
    ElemInfo info;
    getElemInfo(cur, info);
    std::string t = w2s(info.name);
    normalizeLower(t);
    if (t.find(f8) != std::string::npos) {
      found = true;
      break;
    }
    IUIAutomationElement* next = nullptr;
    if (FAILED(walker->GetNextSiblingElement(cur, &next))) {
      releasePtr(cur);
      cur = nullptr;
      break;
    }
    releasePtr(cur);
    cur = next;
  }
  releasePtr(walker);
  root->Release();
  if (!found) {
    if (cur) cur->Release();
    *err = "no window matches title";
    return false;
  }
  if (name) {
    ElemInfo info;
    getElemInfo(cur, info);
    *name = w2s(info.name);
  }
  *out = cur;
  return true;
}

std::string windowListJson(IUIAutomation* uia) {
  IUIAutomationElement* root = nullptr;
  if (FAILED(uia->GetRootElement(&root))) return gsErr("UIA root element unavailable");
  IUIAutomationTreeWalker* walker = nullptr;
  if (FAILED(uia->get_ControlViewWalker(&walker))) {
    root->Release();
    return gsErr("control view walker unavailable");
  }
  mini::Value arr = jArr();
  int count = 0;
  IUIAutomationElement* child = nullptr;
  if (FAILED(walker->GetFirstChildElement(root, &child))) {
    releasePtr(walker);
    root->Release();
    mini::Value o = jObj();
    o.set("ok", mini::Value::makeBool(true));
    o.set("windows", std::move(arr));
    return mini::dump(o);
  }
  IUIAutomationElement* cur = child;
  while (cur) {
    ElemInfo info;
    getElemInfo(cur, info);
    if (info.ctype == "window" || info.ctype == "pane") {
      mini::Value w = jObj();
UIA_HWND hwnd = nullptr;
      if (SUCCEEDED(cur->get_CurrentNativeWindowHandle(&hwnd))) {
        char buf[32];
        snprintf(buf, sizeof buf, "%llu", (unsigned long long)(uintptr_t)hwnd);
        w.set("hwnd", mini::Value::makeString(buf));
      }
      if (!info.name.empty()) w.set("title", mini::Value::makeString(w2s(info.name)));
      w.set("role", mini::Value::makeString(info.ctype));
      if (info.hasRect) {
        mini::Value r = jArr();
        r.arr.push_back(mini::Value::makeInt(info.rect.left));
        r.arr.push_back(mini::Value::makeInt(info.rect.top));
        r.arr.push_back(mini::Value::makeInt(info.rect.right - info.rect.left));
        r.arr.push_back(mini::Value::makeInt(info.rect.bottom - info.rect.top));
        w.set("rect", std::move(r));
      }
      w.set("enabled", mini::Value::makeBool(info.enabled));
      arr.arr.push_back(std::move(w));
      count++;
    }
    IUIAutomationElement* next = nullptr;
    if (FAILED(walker->GetNextSiblingElement(cur, &next))) {
      releasePtr(cur);
      cur = nullptr;
      break;
    }
    releasePtr(cur);
    cur = next;
  }
  releasePtr(walker);
  root->Release();
  mini::Value o = jObj();
  o.set("ok", mini::Value::makeBool(true));
  o.set("windows", std::move(arr));
  return mini::dump(o);
}

// Shared entry: serialize UIA access, init COM, build the UIA object.
struct UiaCtx {
  ComInit com;
  IUIAutomation* uia = nullptr;
  bool ok = false;
  std::string err;
};

UiaCtx beginUia() {
  UiaCtx ctx;
  if (!ctx.com.init()) {
    ctx.err = "COM init failed";
    return ctx;
  }
  ctx.uia = createUia();
  if (!ctx.uia) {
    ctx.err = "UI Automation is not available on this system";
    return ctx;
  }
  ctx.ok = true;
  return ctx;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

std::string toolDesktopWindows(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::lock_guard<std::mutex> lk(g_uiaMutex);
  UiaCtx ctx = beginUia();
  if (!ctx.ok) return gsErr(ctx.err);
  std::string out = windowListJson(ctx.uia);
  ctx.uia->Release();
  return out;
}

std::string toolDesktopTree(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string window = gsJstr(args, "window");
  std::string path = gsJstr(args, "path");
  std::string name = gsJstr(args, "name");
  std::string role = gsJstr(args, "role");
  std::string autoId = gsJstr(args, "automation_id");
  TreeLimits lim;
  lim.maxDepth = (int)jint(args, "max_depth", 12);
  lim.maxNodes = (int)jint(args, "max_nodes", 300);
  lim.maxChars = (int)jint(args, "max_chars", 12000);

  std::lock_guard<std::mutex> lk(g_uiaMutex);
  UiaCtx ctx = beginUia();
  if (!ctx.ok) return gsErr(ctx.err);

  IUIAutomationElement* win = nullptr;
  std::string winName;
  std::string err;
  if (!resolveWindow(ctx.uia, window, "", &win, &winName, &err)) {
    ctx.uia->Release();
    return gsErr(err);
  }

  // If a path/name/role given, resolve to a sub-element and dump just that.
  IUIAutomationElement* target = win;
  std::string dumpPath;
  if (!path.empty() || !name.empty() || !role.empty() || !autoId.empty()) {
    FindSpec spec;
    spec.name = s2w(name);
    spec.role = role;
    spec.autoId = s2w(autoId);
    IUIAutomationElement* el = nullptr;
    std::string p;
    if (!resolveElement(ctx.uia, win, path, spec, &el, &p)) {
      win->Release();
      ctx.uia->Release();
      return gsErr("element not found (use desktop_windows then desktop_tree first)");
    }
    win->Release();
    target = el;
    dumpPath = p;
  }

  std::vector<std::string> lines;
  int nodeCount = 0;
  size_t usedChars = 0;
  walkTree(ctx.uia, target, 0, lines, nodeCount, usedChars, lim, dumpPath);
  target->Release();
  ctx.uia->Release();

  std::string body;
  for (auto const& l : lines) body += l + "\n";
  mini::Value o = jObj();
  o.set("ok", mini::Value::makeBool(true));
  o.set("window", mini::Value::makeString(winName));
  o.set("nodes", mini::Value::makeInt(nodeCount));
  o.set("tree", mini::Value::makeString(body));
  return mini::dump(o);
}

std::string toolDesktopClick(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string window = gsJstr(args, "window");
  std::string path = gsJstr(args, "path");
  std::string name = gsJstr(args, "name");
  std::string role = gsJstr(args, "role");
  std::string autoId = gsJstr(args, "automation_id");
  bool hasX = args.has("x"), hasY = args.has("y");

  std::lock_guard<std::mutex> lk(g_uiaMutex);
  UiaCtx ctx = beginUia();
  if (!ctx.ok) return gsErr(ctx.err);

  // Raw coordinate click (no window resolution needed)
  if (hasX && hasY) {
    long x = (long)jdbl(args, "x", 0);
    long y = (long)jdbl(args, "y", 0);
    std::string button = gsJstr(args, "button");
    int clicks = (int)jint(args, "click_count", 1);
    if (clicks < 1 || clicks > 2) clicks = 1;
    SetCursorPos(x, y);
    DWORD down = button == "right" ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    DWORD up = button == "right" ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    for (int i = 0; i < clicks; i++) {
      mouse_event(down, 0, 0, 0, 0);
      mouse_event(up, 0, 0, 0, 0);
    }
    mini::Value o = jObj();
    o.set("ok", mini::Value::makeBool(true));
    o.set("x", mini::Value::makeInt(x));
    o.set("y", mini::Value::makeInt(y));
    return mini::dump(o);
  }

  // Element click
  IUIAutomationElement* win = nullptr;
  std::string err;
  if (!resolveWindow(ctx.uia, window, "", &win, nullptr, &err)) {
    ctx.uia->Release();
    return gsErr(err);
  }
  FindSpec spec;
  spec.name = s2w(name);
  spec.role = role;
  spec.autoId = s2w(autoId);
  IUIAutomationElement* el = nullptr;
  std::string p;
  if (!resolveElement(ctx.uia, win, path, spec, &el, &p)) {
    win->Release();
    ctx.uia->Release();
    return gsErr("element not found (use desktop_tree to inspect the window first)");
  }
  std::string clickErr = clickElement(ctx.uia, el);
  el->Release();
  win->Release();
  ctx.uia->Release();
  if (!clickErr.empty()) return gsErr(clickErr);
  mini::Value o = jObj();
  o.set("ok", mini::Value::makeBool(true));
  if (!p.empty()) o.set("path", mini::Value::makeString(p));
  if (!name.empty()) o.set("name", mini::Value::makeString(name));
  return mini::dump(o);
}

std::string toolDesktopType(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string text = gsJstr(args, "text");
  if (text.empty()) return gsErr("missing 'text' string");
  std::string window = gsJstr(args, "window");
  std::string path = gsJstr(args, "path");
  std::string name = gsJstr(args, "name");
  bool clear = jint(args, "clear", 0) != 0;

  std::lock_guard<std::mutex> lk(g_uiaMutex);
  UiaCtx ctx = beginUia();
  if (!ctx.ok) return gsErr(ctx.err);

  IUIAutomationElement* el = nullptr;
  IUIAutomationElement* win = nullptr;
  std::string err;
  if (!path.empty() || !name.empty()) {
    if (!resolveWindow(ctx.uia, window, "", &win, nullptr, &err)) {
      ctx.uia->Release();
      return gsErr(err);
    }
    FindSpec spec;
    spec.name = s2w(name);
    IUIAutomationElement* resolved = nullptr;
    std::string p;
    if (!resolveElement(ctx.uia, win, path, spec, &resolved, &p)) {
      win->Release();
      ctx.uia->Release();
      return gsErr("element not found");
    }
    el = resolved;
  } else {
    // Type into whatever has focus
    if (FAILED(ctx.uia->GetFocusedElement(&el)) || !el) {
      ctx.uia->Release();
      return gsErr("no focused element to type into");
    }
  }

  std::wstring wtext = s2w(text);
  if (clear) {
    std::string se = setValueText(el, L"");
    if (!se.empty() && !(se.find("value pattern") != std::string::npos)) {
      el->Release();
      if (win) win->Release();
      ctx.uia->Release();
      return gsErr(se);
    }
  }
  std::string setErr = setValueText(el, wtext);
  if (setErr.empty()) {
    if (win) win->Release();
    el->Release();
    ctx.uia->Release();
    mini::Value o = jObj();
    o.set("ok", mini::Value::makeBool(true));
    o.set("method", mini::Value::makeString("value_pattern"));
    o.set("text", mini::Value::makeString(text));
    return mini::dump(o);
  }

  // Fallback: focus the element and type via SendInput (keystroke simulation)
  if (FAILED(el->SetFocus())) {
    if (win) win->Release();
    el->Release();
    ctx.uia->Release();
    return gsErr(setErr);
  }
  // SendInput per character (Unicode-safe)
  for (wchar_t c : wtext) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = c;
    in.ki.dwFlags = KEYEVENTF_UNICODE;
    SendInput(1, &in, sizeof(INPUT));
    in.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (win) win->Release();
  el->Release();
  ctx.uia->Release();
  mini::Value o = jObj();
  o.set("ok", mini::Value::makeBool(true));
  o.set("method", mini::Value::makeString("sendinput"));
  o.set("text", mini::Value::makeString(text));
  return mini::dump(o);
}

// VK lookup for named keys in desktop_key combos.
struct KeyName {
  const char* name;
  WORD vk;
};
static const KeyName kKeyNames[] = {
    {"enter", VK_RETURN},   {"return", VK_RETURN}, {"esc", VK_ESCAPE},
    {"escape", VK_ESCAPE},  {"tab", VK_TAB},       {"space", VK_SPACE},
    {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"del", VK_DELETE},
    {"insert", VK_INSERT},  {"home", VK_HOME},     {"end", VK_END},
    {"pgup", VK_PRIOR},     {"pageup", VK_PRIOR},  {"pgdn", VK_NEXT},
    {"pagedown", VK_NEXT},  {"up", VK_UP},         {"down", VK_DOWN},
    {"left", VK_LEFT},      {"right", VK_RIGHT},   {"ctrl", VK_CONTROL},
    {"control", VK_CONTROL}, {"alt", VK_MENU},     {"shift", VK_SHIFT},
    {"win", VK_LWIN},       {"cmd", VK_LWIN},      {"f1", VK_F1},
    {"f2", VK_F2},          {"f3", VK_F3},         {"f4", VK_F4},
    {"f5", VK_F5},          {"f6", VK_F6},         {"f7", VK_F7},
    {"f8", VK_F8},          {"f9", VK_F9},         {"f10", VK_F10},
    {"f11", VK_F11},        {"f12", VK_F12},       {"caps", VK_CAPITAL},
    {"capslock", VK_CAPITAL},
};

WORD keyToVk(std::string const& k) {
  if (k.size() == 1) {
    char c = (char)tolower((unsigned char)k[0]);
    if (c >= 'a' && c <= 'z') return (WORD)(0x41 + (c - 'a'));
    if (c >= '0' && c <= '9') return (WORD)(0x30 + (c - '0'));
  }
  for (auto const& kn : kKeyNames)
    if (k == kn.name) return kn.vk;
  return 0;
}

void sendKeyDown(WORD vk) {
  INPUT in = {0};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  SendInput(1, &in, sizeof(INPUT));
}
void sendKeyUp(WORD vk) {
  INPUT in = {0};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = vk;
  in.ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(1, &in, sizeof(INPUT));
}

std::string toolDesktopKey(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string keys = gsJstr(args, "keys");
  if (keys.empty()) return gsErr("missing 'keys' string");
  std::string window = gsJstr(args, "window");

  std::lock_guard<std::mutex> lk(g_uiaMutex);
  UiaCtx ctx = beginUia();
  if (!ctx.ok) return gsErr(ctx.err);

  // Optional: focus a window first so the keys land there
  if (!window.empty() && window != "focused") {
    IUIAutomationElement* win = nullptr;
    std::string err;
    if (resolveWindow(ctx.uia, window, "", &win, nullptr, &err)) {
      win->SetFocus();
      win->Release();
    }
  }

  std::vector<WORD> mods;
  std::vector<WORD> main;
  // Split by '+'
  std::string cur;
  std::vector<std::string> parts;
  for (char c : keys) {
    if (c == '+') { parts.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  if (!cur.empty()) parts.push_back(cur);
  for (auto const& p : parts) {
    WORD vk = keyToVk(p);
    if (!vk) {
      ctx.uia->Release();
      return gsErr("unknown key '" + p + "'");
    }
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN)
      mods.push_back(vk);
    else
      main.push_back(vk);
  }
  if (main.empty()) {
    // Only modifiers: tap them
    for (auto vk : mods) { sendKeyDown(vk); }
    for (auto vk : mods) { sendKeyUp(vk); }
  } else {
    for (auto vk : mods) sendKeyDown(vk);
    for (auto vk : main) {
      sendKeyDown(vk);
      sendKeyUp(vk);
    }
    for (auto vk : mods) sendKeyUp(vk);
  }
  ctx.uia->Release();
  mini::Value o = jObj();
  o.set("ok", mini::Value::makeBool(true));
  o.set("keys", mini::Value::makeString(keys));
  return mini::dump(o);
}

std::string toolDesktopScroll(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string window = gsJstr(args, "window");
  std::string path = gsJstr(args, "path");
  std::string name = gsJstr(args, "name");
  std::string dir = gsJstr(args, "direction");
  if (dir.empty()) dir = "down";
  int amount = (int)jint(args, "amount", 1);

  std::lock_guard<std::mutex> lk(g_uiaMutex);
  UiaCtx ctx = beginUia();
  if (!ctx.ok) return gsErr(ctx.err);

  IUIAutomationElement* el = nullptr;
  IUIAutomationElement* win = nullptr;
  std::string err;
  if (!path.empty() || !name.empty()) {
    if (!resolveWindow(ctx.uia, window, "", &win, nullptr, &err)) {
      ctx.uia->Release();
      return gsErr(err);
    }
    FindSpec spec;
    spec.name = s2w(name);
    IUIAutomationElement* resolved = nullptr;
    std::string p;
    if (!resolveElement(ctx.uia, win, path, spec, &resolved, &p)) {
      win->Release();
      ctx.uia->Release();
      return gsErr("element not found");
    }
    el = resolved;
  } else {
    if (!resolveWindow(ctx.uia, window, "", &win, nullptr, &err)) {
      ctx.uia->Release();
      return gsErr(err);
    }
    el = win;
    win = nullptr;  // ownership transferred; don't double-release
  }

  int horiz = 0, vert = 0;
  if (dir == "down") vert = -amount;
  else if (dir == "up") vert = amount;
  else if (dir == "right") horiz = -amount;
  else if (dir == "left") horiz = amount;
  else {
    el->Release();
    if (win) win->Release();
    ctx.uia->Release();
    return gsErr("direction must be up/down/left/right");
  }
  std::string scrollErr = scrollElement(el, horiz, vert);
  el->Release();
  if (win) win->Release();
  ctx.uia->Release();
  if (!scrollErr.empty()) return gsErr(scrollErr);
  mini::Value o = jObj();
  o.set("ok", mini::Value::makeBool(true));
  o.set("direction", mini::Value::makeString(dir));
  return mini::dump(o);
}

std::string toolDesktopWait(std::string const& argsJson) {
  mini::Value args;
  if (!mini::tryParse(argsJson, args)) return gsErr("arguments must be a JSON object");
  std::string window = gsJstr(args, "window");
  std::string name = gsJstr(args, "name");
  std::string role = gsJstr(args, "role");
  int timeoutMs = (int)jint(args, "timeout_ms", 10000);
  if (name.empty() && role.empty()) return gsErr("need 'name' or 'role' to wait for");

  std::lock_guard<std::mutex> lk(g_uiaMutex);
  UiaCtx ctx = beginUia();
  if (!ctx.ok) return gsErr(ctx.err);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  FindSpec spec;
  spec.name = s2w(name);
  spec.role = role;
  int iter = 0;
  for (;;) {
    IUIAutomationElement* win = nullptr;
    std::string err;
    if (resolveWindow(ctx.uia, window, "", &win, nullptr, &err)) {
      IUIAutomationElement* el = nullptr;
      std::string p;
      int idx = 0;
      bool found = findElementDFS(ctx.uia, win, spec, &el, &p, &idx, 0);
      win->Release();
      if (found) {
        el->Release();
        ctx.uia->Release();
        mini::Value o = jObj();
        o.set("ok", mini::Value::makeBool(true));
        o.set("found", mini::Value::makeBool(true));
        if (!p.empty()) o.set("path", mini::Value::makeString(p));
        if (!name.empty()) o.set("name", mini::Value::makeString(name));
        return mini::dump(o);
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    iter++;
  }
  ctx.uia->Release();
  mini::Value o = jObj();
  o.set("ok", mini::Value::makeBool(true));
  o.set("found", mini::Value::makeBool(false));
  o.set("note", mini::Value::makeString("timeout waiting for element"));
  return mini::dump(o);
}

}  // namespace agent