#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/overlay.h"

// THE BUG REPORT (F12): screenshots + a description, filed into one folder.
//
//   F12        photograph the frame as presented (HUD, panels, TAA and all),
//              then pause and open the report panel over it.
//   add another  fold the panel away and give the game back; the next F12
//              appends a picture to the SAME report. Shift+F12 returns to the
//              panel without taking one.
//   submit     writes bug_reports/<YYYY-MM-DD_HH-MM-SS>/report.md + shot_N.png
//              beside assets/ (gitignored). Every picture carries its own
//              camera and a `--at x,y,z,yaw,pitch` line that boots the game
//              standing there, looking that way.
//
// The panel is a drawing function over UIState::bug, like the conversation
// panel: it writes intent latches (wantSubmit / wantAddAnother / wantDiscard /
// wantRemove) and main.cpp acts on them — capture, pause, cursor, the write.
// Nothing here touches the sim.
void DrawBugReport(UIState& s);

// Append one screenshot (RGBA8, top row first) to the open report and build
// its pixel-mosaic thumbnail. `where` is that picture's camera (eye, view,
// crosshair, the `--at` line that starts the game there), written under its
// heading in report.md.
void BugReportAddShot(UIState& s, std::vector<uint8_t> rgba, uint32_t w, uint32_t h,
                      std::string where);

// Write the report: <root>/<stamp>/report.md (the text, the shots inline, and
// `context` as a trailing section) plus shot_1.png ... Returns false with
// `err` set on failure; `outDir` names the folder written either way.
bool WriteBugReport(const std::string& root, const UIState::BugReportUI& r,
                    const std::string& context, std::string& outDir, std::string& err);
