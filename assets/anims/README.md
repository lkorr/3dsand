# assets/anims — the shared clip library

One keyframed clip per `<name>.json`, in the same schema as a mob sidecar's
`clips` entries (`durationMs`, `loop`, `mode`, `blendInMs`, `blendOutMs`,
`mask`, `tracks`) plus:

- `name` — the clip's name as `Mob::PlayClip` and `attack_styles.json`'s
  `clip` field use it (default: the file stem — keep them equal).
- `sidecarVoxelsPerMetre` — the world-length stamp a sidecar carries, so a
  clip's position keys (world voxels) scale with the voxel size.

Written by the tuner's clip lane ("→ library"), read back by "← library"
(which copies a file into the open sidecar for editing). `LoadMobDefs`
compiles every file here onto every rig whose part names it fits; a sidecar
clip of the same name wins on that rig, and a file with no usable track on a
rig is skipped silently. Hot-reloads with R. The `mob` selftest gate asserts
every file here compiled onto the human under its stem.
