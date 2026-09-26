# Authored world-edit layers

`.svedit` files here are hand-built voxel patches over worldgen, saved from the
tuner's voxel view (Environment > World map > Preview > Voxels). Name one in
`assets/materials/tuning.json` as `world.editLayer` (the World map page's
**edits** selector writes it) and the engine applies it to every chunk it
generates -- startup worldgen and every streaming refill -- through the
MutationQueue. F5 / F7 / Apply to game re-read it in a running game.

They are *layers*, not saves: seed-independent, diffable, and composable with a
worldgen change. See `DESIGN.md` §9c and `src/sim/worldedit.h`.

**A layer moves the world hash**, because it puts voxels in the world. Leave
`world.editLayer` empty for a pristine world, and never set it in a test.
