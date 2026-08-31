/* ==========================================================================
   Sound slot catalog — the single description of every sound a thing can make.
   ==========================================================================
   The Audio tab and the wiki's Audio section are both built entirely from this
   table, and nothing else in the tuner knows what a "footstep" or a "hurt" is.
   That is the point: adding "the sound a mob makes when it lands" should be one
   row here plus the C++ that fires it, never a new panel.

   A SLOT is one authored binding: an owner (a material, a mob) names a sound
   SET for one event. Sets are folders under assets/sounds/ — see
   src/audio/library.h; the tuner never invents structure the engine cannot
   scan.

   Slot fields:
     k        key written into the owner's JSON
     n        display name
     d        what triggers it, in plain terms — shown as help text
     prefix   the sound-set namespace this slot binds into. An authored value
              of "leaf" in a slot with prefix 'footsteps' resolves to the set
              "footsteps/leaf". This mirrors cues.cpp, which does exactly that
              concatenation, and it is why the tuner can offer a correct set
              list per slot rather than every set in the project.
     fires    where the engine calls it, so the wiki can say "nothing triggers
              this yet" honestly instead of implying a wired-up feature
     fallback how a missing binding is resolved, described for the reader. The
              real fallback lives in C++ (cues.cpp FallbackFootstep); this
              string must be kept honest against it.
     gain/pitch  presentation notes shown on the slot

   OWNER KINDS. `store` says where a binding is written:
     'material'  materials.json, in the material's "sounds" object
     'mob'       assets/mobs/<name>.json, in its "sounds" object
   Both are edited in place through the tuner's existing save paths — a sound
   binding is an ordinary authored field, not a fourth file to keep in sync.

   BACK COMPATIBILITY. materials.json historically carried a flat
   "footstep": "leaf" key. Both the engine and this tuner still read it, and
   the tuner writes through to whichever form the material already uses, so an
   old file is never silently rewritten into a new shape.               */

const SOUND_SCHEMA = {

  // ---- materials: what a SURFACE sounds like ----------------------------
  material: {
    store: 'material',
    title: 'Surface sounds',
    icon: '\u{1FAA8}',
    blurb: 'What this material sounds like when something interacts with it. ' +
           'Bindings are by MATERIAL, but a material that names nothing falls ' +
           'back by TAG — the same guard against the N×M explosion that ' +
           'reactions use, so a new material is audible the day it is added.',
    // The legacy flat key, still honoured on read and preserved on write.
    legacyKey: 'footstep',
    legacySlot: 'footstep',
    slots: [
      {k:'footstep', n:'footstep', prefix:'footsteps',
       d:'A foot planted on this surface. Fires from the avatar’s own gait at the frame each foot plants, not from a distance accumulator, so it stays in step with the animation.',
       fires:'audio::Cues::Footstep — src/audio/cues.cpp',
       fallback:'by tag: foliage→leaf, organic→branch, soil/mineral→path; liquids and gases are deliberately silent.',
       gain:'scales with walk→sprint speed (Audio tuning group)',
       pitch:'random jitter ± a fixed left/right detune'},

      {k:'land', n:'landing', prefix:'footsteps',
       d:'Touchdown after a fall onto this surface. Louder and pitched down with impact speed — that pitch drop is what separates a landing from a step without a second set of samples.',
       fires:'audio::Cues::Land — src/audio/cues.cpp',
       fallback:'the footstep set for this material.',
       gain:'scales with fall speed up to the landing-full-speed knob',
       pitch:'down to −22% at full impact'},

      {k:'impact', n:'impact', prefix:'impacts',
       d:'Debris or a thrown body striking this material. The material named is the one that was STRUCK, read from the grid at the contact point — a log landing on stone sounds like stone.',
       fires:'audio::Cues::Impact — from DebrisSystem::ImpactEvents(), a Jolt contact listener. Gated on contact speed (audio.impactMinSpeed), rate-limited per body (audio.impactMinGap) and capped per step, so a settling pile is silent. Also fired from the MPM fluid splash in main.cpp.',
       fallback:'the footstep set for this material, pitched and gained differently.',
       gain:'scales with impact energy',
       pitch:'heavier impact → lower'},

      {k:'break', n:'break / shatter', prefix:'breaks',
       d:'This material coming apart. Fires when a chunk of it loses its support and detaches as a rigidbody — a limb cut off a tree, a ledge undermined, a wall blasted through. The piece’s dominant material decides the sound, so a mostly-wood island snaps like wood even with a little ash in it.',
       fires:'audio::Cues::Break — src/audio/cues.cpp, from DebrisSystem::BreakEvents()',
       fallback:'silent — a step is not a shatter, so there is deliberately no surrogate.',
       gain:'scales with the size of the piece',
       pitch:'centre set by piece size (twig high, log low), then a random ±breakPitchSemitones per event'},

      {k:'ambience', n:'ambience loop', prefix:'ambience',
       d:'A positioned looping bed for a body of this material, e.g. a lava lake or a waterfall. Automatic: the engine scans the CPU mirror around the player twice a second and keeps ONE loop on the largest nearby body of an ambience-bound material. Its position is the CENTROID of that body (so a shoreline pans toward the water as you walk along it), and its gain is how much of the material is nearby — a puddle is under the floor and silent, a lake is at full gain.',
       fires:'audio::Cues::ProbeAmbience + UpdateAmbience — src/audio/cues.cpp, driven from Cues::Update. Only ONE bed plays at a time: two lakes on opposite sides of the player is a clustering problem the engine deliberately does not try to answer.',
       fallback:'silent.',
       gain:'audio.ambienceVolume, scaled by how much of the material is nearby',
       radius:'audio.ambienceRadius'},
    ],
  },

  // ---- mobs: what a CREATURE sounds like --------------------------------
  // The slot list a character author actually reaches for. Drop a .wav on one
  // of these and it lands in mobs/<mob>/<slot>/ as a numbered variant.
  mob: {
    store: 'mob',
    title: 'Creature sounds',
    icon: '\u{1F43A}',
    blurb: 'Bound per mob in its own .json sidecar, so a mob stays one .vox ' +
           'plus one .json with its sounds included. A slot left empty is ' +
           'silent — there is no cross-mob fallback, because one creature ' +
           'borrowing another’s voice is always wrong.',
    slots: [
      {k:'hurt', n:'takes damage', prefix:'mobs',
       d:'Struck but still alive. Fires once per damage event, so a burst of hits should not fire a burst of voices — the engine rate-limits it twice over: MobSystem emits at most one hurt per creature per tick, and the audio layer then enforces a minimum gap per source.',
       fires:'audio::Cues::MobSound(Hurt) — from MobSystem::VoiceEvents(), raised by MobSystem::Damage (laser, melee) and by CarveLimb (explosions, blasts, the laser kerf) on the surviving path. A blow that severs says nothing here: the sever cue already speaks, and falls back to this set.',
       fallback:'silent.',
       gain:'scales with the fraction of max hp removed'},

      {k:'death', n:'dies', prefix:'mobs',
       d:'The killing blow. Fired once, at the moment the mob is marked dead, before the ragdoll takes over — positioned on the root limb’s live transform, not on the spawn corner.',
       fires:'audio::Cues::MobSound(Death) — from MobSystem::VoiceEvents(), raised in MobSystem::Die(), which is the single choke point every kill funnels through.',
       fallback:'silent.'},

      {k:'sever', n:'limb severed', prefix:'mobs',
       d:'A limb comes off. Distinct from taking damage because dismemberment is a state change the player should hear, not merely a bigger hit. This is the CREATURE’s voice; the wet mechanical sound of the cut itself is the `dismember` slot, and a sword hit plays both.',
       fires:'audio::Cues::MobSound(Sever) — src/audio/cues.cpp',
       fallback:'the hurt set, if one is bound.'},

      {k:'dismember', n:'cut apart (blade)', prefix:'mobs',
       d:'The CUT — flesh and bone parting under an edge, as opposed to the creature’s cry, which is `sever`. Fires only when a limb is taken off by a BLADE: an explosion or a laser removes the same limb without this sound, because neither one saws through anything. Both this and `sever` fire for one sword blow.',
       fires:'audio::Cues::MobSound(Dismember) — from the melee sweep in main.cpp',
       fallback:'silent.',
       gain:'scales with blade speed',
       pitch:'random jitter, and lower for a faster cut'},

      {k:'bleed', n:'bleeding (loop)', prefix:'gore',
       d:'A positioned loop that runs while this creature is losing a lot of blood, and fades out as the wound does. Started when the bleed budget crosses the on-threshold — a scratch is silent, an amputation is not — and its gain tracks how hard the wound is still pumping, so one loop covers everything from a deep cut to a fresh stump.',
       fires:'audio::Cues::MobBleed — src/audio/cues.cpp, driven from main.cpp',
       fallback:'silent.',
       gain:'tracks the wound’s remaining bleed budget'},

      {k:'idle', n:'idle vocal', prefix:'mobs',
       d:'Occasional noise while alive and unaware. The cheapest way to make a world feel inhabited — and the fastest way to make it maddening, so keep the interval long.',
       fires:'NOT WIRED. Needs a per-mob idle timer and a notion of “unaware”, neither of which exists: MobSystem::DecideIntent has no persistent state at all beyond a blocked-ahead counter.',
       fallback:'silent.'},

      {k:'alert', n:'notices you', prefix:'mobs',
       d:'The moment this mob acquires the player. Doubles as the player’s only warning, so it should be legible over distance.',
       fires:'NOT WIRED, and not wireable today. Mobs have no awareness of the player whatsoever — DecideIntent’s only sensor is a terrain probe (GroundSense) and there is no target, no state enum and no previous-state field to difference. This needs the AI seam DESIGN.md §“Mob steering” describes, not an audio change.',
       fallback:'silent.'},

      {k:'attack', n:'attacks', prefix:'mobs',
       d:'The swing, lunge or shot itself.',
       fires:'NOT WIRED. Mobs never attack: the one PlayClip(“attack”) in the engine is a FLINCH on being hit, not a swing. Needs a mob attack action to exist first.',
       fallback:'silent.'},

      {k:'step', n:'footstep', prefix:'footsteps',
       d:'This mob’s own step, overriding the surface it walks on. For creatures whose feet are the story — something hooved or metal reads by its gait, not by the ground.',
       fires:'audio::Cues::Footstep, when the mob overrides the surface',
       fallback:'the SURFACE material’s footstep set (the usual case).'},
    ],
  },

  // ---- the world itself -------------------------------------------------
  // Beds that belong to no material and no creature. There is exactly one
  // owner, so these are NOT authored per entity: the set name is fixed in
  // audio/cues.cpp and what the tuner exposes is when and how loudly it plays
  // (the audio.night* group). Listed here anyway so the Audio tab and the wiki
  // can show the slot and say honestly what triggers it.
  world: {
    store: 'none',
    title: 'World ambience',
    icon: '\u{1F319}',
    blurb: 'Non-diegetic beds tied to world state rather than to an object. ' +
           'One owner, so the binding lives in code and the TUNING is what ' +
           'you author — see the Audio group for the night bed’s volume, ' +
           'its chance of starting, and how long it waits between plays.',
    slots: [
      {k:'night', n:'night bed', prefix:'ambience',
       d:'A rare, quiet bed for deep night. Not a permanent loop: once past dusk the engine rolls against nightChance every nightRetrySeconds, plays one pass, then waits again — so the night is mostly silent and the bed is an event when it does arrive. Fades out at dawn. The asset is pre-folded to loop seamlessly (scripts/import_sounds.py), because the engine’s loop path wraps the playhead with no crossfade of its own.',
       fires:'audio::Cues::SetNightAmbience — driven from main.cpp off the day phase',
       fallback:'silent.',
       gain:'audio.nightVolume, eased in and out across dusk/dawn'},
    ],
  },

  // ---- melee combat -----------------------------------------------------
  // The three sounds a fight makes that belong to no material and no creature:
  // the AIR a blade moves, the BLOW landing on a body, and STEEL stopping
  // steel. Owned like the night bed (store 'none'): there is exactly one set
  // per slot, fixed in audio/cues.cpp, and what you author is the TUNING —
  // the combatfx group carries each one's volume, the whoosh's speed window
  // and its pitch ramp.
  //
  // WHY NOT PER-ITEM. A sword and a cleaver want different whooshes, and this
  // is deliberately not that: the binding would belong on ItemDef, which means
  // a fourth authoring surface and a fallback chain, for a difference nobody
  // can currently hear. When items DO get voices, these three stay as the
  // fallback and the item's own key overrides them — the same shape `step`
  // has against `footstep` above.
  combat: {
    store: 'none',
    title: 'Melee combat',
    icon: '⚔',
    blurb: 'Swinging, hitting and being stopped. One owner, so the set names ' +
           'are fixed in code; the combatfx tuning group is where the volume, ' +
           'the pitch ramp and the speed threshold live.',
    slots: [
      {k:'whoosh', n:'swing (air)', prefix:'melee',
       d:'The blade moving. Fires ONCE on the tick a guard commits to a cut, not while it is being aimed — a stroke you are steering has not happened yet. Both the volume and the pitch come off the stroke speed the game actually read from the mouse, so a lazy wave is quiet and low and a real flick is loud and tight; a stroke under combatfx.whooshMinSpeed makes no sound at all, which is the audio half of melee’s “speed is the damage” law.',
       fires:'audio::Cues::Combat(Whoosh) — main.cpp, off a MeleeState phase edge into Slash',
       fallback:'silent.',
       gain:'combatfx.whooshVolume, scaled by stroke speed',
       pitch:'combatfx.whooshRateSlow..whooshRateFast across the speed window'},

      {k:'flesh', n:'blow lands (body)', prefix:'melee',
       d:'The edge going into a living body — the THUD of the blow, as distinct from `dismember`, which is the wet parting of a limb that comes off, and from the creature’s own cry, which is `hurt`. All three can fire for one sword blow and they are not the same sound: this one fires for EVERY landed cut, including the dozen that never sever anything.',
       fires:'audio::Cues::Combat(Flesh) — main.cpp, when the melee sweep hurts a live mob',
       fallback:'silent.',
       gain:'combatfx.fleshVolume, scaled by the blow’s power (speed x edge alignment)',
       pitch:'lower for a heavier blow, the same shape Impact uses'},

      {k:'clang', n:'blocked (steel)', prefix:'melee',
       d:'A cut stopped by something that is not flesh: a parry, a shield, a blade caught on a held weapon. Deliberately a separate slot from `flesh` rather than a variant of it, because the two carry opposite information to the player — one says the blow landed, the other says it did not, and a fight is unreadable if they sound alike.',
       fires:'audio::Cues::Combat(Clang) — main.cpp, from CombatBlockCue(). Wired for the melee sweep hitting a non-flesh body; the BLOCK proper (an NPC raising a guard) lands with phase C’s BlockEvent, which calls the same hook.',
       fallback:'silent.',
       gain:'combatfx.clangVolume, scaled by the blow’s power',
       pitch:'higher for a faster blow'},
    ],
  },
};

// Every namespace a slot can bind into, for the set browser's grouping and for
// "which sets exist that nothing uses" reports.
const SOUND_PREFIXES = (() => {
  const s = new Set();
  for (const owner of Object.values(SOUND_SCHEMA))
    for (const slot of owner.slots) s.add(slot.prefix);
  return [...s].sort();
})();

// A slot's authored value is a set name RELATIVE to its prefix; this is the one
// place that concatenation happens on the tuner side. It must stay identical to
// the one in src/audio/cues.cpp.
function soundSetName(slot, value) {
  if (!value) return '';
  // An authored value containing '/' is already a full set name — an escape
  // hatch for pointing a slot at a set outside its namespace without inventing
  // a second syntax for it.
  return value.includes('/') ? value : slot.prefix + '/' + value;
}
