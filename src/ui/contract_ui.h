#pragma once

#include "ui/overlay.h"

// THE CONTRACT EDITOR AND THE DEMON CONVERSATION'S STRIP (docs/PLAN_demons.md
// D5; game/contract.h, game/demon_talk.h).
//
// Both are STRICTLY DRAWING FUNCTIONS over UIState, like the character screen:
//
//   DrawContractEditor  UIState::contractEd. Opened from the spellbook's
//                       header ("contracts"). A list of the player's pages and
//                       the stock ones on the left; the page being written on
//                       the right as CLAUSE CARDS built from tokens -- the kind
//                       (MUST / NEVER / IF), the verb, who it is about, the
//                       condition, the consequence -- each token a click that
//                       steps through its choices. SIMPLE by default (a stock
//                       template plus "add clause"); FULL shows the selector,
//                       trigger and counter text fields. The WEIGHT is
//                       re-computed every frame from contract_tariff.json, one
//                       line per term, with the UPKEEP it will reserve. The
//                       only write is `contractEd.op` (save / delete), which
//                       main.cpp applies to PlayerCaster::contracts.
//   DrawDemonTalkStrip  UIState::demonTalk, over the conversation panel while
//                       the speaker is a contained demon: the circle against
//                       its power, the gaze (and a look-away toggle), the
//                       contract it is bound by, and -- after the dialogue's
//                       present_contract -- the picker. Its latches
//                       (presentPick, lookAwayToggle) become the tick command
//                       in main.cpp.
//
// Pixel chrome (ui/theme.h) and every string fitted to its box (Fit()).
void DrawContractEditor(UIState& s);
void DrawDemonTalkStrip(UIState& s);
