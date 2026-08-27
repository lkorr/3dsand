# Authored world-edit layers

`.svedit` files here are hand-built voxel patches over worldgen, saved from the
tuner's Worldgen tab (the **Voxels** view). Name one in
`assets/materials/tuning.json` as `worldgen.editLayer` and the engine applies it
to every chunk it generates — startup worldgen and every streaming refill —
through the MutationQueue.

They are *layers*, not saves: seed-independent, diffable, and composable with a
worldgen change. See `DESIGN.md` §9c and `src/sim/worldedit.h`.

**A layer moves the world hash**, because it puts voxels in the world. Leave
`worldgen.editLayer` empty for a pristine world, and never set it in a test.
