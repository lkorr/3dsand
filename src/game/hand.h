#pragma once
#include <cstdint>
#include <cstring>
#include <string>

// WHICH HAND. The one vocabulary every dual-wield seam speaks: the kit's two
// hand slots (game/equipment.h EquipSlotId::HandR/HandL), the rig's two held
// slots (Mob::held_), the socket context an item is gripped through
// ("held_right"/"held_left", MobDef::sockets and ItemDef::grip), the stroke
// that swings it (StrokeCursor::hand) and the mouse button that fires it
// (LMB = right, RMB = left; session.cpp).
//
// Dependency-free on purpose, like game/kitref.h: the UI, the save, the wire
// and the rig all need to NAME a hand, and none of them should have to pull
// the item system in to do it.
//
// THE RIGHT HAND IS INDEX 0 because every creature authored before dual
// wielding holds its one item in `held_right`, and a zero-initialised
// "which hand" field on any record that predates this header must keep
// meaning what it meant.
enum class Hand : uint8_t { Right = 0, Left = 1 };
constexpr int kHands = 2;

inline int HandIndex(Hand h) { return h == Hand::Left ? 1 : 0; }
inline Hand HandAt(int i) { return i == 1 ? Hand::Left : Hand::Right; }
inline Hand OtherHand(Hand h) {
  return h == Hand::Left ? Hand::Right : Hand::Left;
}

// The socket context an item is gripped through in this hand. A rig publishes
// one socket per context (MobDef::sockets); a def that authors only
// `held_right` gets `held_left` DERIVED at load by mirroring it onto the
// opposite hand (MobDef::MirrorHeldSockets), and an item that authors only
// the right grip is held the same way in the left (ItemDef::Grip).
inline const char* HandContext(Hand h) {
  return h == Hand::Left ? "held_left" : "held_right";
}
inline bool HandFromContext(const char* ctx, Hand& out) {
  if (ctx == nullptr) return false;
  if (std::strcmp(ctx, "held_right") == 0) { out = Hand::Right; return true; }
  if (std::strcmp(ctx, "held_left") == 0) { out = Hand::Left; return true; }
  return false;
}

// +1 for the body's right, -1 for its left: the sign a mirrored stroke flips
// the lateral axis by (strokes.h "THE LEFT HAND IS THE RIGHT, MIRRORED").
inline float HandSideSign(Hand h) { return h == Hand::Left ? -1.0f : 1.0f; }

// For the player's text: "your left hand".
inline const char* HandName(Hand h) {
  return h == Hand::Left ? "left" : "right";
}

// A rig part name's side suffix swapped: "fist.R" <-> "fist.L". How a style
// authored for the right arm names the same weapon on the left one. A name
// with no side suffix is returned as is (a jaw has no mirror).
inline std::string MirrorSideName(const std::string& in) {
  std::string out = in;
  const size_t n = out.size();
  if (n >= 2 && out[n - 2] == '.') {
    if (out[n - 1] == 'R') out[n - 1] = 'L';
    else if (out[n - 1] == 'L') out[n - 1] = 'R';
  }
  return out;
}
