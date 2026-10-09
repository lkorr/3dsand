// selftest_bugreport.cpp — the F12 ticket files (ui/bugreport.h).
//
// `bugreport-md`: CPU only, a temp dir, milliseconds. The report.md split
// (description / shot sections / context) round-trips byte for byte, an
// append rewrites only the description and adds `### Shot N` sections before
// `## Context`, and a real new report + append through the PNG writer and the
// mosaic reader lands shot_2.png with the right section and colours. The
// panel itself is ImGui and is exercised by SANDVOX_BUGREPORT_SCRIPT=1.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test/selftest.h"
#include "ui/bugreport.h"

namespace selftest {
namespace {

std::string ReadAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

UIState::BugReportUI::Shot Solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b,
                                 const std::string& where) {
  UIState::BugReportUI::Shot sh;
  sh.w = w;
  sh.h = h;
  sh.where = where;
  sh.rgba.resize((size_t)w * h * 4);
  for (size_t i = 0; i < (size_t)w * h; i++) {
    sh.rgba[i * 4 + 0] = r;
    sh.rgba[i * 4 + 1] = g;
    sh.rgba[i * 4 + 2] = b;
    sh.rgba[i * 4 + 3] = 255;
  }
  return sh;
}

Status GateBugReportMd(Ctx&, std::string& detail) {
  int checks = 0;
  bool ok = true;
  auto check = [&](bool c, const char* what) {
    checks++;
    if (!c) {
      ok = false;
      std::printf("  bugreport-md FAIL: %s\n", what);
    }
  };

  // ---- 1. the game's own layout round-trips --------------------------------
  const std::string ctx =
      "## Context (at the first shot)\n\n- map: default\n- sim tick: 452\n";
  const std::string md =
      "# Bug report 2026-10-07 20:46:57\n\nfirst line\nsecond line\n\n## Screenshots\n\n" +
      BugReportShotSection(1, 1880, 1441, "- eye: (1, 2, 3)\n- go there: `x`\n") +
      BugReportShotSection(2, 1880, 1441, "") + ctx + "\n## Progress\n\nhalf fixed\n";
  const BugReportMd m = ParseBugReportMd(md);
  check(m.pre == "# Bug report 2026-10-07 20:46:57\n\n", "the title line is kept apart");
  check(m.text == "first line\nsecond line", "the description is the text above ## Screenshots");
  check(m.shotCount == 2, "both shot sections counted");
  check(m.tail.rfind("## Context", 0) == 0, "the tail starts at ## Context");
  check(ComposeBugReportMd(m, m.text, m.tags, "") == md, "parse + compose is byte-identical");

  // CRLF in (an editor saved it) reads the same.
  {
    std::string crlf;
    for (char c : md) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    check(ParseBugReportMd(crlf).text == m.text, "a CRLF file parses to the same text");
  }

  // ---- 2. an edit + an appended section ------------------------------------
  {
    const std::string sec = BugReportShotSection(3, 64, 32, "- added: now\n- eye: (4, 5, 6)\n");
    const std::string out = ComposeBugReportMd(m, "rewritten\n\n\n", {}, sec);
    const size_t s1 = out.find("### Shot 1"), s3 = out.find("### Shot 3"),
                 c = out.find("## Context"), pr = out.find("## Progress");
    check(out.find("rewritten\n\n## Screenshots") != std::string::npos,
          "the new text replaces the old, trailing blank lines trimmed");
    check(out.find("first line") == std::string::npos, "the old text is gone");
    check(s1 != std::string::npos && s3 != std::string::npos && s1 < s3 && s3 < c && c < pr,
          "the new section lands after the old ones and before ## Context");
    check(out.find("![shot 3](shot_3.png)") != std::string::npos, "the new section links its PNG");
    check(out.substr(c) == m.tail, "context and progress are untouched");
    check(ParseBugReportMd(out).shotCount == 3, "the result parses as three shots");
    check(ComposeBugReportMd(m, "", {}, "").find("(no description)") != std::string::npos,
          "an emptied description is written as (no description)");
  }

  // ---- 3. a file with no ## Screenshots (hand-written) ----------------------
  {
    const BugReportMd h = ParseBugReportMd("# Bug report x\n\njust words\n");
    check(h.text == "just words" && h.shotCount == 0, "a bare report parses");
    const std::string out = ComposeBugReportMd(h, h.text, {}, BugReportShotSection(1, 8, 8, ""));
    check(out.find("just words\n\n## Screenshots\n\n### Shot 1") != std::string::npos,
          "appending to it adds the ## Screenshots heading");
  }

  // ---- 3b. tags ---------------------------------------------------------------
  {
    check(m.tags.empty(), "a report with no tags line is untagged");
    check(NormalizeBugTag("  Far Field LOD! ") == "far-field-lod" && NormalizeBugTag(" , ").empty(),
          "tags normalize to lower-case [a-z0-9-_]");
    const std::string tagged =
        "# Bug report t\n\ntags: lod, graphics\n\nthe seam\n\n## Screenshots\n\n" +
        BugReportShotSection(1, 8, 8, "") + ctx;
    const BugReportMd t = ParseBugReportMd(tagged);
    check(t.tags == std::vector<std::string>{"lod", "graphics"} && t.text == "the seam",
          "the tags line under the title parses, and is not the description");
    check(ComposeBugReportMd(t, t.text, t.tags, "") == tagged, "a tagged report round-trips");
    check(ParseBugReportMd("# x\n\ntags: LOD,, lod , Water\n\ny\n").tags ==
              std::vector<std::string>{"lod", "water"},
          "a hand-edited tags line is normalized and de-duplicated");
    const std::string untag = ComposeBugReportMd(t, t.text, {}, "");
    check(untag.find("tags:") == std::string::npos && ParseBugReportMd(untag).text == "the seam",
          "dropping every tag drops the line");
    const std::string retag = ComposeBugReportMd(m, m.text, {"physics"}, "");
    check(retag.rfind("# Bug report 2026-10-07 20:46:57\n\ntags: physics\n\nfirst line", 0) == 0,
          "tagging an untagged report puts the line right under the title");
  }

  // ---- 4. the real files: a new report, then an append to it ---------------
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "sandvox_bugreport_gate";
  std::error_code ec;
  fs::remove_all(root, ec);
  {
    UIState::BugReportUI r;
    r.text = "the new ticket";
    r.tags = {"lod", "My Custom"};
    r.shots.push_back(Solid(64, 32, 200, 40, 40, "- eye: first\n"));
    std::string dir, err;
    check(WriteBugReport(root.string(), r, "- map: harness\n", dir, err), "a new report writes");
    check(fs::exists(fs::path(dir) / "shot_1.png"), "shot_1.png written");
    check(ReadAll(fs::path(dir) / "report.md").find("\n\ntags: lod, my-custom\n\nthe new ticket") !=
              std::string::npos,
          "a new report writes its tags line under the title");
    check(ReadAll(root / "tags.txt") == "my-custom\n",
          "a custom tag (and only it, not a starter one) joins tags.txt");

    UIState::BugReportUI none;
    std::string d2, e2;
    check(!WriteBugReport(root.string(), none, "", d2, e2) && !e2.empty(),
          "a new report with no picture is refused");

    UIState::BugReportUI add;
    add.target = dir;
    add.text = "the new ticket\n\nand an addition";
    add.tags = {"lod", "physics"};
    add.shots.push_back(Solid(64, 32, 40, 40, 200, "- eye: second\n"));
    std::string d3;
    check(WriteBugReport(root.string(), add, "IGNORED", d3, err) && d3 == dir,
          "an append writes into the ticket's own folder");
    const std::string after = ReadAll(fs::path(dir) / "report.md");
    const BugReportMd a = ParseBugReportMd(after);
    check(a.text == "the new ticket\n\nand an addition", "the edited text is on disk");
    check(a.tags == std::vector<std::string>{"lod", "physics"}, "the edited tags are on disk");
    check(a.shotCount == 2 && fs::exists(fs::path(dir) / "shot_2.png"),
          "shot_2.png and its section are on disk");
    check(after.find("- eye: first\n") < after.find("### Shot 2") &&
              after.find("- eye: second\n") > after.find("### Shot 2"),
          "each picture keeps its own camera lines");
    check(after.find("IGNORED") == std::string::npos &&
              a.tail == "## Context (at the first shot)\n\n- map: harness\n",
          "the context block is the first report's, unchanged");
    check(!fs::exists(fs::path(dir) / "report.md.tmp"), "no temp file left behind");
    int dirs = 0;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
      dirs += it->is_directory() ? 1 : 0;
    check(dirs == 1, "an append makes no new folder");

    uint32_t w = 0, h = 0, tw = 0, th = 0;
    std::vector<uint32_t> px;
    std::string le;
    const bool loaded =
        LoadBugShotMosaic((fs::path(dir) / "shot_2.png").string(), 16, w, h, tw, th, px, le);
    check(loaded && w == 64 && h == 32 && tw == 16 && th == 8 && px.size() == 128,
          "the mosaic reader sizes the written PNG");
    // IM_COL32 packs R in the low byte.
    if (loaded && !px.empty())
      std::printf("  bugreport-md: shot_2 mosaic[0] = 0x%08x (want 0xffc82828)\n", px[0]);
    check(loaded && !px.empty() && (px[0] & 0xFF) == 40 && ((px[0] >> 16) & 0xFF) == 200,
          "and reads back its colour");
  }
  fs::remove_all(root, ec);

  detail = Format("%d checks", checks);
  std::printf("bugreport-md: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& BugReportGates() {
  static const std::vector<Gate> g = {
      {"bugreport-md", "ui", {}, false, GateBugReportMd},
  };
  return g;
}

}  // namespace selftest
