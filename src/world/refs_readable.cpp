// refs_readable.cpp — the `readable` ref kind (docs/PLAN_demons.md D2).
//
// A THING YOU READ: a book on a lectern, a note pinned to a door, a carved
// stone. The smallest general mechanism the plan asked for, and deliberately
// not an item: the thing is VOXELS someone built (the edit layer, a house's
// blueprint, a brush) and the ref only says "this cell reads as <dialogue>".
//
//   { "id": "harrowby/smithy_notes", "kind": "readable", "pos": [x, y, z],
//     "props": { "title": "Osric's daybook", "dialogue": "osric_notes" } }
//
//   props.dialogue  assets/dialogue/<name>.json, REQUIRED. Its nodes are the
//                   pages: [continue] turns the page, `choices` are fine too.
//                   The file's `speaker` is the header ("the book"); there is
//                   no body, so the conversation has speaker mob id 0 and is
//                   never ended for "the speaker is gone".
//   props.title     what the prompt and the header call it ("Read <title>").
//                   Default: "the page".
//   props.verb      the prompt's verb (default "Read"): "Study", "Look at".
//
// Using it is the ordinary use verb (G; refs_game.cpp TickRefs, from
// TickInput::useRef inside TickAuthority), so a read is replayed like every
// other use and anything the pages DO (dialogue `do`: set / give / grant /
// learn ...) runs on the tick. Nothing here writes a voxel, has a delta, or
// ticks: a readable costs nothing while nobody reads it (rule 2).

#include <string>
#include <vector>

#include "game/dialogue.h"
#include "world/refs.h"

namespace refs {

namespace {

std::string Prop(const Ref& r, const char* k, const char* dflt) {
  auto it = r.props.find(k);
  return (it != r.props.end() && it->is_string() && !it->get<std::string>().empty())
             ? it->get<std::string>()
             : std::string(dflt);
}

}  // namespace

void RegisterReadableKinds() {
  RefKind k;
  k.name = "readable";
  k.validate = [](const Ref& r, std::vector<std::string>& p) {
    auto it = r.props.find("dialogue");
    if (it == r.props.end() || !it->is_string() || it->get<std::string>().empty())
      p.push_back("props.dialogue: names the conversation file that holds the pages "
                  "(assets/dialogue/<name>.json)");
    for (const char* s : {"title", "verb"}) {
      auto t = r.props.find(s);
      if (t != r.props.end() && !t->is_string())
        p.push_back(std::string("props.") + s + ": is text");
    }
  };
  k.usePrompt = [](RefCtx&, const Ref& r) {
    return Prop(r, "verb", "Read") + " " + Prop(r, "title", "the page");
  };
  k.onUse = [](RefCtx& c, const Ref& r, RefUse& u) {
    const std::string title = Prop(r, "title", "the page");
    if (c.talk == nullptr || u.session == nullptr) {
      u.message = "There is writing on " + title + ", but nothing to read it with here.";
      return;
    }
    dialogue::Speaker spk;
    spk.refId = r.id;
    spk.name = title;
    std::string why;
    if (!dialogue::Begin(*c.talk, *u.session, spk, Prop(r, "dialogue", ""), c.tick, &why))
      u.message = title + ": " + why;
  };
  // A book is small: the look ray must pass within ~0.4 m of its cell.
  k.useRadius = 4.0f;
  Kinds().Register(std::move(k));
}

}  // namespace refs
