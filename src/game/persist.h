#pragma once

#include "game/avatar.h"
#include "game/mob.h"
#include "phys/debris.h"
#include "sim/worldio.h"

// The ONE place the entity systems register into the save format
// (sim/worldio.h entities.sve). Both the frame loop and the selftest build
// their EntityIO here, so a system added in one place is persistable in both —
// and a system that is NOT registered here is structurally unable to persist,
// which is exactly the audit finding this closes.
//
// Extension path: give the new system SaveState/LoadState (its bytes are its
// own business), pick a fresh FourCC, append a section here. The container
// never changes; older builds skip the unknown section.
//
// `avatar` is nullable (headless paths without a player body): its section is
// simply absent, and a save without it loads the avatar fresh.
//
// LIFETIME: the returned sections capture the systems by reference; the
// EntityIO must not outlive them.
EntityIO MakeEntityIO(DebrisSystem& debris, MobSystem& mobs,
                      PlayerAvatar* avatar);
