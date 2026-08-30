#pragma once

#include "ui/overlay.h"

// THE CHARACTER SCREEN (I): the panel the armour, weapon, pickup and spell-
// acquisition systems will land into.
//
// Three regions, drawn over a dimmed but still-live world (the sim keeps
// running — this is WoW, not single-player Minecraft, and a real-time engine
// whose MutationQueue is a future network stream cannot honestly pause):
//
//   LEFT    the character panel — a LIVE avatar portrait with armour slots
//           flanking it, sheath + quick slots beneath, and a CHARACTER/HEALTH
//           toggle that swaps the same frame between gear and an injury
//           inspector.
//   TOP-R   the arsenal — every glyph the player owns, and the ten bound slots
//           that ARE the magic-mode number row. Binding here rewires the live
//           hotkeys, because the panel and the keys read one mirror.
//   BOTTOM-R the bag (4x8) and the hotbar row, with drag between all of them.
//
// STRICTLY A DRAWING FUNCTION. It reads UIState's mirrors and writes UIState's
// intent latches; it never touches an Inventory, an Equipment, a GlyphInventory
// or the avatar. main.cpp consumes the latches and calls the real methods —
// the contract ui/overlay.h has always stated, extended to a screen big enough
// that breaking it would be tempting.
void DrawInventoryScreen(UIState& s);
