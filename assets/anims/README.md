# assets/anims — the shared clip library

One keyframed clip per `<name>.json`, in the same schema as a mob sidecar's
`clips` entries (`durationMs`, `loop`, `mode`, `blendInMs`, `blendOutMs`,
`mask`, `tracks`) plus:

- `name` — the clip's name as `Mob::PlayClip` and `attack_styles.json`'s
  `clip` field use it (default: the file stem — keep them equal).
- `sidecarVoxelsPerMetre` — the world-length stamp a sidecar carries, so a
  clip's position keys (world voxels) scale with the voxel size.
- `quietAdditive` — override clips only, `{ "<part>": 0..1 }`: how much of
  every additive layer (walk/run/idle swing) on that part this clip silences
  at full weight. Additives land after the override blend, so without it a
  held pose takes the walk's arm swing on top of itself.
- `pitchFollow` — override clips only, `{ "<part>": -1..1 }`: the share of the
  eased look pitch the part turns through about its own X (pose.cpp).

Both mirror with the clip (`<name>.mirror` swaps the part names). The tuner's
clip lane does not edit them; hand-edit the file.

Posing: the clip lane's FRAMES list and ✋ pose handles (rig.js section 6c)
edit these keys as whole poses frame by frame — drag hands, feet, elbows,
knees, hips, torso, head; mirror, flip, copy/paste. Import a library clip
with "← library", pose it, write it back with "→ library".

Written by the tuner's clip lane ("→ library"), read back by "← library"
(which copies a file into the open sidecar for editing). `LoadMobDefs`
compiles every file here onto every rig whose part names it fits; a sidecar
clip of the same name wins on that rig, and a file with no usable track on a
rig is skipped silently. Hot-reloads with R. The `mob` selftest gate asserts
every file here compiled onto the human under its stem.
