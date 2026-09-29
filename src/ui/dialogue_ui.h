#pragma once

#include "ui/overlay.h"

// THE CONVERSATION PANEL (docs/PLAN_world_editor.md §2.7, P3).
//
// A strip of the character screen's chrome along the bottom of the screen:
// the speaker's name in the header bar, a monogram in the portrait slot, the
// line wrapped in the pixel font, and the choices as numbered rows under a
// bronze rule — "[continue]" when the node has none.
//
// STRICTLY A DRAWING FUNCTION, like DrawInventoryScreen. It reads
// UIState::talk (the mirror main.cpp fills from dialogue::MakeView) and
// writes one latch, `talk.pick`, when a row is clicked. main.cpp puts the
// latch AND the 1-9 / Space / Esc keys into the tick command, and the tick
// advances the conversation. The panel never does.
void DrawDialoguePanel(UIState& s);
