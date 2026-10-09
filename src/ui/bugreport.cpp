// bugreport.cpp — the F12 bug report panel and its writer. See bugreport.h.
//
// THE LOOK is the conversation panel's (ui/theme.h): lit panel body, 9-slice
// frame, header bar, the 26 px pixel face, everything on the 2 px lattice.
// The thumbnails are MOSAICS — the shot box-averaged down to a few thousand
// cells and drawn as filled rects — because the engine has no texture-sampling
// path for UI images (theme.h, "WHY CHROME LIVES IN THE FONT ATLAS"), and a
// 4 px cell is pixel art anyway.
//
// PNG via Windows' own codec (WIC): no image library is vendored, and a 1080p
// BMP is 6 MB a picture.

#include "ui/bugreport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

#include <imgui_internal.h>  // GetActiveID / ClearActiveID (the text box hand-off)

#include "ui/theme.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace sandvox {
std::string AssetDir();  // test/support.cpp: the one asset-path chokepoint
}

namespace {

constexpr float kFrame = 16.0f;   // the 9-slice frame's thickness (inventory_ui)
constexpr float kPad = 18.0f;
constexpr float kMaxW = 1100.0f;
constexpr int kThumbCells = 60;   // thumbnail width in mosaic cells
constexpr float kCell = 4.0f;     // one cell, screen px
constexpr float kThumbGap = 14.0f;
constexpr float kTextH = 220.0f;
constexpr float kListW = 220.0f;   // the ticket list column
constexpr float kListGap = 20.0f;  // between it and the report
constexpr float kListCell = 3.0f;  // a list picture's cell, screen px (60 cells = 180 px)
constexpr float kRowPad = 8.0f;
constexpr float kMinH = 640.0f;    // tall enough for the list to show three tickets
constexpr float kChipGap = 6.0f;   // between tag chips
constexpr float kTagInputW = 150.0f;  // the "new tag" box

float Snap(float v) { return std::floor(v * 0.5f) * 2.0f; }

// std::string-backed InputText (the imgui_stdlib pattern; that file is not
// compiled into imgui_lib).
int ResizeCb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    std::string* s = (std::string*)d->UserData;
    s->resize((size_t)d->BufTextLen);
    d->Buf = s->data();
  }
  return 0;
}

#ifdef _WIN32
bool WritePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t w,
              uint32_t h, std::string& err) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  IWICImagingFactory* fac = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) { err = "WIC factory"; break; }
    if (FAILED(fac->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) {
      err = "open " + path.string();
      break;
    }
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props)) ||
        FAILED(frame->SetSize(w, h))) {
      err = "PNG encoder";
      break;
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) {
      err = "PNG pixel format";
      break;
    }
    const uint32_t stride = w * 3;
    std::vector<uint8_t> bgr((size_t)stride * h);
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
      bgr[i * 3 + 0] = rgba[i * 4 + 2];
      bgr[i * 3 + 1] = rgba[i * 4 + 1];
      bgr[i * 3 + 2] = rgba[i * 4 + 0];
    }
    if (FAILED(frame->WritePixels(h, stride, (UINT)bgr.size(), bgr.data())) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit())) {
      err = "PNG write";
      break;
    }
    ok = true;
  } while (false);
  if (props) props->Release();
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  if (fac) fac->Release();
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}
#else
bool WritePng(const std::filesystem::path&, const std::vector<uint8_t>&, uint32_t, uint32_t,
              std::string& err) {
  err = "PNG writing is Windows-only (WIC)";
  return false;
}
#endif

#ifdef _WIN32
bool LoadMosaicWic(const std::filesystem::path& path, uint32_t cells, uint32_t& w, uint32_t& h,
                   uint32_t& tw, uint32_t& th, std::vector<uint32_t>& thumb, std::string& err) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  IWICImagingFactory* fac = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) { err = "WIC factory"; break; }
    if (FAILED(fac->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame))) {
      err = "open " + path.string();
      break;
    }
    UINT fw = 0, fh = 0;
    if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) { err = "PNG size"; break; }
    w = fw;
    h = fh;
    tw = std::max(1u, cells);
    th = std::max(1u, (uint32_t)std::lround((double)cells * h / w));
    // The scaler first: Fant box-filters the whole picture down to the mosaic
    // in one pass, so a 2560-wide shot never sits in memory whole. The
    // converter LAST, because the scaler may hand back its own pixel layout
    // (measured: RGBA in, BGRA out when the converter fed the scaler) and
    // only the converter's output format is a promise.
    if (FAILED(fac->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)) ||
        FAILED(fac->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      err = "PNG convert";
      break;
    }
    std::vector<uint8_t> px((size_t)tw * th * 4);
    if (FAILED(conv->CopyPixels(nullptr, tw * 4, (UINT)px.size(), px.data()))) {
      err = "PNG decode";
      break;
    }
    thumb.resize((size_t)tw * th);
    for (size_t i = 0; i < thumb.size(); i++)
      thumb[i] = IM_COL32(px[i * 4 + 0], px[i * 4 + 1], px[i * 4 + 2], 255);
    ok = true;
  } while (false);
  if (scaler) scaler->Release();
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (dec) dec->Release();
  if (fac) fac->Release();
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}
#else
bool LoadMosaicWic(const std::filesystem::path&, uint32_t, uint32_t&, uint32_t&, uint32_t&,
                   uint32_t&, std::vector<uint32_t>&, std::string& err) {
  err = "PNG reading is Windows-only (WIC)";
  return false;
}
#endif

// A mosaic at `cell` px a cell, framed. `px` null = not decoded yet: the box
// it will fill, dark.
void Mosaic(ImDrawList* dl, ImVec2 a, uint32_t tw, uint32_t th, const uint32_t* px, float cell) {
  const ImVec2 b(a.x + tw * cell, a.y + th * cell);
  ui::DropShadow(dl, a, b, 6.0f, 0.5f);
  if (!px) {
    dl->AddRectFilled(a, b, ui::ColDeep());
  } else {
    for (uint32_t y = 0; y < th; y++)
      for (uint32_t x = 0; x < tw; x++) {
        const ImVec2 p(a.x + x * cell, a.y + y * cell);
        dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), px[(size_t)y * tw + x]);
      }
  }
  ui::Draw9(dl, "panel_inner", ImVec2(a.x - 4, a.y - 4), ImVec2(b.x + 4, b.y + 4));
}

std::string LocalTime(const char* fmt) {
  const std::time_t now = std::time(nullptr);
  std::tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &now);
#else
  localtime_r(&now, &lt);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, fmt, &lt);
  return buf;
}

std::string TrimRight(std::string t) {
  while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
  return t;
}

// The first line at or after `from` (a line start) that begins with `prefix`.
size_t FindLine(const std::string& s, const char* prefix, size_t from) {
  const size_t n = std::strlen(prefix);
  for (size_t p = from; p < s.size();) {
    if (s.compare(p, n, prefix) == 0) return p;
    p = s.find('\n', p);
    if (p == std::string::npos) break;
    p++;
  }
  return std::string::npos;
}

// shot_<N>.png -> N, else 0.
int ShotIndex(const std::string& name) {
  if (name.size() < 10 || name.rfind("shot_", 0) != 0 ||
      name.compare(name.size() - 4, 4, ".png") != 0)
    return 0;
  int n = 0;
  for (size_t i = 5; i + 4 < name.size(); i++) {
    if (!std::isdigit((unsigned char)name[i])) return 0;
    n = n * 10 + (name[i] - '0');
    if (n > 100000) return 0;
  }
  return n;
}

// ---- THE TICKET LIST: derived from bug_reports/, scanned when the panel
// opens and thrown away when it closes or folds (a few KB of mosaics). -------
constexpr uint32_t kLibCells = 60;  // a ticket picture's mosaic, cells wide

struct LibShot {
  int n = 0;
  std::filesystem::path png;
  bool tried = false, ok = false;
  uint32_t w = 0, h = 0, tw = kLibCells, th = 32;
  std::vector<uint32_t> thumb;
};
struct LibEntry {
  std::string dir;    // the folder, as WriteBugReport will be handed it
  std::string stamp;  // its name
  std::string text;   // the description as on disk ("" for "(no description)")
  std::vector<std::string> tags;
  std::vector<LibShot> shots;
};
struct Parked {  // the unsaved edit of a ticket not on the panel
  std::string text;
  std::vector<std::string> tags;
};
struct Library {
  bool scanned = false;
  std::vector<LibEntry> entries;
  std::map<std::string, Parked> edits;
  std::vector<std::string> known;  // every tag the panel offers, starter set first
};
Library g_lib;
std::string g_filter;    // the list's tag filter ("" = all); kept across opens
std::string g_tagInput;  // the "new tag" box

bool HasTag(const std::vector<std::string>& t, const std::string& x) {
  return std::find(t.begin(), t.end(), x) != t.end();
}

std::string JoinTags(const std::vector<std::string>& t) {
  std::string s;
  for (const auto& x : t) s += (s.empty() ? "" : ", ") + x;
  return s;
}

void AddKnown(const std::string& tag) {
  if (!tag.empty() && !HasTag(g_lib.known, tag)) g_lib.known.push_back(tag);
}

// Custom tags join <root>/tags.txt (one a line) so the next panel offers
// them. Best effort: a tag that fails to persist is still on its report.
void RememberTags(const std::filesystem::path& root, const std::vector<std::string>& tags) {
  std::vector<std::string> have = BugReportStarterTags();
  {
    std::ifstream f(root / "tags.txt");
    for (std::string l; std::getline(f, l);)
      if (const std::string t = NormalizeBugTag(l); !t.empty()) have.push_back(t);
  }
  std::ofstream out;
  for (const auto& t : tags) {
    if (t.empty() || HasTag(have, t)) continue;
    if (!out.is_open()) out.open(root / "tags.txt", std::ios::app | std::ios::binary);
    out << t << "\n";
    have.push_back(t);
  }
}

// Flow-lay chips of `labels` (ui::Button sizing, the current font) from `at`
// across `width`, plus one `trailing`-wide slot after them when > 0. Returns
// each slot's corner; `bottom` = the y under the last row.
std::vector<ImVec2> LayoutChips(const std::vector<std::string>& labels, ImVec2 at, float width,
                                float trailing, float& bottom) {
  const float h = ImGui::GetTextLineHeight() + 8;
  std::vector<ImVec2> out;
  float x = at.x, y = at.y;
  auto place = [&](float w) {
    if (x > at.x && x + w > at.x + width) {
      x = at.x;
      y += h + kChipGap;
    }
    out.push_back(ImVec2(x, y));
    x += w + kChipGap;
  };
  for (const auto& l : labels) place(ImGui::CalcTextSize(l.c_str()).x + 24);
  if (trailing > 0) place(trailing);
  bottom = y + h;
  return out;
}

std::filesystem::path TicketRoot() {
  // The same folder main.cpp hands WriteBugReport.
  return std::filesystem::path(sandvox::AssetDir() + "/../bug_reports").lexically_normal();
}

void ScanLibrary() {
  namespace fs = std::filesystem;
  g_lib = Library{};
  g_lib.scanned = true;
  std::error_code ec;
  const fs::path root = TicketRoot();
  for (const auto& t : BugReportStarterTags()) AddKnown(t);
  {
    std::ifstream f(root / "tags.txt");
    for (std::string l; std::getline(f, l);) AddKnown(NormalizeBugTag(l));
  }
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    const std::string name = it->path().filename().string();
    if (name == "fixed" || !fs::exists(it->path() / "report.md", e2)) continue;
    LibEntry e;
    e.dir = it->path().string();
    e.stamp = name;
    {
      std::ifstream f(it->path() / "report.md", std::ios::binary);
      std::string md((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      BugReportMd m = ParseBugReportMd(std::move(md));
      e.text = std::move(m.text);
      if (e.text == "(no description)") e.text.clear();
      e.tags = std::move(m.tags);
      for (const auto& t : e.tags) AddKnown(t);
    }
    for (fs::directory_iterator jt(it->path(), e2), jend; !e2 && jt != jend; jt.increment(e2)) {
      const int n = ShotIndex(jt->path().filename().string());
      if (n <= 0) continue;
      LibShot sh;
      sh.n = n;
      sh.png = jt->path();
      e.shots.push_back(std::move(sh));
    }
    std::sort(e.shots.begin(), e.shots.end(),
              [](const LibShot& a, const LibShot& b) { return a.n < b.n; });
    g_lib.entries.push_back(std::move(e));
  }
  // Stamps sort as time: newest first.
  std::sort(g_lib.entries.begin(), g_lib.entries.end(),
            [](const LibEntry& a, const LibEntry& b) { return a.stamp > b.stamp; });
}

LibEntry* FindEntry(const std::string& dir) {
  for (auto& e : g_lib.entries)
    if (e.dir == dir) return &e;
  return nullptr;
}

// One decode a frame, the ticket on the panel first, then each list row's
// first picture: ~30 ms of WIC per 2560x1378 PNG would be a visible stall if
// the whole list decoded at once (the world is paused behind the panel).
void PumpOneMosaic(const std::string& target) {
  LibShot* next = nullptr;
  if (LibEntry* e = target.empty() ? nullptr : FindEntry(target))
    for (auto& sh : e->shots)
      if (!sh.tried) { next = &sh; break; }
  if (!next)
    for (auto& e : g_lib.entries)
      if (!e.shots.empty() && !e.shots[0].tried) { next = &e.shots[0]; break; }
  if (!next) return;
  next->tried = true;
  std::string err;
  next->ok = LoadMosaicWic(next->png, kLibCells, next->w, next->h, next->tw, next->th,
                           next->thumb, err);
  if (!next->ok)
    std::fprintf(stderr, "bug report: thumbnail %s: %s\n", next->png.string().c_str(),
                 err.c_str());
}

// Point the panel at `dir` ("" = the new report), parking the text and tags
// it leaves.
void SelectTicket(UIState::BugReportUI& r, const std::string& dir) {
  if (dir == r.target) return;
  if (r.target.empty()) {
    r.newText = r.text;
    r.newTags = r.tags;
  } else {
    g_lib.edits[r.target] = Parked{r.text, r.tags};
  }
  r.target = dir;
  r.discardArmed = false;
  if (dir.empty()) {
    r.text = r.newText;
    r.tags = r.newTags;
  } else if (auto it = g_lib.edits.find(dir); it != g_lib.edits.end()) {
    r.text = it->second.text;
    r.tags = it->second.tags;
  } else {
    const LibEntry* e = FindEntry(dir);
    r.text = e ? e->text : std::string();
    r.tags = e ? e->tags : std::vector<std::string>{};
  }
}

// `s` cut to `width` px with "..." (the current font).
std::string Fit(const std::string& s, float width) {
  if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
  std::string t = s;
  while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > width) t.pop_back();
  return t + "...";
}

// The first `maxLines` lines of `text` word-wrapped to `width` (newlines
// read as spaces); the last one ends "..." if there was more.
std::vector<std::string> Blurb(const std::string& text, float width, int maxLines) {
  std::vector<std::string> words;
  std::string w;
  for (char c : text) {
    if (std::isspace((unsigned char)c)) {
      if (!w.empty()) words.push_back(w), w.clear();
    } else {
      w += c;
    }
  }
  if (!w.empty()) words.push_back(w);
  std::vector<std::string> lines;
  std::string cur;
  size_t i = 0;
  for (; i < words.size(); i++) {
    const std::string cand = cur.empty() ? words[i] : cur + " " + words[i];
    if (cur.empty() || ImGui::CalcTextSize(cand.c_str()).x <= width) {
      cur = cand;
      continue;
    }
    lines.push_back(cur);
    cur = words[i];
    if ((int)lines.size() == maxLines) break;
  }
  bool more = i < words.size();
  if ((int)lines.size() < maxLines && !cur.empty()) {
    lines.push_back(cur);
    more = false;
  }
  for (auto& l : lines) l = Fit(l, width);
  if (more && !lines.empty()) lines.back() = Fit(lines.back() + " ......", width);
  return lines;
}

// "2026-10-09_15-00-36" -> "2026-10-09 15:00:36"; anything else as is.
std::string StampLabel(const std::string& stamp) {
  if (stamp.size() < 19 || stamp[10] != '_') return stamp;
  std::string t = stamp.substr(0, 19);
  t[10] = ' ';
  t[13] = ':';
  t[16] = ':';
  return t + stamp.substr(19);
}

}  // namespace

// ---- report.md -----------------------------------------------------------------

std::string NormalizeBugTag(const std::string& in) {
  std::string t;
  for (char c : in) {
    const unsigned char u = (unsigned char)c;
    if (std::isspace(u)) {
      if (!t.empty() && t.back() != '-') t += '-';
    } else if (std::isalnum(u) || c == '-' || c == '_') {
      t += (char)std::tolower(u);
    }
  }
  while (!t.empty() && t.back() == '-') t.pop_back();
  while (!t.empty() && t.front() == '-') t.erase(t.begin());
  if (t.size() > 24) t.resize(24);
  return t;
}

const std::vector<std::string>& BugReportStarterTags() {
  static const std::vector<std::string> k = {
      "graphics", "lod",   "water", "physics", "worldgen", "ui",    "combat",
      "mobs",     "audio", "perf",  "spells",  "fire",     "crash",
  };
  return k;
}

BugReportMd ParseBugReportMd(std::string md) {
  md.erase(std::remove(md.begin(), md.end(), '\r'), md.end());
  BugReportMd m;
  size_t p = 0;
  if (md.rfind("# ", 0) == 0) {
    const size_t e = md.find('\n');
    p = e == std::string::npos ? md.size() : e + 1;
    while (p < md.size() && md[p] == '\n') p++;
    m.pre = md.substr(0, p);
  }
  // The tags line, if the next line is one: `tags: lod, graphics`.
  if (md.compare(p, 5, "tags:") == 0) {
    size_t e = md.find('\n', p);
    if (e == std::string::npos) e = md.size();
    const std::string line = md.substr(p + 5, e - p - 5);
    size_t a = 0;
    while (a <= line.size()) {
      size_t c = line.find(',', a);
      if (c == std::string::npos) c = line.size();
      const std::string t = NormalizeBugTag(line.substr(a, c - a));
      if (!t.empty() && !HasTag(m.tags, t)) m.tags.push_back(t);
      a = c + 1;
    }
    p = e;
    while (p < md.size() && md[p] == '\n') p++;
  }
  const size_t h = FindLine(md, "## ", p);
  if (h == std::string::npos) {
    m.text = TrimRight(md.substr(p));
    return m;
  }
  m.text = TrimRight(md.substr(p, h - p));
  size_t t = h;
  if (md.compare(h, 14, "## Screenshots") == 0) {
    const size_t nl = md.find('\n', h);
    t = nl == std::string::npos ? std::string::npos : FindLine(md, "## ", nl + 1);
    if (t == std::string::npos) t = md.size();
    m.shots = md.substr(h, t - h);
    for (size_t k = FindLine(m.shots, "### Shot ", 0); k != std::string::npos;
         k = FindLine(m.shots, "### Shot ", k + 1))
      m.shotCount++;
  }
  m.tail = md.substr(t);
  return m;
}

std::string ComposeBugReportMd(const BugReportMd& m, const std::string& text,
                               const std::vector<std::string>& tags,
                               const std::string& newSections) {
  const std::string pre = m.pre.empty() ? std::string("# Bug report\n\n") : m.pre;
  const std::string t = TrimRight(text);
  std::string out = pre;
  if (!tags.empty()) out += "tags: " + JoinTags(tags) + "\n\n";
  out += (t.empty() ? std::string("(no description)") : t) + "\n\n";
  std::string shots = m.shots;
  if (!newSections.empty()) {
    if (shots.empty()) shots = "## Screenshots\n\n";
    while (shots.size() < 2 || shots.compare(shots.size() - 2, 2, "\n\n") != 0) shots += "\n";
  }
  out += shots + newSections + m.tail;
  return out;
}

std::string BugReportShotSection(int n, uint32_t w, uint32_t h, const std::string& where) {
  char head[128];
  std::snprintf(head, sizeof head, "### Shot %d (%ux%u)\n\n![shot %d](shot_%d.png)\n\n", n, w, h,
                n, n);
  return head + where + (where.empty() ? "" : "\n");
}

bool LoadBugShotMosaic(const std::string& path, uint32_t cells, uint32_t& w, uint32_t& h,
                       uint32_t& tw, uint32_t& th, std::vector<uint32_t>& thumb,
                       std::string& err) {
  return LoadMosaicWic(std::filesystem::path(path), cells, w, h, tw, th, thumb, err);
}

void BugReportAddShot(UIState& s, std::vector<uint8_t> rgba, uint32_t w, uint32_t h,
                      std::string where) {
  UIState::BugReportUI::Shot sh;
  sh.w = w;
  sh.h = h;
  sh.where = std::move(where);
  sh.tw = kThumbCells;
  sh.th = std::max(1u, (uint32_t)std::lround((double)kThumbCells * h / std::max(1u, w)));
  sh.thumb.resize((size_t)sh.tw * sh.th);
  for (uint32_t ty = 0; ty < sh.th; ty++)
    for (uint32_t tx = 0; tx < sh.tw; tx++) {
      const uint32_t x0 = tx * w / sh.tw, x1 = std::max(x0 + 1, (tx + 1) * w / sh.tw);
      const uint32_t y0 = ty * h / sh.th, y1 = std::max(y0 + 1, (ty + 1) * h / sh.th);
      uint64_t r = 0, g = 0, bl = 0, n = 0;
      for (uint32_t y = y0; y < y1 && y < h; y++)
        for (uint32_t x = x0; x < x1 && x < w; x++) {
          const uint8_t* p = &rgba[((size_t)y * w + x) * 4];
          r += p[0];
          g += p[1];
          bl += p[2];
          n++;
        }
      n = std::max<uint64_t>(n, 1);
      sh.thumb[(size_t)ty * sh.tw + tx] =
          IM_COL32((int)(r / n), (int)(g / n), (int)(bl / n), 255);
    }
  sh.rgba = std::move(rgba);
  s.bug.shots.push_back(std::move(sh));
}

// The report's tags as stored: normalized, de-duplicated, in pick order.
static std::vector<std::string> CleanTags(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  for (const auto& t : in)
    if (const std::string n = NormalizeBugTag(t); !n.empty() && !HasTag(out, n)) out.push_back(n);
  return out;
}

bool AppendBugReport(const std::string& dirS, const UIState::BugReportUI& r, std::string& err) {
  namespace fs = std::filesystem;
  const fs::path dir(dirS);
  std::string md;
  {
    std::ifstream f(dir / "report.md", std::ios::binary);
    if (!f) {
      err = "no report.md in " + dirS;
      return false;
    }
    md.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  const BugReportMd m = ParseBugReportMd(std::move(md));
  // Number after the highest picture on EITHER side: a PNG nobody listed and
  // a section whose PNG went missing both keep their number.
  int top = m.shotCount;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    top = std::max(top, ShotIndex(it->path().filename().string()));
  const std::string added = LocalTime("%Y-%m-%d %H:%M:%S");
  std::string sections;
  std::vector<fs::path> written;
  for (size_t i = 0; i < r.shots.size(); i++) {
    const auto& sh = r.shots[i];
    const int n = top + 1 + (int)i;
    const fs::path png = dir / ("shot_" + std::to_string(n) + ".png");
    if (!WritePng(png, sh.rgba, sh.w, sh.h, err)) {
      for (const auto& p : written) fs::remove(p, ec);  // only the files THIS save made
      return false;
    }
    written.push_back(png);
    sections += BugReportShotSection(n, sh.w, sh.h, "- added: " + added + "\n" + sh.where);
  }
  const std::vector<std::string> tags = CleanTags(r.tags);
  const std::string out = ComposeBugReportMd(m, r.text, tags, sections);
  const fs::path tmp = dir / "report.md.tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    f << out;
    if (!f.good()) {
      err = "write report.md.tmp";
      return false;
    }
  }
  fs::rename(tmp, dir / "report.md", ec);
  if (ec) {
    err = "replace report.md: " + ec.message();
    return false;
  }
  RememberTags(dir.parent_path(), tags);
  return true;
}

bool WriteBugReport(const std::string& root, const UIState::BugReportUI& r,
                    const std::string& context, std::string& outDir, std::string& err) {
  namespace fs = std::filesystem;
  if (!r.target.empty()) {
    outDir = r.target;
    return AppendBugReport(r.target, r, err);
  }
  if (r.shots.empty()) {
    err = "a new report needs a picture (F12 takes one)";
    return false;
  }
  const std::string stamp = LocalTime("%Y-%m-%d_%H-%M-%S");
  const std::string human = LocalTime("%Y-%m-%d %H:%M:%S");
  fs::path dir = fs::path(root).lexically_normal() / stamp;
  // Two submits inside one second get their own folders.
  for (int k = 2; fs::exists(dir); k++)
    dir = fs::path(root).lexically_normal() / (stamp + "_" + std::to_string(k));
  outDir = dir.string();
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    err = "mkdir " + outDir + ": " + ec.message();
    return false;
  }
  for (size_t i = 0; i < r.shots.size(); i++) {
    const auto& sh = r.shots[i];
    if (!WritePng(dir / ("shot_" + std::to_string(i + 1) + ".png"), sh.rgba, sh.w, sh.h, err))
      return false;
  }
  const std::string text = TrimRight(r.text);
  std::ofstream md(dir / "report.md", std::ios::binary);
  if (!md) {
    err = "open report.md";
    return false;
  }
  const std::vector<std::string> tags = CleanTags(r.tags);
  md << "# Bug report " << human << "\n\n";
  if (!tags.empty()) md << "tags: " << JoinTags(tags) << "\n\n";
  md << (text.empty() ? std::string("(no description)") : text) << "\n\n";
  md << "## Screenshots\n\n";
  for (size_t i = 0; i < r.shots.size(); i++)
    md << BugReportShotSection((int)i + 1, r.shots[i].w, r.shots[i].h, r.shots[i].where);
  md << "## Context (at the first shot)\n\n" << context;
  if (!md.good()) {
    err = "write report.md";
    return false;
  }
  md.close();
  RememberTags(fs::path(root).lexically_normal(), tags);
  return true;
}

bool BugReportSelectTicket(UIState& s, int index) {
  if (!g_lib.scanned) ScanLibrary();
  if (index < 0 || index >= (int)g_lib.entries.size()) return false;
  SelectTicket(s.bug, g_lib.entries[(size_t)index].dir);
  return true;
}

void DrawBugReport(UIState& s) {
  UIState::BugReportUI& r = s.bug;
  if (r.hideForShot) return;
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  r.toastLeft = std::max(0.0f, r.toastLeft - ImGui::GetIO().DeltaTime);
  const ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoSavedSettings;

  // ---- folded: a strip at the top saying a report is waiting --------------
  if (!r.open) {
    if (g_lib.scanned) g_lib = Library{};  // the list belongs to the open panel; free it
    const bool toast = !r.toast.empty() && r.toastLeft > 0.0f;
    if (!r.collecting && !toast) return;
    ImGui::PushFont(ui::FontSmall());
    char line[320];
    if (r.collecting && !r.target.empty())
      std::snprintf(line, sizeof line,
                    "BUG REPORT  adding to %s  %zu new shot%s   F12 capture next   shift+F12 back to report",
                    std::filesystem::path(r.target).filename().string().c_str(), r.shots.size(),
                    r.shots.size() == 1 ? "" : "s");
    else if (r.collecting)
      std::snprintf(line, sizeof line,
                    "BUG REPORT  %zu shot%s   F12 capture next   shift+F12 back to report",
                    r.shots.size(), r.shots.size() == 1 ? "" : "s");
    else
      std::snprintf(line, sizeof line, "%s", r.toast.c_str());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(line);
    const ImVec2 a(Snap((disp.x - ts.x) * 0.5f - 14), 12), b(Snap(a.x + ts.x + 28), Snap(12 + ts.y + 14));
    ui::PanelBody(dl, a, b);
    dl->AddRectFilled(a, ImVec2(a.x + 2, b.y), r.collecting ? ui::ColBloodHi() : ui::ColGold());
    ui::ShadowText(dl, ImVec2(a.x + 14, Snap(a.y + 7)), ui::ColGoldPale(), line);
    ImGui::PopFont();
    return;
  }
  if (!g_lib.scanned) ScanLibrary();
  const bool existing = !r.target.empty();
  const LibEntry* cur = existing ? FindEntry(r.target) : nullptr;

  // ---- the panel ------------------------------------------------------------
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  ui::ScreenDim(bg, disp, 0.35f, 0.3f);

  // What the picture grid shows: the ticket's own pictures, then the ones
  // taken now (only those can be removed).
  struct Pic {
    uint32_t tw, th, w, h;
    const uint32_t* px;
    int n;          // the number it has (or will have) in the ticket
    int removable;  // index into r.shots, or -1
  };
  std::vector<Pic> pics;
  int nextN = 1;
  if (cur)
    for (const auto& sh : cur->shots) {
      pics.push_back({sh.tw, sh.th, sh.w, sh.h, sh.ok ? sh.thumb.data() : nullptr, sh.n, -1});
      nextN = std::max(nextN, sh.n + 1);
    }
  for (size_t i = 0; i < r.shots.size(); i++) {
    const auto& sh = r.shots[i];
    pics.push_back({sh.tw, sh.th, sh.w, sh.h, sh.thumb.data(), nextN + (int)i, (int)i});
  }

  ImGui::PushFont(ui::FontLarge());
  const float lineH = ImGui::GetTextLineHeight();
  const float panelW = Snap(std::min(kMaxW + kListW + kListGap, disp.x - 80.0f));
  const float inner = panelW - 2 * (kFrame + kPad) - kListW - kListGap;
  // Pictures at 4 px a cell while they fit in two rows, else at 2.
  auto perRowAt = [&](float c) {
    return std::max(1, (int)((inner + kThumbGap) / (kThumbCells * c + kThumbGap)));
  };
  const float cell = (int)pics.size() > 2 * perRowAt(kCell) ? kCell * 0.5f : kCell;
  const float thumbW = kThumbCells * cell;
  const int perRow = perRowAt(cell);
  float thumbH = 0.0f;
  for (const auto& p : pics) thumbH = std::max(thumbH, p.th * cell);
  const int rows = std::max(1, ((int)pics.size() + perRow - 1) / perRow);
  const float capH = 22.0f;  // the "#1  1920x1080" caption under each
  const float shotsH = pics.empty() ? 40.0f : rows * (thumbH + capH + kThumbGap);
  const float subH = lineH + 12;
  // The tag chips: every known tag plus any this report carries that is not
  // (yet) known, then the "new tag" box and its button, in small type.
  std::vector<std::string> chipTags = g_lib.known;
  for (const auto& t : r.tags)
    if (!HasTag(chipTags, t)) chipTags.push_back(t);
  ImGui::PushFont(ui::FontSmall());
  const float addW = ImGui::CalcTextSize("add tag").x + 24;
  float tagsBottom = 0.0f;
  LayoutChips(chipTags, ImVec2(0, 0), inner, kTagInputW + 6 + addW, tagsBottom);
  ImGui::PopFont();
  const float tagsH = subH + tagsBottom + 12;
  const float contentH = kFrame + ui::kHeaderH + 14 + subH + shotsH + tagsH + 40 + kTextH + 18 +
                         lineH + 34 + kPad + kFrame;
  const float panelH = Snap(std::min(disp.y - 40.0f, std::max(contentH, kMinH)));
  const ImVec2 wp(Snap((disp.x - panelW) * 0.5f), Snap((disp.y - panelH) * 0.5f));
  const ImVec2 wb(wp.x + panelW, wp.y + panelH);

  ImGui::SetNextWindowPos(wp);
  ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
  ImGui::Begin("##bugreport", nullptr, kFlags);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ui::PanelStyle st;
  st.darkMix = 0.34f;
  st.shadow = 18.0f;
  ui::PanelBody(dl, wp, wb, st);
  ui::Draw9(dl, "panel", wp, wb);
  const float hb = ui::HeaderBar(dl, ImVec2(wp.x + kFrame, wp.y + kFrame), panelW - 2 * kFrame,
                                 "BUG REPORT", nullptr);
  {
    ImGui::PushFont(ui::FontSmall());
    const char* hint = existing ? "ctrl+enter  save    esc  leave the text box"
                                : "ctrl+enter  submit    esc  leave the text box";
    const ImVec2 hs = ImGui::CalcTextSize(hint);
    ui::ShadowText(dl, ImVec2(Snap(wb.x - kFrame - 14 - hs.x),
                              Snap(wp.y + kFrame + (ui::kHeaderH - hs.y) * 0.5f)),
                   ui::Fade(ui::ColParchDim(), 0.85f), hint);
    ImGui::PopFont();
  }

  const float top = hb + 14;
  const float lx = wp.x + kFrame + kPad;
  const float cx = lx + kListW + kListGap;

  // ---- the ticket list (left column) ------------------------------------------
  {
    const float colH = Snap(wb.y - kFrame - kPad - top);
    const float rx = Snap(cx - kListGap * 0.5f);  // the rule between list and report
    dl->AddRectFilled(ImVec2(rx, top), ImVec2(rx + 2, top + colH), ui::Fade(ui::ColBronze(), 0.6f));
    // The filter: "all" + every tag an open ticket carries. One click.
    std::vector<std::string> used;
    for (const auto& e : g_lib.entries)
      for (const auto& t : e.tags)
        if (!HasTag(used, t)) used.push_back(t);
    std::sort(used.begin(), used.end());
    if (!g_filter.empty() && !HasTag(used, g_filter)) g_filter.clear();
    float listTop = top;
    if (!used.empty()) {
      ImGui::PushFont(ui::FontSmall());
      std::vector<std::string> labels = {"all"};
      labels.insert(labels.end(), used.begin(), used.end());
      float bottom = 0.0f;
      const auto at = LayoutChips(labels, ImVec2(lx, top), kListW, 0.0f, bottom);
      for (size_t i = 0; i < labels.size(); i++) {
        const bool on = i == 0 ? g_filter.empty() : g_filter == labels[i];
        ImGui::PushID((int)i);
        if (ui::Button("##filter", at[i], labels[i].c_str(), on))
          g_filter = i == 0 || on ? std::string() : labels[i];
        if (ImGui::IsItemHovered())
          ui::Tip(i == 0 ? "every open ticket" : "only tickets tagged with this");
        ImGui::PopID();
      }
      ImGui::PopFont();
      listTop = Snap(bottom + 10);
    }
    const float listH = Snap(top + colH - listTop);
    ImGui::SetCursorScreenPos(ImVec2(lx, listTop));
    ImGui::BeginChild("##bugtickets", ImVec2(kListW, listH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* cd = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(ui::FontSmall());
    const float sl = ImGui::GetTextLineHeight();
    const float textW = rowW - 2 * kRowPad;
    float py = base.y;
    // One row: the first picture, a title line, the tags, two lines of text.
    auto row = [&](const char* id, const std::string& title, ImU32 titleCol,
                   const std::string& text, const std::vector<std::string>& tags, uint32_t tw,
                   uint32_t th, const uint32_t* px, bool sel) {
      const float picH = th * kListCell;
      const float h = kRowPad + picH + 10 + sl * 4 + 6 + kRowPad;
      ImGui::SetCursorScreenPos(ImVec2(base.x, py));
      const bool clicked = ImGui::InvisibleButton(id, ImVec2(rowW, h));
      const bool hov = ImGui::IsItemHovered();
      const ImVec2 a(base.x, py), b(base.x + rowW, py + h);
      if (sel) {
        cd->AddRectFilled(a, b, ui::Fade(ui::ColGold(), 0.16f));
        cd->AddRect(a, b, ui::ColGold(), 0.0f, 0, 2.0f);
      } else if (hov) {
        cd->AddRectFilled(a, b, ui::Fade(ui::ColHi(), 0.45f));
      }
      const float picX = Snap(a.x + (rowW - tw * kListCell) * 0.5f);
      Mosaic(cd, ImVec2(picX, Snap(a.y + kRowPad)), tw, th, px, kListCell);
      float ty = Snap(a.y + kRowPad + picH + 10);
      ui::ShadowText(cd, ImVec2(a.x + kRowPad, ty), titleCol, Fit(title, textW).c_str());
      ty += sl + 2;
      ui::ShadowText(cd, ImVec2(a.x + kRowPad, ty),
                     tags.empty() ? ui::Fade(ui::ColParchDim(), 0.7f) : ui::ColSteel(),
                     Fit(tags.empty() ? std::string("untagged") : "[" + JoinTags(tags) + "]", textW)
                         .c_str());
      ty += sl + 2;
      const bool none = text.empty();
      for (const auto& l : Blurb(none ? std::string("(no description)") : text, textW, 2)) {
        ui::ShadowText(cd, ImVec2(a.x + kRowPad, ty), none ? ui::ColParchDim() : ui::ColParch(),
                       l.c_str());
        ty += sl;
      }
      py = Snap(py + h + 8);
      return clicked;
    };
    {
      // The new report: the pictures taken now stand for it while it is the target.
      const bool sel = !existing;
      const auto* first = sel && !r.shots.empty() ? &r.shots[0] : nullptr;
      if (row("##newticket", "NEW REPORT", ui::ColGoldPale(), sel ? r.text : r.newText,
              sel ? r.tags : r.newTags, kThumbCells, first ? first->th : 32,
              first ? first->thumb.data() : nullptr, sel))
        SelectTicket(r, "");
      if (ImGui::IsItemHovered()) ui::Tip("file these pictures as a new ticket");
    }
    for (size_t i = 0; i < g_lib.entries.size(); i++) {
      const LibEntry& e = g_lib.entries[i];
      const bool sel = e.dir == r.target;
      char title[96];
      std::snprintf(title, sizeof title, "%s  %zu shot%s", StampLabel(e.stamp).c_str(),
                    e.shots.size(), e.shots.size() == 1 ? "" : "s");
      const LibShot* first = e.shots.empty() ? nullptr : &e.shots[0];
      std::string text = e.text;
      std::vector<std::string> tags = e.tags;
      if (sel) {
        text = r.text;
        tags = r.tags;
      } else if (auto it = g_lib.edits.find(e.dir); it != g_lib.edits.end()) {
        text = it->second.text;
        tags = it->second.tags;
      }
      // The filter reads the tags as they are on the panel, so a ticket being
      // retagged does not vanish from under the cursor.
      if (!g_filter.empty() && !sel && !HasTag(tags, g_filter)) continue;
      ImGui::PushID((int)i);
      const bool clicked = row("##ticket", title, sel ? ui::ColGoldHi() : ui::ColGold(), text,
                               tags, first ? first->tw : kLibCells, first ? first->th : 32,
                               first && first->ok ? first->thumb.data() : nullptr, sel);
      if (ImGui::IsItemHovered()) ui::Tip("add to this ticket: edit its text, append pictures");
      ImGui::PopID();
      if (clicked) SelectTicket(r, e.dir);
    }
    if (g_lib.entries.empty()) {
      ui::ShadowText(cd, ImVec2(base.x + kRowPad, py), ui::ColParchDim(),
                     Fit("no open tickets", textW).c_str());
      py += sl;
    }
    ImGui::SetCursorScreenPos(ImVec2(base.x, py));
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::PopFont();
    ImGui::EndChild();
  }

  float y = top;
  {
    const std::string head =
        existing ? "TICKET " + std::filesystem::path(r.target).filename().string()
                 : std::string("NEW REPORT");
    y = ui::Subheading(dl, ImVec2(cx, y), inner, head.c_str()) + 6;
  }

  // ---- the pictures -----------------------------------------------------------
  ImGui::PushFont(ui::FontSmall());
  if (pics.empty())
    ui::ShadowText(dl, ImVec2(cx, y + 6), ui::ColParchDim(),
                   Fit("no pictures yet: 'add another', then F12", inner).c_str());
  for (size_t i = 0; i < pics.size(); i++) {
    const Pic& p = pics[i];
    const int col = (int)i % perRow, rw = (int)i / perRow;
    const ImVec2 a(Snap(cx + col * (thumbW + kThumbGap) + 4),
                   Snap(y + rw * (thumbH + capH + kThumbGap) + 4));
    Mosaic(dl, a, p.tw, p.th, p.px, cell);
    char cap[64];
    const bool isNew = existing && p.removable >= 0;
    if (p.w)
      std::snprintf(cap, sizeof cap, "#%d  %ux%u%s", p.n, p.w, p.h, isNew ? "  new" : "");
    else
      std::snprintf(cap, sizeof cap, "#%d", p.n);
    ui::ShadowText(dl, ImVec2(a.x, a.y + p.th * cell + 6), isNew ? ui::ColGoldHi() : ui::ColParch(),
                   Fit(cap, thumbW - (p.removable >= 0 ? 30.0f : 0.0f)).c_str());
    if (p.removable >= 0) {
      ImGui::PushID((int)i);
      if (ui::Button("##rm", ImVec2(a.x + thumbW - 26, a.y + p.th * cell + 4), "x"))
        r.wantRemove = p.removable;
      if (ImGui::IsItemHovered()) ui::Tip("remove this picture");
      ImGui::PopID();
    }
  }
  ImGui::PopFont();
  y = Snap(y + shotsH + 4);

  // ---- the tags -----------------------------------------------------------------
  y = ui::Subheading(dl, ImVec2(cx, y), inner, "TAGS") + 6;
  {
    ImGui::PushFont(ui::FontSmall());
    float bottom = 0.0f;
    const auto at = LayoutChips(chipTags, ImVec2(cx, y), inner, kTagInputW + 6 + addW, bottom);
    for (size_t i = 0; i < chipTags.size(); i++) {
      const bool on = HasTag(r.tags, chipTags[i]);
      ImGui::PushID((int)i);
      if (ui::Button("##tag", at[i], chipTags[i].c_str(), on)) {
        if (on) r.tags.erase(std::find(r.tags.begin(), r.tags.end(), chipTags[i]));
        else r.tags.push_back(chipTags[i]);
      }
      ImGui::PopID();
    }
    // A custom tag: typed, then Enter or "add tag". It is offered from then
    // on (bug_reports/tags.txt, written when the report is saved).
    const ImVec2 ia = at.back();
    ImGui::SetCursorScreenPos(ImVec2(ia.x, ia.y));
    ImGui::SetNextItemWidth(kTagInputW);
    bool add = ImGui::InputTextWithHint(
        "##newtag", "new tag", g_tagInput.data(), g_tagInput.capacity() + 1,
        ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, ResizeCb,
        &g_tagInput);
    if (ui::Button("##addtag", ImVec2(ia.x + kTagInputW + 6, ia.y), "add tag")) add = true;
    if (ImGui::IsItemHovered()) ui::Tip("a tag of your own: lower case, offered from now on");
    if (add) {
      const std::string t = NormalizeBugTag(g_tagInput);
      if (!t.empty()) {
        AddKnown(t);
        if (!HasTag(r.tags, t)) r.tags.push_back(t);
      }
      g_tagInput.clear();
    }
    ImGui::PopFont();
    y = Snap(bottom + 12);
  }

  // ---- the text ---------------------------------------------------------------
  y = ui::Subheading(dl, ImVec2(cx, y), inner, "WHAT WENT WRONG") + 6;
  ImGui::SetCursorScreenPos(ImVec2(cx, y));
  if (r.focusText) {
    ImGui::SetKeyboardFocusHere();
    r.focusText = false;
  }
  // Esc inside an ImGui text box REVERTS everything typed since it was
  // focused. That is a whole bug description gone to the key a player reaches
  // for to "get out of the box" — so Esc keeps the text and only lets go.
  // A focused ImGui text box writes ITS buffer back every frame, so text set
  // from outside (a ticket picked from the list, the scripted run) would be
  // overwritten by what the box held. Let go of the box when that happens;
  // the next click re-reads r.text.
  static std::string seen;
  if (r.text != seen && ImGui::GetActiveID() == ImGui::GetID("##bugtext")) ImGui::ClearActiveID();
  const std::string before = r.text;
  const bool entered = ImGui::InputTextMultiline(
      "##bugtext", r.text.data(), r.text.capacity() + 1, ImVec2(inner, kTextH),
      ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, ResizeCb,
      &r.text);
  if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    r.text = before;
  seen = r.text;
  if (r.text.empty() && !ImGui::IsItemActive()) {
    ImGui::PushFont(ui::FontSmall());
    ui::ShadowText(dl, ImVec2(cx + 10, y + 8), ui::Fade(ui::ColParchDim(), 0.7f),
                   Fit("what you did, what you saw, what you expected. 'add another' to pair more "
                       "pictures.",
                       inner - 20)
                       .c_str());
    ImGui::PopFont();
  }
  // Ctrl+Enter validates a multiline box (plain Enter is a newline).
  if (entered) r.wantSubmit = true;
  y = Snap(y + kTextH + 18);

  // ---- the buttons ------------------------------------------------------------
  float bx = cx;
  if (ui::Button("##add", ImVec2(bx, y), "add another")) r.wantAddAnother = true;
  if (ImGui::IsItemHovered())
    ui::Tip(existing ? "give the game back; F12 takes the next picture for this ticket"
                     : "give the game back; F12 takes the next picture for this same report");
  bx += ImGui::GetItemRectSize().x + 12;
  if (ui::Button("##submit", ImVec2(bx, y), existing ? "save" : "submit", true))
    r.wantSubmit = true;
  if (existing && ImGui::IsItemHovered())
    ui::Tip("write the text and append the new pictures to this ticket");
  bx += ImGui::GetItemRectSize().x + 12;
  const char* discardLabel = r.discardArmed ? "discard? click again"
                             : existing     ? "discard changes"
                                            : "discard";
  if (ui::Button("##discard", ImVec2(bx, y), discardLabel)) {
    if (r.discardArmed) r.wantDiscard = true;
    r.discardArmed = !r.discardArmed;
  }
  if (existing && ImGui::IsItemHovered())
    ui::Tip("drop the new pictures and the edits; the ticket on disk stays as it was");
  bx += ImGui::GetItemRectSize().x + 12;
  {
    char n[64];
    std::snprintf(n, sizeof n, "%zu %spicture%s", r.shots.size(), existing ? "new " : "",
                  r.shots.size() == 1 ? "" : "s");
    const ImVec2 ns = ImGui::CalcTextSize(n);
    if (cx + inner - ns.x > bx)
      ui::ShadowText(dl, ImVec2(Snap(cx + inner - ns.x), Snap(y + 4)), ui::ColParchDim(), n);
  }
  // A failed submit keeps the panel up; say why under the buttons.
  if (!r.toast.empty() && r.toastLeft > 0.0f) {
    ImGui::PushFont(ui::FontSmall());
    // (the last save's "saved" line is still up when the panel reopens soon after)
    const bool good = r.toast.rfind("bug report saved", 0) == 0;
    ui::ShadowText(dl, ImVec2(cx, Snap(y + lineH + 14)), good ? ui::ColGold() : ui::ColBloodHi(),
                   Fit(r.toast, inner).c_str());
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopFont();
  PumpOneMosaic(r.target);
}
