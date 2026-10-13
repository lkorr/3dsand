#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/overlay.h"

// THE BUG REPORT (F12): screenshots + a description, filed into one folder.
//
//   F12        photograph the frame as presented (HUD, panels, TAA and all),
//              then pause and open the report panel over it. The last second
//              is kept too, as a 4x4 contact sheet (ui/framering.h); each
//              picture's "1 s" toggle files the sheet instead of the still.
//   add another  fold the panel away and give the game back; the next F12
//              appends a picture to the SAME report. Shift+F12 opens the panel
//              without taking one.
//   submit     writes bug_reports/<YYYY-MM-DD_HH-MM-SS>/report.md + shot_N.png
//              beside assets/ (gitignored). Every picture carries its own
//              camera and a `--at x,y,z,yaw,pitch` line that boots the game
//              standing there, looking that way.
//
// THE TICKET LIST. The panel's left column lists the OPEN tickets (every
// folder directly in bug_reports/, newest first; `fixed/` is not shown) by
// their first picture and the start of their text, under a "new report" row.
// Picking a ticket points the panel at it: its pictures are shown, its text
// is loaded into the box, and "save" appends the pictures taken now as
// shot_{N+1}.png ... with their own `### Shot N` sections and rewrites the
// text, leaving every existing shot section and the `## Context` block as
// they were. Nothing ever deletes a ticket or one of its pictures.
//
// TAGS. A report carries any number of category tags ("lod", "physics",
// ...) as ONE line right under the title, `tags: lod, graphics`, so an agent
// finds every LOD ticket with
//   grep -l "^tags:.*\blod\b" bug_reports/*/report.md
// Tags are lower-case [a-z0-9-_]. The panel offers a starter set plus every
// tag in bug_reports/tags.txt (custom tags are appended there on save) and
// every tag an open ticket uses; the ticket list filters by one.
//
// The panel is a drawing function over UIState::bug, like the conversation
// panel: it writes intent latches (wantSubmit / wantAddAnother / wantDiscard /
// wantRemove) and main.cpp acts on them — capture, pause, cursor, the write.
// Nothing here touches the sim.
void DrawBugReport(UIState& s);

// Append one screenshot (RGBA8, top row first) to the open report and build
// its pixel-mosaic thumbnail. `where` is that picture's camera (eye, view,
// crosshair, the `--at` line that starts the game there), written under its
// heading in report.md. `sheet` (optional) is the last second as a contact
// sheet (ui/framering.h) with `sheetNote` naming its tiles; the panel lets the
// owner file that instead of the still, per picture.
void BugReportAddShot(UIState& s, std::vector<uint8_t> rgba, uint32_t w, uint32_t h,
                      std::string where, std::vector<uint8_t> sheet = {},
                      uint32_t sheetW = 0, uint32_t sheetH = 0, std::string sheetNote = {});

// Write the report. With `r.target` empty: a NEW folder <root>/<stamp>/ with
// report.md (the text, the shots inline, and `context` as a trailing section)
// plus shot_1.png ... With `r.target` naming a ticket folder: APPEND to it
// (see AppendBugReport; `root` and `context` are unused). Returns false with
// `err` set on failure; `outDir` names the folder written either way.
bool WriteBugReport(const std::string& root, const UIState::BugReportUI& r,
                    const std::string& context, std::string& outDir, std::string& err);

// Point the panel at the index'th open ticket (0 = newest), scanning the
// ticket folder if it has not been. False if there is no such ticket. The
// scripted F12 run (SANDVOX_BUGREPORT_SCRIPT) uses it; the panel's own
// clicks do the same thing.
bool BugReportSelectTicket(UIState& s, int index);

// ---- report.md, as text (pure; the `bugreport-md` gate) --------------------
//
// A report.md is: a `# Bug report <time>` line, an optional `tags: a, b` line,
// the owner's description, then
// `## Screenshots` with one `### Shot N (WxH)` section per picture, then
// `## Context (at the first shot)` and whatever a working session appended
// after it (`## Progress`). The split keeps everything but the description
// VERBATIM so a rewrite cannot disturb what it did not mean to change.
struct BugReportMd {
  std::string pre;    // the title line and the blank line(s) after it
  std::vector<std::string> tags;  // the `tags:` line under it (absent = none)
  std::string text;   // the description, trailing whitespace trimmed
  std::string shots;  // from "## Screenshots" up to the next "## " heading
  std::string tail;   // that heading ("## Context ...") to the end of the file
  int shotCount = 0;  // "### Shot " headings seen in `shots`
};
BugReportMd ParseBugReportMd(std::string md);
// The file again, with `text` as the description, `tags` as the tags line
// (none = no line) and `newSections` (whole `### Shot N` sections) appended
// after the existing ones. Compose(Parse(md), parsed.text, parsed.tags, "")
// == md for a file the game wrote.
std::string ComposeBugReportMd(const BugReportMd& m, const std::string& text,
                               const std::vector<std::string>& tags,
                               const std::string& newSections);
// A tag as stored: trimmed, lower case, spaces to '-', anything outside
// [a-z0-9-_] dropped. "" = not a tag.
std::string NormalizeBugTag(const std::string& t);
// The tags every panel offers before any custom one.
const std::vector<std::string>& BugReportStarterTags();
// One `### Shot N` section exactly as a new report writes it.
std::string BugReportShotSection(int n, uint32_t w, uint32_t h, const std::string& where);

// Append `r.shots` to the ticket folder `dir` (numbered after the highest
// shot already there, on disk or in report.md) and replace its description
// with `r.text` and its tags with `r.tags`. report.md is rewritten through a temp file + rename.
bool AppendBugReport(const std::string& dir, const UIState::BugReportUI& r, std::string& err);

// Decode a PNG straight down to a `cells`-wide mosaic (IM_COL32 order),
// reporting the picture's own size. WIC; false with `err` off Windows.
bool LoadBugShotMosaic(const std::string& path, uint32_t cells, uint32_t& w, uint32_t& h,
                       uint32_t& tw, uint32_t& th, std::vector<uint32_t>& thumb,
                       std::string& err);
