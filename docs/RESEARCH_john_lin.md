# John Lin (`lin5159`) — mined wisdom from 15,194 Discord messages

**Who.** `lin5159` is **John Lin** (@ProgrammerLin, "Lin X", voxely.net) — the author of
[*The Perfect Voxel Engine*](refs/perfect_voxel_engine.md), which CLAUDE.md's design
guidelines are already distilled from. He is the single most cited independent voxel
engine developer of the last decade: the falling-leaves/god-rays path-traced voxel forest
that went viral in 2020 is his.

**Corpus.** VoxelGameDev Discord, **2019-12-31 → 2021-05-31**, 15,194 messages by him
across 12 channels. Extracted to `docs/refs/lin/` (see the README there). This is a
17-month *continuous dev log* of one person building an infinite, path-traced, physically
simulated voxel world alone — including every dead end, every rewrite, and every number he
measured. It is far more useful than the blog post, because the blog post is the
conclusion and this is the derivation.

**Why he matters to us specifically.** He solved, or failed at and documented the failure
of, almost exactly the set of problems this engine has: an infinite voxel world with
sub-decimetre voxels, GPU-resident storage that must be *modified* every frame, fluids
that must not evaporate, rigid bodies made of voxels, and lighting that has to survive all
of it. Where we differ (he is a path tracer, we are a cellular automaton; he has no
determinism requirement, we treat it as inviolable) the difference is usually *informative*
rather than disqualifying.

**The single most important thing in the whole corpus** is not a technique, it's a
trajectory. Read §0 first.

**Review pass (2026-09-01).** The first draft was written from the corpus alone. This
revision checked every "Compared to us" paragraph against the engine as it stands on
`main` — `raymarch.wgsl`, `shadow_resolve.wgsl`, `pass_table.def`, `worldgen.wgsl`,
`rhi_vulkan.cpp`, `pagetable.h`, `worldio.cpp`, and the measured numbers in
`docs/PLAN_surface_flight_perf.md` and `--render-budget` — and rewrote the ones that
were wrong. They are marked **[corrected]**; ones that checked out are marked
**[verified]**. The biggest reversals: the CA *is* a colouring scheme (§6.5, the draft
said the opposite); MPM liquids *already* spawn speed-keyed spray (§6.4); the sub-chunk
occupancy mask exists but is compiled out because a *skip* at 4³ was measured and
refuted (§3.3); the pool-fragmentation warning has no mechanism at our page granularity
(§4.2); op-based saves don't work for a world that evolves without input (§4.3); and
integer ops are not half-rate on Turing (§1.6). §11 was re-ranked accordingly and §13
is the resulting to-do list.

---

## 0. The trajectory — what he tried, in order, and what he concluded

This is the spine of everything below. He did not arrive at his design; he was driven to it
by five successive failures, each of which he documented.

| Era | Approach | Why he left it |
|---|---|---|
| pre-2019 | Isosurface extraction: dual contouring, manifold DC, dual marching cubes, adaptive edge spinning, chunked LOD | *"then i ditched lod and meshing, started researching ray tracing, and never looked back"* (2020-11-28) |
| 2019-12 → 2020-04 | **Vulkan RTX** (BVH, TLAS/BLAS, AABB primitives) | **BVH update cost.** *"BVH rebuilds in optix/vulkan rt eat like several ms of time, imagine doing it for large parts of the world every frame. can't, just not an option."* A spinning windmill forced a choice between "no ray tracing at all" and "heavy penalty just because it's rotating." |
| 2020-04 → 2020-07 | **Custom compute DDA** over an n-ary tree (8³ children, 4 levels = 4096³) | Kept, and it beat ESVO after register optimisation — but memory scaled badly and empty-space skipping was limited |
| 2020-07 → 2021-04 | **SVO / ESVO / SVDAG** with contours, mipmaps, BC-compressed attributes | *"the gains came from dropping dags/svos. octrees are extremely terrible for ray tracing, and dags actually do a very poor job of representing sparse voxel data despite their gains over esvo, which is also a terrible structure for representing sparse voxels"* (2021-04-25) |
| 2021-04 → | **Instance-based**: real scanned rock/plant models placed and rotated over the terrain, two-level structure, memory traded directly for speed | Where the corpus ends. 500 fps @1080p with per-pixel shadows. |

**Three conclusions he states outright and never walks back:**

1. **Two-level acceleration is non-negotiable for anything dynamic.**
   > *"you need to abandon the idea of single grid for everything because considering 64³ is
   > over 200k voxels worth, it immediately becomes pointless... this problem is basically
   > the 3d equivalent of 2d rasterization. look at how graphics dev is typically done. wipe
   > every frame clean, build your 3d array by combining sub 3d arrays, do all the heavy
   > transformations per frame on the gpu. this is also why acceleration structures for ray
   > tracing have 2 levels — top level and bottom level. sure, with one level you might have
   > better ray tracing performance but good luck updating it when objects move."* (2020-07-26)

2. **Update cost dominates trace cost.** Every structure decision he ever reversed was
   reversed for update cost, never for trace throughput.

3. **Rendering is the small part.** Restated a dozen times. *"Rendering is the ez part. Good
   luck generating enough interesting world to fill up 32km and being able to simulate it all."*

**Compared to us [corrected]:** the first draft called the page pool "his two-level
structure". It is not. `pageTable[slot]` → page is a two-level *address* map (it decides
where a chunk's memory lives), not a two-level *acceleration* structure (which decides what
a ray may skip). The trace-side analogue in our engine is chunk occupancy counts → voxel
DDA, with a third level built and refuted (§3.3). The real TLAS/BLAS analogue is the
**microbody path**: each rotated body is a box rasterised and DDA'd against its own brick,
over a static-per-frame world — his row-5 design ("instances with transforms"), reached
for the same reason (a rotating body must not re-bake the world).

What does transfer is the update-cost argument, and we already satisfy it the right way:
the occupancy summary is rebuilt each tick over the compacted dirty list only
(`sim_occupancy`, `DispatchWorkgroupsIndirect`), so the "acceleration structure" costs
what the activity costs. The rule to keep from him: never add a **build-once** structure
over the sim buffers — a BVH, an SVO, a mip pyramid rebuilt wholesale — because the CA
invalidates it every tick. The JITTER sentinel is the right kind of optimisation for the
same reason: it makes a chunk cheap to *not* materialise rather than cheap to rebuild.

---

## 1. Performance

### 1.1 The GPU lever he cared about most: registers and dynamic indexing

This is the highest-value technical content in the corpus and it is almost entirely absent
from public voxel writing.

> *"i just read that an array declared inside a gpu kernel is pushed into local memory —
> `int foo[4];` for instance — because registers aren't indexable, and there's no stack."*
> (2020-04-18)

> *"psa: doing `int foo[4]; int a = foo[0];` is just as slow as doing `ivec4 foo; int a =
> foo[0];`. a workaround is the vector approach:*
> ```c
> ivec4 foo;
> int index = 0;
> int a = dot(foo, bvec4(index == 0, index == 1, index == 2, index == 3));
> ```
> *the idea is the compiler is able to operate on the components of the vector ahead of
> time. just eliminated another one of those cases — 60 fps to 90 fps."*

And the stack version, applied to his 4-level hierarchy traversal:

> *"rather than store the active layer 0-3 and read from/write to `variable[layer]`, i
> switched to using a temporary variable and then only writing to/reading from when it
> dives deeper into the hierarchy or comes up. first time i did that it went from 30-60,
> then 60-90... and with those two changes, this now wipes the floor of the efficient
> sparse voxel octree tracer."* (2020-04-18)

Related, from #programming (2020-07-24):
> *"they have no performance penalty. however, any indexing that is done dynamically that
> the compiler cannot determine at compile time gets pushed to global memory and will hurt
> performance. `vec4 myarray;` or `float myarray[4];` are the same. `myarray[0]` or
> `myarray.x` is the same. `myarray[some dynamic variable]` is **not** the same."*

**Compared to us [corrected]:** `gotcha-dynamic-index-on-byvalue-uniform` is one instance
of the general rule, and it cost 10.8x. But the first draft over-applied it: "every small
fixed-size array in raymarch/sim_fluid/the 27-tap loop" is wrong. The in-function arrays
in the MPM kernels (`dxs[3]`, `wxy[9]` at `sim_fluid.wgsl:584`) and the `xs/ys/zs[2]` in
`markDirty` are indexed by the counters of constant-trip loops; after unrolling every
index is a literal and there is nothing to spill. Lin's rule bites only when the index is
a **runtime value**. In our renderer those are the axis-indexed component accesses:
`rd[axis]`, `n[h.axis] = -h.sgn`, `hp[a1]`, `d1[a1] = s1` in `voxelAO`
(`raymarch.wgsl:2437`), `nn[a1] +=` in the ice grain, `nLocal[axis]` in
`microbody.wgsl` — each a dynamic component read/write into a `vec3`, his
`ivec4 foo; foo[index]` case verbatim. Whether the NVIDIA back end turns those into a
local-memory round trip or a three-way select is not knowable from the WGSL, and not
from `--render-budget` either: footprint measured by const-folding a feature away
conflates registers with spills.

**Action item (revised):** the tool that answers this is not a grep, it is a
register/spill readout per pipeline. Vulkan exposes exactly that:
`VK_KHR_pipeline_executable_properties` returns per-stage statistics on NVIDIA
(register count, local-memory bytes, spill stores/loads). We query nothing of the sort —
`src/gpu/` has no `PipelineExecutable*` at all. A `--shader-stats` flag printing one line
per pipeline would turn "is `trace()` spilling?" from a 30-minute A/B into a lookup, and
would have named the by-value-uniform bug in one run. Then, and only then, rewrite the
`[axis]` sites as `select` chains, and only where the readout shows local memory. The
same readout checks the unrolling assumption above: if `sim_fluid` reports local memory,
that assumption is what failed.

### 1.2 Write to an image, not a buffer

> *"for some reason, writing to the output buffer in a checkerboard-like pattern is slower
> than just tracing the whole screen and writing the full buffer. i could launch 1/4 the
> rays and it's somehow slower... i'm going to switch my output from a buffer to an image
> and see if it makes a difference."*
> *"ohhhhh yeah, that did it. switched it to an image. 260fps @ 1080p."*
>
> *"without the checkerboard optimization, it was ~70-80 fps. with the checkerboard
> optimization and a buffer and literally 1/4 the computation work, it was at 60-70 fps.
> with the checkerboard optimization and switch to image, it's ~240 fps."* (2020-02-01)

Cause: `VK_IMAGE_TILING_OPTIMAL` gives the driver a tiled memory layout matched to 2D
access patterns; a linear buffer does not. He later suspected memory *property flags* were
also part of it — see 1.3.

**Compared to us [verified — does not apply as stated]:** `raymarch.wgsl` is a
**fragment shader** on a fullscreen triangle (`fn vs` / `fn fs`), so its output is a
colour attachment in optimal tiling — the fast path he switched *to*. The concern would
return only if the primary ray moved to compute. The shadow resolve pass *is* compute and
writes a buffer, but a hash-keyed one, random-access by design, which gains nothing from
tiling. The narrower lesson that does transfer: **2D-coherent reads and writes belong in
images.** Nothing we keep in a storage buffer is 2D-coherent today. If a half-resolution
primary pass or a reprojection history is ever added (§2.6), both are 2D-coherent and
must be images, not buffers.

### 1.3 Host-visible buffers are a trap

> *"i've been creating all my buffers with `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
> VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT` flags and TIL that's potentially very slow. i guess
> i have to switch to staging buffers. i am very sad."*
> *"...10 fps to 60"* (2020-02-10)
>
> *"this could explain why i saw like a 500% performance gain when switching from a buffer
> to a texture. it may not have been entirely the memory layout at hand here."*

And the upload fix that mattered most:

> *"it was related to uploading voxel data to the gpu, which was a huge bottleneck. turns
> out that vulkan buffer copying supports **region batching**, where you just give it a
> list of what to copy where, then submit it. so that fixed it, haven't needed to do
> anything fancy since."* (2020-06-09)

**Compared to us [verified]:** clean on the property flags. `Backend::CreateBuffer`
(`rhi_vulkan.cpp:631-699`) has one policy: anything without `MapRead|MapWrite` is
`DEVICE_LOCAL`; the host-visible branch is entered only by buffers whose usage is
`CopySrc`/`CopyDst` alone, never `Storage`/`Uniform`/`Indirect`. No kernel reads
host-coherent memory. Uploads go through one mapped staging ring.

The batching claim was half right. `FlushUploads` (`:775-848`) emits **one
`vkCmdCopyBuffer` with one region per pending write** — the multi-region form he found is
not used. It does not matter yet: the size classifier routes anything ≤64 KiB through
`vkCmdUpdateBuffer` inline, which covers every mutation buffer and every streamed page
(16 KiB), so a typical tick issues 0–2 real copies (`cellOps` up to 512 KiB, `spawnOps`
128 KiB, when they exceed the class-A cap). Revisit only if `kFetchPerTick` or the page
size grows past the inline threshold; then 64 single-region copies per tick should become
one 64-region copy. Noted, not done.

### 1.4 Never reset — timestamp instead

> *"don't ever reset data if you can avoid it. I managed to make my photon mapping nearly
> free by avoiding resets and just storing a timestamp. Otherwise resetting is slower than
> the mapping itself."* (2020-01-11)

> *"timestamps are useful in widescale temporal stuff, especially on the gpu. eg. in a
> particle system, where you don't want to update the lifetime every frame every particle."*

**Compared to us [verified — audit done, nothing found]:** our **tick stamp** (word
bits 16–18, cycling 1..7, `STAMP_NEVER`=0) is the same invention, and the shadow cache
applies it a second time: a bucket carries the frame index it was resolved in
(`shadowStateResolved(state) == curFrame`) and the 8 MiB table is never cleared — only
the four-word request header is reset by `shadow_prepare`. The per-tick `Fill` rows
(`pass_table.def:107-118`) total ~1.3 MiB with explosions idle (`dirtyOut` 128 KiB,
`claim` 1 MiB, three tiny counters) — single-digit microseconds of fill bandwidth on a
3060 Ti. `expMask` (2.1 MiB) fills only while explosions exist; `fluidClear` and
`seam_cell_clear` are indirect over active blocks. **There is no reset-dominated
subsystem here.** The one stamp that would still be worth having is diagnostic, not
speed: storing the tick in `dirtyOut` instead of `1u` would let a stale buffer be
re-compacted for inspection. Low value.

### 1.5 Morton codes are a trap (his opinion, strongly held)

> *"i don't even know why morton codes are so expensive to calculate. it's literally just a
> handful of bit ops (not using the LUT approach because i'm 99% sure it's slower)"*
> *"so in reality, morton codes are good for nothing, not even improving memory access
> patterns because they destroy those gains. (/rant)"*
> *"like breaking up the chunk into smaller chunks and laying it out along the zyx — then
> it's just a few divs/mods to convert back and forth. that's what i use everywhere else in
> my engine."* (2020-08-30)

`pext`/`pdep` are not an option if you want AMD performance, which forces AVX2 or bit-twiddling.

**Compared to us:** we don't use Morton. Good. This is a "don't be tempted" note for
whenever someone proposes Z-ordering the page pool for locality.

### 1.6 Integer ops are half-rate on Ampere (not Turing, and not the way it sounds)

> *"from the ampere whitepaper. int ops are half as fast as flops. i actually thought that
> was more a thing of the past but apparently not."* (2021-05-06)

**Compared to us [corrected]:** the first draft said "half-rate on Turing and Ampere"
and called it "the price of determinism". Both halves need fixing.

- *Architecture.* Turing gave each SM partition 16 FP32 + 16 INT32 lanes: integer and
  float at the **same** rate, concurrently. Ampere (our RTX 3060 Ti) added a second FP32
  datapath — 32 FP32 lanes, 16 of which can alternatively run INT32. Ampere doubled float
  throughput and left integer where Turing had it; "half rate" is relative to Ampere's own
  floats, not a regression. And the split is by instruction class, not by type: integer
  multiply-add (`IMAD`) issues on the FMA pipe, so what sits on the narrower pipe is the
  logic/shift/add class (`LOP3`, `SHF`, `IADD3`) — the bit-twiddling of
  `voxMat`/`voxState`, `hash3`, and the fixed-point rounding helpers.
- *Does it matter?* Only if a kernel is ALU-bound. The CA reads a 27-cell neighbourhood
  through a page-table indirection per cell; the MPM P2G/G2P kernels are 27-tap atomics.
  Those are latency- and bandwidth-bound, and §1.7 is Lin agreeing. The one place hashing
  was the cost was the CPU `Classify` (`project-cheap-math-perf-pass`: 3.4x from an 8-lane
  PCG) — a fewer-hashes fix, not a pipe fix.
- *The const-eval note was wrong.* The `sim.fluid*` / `sim.wind*` float→fixed const-eval
  removes work at **compile** time; the runtime kernel is still integer arithmetic on the
  same pipes. Its value is determinism plus human units, not ALU rate.

The residue worth keeping: **when an integer kernel is ALU-bound, throughput comes from
fewer bit ops per cell** — decode the neighbourhood once, hoist `% 3` and `windPhaseQ`
(done in the cheap-math pass), pack once and unpack once — never from a float rewrite,
which rule 1 forbids anyway.

### 1.7 Everything is memory latency

> *"my tracing along with all other tracing is bottlenecked by memory latency."* (2020-11-03)

> *"more compact memory = more coverage in data per memory fetch = potentially higher cache
> hits = blows any cycles spent doing some bitwise ops out of the water."* (2020-08-24)

> *"realworld case: i had esvo implemented with my own structure, where i treated each node
> of the octree (including leaves) as their own nodes with their own data... by switching to
> a representation where the leaf nodes weren't actually nodes, just products of the parents,
> and storing the leaf masks in the parents, the performance went up noticeably. and before
> that, i had a raw node representation that wasn't compacted and that was even slower. so
> it's much faster to compact the nodes and save on fetches as much as possible. the only
> thing you're doing extra is a couple bitwise operators anyway."* (2020-08-01)

**Compared to us [verified]:** this is the justification for the 32-bit voxel word being
*full* and for refusing to widen it. It's also the argument against a sparse auxiliary
layer unless it's genuinely sparse — an extra indirection per voxel is worse than several
bit ops. And it names the cost of the page table honestly: `pageTable[slot]` is one extra
dependent fetch per voxel read, which is why both DDAs cache the chunk's page entry
(`cchPt`) and re-read it only on a chunk crossing — the indirection is paid per chunk,
not per cell. Any new per-voxel side table must be able to say the same.

### 1.8 Ray coherence, divergence, and pass splitting

> *"launching the rays in blocks is basically like sorting them, which means they're more
> likely to take the same paths — especially in a voxel scene."*

> *"when casting my shadow rays, if i do soft shadows as opposed to hard shadows, they're
> like 50% slower. just that slight variation in ray direction causes it to be that much
> slower."* (2020-02-02)

> *"if the sun is near the horizon so the shadow rays are horizontal, then they're waaay
> slower."*

> *"do traces like direct light, primary, and indirect light in separate passes because this
> will net a slight performance edge due to the gpu thread scheduling"* and *"the smaller
> the shader, the fewer the registers that are needed per thread invocation... the more
> registers the shader requires, the fewer threads can be launched OR the more registers
> are spilled into global memory."* (2020-05-01)

> *"people like to think branching on gpus has gotten faster, but it hasn't. there are just
> more cores and more powerful ones so some dead threads dropping perf to 2 gens ago
> doesn't feel as bad."* (2021-02-11)

**Compared to us [verified, sharpened]:** `--render-budget` (RTX 3060 Ti, 1080p,
overlook, 14.66 ms, 2026-08-30/09-01) put a number on his pass-splitting argument: of
the ~5.9 ms a shadow costs, **3.59 ms is the register footprint of the `trace()` call
site existing in the fragment shader at all**, and the reflection call sites carry
another ~3.5 ms of the same. The shadow half is being fixed the way he prescribes — the
ray moved to its own compute pass (`shadow_resolve.wgsl`) with a purpose-built
media-blind DDA, and the call site deleted from `fs` behind a `const`. Two inlined
`trace()` copies remain in the fragment shader: `traceReflection`
(`raymarch.wgsl:3127`) and `traceRefraction` (`:5930`), both media-blind. They are the
next ~3.5 ms, and the fix is the same shape: a slim `traceOpaque()` whose result has five
fields instead of `Hit`'s 23, or — harder, because reflections are view-dependent — the
cache/resolve architecture keyed on (patch, view octant).

The sun-angle point is real and unmeasured. `--render-budget` runs one overlook camera at
`FindNoonTick()`; there is no dusk arm and no submerged arm. A horizontal shadow ray
crosses roughly twice the chunks a near-vertical one does before its first blocker,
`godRays` early-outs below 0.08 elevation, and the cascade shadow reach is a fixed 60 m in
any direction. **Add a dusk arm before anyone tunes a shadow constant against noon.**

### 1.9 The optimisation philosophy (the story he tells twice)

> *"a company's local server... slowly as the morning picked up it'd get bogged down with
> queries... so they hired some guy who took weeks to optimize the query algorithm and he
> came up with a real genius solution — a brilliant algorithm that was 10 times faster... so
> he presented it with a big fat smile, ready to be met with cheers. but instead, they
> responded with 'oh, we actually found that X queries were taking the longest but were only
> 1% of the queries executed, and so we just split them up and gave YZ queries their own
> dedicated server, so now the whole system is 100x faster and everybody is happy'."*
>
> ***"optimize your overall algorithm for the most common case, not the structures that you
> use in your algorithm."*** (2020-07-10)

His own worked example, the next day:

> *"my collision detection for very high quality was suuuuuper slow... mainly because of an
> expensive inner loop that would execute like >1000 times per object. i could've spent
> weeks optimizing that inner loop, adding SSE intrinsics, optimizing the GJK algorithm...
> but instead i switched to a medium-high collision detection that has no visual difference
> in 99% of objects, and changed the paradigm to break up parts of the object to get a free
> broad phase and reduce that >1000 iteration inner loop to < 10 iterations and now it's
> like 100x faster and it only took a day."*

And the corollary he repeats:

> *"you have to dev feature first and optimize later. otherwise you end up optimizing designs
> you'll scrap and have to change out later, and now you've just wasted a whole bunch of
> time."* (2020-01-04)
>
> *"i'll never forget how much time i spent optimizing certain algorithms just to throw the
> algorithm out the window. or had to add a feature that made the optimization pointless."*
>
> *"i spent a long, long time optimizing a mesher a couple years ago, and looking back it was
> so much time wasted. now in my game i don't even measure that. no z-ordering even, just a
> flat array of voxels and i go over it naively."*

**Compared to us:** CLAUDE.md's "verification is a BUDGET, not a reflex" section and rule 6
("a bare count is not a measurement — add attribution to the reporter, don't A/B eliminate")
are the same instinct applied to *diagnosis* rather than *optimisation*. Lin's version
applied to *optimisation* is the half we haven't written down: **before optimising a hot
loop, ask whether the paradigm can eliminate the loop.** The 58-page-fault story in
CLAUDE.md is literally this lesson learned the expensive way.

---

## 2. Visuals and lighting

### 2.1 "Lighting is everything"

> *"people seem to think graphics are everything. some people argue and say gameplay is
> everything. both people are wrong. **lighting is everything.**"* (2020-12-16)

> *"i finally tackled and solved the biggest challenge of this project — not physics, not
> water, not performance, not memory usage, but **scene composition**. finally all lighting
> scenarios look and feel normal. direct sunlight + god rays + color bleeding + lighting
> curves + color saturation = all balanced nicely."* (2020-08-23)

### 2.2 Tonemapping is the hard part, and it is a 10-dimensional balance

> *"balancing indoor light and outdoor light without doing hdr exposure trickery is very
> hard. you want indoor areas to appear lit up from a window, and you want outdoor areas to
> appear bright in sunny areas, while also appearing dark but still bright in shaded areas.
> and then you also want the colors to appear nice."*
>
> *"aces and uncharted 2 are the most common used aside from `pow(color, 1/gamma)`, but they
> produce very poor results for my environment. and then on top of that, you can't
> overcompensate dark areas too much otherwise it'll introduce noise from lack of samples and
> high variance. so you're trying to balance like 20 things at once."*
>
> *"the problem is that in color space, the sunlight strength is like 37.0, so curving the
> light from 0.0-1.0 is non-trivial. tonemapping and color space is an entire field."*
> (2020-10-19)

> *"the main challenge is the intensity of the sunlight. too high, and it introduces bright
> spots. too low, and the scene is too dark... irl there are like a bajillion photons so
> it's not a problem, but sadly i only have the budget for like 10k. so more photons helps,
> but then hurts performance. and increasing the denoiser tolerance also improves it at the
> expense of other artifacts. and then there's ao to balance. and temporal stuff. it's like
> a venn diagram with 10 circles, and trying to find the center."* (2020-12-18)

**Compared to us:** `gotcha-per-channel-reinhard-desaturates` (tonemap luminance, not
channels) is one circle of that venn diagram. `project-daynight-sky` records two more
(the khaki-sky coefficient traps). Lin's contribution is the framing: **there is no
"correct" tonemapper; there is a set of mutually hostile constraints and a hand-tuned
compromise**, and it is worth a named tuning group with a visual regression, not a constant.

### 2.3 Colour bleed is a *deliberate* exaggeration

> *"i dialed the indirect colored strength way up when correcting colors the other day
> because you really feel it. when there's lots of green grass in the shadowed areas, it
> feels like a jungle because the rock has this green tint. when it's dialed down, you don't
> get that feeling."* (2020-02-11)

> *"atm there's no variance in voxel color. the tree trunks are a uniform color, the leaves
> in a tree are a uniform color, the grass is (almost) a uniform color, and the terrain is a
> uniform color too. **the variation is just from the normals and lighting.**"* (2020-02-11)

**Compared to us [corrected]:** the first draft said we "went the other way" and that
tints "eat word bits". Neither is right. The `state % 3` palette jitter that breaks up
flat colour rides the state nibble liquids already use for fullness — it costs no bits
the word wasn't spending — and the `MATF_TINTED` work (`project-voxel-colour-tints`)
exists so an entity's colour *survives into its rubble*, not to add variety. Lin's real
claim — that perceived richness comes from lighting on flat colour — still deserves a
test, and the test is free: zero the jitter amplitude and look. If the world reads flat,
the missing ingredient is the indirect term (§2.4), not more palette entries. (He *did*
later add weighted palettes — §4.4 — for *material* variety, not to break up flat colour.)

### 2.4 GI: photon map + final gather (his settled answer)

The technique he returns to over and over, and the one he ends on:

> *"the most important thing to get a low noise path traced scene is to reduce variance, and
> you can do that by **storing a low quality, low frequency lighting in some world structure
> and then when you trace rays per pixel, you look up the light where it hit.**"* (2020-10-16)

> *"basically you cast a bunch of bounced rays *from* the light sources, then do what's
> called a final gather, which is where you do your per-pixel ray tracing, and where it hits,
> you sample the photon map. **you can get like 5 bounces and little variance at minimal
> cost.**"* (2021-03-20)

> *"my solution has been photon mapping + final gather. path tracing 5 bounces is completely
> unreasonable, but if you photon map with 5 bounces from light sources, then it creates a
> pretty decent map to then path trace against with low variance."* (2021-04-25)

> *"then as an added bonus, you store that final gather light back into the place where it
> hit — and then you get unlimited bounces."* (2020-08-16)

The world structure to store it in went through three iterations: light probes (leaked) →
simplified planes (too coarse) → **anisotropic downsampled voxel faces at ~1/4 res**:

> *"the idea is to photon map, store the light in the downsampled voxel faces (probably 1/4
> res), then cast a final gather against this and it'll be leak-free. the only cases it'd
> leak are for ultra thin like 2 voxels wide geometry, which isn't very likely."* (2020-10-17)

And the cost measurement that makes it viable:

> *"my ray tracer is okay, the difference between tracing at full voxel res and 1/8 voxel res
> is about 2ms."*

**Compared to us [corrected and made concrete] — still the biggest idea we are not
using.** We have no indirect term. Shading is `albedo * (ambientAt(n) * ao + sun)`
(`raymarch.wgsl:6548`): `ambientAt` is a sky/ground hemisphere lerp on `n.y`, `ao` is a
three-tap voxel AO, `sun` is the shadow ray. Nothing knows a cave is dark, a room is lit by
one window, or that grass under a canopy is in shade. That is the largest visual gap in
the engine and Lin's settled answer is the right shape for it — but the first draft's
sketch ("a `kWorldN/4` grid, one value per 4³ block, written by rays from the sun")
repeats the mistake he documented, and misses what we already have.

1. **A scalar per block leaks.** He tried probes (leaked), then planes (too coarse), and
   landed on **anisotropic downsampled faces**: light stored per *face direction* of the
   coarse cell. He says it leaks only for ≤2-voxel geometry; our world is full of 1-voxel
   geometry (ruin walls, floors, trunks, mobs). So: **six values per 4³ block**, one per
   face, and a receiver reads only faces that face it. Dense that is 2M blocks × 6 ×
   RGB9E5 = 48 MiB; sparse over surface-bearing blocks it is a fraction of that.
2. **We don't need photon rays from the sun — the shadow cache is the first bounce.**
   `shadow_resolve` already decides, per surface patch, "is this patch sunlit", and knows
   the patch's cell and face. Depositing `albedo × sunlight` into that block-face as it
   resolves is direct-light injection for free, activity-scaled (only requested patches,
   i.e. what is on screen or near it). Emissives (lava, fire) inject from the
   `sim_occupancy` dirty-list walk. The limitation is the usual screen-space one — light
   comes from what has been *seen* — which a low-rate world-space walk over surface blocks
   fills in.
3. **The gather is the expensive part; make it a coarse cone, not a ray.** At the primary
   hit, step 3–4 blocks through the block grid along the normal's hemisphere (his "trace
   at 1/8 res costs 2 ms") and accumulate the facing faces. Write the gathered result back
   into the receiver's own block-face at low weight → multi-bounce over frames.
4. **The sun moves.** With a day/night cycle the whole grid ages even when the world is
   idle. A rolling refresh budget (N blocks per frame, oldest first, plus decay toward the
   current sun) is what bounds the cost — "re-light where matter changed" alone is not
   enough here, and the first draft assumed it was.
5. **Hash and determinism:** trivially excluded — a render buffer like the shadow cache,
   written by the resolve pass, never read by the sim.

**Cheapest first step, before any of that: a sky-visibility (openness) value per
block-face**, computed by the same resolve pass with a short upward march, replacing the
`n.y` lerp in `ambientAt`. By itself it makes caves dark, overhangs shaded and interiors
dim; it reuses the pass and buffer the GI needs; and it can be judged by eye before
committing to the bounce. That is the plan doc's phase 0.

### 2.5 DDGI leaks (do not trust the paper)

> *"ddgi, the one morgan mcguire preached was leak-free over and over — which is what led me
> to using it and it most definitely was not."*
> *"the talk & paper are a bit misleading. it does matter, and it also does leak, and it's not
> noise free — the noise just exists elsewhere (in world space). the culling is based on
> variance shadow maps, and variance shadow maps leak by nature. and there's a fall back for
> when there aren't any probes in view = leak."* (2020-08-23, 2021-01-21)

He shipped it in his engine for months and then tore it out. *"ditching ddgi because leaks."*

### 2.6 Denoising, concretely

The adaptive temporal blend (his stripped-down A-SVGF), posted verbatim (2020-01-21):
```c
vec3 diff = v.light - out_color.xyz;
float lum_diff = clamp(dot(diff, diff), 0, 1);
float temporal = lum_diff - lum_diff * lum_diff;
```

The reprojection + gather, in full (2021-02-18):
> *"what you want to do after you have the old coords is sample around the old coords, take
> the world position at each coord, and weigh it based on how far it is from the position you
> reprojected. a simple solution is just `weight = 1.0 / distance(sampled_world_pos,
> expected_world_pos);` — you can replace the `1.0` with a gaussian kernel. then do
> `sum_weight += weight; total_sample += sampled_light_at_coords * weight;` and divide at
> the end.*
>
> *and **then** for the temporal part, you'd do `mix(old_sample, new_sample, alpha)`.
> eventually what you want is to make alpha `1.0 / num_frames` where `num_frames` is how many
> frames that pixel has been in view, so when you have a disocclusion it totally throws away
> the old sample. and if you have little movement, it approaches 0."*

Blue noise, with the caveat that matters:

> *"don't mirror [repeat the noise texture] — because if you plan to do a spatial filter
> you're killing a large region because a good portion of the samples will have the same
> directions. repeat will separate it."* (2020-11-20)

> *"never trust a still image regarding denoisers. they're a scam :p"* (2020-12-11)

Spatial filter of record: **the edge-avoiding À-Trous wavelet** (Dammertz 2010), which he
recommends to everyone and describes as *"you weigh the neighbor samples based on
differences in positions/normals/optionally other properties, and each iteration of the
blur you go further out — so 1 pixel, 2 pixels, 4 pixels."* He also warns it *"overblends
and underblends at the same time"* and *"the wavelet is so noticeable on clean surfaces."*

**Compared to us [verified — not applicable yet, and worth saying why]:** our renderer
is **deterministic per pixel**. No stochastic AO, no jittered shadow rays, no path
samples, so there is no variance to denoise, and no history exists — `raymarch.wgsl` has
no reprojection and no previous-frame texture. Our dithers are deliberately **time-free**
(`farDither`, the god-ray start jitter) so they don't crawl; his are the opposite trade,
resolved by temporal accumulation. The moment anything stochastic lands — soft shadow
penumbrae, a noisy GI gather (§2.4), rough reflections (§2.8) — this section becomes the
checklist, and the first thing needed is a reprojection buffer, which is also what a
half-resolution primary (§1.2) would need. Build the history buffer once, for all three.

### 2.7 God rays and fog — the thing he never solved

> *"god rays & fog is the biggest composition element i struggle with and haven't gotten
> right all year."* (2020-08-18)

The optimisation that made them affordable:

> *"the way i was doing it yesterday was ray marching from the camera to the visible pixel as
> normal, but each step i'd cast a shadow ray to the sun, and it was verrry slow. this time i
> just **sample the shadow map** instead and it's basically free for 100+ steps."*
> *"like 100x faster than casting shadow rays each step."* (2020-05-08) — 16 ms → ~3 ms.

The tradeoff: an undersized shadow map makes god rays alias and twitch as the camera moves.

**Compared to us [corrected]:** the first draft said the fire/smoke media march is "the
exact pathology Lin diagnosed". It is not. The media march inside `trace()` accumulates
tau and tint per cell and never casts a sun ray (`raymarch.wgsl:1656-1900`) — smoke is
*unlit* rather than expensively lit — and the 2026-08 fire collapse was a missing
transmittance early-out plus occupancy counting smoke as a blocker, both fixed that day.
**The per-step shadow ray exists in exactly one place: underwater god rays.** `godRays`
(`raymarch.wgsl:3347`) calls `trace()` once per march step —
`TUNE_GODRAY_STEPS × TUNE_GODRAY_SHADOW_STEPS` per submerged pixel — which is Lin's 16 ms
version verbatim, and his fix is ~100x on that path. The voxel-keyed shadow cache cannot
serve it: the cache is keyed on surface *patches* and a god-ray sample is a point in a
volume. What can serve it is a **sun-space depth map** — one small orthographic render of
blockers along the key light, refreshed when the sun or the dirty set moves — or, cruder,
a per-column "highest blocker" for a near-vertical sun. The same structure is what would
let smoke plumes self-shadow (the unlit-smoke look is the visual cost of the current
early-out) and would give §2.4's injection a source that doesn't march. Note also that
`--render-budget` has no submerged camera, so this path has never been measured.

### 2.8 Cheap roughness (a hack worth stealing)

> *"a very cheap approximation of rough specular which imitates the anisotropic blurring by
> just increasing the blurring tolerance based on raycast distance... it's literally **4x
> faster and way less noisy** than randomizing the rays."* (2021-01-14)

And the general form: *"all roughness is is `mix(diffuse, reflected, n)`"* where diffuse is
cosine-weighted hemisphere sampling and reflected is weighted within a limited hemisphere
(full hemisphere → diffuse, zero → mirror), blended by Fresnel for dielectrics or a constant
for metals. *"roughness can be recreated with phong. all the other stuff makes very subtle
differences at waaaay more compute cost."*

### 2.9 Grass — the year-long bottleneck, and the fix

Grass was his #1 renderer cost for over a year. The failed design:

> *"the way i do grass is create an aabb for each blade, and then in the ray intersection
> test for the aabb, calculate the curve and voxels that make up the blade, and then ray-aabb
> test all of them. which is fine, but the fact that so many aabbs overlap isn't good. **BVHs
> don't do well with overlapping geometry.**"* (2019-12-31)

The fix, three weeks later:

> *"from a noise lookup + several aabb tests + several overlapping aabbs + clipping + several
> buffer reads + trig functions **down to a single buffer read + one bitwise op + one aabb
> test**, and a nearly optimal bvh."* (2020-01-22)

He also kept a **separate simplified world for path tracing** with the waving grass removed
entirely — see §3.3.

**Compared to us:** our tall grass is `MICROF_STRANDS` blades with a ray-shear brick sway,
which is closer to his *fixed* design (derived, no per-blade geometry) than his broken one.
The transferable lesson is the separate simplified world for secondary rays.

### 2.10 Miscellaneous shading notes worth keeping

- **Voxel normals from a neighbourhood average:** *"loop around a 5x5x5 neighbor radius, and
  if the voxel is solid then you add the local pos to the average position, then
  divide/normalize and you get a normal. 3x3x3 won't result in a smooth curve."*
- **Marching-cubes smooth normals** come from interpolating *gradients*, not positions:
  compute `dx = noise(x+1,..) - noise(x-1,..)` per corner, lerp the normal by the same `mu`.
- **Face normal from a slab test** — he posted the exact branchless-ish routine twice
  (`ray_aabb_face`), returning `0..5` and reconstructing `vec3(face%3==0, face%3==1, face%3==2)`.
- **Flooring the hit voxel:** *"if you want to floor the voxel position at a ray intersection
  via `floor(origin + dir*t)`, use `t = mix(tmin, tmax, 0.5)`. **no amount of epsilon in the
  first expression will give the correct result 100% of the time, but the second one
  will.**"* (2020-11-20) — a genuinely non-obvious robustness fix. *[Checked: our DDAs
  never floor a hit — the traversal carries the cell — and the one place a position is
  re-floored, the chunk-skip landing (`raymarch.wgsl:1616`, `shadow_resolve.wgsl`), applies
  `t = tOut + 1e-4` **and** forces the crossing on the exit axis, which is a stronger guard
  than his midpoint. `sim_pick.wgsl:25` floors a ray *origin*, not a hit. Nothing to do.]*
- **NaN ray directions hang the GPU:** `1.0/0.0` in the reciprocal direction is the classic
  cause of a traversal loop that never terminates.
- **Cheap ambient:** `mix(0.5, 1.0, dot(normal, normalize(vec3(0.7,1.0,0.5))) * 0.5 + 0.5)`.

---

## 3. Level of detail

### 3.1 His headline verdict: LOD-first is how voxel engines die

> *"this is an old project where lod was the focus, and what i ended up with was a plain and
> boring world with a cool view distance. **and that's what happens with basically every proc
> gen lod world.**"* (2020-05-06)

> *"the real lod troubles will come when you become depressed and turn to drugs after you get
> it working for terrain and then realize there are so many more problems to solve and no good
> way of solving them."* (2020-12-31)

> *"you have to give up a lot of up close attention to get a view distance like that. there's
> a good reason minecraft's view distance doesn't stretch on for hundreds of kms... other stuff
> as well like lighting and any sort of physics wouldn't work on a scale that large, and world
> gen wouldn't be able to be detailed."* (2021-01-09)

He explicitly **turned LOD off** for most of 2020 to avoid designing around it:

> *"i'm purposely testing everything with no lod, so the lighting calculations/sound/far view
> distance all runs at 5x the detail level than needed. just waiting to flip the switch."*

### 3.2 LOD in a ray marcher is nearly free (and half a line of code)

> *"all you do is once a ray goes long enough, you start saying non-leaf nodes are leaf nodes.
> and the further the ray goes the shallower the ray goes. it's really not that complicated,
> it's like half a line of code."* (2021-04-30)

Split criterion:
> *"if the distance between the node center and the camera is < the node size * some scaling
> factor, then you split it. if it's > the node size * some scaling factor then you group it.
> some scaling factor is 1.414 or greater."* (2021-02-07)

### 3.3 The real win is LOD on *secondary* rays, not primary

This is the sharpest LOD insight in the corpus and it is barely mentioned anywhere else.

> *"the best part about this is i can make the AO rays sample **max level − 1** and get 300-500
> fps instead, with better results imo. **can't do that with RTX.**"* (2020-04-20)

> *"i've got a simplified version of the scene to path trace against. just by downsampling this
> way... the path tracing is at least **3x faster**. this will help with photon mapping, heavy
> noise reduction in the scene, aliasing issues... and of course speed."* (2020-01-07)
>
> *"the simplified world is a very key part in the path tracing being fast because it contains
> an optimized bvh with **no waving grass**."*
>
> *"here is base scene rendering using the level of detail... **can't even tell.**"*

"Better results" is not a typo: coarser secondary rays are *less noisy*, because a mip-level
sample is a pre-integrated average.

**Compared to us [corrected]:** the first draft said we "do not use a coarser
representation for shadow rays". We do, for *skipping*: both DDAs skip on chunk blocker
counts, and a 4³ blocker mask exists (`kSubOccDim`, two classes, in the tail of the
occupancy buffer). The sub-block *skip* was built, measured and **refuted**
(`PLAN_surface_flight_perf.md` Correction 6: the mean chord through a 4-voxel box is
`(2/3)·4 = 2.7` voxels, less than a box-exit jump costs; it ships behind
`const SUBOCC_SKIP = false` and Tint folds it away). Two other things the draft got wrong:
`gotcha-cascade-shadow-has-no-early-out` (48x) refutes marching near shadows through the
*cascade*, which has no early exit — it says nothing about a coarse level inside the fine
DDA; and his AO number (300→500 fps) doesn't transfer at all, because our AO is three
taps, not rays.

Lin's idea is a different operation from skipping: **terminate on a non-empty coarse
block.** Treat a set blocker bit as a hit once the ray is far from its receiver. That has
nothing to do with chord length — it deletes the fine steps *inside* occupied blocks and
makes sparse geometry (canopy, grass, rubble) opaque to secondary rays, which is why he
got *less* noise as well as more speed. Where it lives for us: `shadowMarch` in
`shadow_resolve.wgsl`, gated on `tCur > D` so near-receiver results stay identical (the
`shadow-cache` gate asserts the two DDAs agree — keep that true inside D). Expected gain
is modest and should be stated so: the cache already cut shadow rays ~7x (1.35M →
~200k/frame), so the traversal being LODed is a fraction of what it was; the visible
effect is heavier, calmer canopy shadows. One-branch experiment, one `--render-budget`
run, no hash.

### 3.4 The thin-wall problem, and his answer

Downsampling voxels destroys thin geometry — a 1-voxel wall vanishes or becomes opaque at
mip 3. His fix was **anisotropic voxels / extended contours**:

> *"esvo contours, but instead of 1 plane it's 3 planes. makes a moderate difference. the big
> thing is for doing lighting calculations, they provide **actual normals for way downsampled
> data**, which will be especially useful for thin features. eg. walls."* (2020-07-28)

> *"they're voxels, just with extra info that alleviates the stupid downfall of lod voxels,
> which is the poor surface representation. **so a thin wall will remain a thin wall even at 5
> lower levels of detail.**"* (2020-10-15)

Also, the leak this fixes at the source:
> *"i fixed a major oversight where i forgot to downsample empty/totally full chunks that led
> to light leaks, but they are no more."*

**Compared to us:** this is the far cascade's known weak point. A cascade cell is
**point-sampled at its centre voxel** (DESIGN §9), chosen so edits and the sieve agree
exactly; the cost is that a 1-voxel wall at level ≥2 survives or vanishes by sampling
luck, and `kFarN` had to double (256→512) to keep small structures on the horizon. His
contours are too heavy for a byte-per-cell cascade, but a cheap half-step exists: **one
conservative "any blocker" bit per cascade cell beside the point-sampled material**, used
by `farShadowed` and by the cascade *hit* test, with the material only choosing colour.
Thin walls then occlude and shadow at every level. The trade is that fences read as walls
at distance — on the horizon, correct. Render-only, no hash.

### 3.5 Cone tracing is the AA answer, and nobody will give it to us

> *"if you use cones for primary rays you can alleviate aliasing in the distance because
> you're sampling a mipmapped volume."*
>
> *"i posted an issue on the vulkan ray tracing github repo asking about voxel/cone tracing
> support and they said it's not one of their priorities."* (2020-08-31)

He also measured that **ESVO's own beam optimisation didn't pay off** on modern hardware:
> *"in my case it's far slower, as is using contours. so idk where esvo gets their numbers, or
> if it was just a good optimization for way old hardware."* (2021-04-16)

### 3.6 LOD pop is the thing that made him abandon LOD entirely

> *"i wanted a big view distance, **no lod pop-ins**, and fine control over the proc gen so
> this is what i found works"* — i.e. instance placement of real assets, no mip pyramid.

> *"it would be nice if the transitions were seamless. maybe through some fading. the problem
> is more so the terrain gen just not being able to keep up at a sub-pixel level."*

---

## 4. Memory

### 4.1 The philosophy

> *"Memory should be treated as expendable until you need to optimize or are shipping
> something. **It's like the last thing that a design should be based on.**"* (2020-02-10)

Which he then spent a year violating out of necessity. The measured numbers:

| Thing | Cost |
|---|---|
| 4096³ world, uncompressed | 6–10 GB (2020-04) |
| Same, after palette (3-bit) colour compression | 13.8% of raw colour |
| Normals, packed | ~33% |
| PhysX collision meshes at full voxel res | 15 GB |
| Same at **half** voxel res | **7 GB**, *"no noticeable difference in quality"* |
| Uncompressed animated 4096³ tree, 30 frames | **1 GB per tree** |
| Same with DAG + BC1 | ~2 GB for the *whole* animated set (98–99% ratio), still called *"not feasible"* |
| A "temple" scene saved as voxels | 22 GB → later 4 GB with some compression |
| The same scene saved as **build operations** | **107 KB zipped** |
| Realistic floor, albedo + normals + tree + mipmaps | *"somewhere high like 8 bytes per voxel"* |
| Best published SVDAG result he cites (128K³, 18.4 B nodes) | 7.63 GB ≈ **5 bits/voxel** |

### 4.2 The bug worth memorising: pool fragmentation causes *progressive* slowdown

> *"i have this issue in my engine where the more of the world that gets explored, the worse
> the rendering performance is, at least with the shadow mapping (starts at like 3ms and
> eventually shoots up to 16ms). well i narrowed down the cause to be **fragmentation of the
> voxel data in the memory pools**, likely cross-pooling. this was my first suspicion but i
> was really hoping that it wasn't the case because that's like nightmare status. so now what
> i need is pools/buffers per some chunk size."* (2020-05-09)

The diagnostic that clinched it is a beautiful piece of debugging:

> *"what really sealed this being the problem was in the shadow map, i would lookup the voxel
> attributes (albedo & normal) for debugging visualization, and when i **accidentally turned
> that off, the framerate shot way up**. it's the only thing that makes sense. ray step counts
> and everything else is the exact same otherwise."*

And why primary rays didn't show it:
> *"primary rays were cast using perspective projection, so there was natural deviation
> anyway — they were already close to worst-case. shadow maps were cast using orthographic
> and covering the entire surface of the world, so they **start out best case with lots of
> coherency**"* — and therefore had the most to lose.

**Compared to us [corrected]:** the first draft called this "the most directly
transferable warning in the document" and proposed a neighbour-page-delta metric.
Examined against our allocator, **the mechanism does not exist here.** Lin's pools held
**variable-size octree nodes**, so "fragmentation" meant one chunk's nodes scattered
across the pool and one coherent ray touching many distant cache lines. Our page is a
**whole 16³ chunk — 16 KiB, one page per chunk** (`kPoolPages = kNumChunks + retire
ceiling`): intra-chunk locality is guaranteed by construction, and a ray reads the page
entry once per chunk crossing (`cchPt`). Whether *neighbouring chunks'* pages are
adjacent never mattered — even the dense identity map puts y- and z-neighbours 32 and
1,024 pages apart — and the only hardware effect of page order at this granularity is TLB
reach (four of our pages share one 64 KiB GPU page), which is second-order. **No
monotonic creep from pool order is predicted for this engine**, and the proposed metric
would measure something that isn't a cost. Dropped.

What *is* transferable is the shape of the bug: **a slow, session-length degradation that
no short benchmark sees.** We have candidates with real mechanisms — the page retire
backlog (`page-roundtrip` fails inside long suites for exactly this reason), the dirty
set that historically could only grow, shadow-cache bucket collisions as more distinct
patches have been seen, MPM particle counts — and no harness that would notice any of
them, because `--autofly-hard` runs a fixed 1,200-tick schedule. CLAUDE.md rule 6
applies: don't instrument one hypothesis, instrument the class. **A `--soak N` mode that
flies the adversarial route for N minutes and writes p50/p99 frame time, resident pages,
retire-queue depth, dirty count and shadow-request overflow per 1,000 frames to
`last_run.json`, with the gate asserting no monotonic slope, catches all of them at
once.** His second lesson stands: read it off the most coherent ray type you have, which
for us is the shadow resolve pass's own GPU time.

### 4.3 Store operations, not voxels

> *"for the building i've decided to store the shapes and re-voxelize them on updates...
> sounds complex and compute-heavy but it saves a lot in the long run, and solves some
> issues. for example, if someone removes a cobblestone block they placed, it doesn't just
> remove a cube and leave bits of cobblestone, it removes the cobblestone and what's left is
> what the user would expect. also small file sizes. and if someone changes textures offline,
> it will reflect in their existing builds."* (2020-11-18)

> *"the save data for the world i've been working on: **just 107kb zipped**."*

**Compared to us [corrected]:** the first draft suggested making `.svr` saves op-based.
That does not transfer, and the reason is the one thing Lin never had: **our world
evolves without input.** His 107 KB is a list of build operations over static terrain;
replaying it re-creates the scene. Replaying our MutationQueue re-creates the scene only
after **re-simulating every tick since worldgen** — deterministic, so possible, but
O(ticks played) — and "semantically correct undo" of a brush stroke is not even defined
once the sand it placed has flowed downhill. `SaveWorld` (`worldio.cpp:112-172`) is
correctly a snapshot: RLE'd voxel words per chunk (`'SVR2'`), entities, meta.

The two benefits he names *do* apply, to the right layer: **authored content**.
`assets/worldedits/*.svedit` layers applied by `worldgen.editLayer` are already op-based
and already give retroactive asset updates — change a material, every world regenerates
with it. The remaining gap is the replay log: the MutationQueue is **never persisted** (no
serializer exists in `src/`), and it is the network stream's prerequisite. Persist it as a
*sidecar* to the snapshot — ops since the last checkpoint — not instead of one.

### 4.4 Attribute compression, measured

- **Palettes with weights** in 21 bits: *"4 bits per material, 3 bits per weight"* × 3
  materials, or an albedo flag with 7 bits per channel. Palette is **per 8×8×8 block**, which
  causes visible blocking at block boundaries when the palettes differ.
- **BC1 vs BC7:** *"BC7 definitely doesn't provide enough uplift in quality to justify the 2x
  cost."* He uses BC1 for colour, normals, and BRDF data. *"i think colours should use bc7,
  but normals can probably use bc1"* was his earlier guess; he settled on BC1 for everything.
- **The indexing trap:** *"in the paper that showed how poor [BC] was, what actually happened
  was they had a very terrible indexing method (z-ordering) which does not at all work well
  for linearized octrees. dumping it in 4x4 blocks works and personally i can't even tell."*
- **Separating materials from geometry costs performance:** *"it involves tracking a running
  sum, which means a thicker stack filled with said sums, so when you miss and pop you can
  pull them out."*
- **DAG-ifying attributes is basically off the table:** *"when you separate material data from
  geometry, you have to rely on an indexing approach which means dictionary compression isn't
  really an option."*

### 4.5 GPU-side allocation

> *"the solution is to just use gpu memory pools and use 32 bit pointers, which store the
> banks in some bits and are word-aligned. then you can pass along your pointers all willy
> nilly and glsl's buffer constraints are a thing of the past."* (2021-03-21)

> *"for the fixed sized pools which make up the majority, i have a mutex for each, a **bitmap
> of dirty indexes**, and a vector containing the regions to be modified for fast gpu
> uploading... the point of the bitmap is to prevent something from getting marked dirty
> twice, and it has a small memory footprint."* (2020-10-05)

> *"You just have a giant buffer on the gpu, write an allocator on the cpu, then rebuild
> chunks of the octree. For the allocator, i use a vector containing free slots and that
> works fine."*

**Compared to us [verified]:** identical architecture to `World::PageOffsetOfSlot` + the
free list + `cpuDirty`, and the double-mark guard is already in: `SlotSet cpuDirty_`
(`pagetable.h:52-88`) is a bitset **plus** a member vector — `Add` tests the bit and
refuses a duplicate, `Clear` walks only the members so a settled world pays nothing. The
2026-08 bug (`gotcha-cpu-dirty-mirror-could-only-shrink`) was a missing *union* with the
GPU's dirty set, not double-marking. Nothing to do.

### 4.6 Don't store the air

> *"that's fine for stuff on the surface, but **you do not need to store gigabytes of air and
> underground that will make no difference to the player 99.99% of the time.**"*

**Compared to us:** this is the JITTER/UNIFORM/EMPTY sentinel argument, made a year before
we made it. `project-page-table-jitter-sentinel` measured 55% residency reduction from
exactly this insight applied to *buried bulk* rather than air.

---

## 5. Ideal data structures

### 5.1 The one-line rankings he actually believes

> *"an octree will never outperform a fine tuned BVH. worst case, BVH will match an octree,
> best case it'll outperform. however, octree comes with numerous benefits: mipmapping, fits
> with voxels, probably faster to update, easier to implement."* (2020-04-12)

> *"if uniform grid vs svo was graphed, uniform grid would be faster up until a resolution of
> like 16, and then svo would pass it."* (2020-11-11)

> *"SVOs are likely going to be the fastest to ray trace for static stuff. bvhs are great for
> arbitrary geometry but expensive to build, but they make a great top level acceleration
> structure. **a flat array is free to build.**"* (2020-10-20)

> *"ray traced svos are very slow. **they're more of a compression scheme than they are a ray
> tracing structure.**"* (2020-07-08)

> *"svdags are basically dictionary compression applied to voxel octrees"* — effective, but
> *"the main tradeoff is the slow generation"* and *"they're totally static and only start
> making sense when you have really high depths."*

...and then the reversal (2021-04-25), after implementing all of them:

> *"the gains came from **dropping dags/svos. octrees are extremely terrible for ray tracing,
> and dags actually do a very poor job of representing sparse voxel data** despite their gains
> over esvo, which is also a terrible structure for representing sparse voxels."*

### 5.2 His actual world hierarchy

> *"i don't actually use chunks in the world data. it goes **voxels → cells → regions → hash
> map.**"* (2020-12-29)

- cells: 64³ voxels
- regions ("mega regions"): 64³ cells = 4096³ voxels
- a 3×3(×3) array of live regions around the player, the rest in a hash map keyed by region
  coordinate

> *"just have a giant chunk that's indexable via hash, a tiny array of those giant chunks
> around the player, and then uniform chunks inside those giant chunks."*

> *"for ray tracing, trace through the middle first since that's the most likely to be hit,
> then figure out the next 2 closest world regions — since it can only ever trace through 2
> others."*

**Compared to us:** our `kWorldN`³ toroidal window + `WindowOrigin()` + slot indices *is*
the "tiny array of giant chunks around the player", collapsed to one region. Lin's version
generalises to an unbounded world by hashing regions. If we ever break the
`maxStorageBufferRange` ceiling that blocks `kWorldN = 1024`, his multi-region layout is the
shape that the split-buffer solution should take — and note his trace order optimisation
(centre region first) falls out of it for free.

### 5.3 The n-ary tree experiment (his best homegrown structure)

> *"instead of 2³ children for an octree, it's n³ children. so when you split a node in my
> case, it creates 512 children [8³]... 4 layers deep you have a 4096³ world."*
> *"it's a balance between region depth and node size. 4³ and might as well just go with a
> regular SVO, 16³ and it's too big."* (2020-04-16)

After the register work of §1.1 this beat ESVO. But:

> *"you don't want to do that. the n-tree is only beneficial if you have a shallow depth. the
> memory usage scales very very badly and your empty space skipping becomes quite limited.
> instead, you'll probably want **an octree with chunks at the leaves.**"* (2020-06-03)

He also tried a two-level 8³→8³ and rejected it: *"it's very memory intense. tracing
performance is pretty good, but you don't have the ability to trace cones."*

**Compared to us:** our chunk (16³) → page layout is the "octree with chunks at the leaves"
recommendation with the octree collapsed to a flat slot table, which is legitimate at a
fixed 512³ window and is what he says is fastest below ~resolution 16 levels.

### 5.4 The design questionnaire (2020-06-20) — quoted in full

Addressed to someone about to build a depth-20 octree. It is the single best paragraph in
the corpus for anyone choosing a voxel data structure, and every question in it is one we
have had to answer:

> *"to try and better solve your problems, ask yourself these questions: how will procedural
> generation work? how will biomes work? terrain gen? vegetation? will trees be a part of the
> voxel density field? how will you rotate the trees if you want them to fall? how will
> collisions work? will they be on the mesh or voxel data? how will building work? will it
> also share the same voxel structure as the terrain and everything else? how will you then
> rotate those objects for physics? translate them? how will you downsample them to view them
> from a distance? how will you store these modified chunks? how will you handle lighting for
> this gigantic world? you can't ray trace it if you go with the meshed chunk approach because
> it's too large to build an acceleration structure efficiently... how will you handle terrain
> materials? material blending? snow? **is a gigantic octree really a good solution for any of
> these questions beyond some terrain with level of detail?**
>
> i highly, highly recommend trying to solve these problems at a smaller scale, even a fixed
> size 1024³ world or something, because even then it's not exactly trivial and is exactly why
> i've dropped level of detail/big view distances until i get everything else right."*

And the companion:

> *"there exists a venn diagram somewhere, with relationships for rendering & ray tracing
> speed, storage/memory usage, networking, physics sim, manipulation speed and fine tiny voxel
> control, and i think i finally cracked the code and found the middle."* (2020-05-24)

> *"implementation difficulty, modification update speed, offline data footprint, memory
> footprint, lod-ability, rendering performance, the ability to have ray tracing from
> arbitrary directions... you could spend 10 years trying to find the best solution. **my
> solution was to sit back and ask what it was i was trying to accomplish, and then work
> backwards from there** — instead of do what every voxel dev does (including myself) and say
> 'okay i want large terrains so i'm gonna need lod and generating simplex noise in a clipmap.
> i dunno how trees will work but i'll figure it out... then lighting? i dunno i'll figure that
> out too after i have my bland generic no mans sky chocolate blobs. then for physics, uhhh
> i'll see when i get there'."* (2021-05-09)

**Compared to us:** this is CLAUDE.md's design guideline #1 ("design data format for the
hardest consumer") in longer form. Our hardest consumer turned out to be *determinism +
sleep*, not rendering, which is why the page table is derived data and the voxel word is
32 bits. Worth noting that Lin's list does **not** include determinism or replay — we have
a constraint he never had, and it is the reason several of his options (atomics, CAS, GPU
allocators with scheduling-dependent order) are closed to us.

### 5.5 On the voxel word itself

> *"My leaf nodes are 12 bytes per voxel — including albedo, normal, and all material
> properties: opacity, roughness, emissivity, and some other properties."* (2020-11-17)
>
> [asked why not index into a block-type table]
> *"sure, i could do that, but it'd only save a few hundred mbs at the most and remove a lot
> of freedom."*

**Compared to us:** the opposite call, and correctly so — he had no simulation to feed, so
per-voxel material properties were affordable and gave artistic freedom. We have 32 bits
total and material behaviour is a *data table* keyed by id (CLAUDE.md guideline #4, "no
closed-ended systems"). Our word is full precisely because we bought sim state with the
bits he spent on appearance. Worth keeping in mind when someone argues for per-voxel
roughness: Lin's engine is what that costs.

---

## 6. Fluid simulation

This is the section where Lin's experience most directly contradicts our architecture, and
also where he most directly predicted the path we ended up on.

### 6.1 He rejected cellular automata water, loudly and repeatedly

> *"c::w [his earlier engine] used cellular automata water. **which has no soul.**"*

> *"velocity is 99% of what makes it look like water. i hate that cellular automata water look
> where it just flows straight down."* (2020-08-13)

> *"cellular automata just averages volumes or flows down"* vs *"the pipe method allows
> flowing in circular directions"*.

> *"i think nel's is CA too, he just updates it really really fast so it appears smooth."*

The steelman from `mikola` in the same conversation, which Lin accepted:
> *"cellular automata stuff can trivially be incompressible but it's bad at capturing
> advection."*

**Compared to us — this is the sharpest criticism in the corpus of what we are.** He is
right about the failure mode: CA liquid without a velocity field looks like it is *falling*,
not *flowing*. We have already hit the structural version of this
(`gotcha-ca-liquid-levelling-limit`: the water mound is the equalise fixpoint) and already
took the countermeasure (the MLS-MPM seam, WP1–4, `docs/PLAN_mpm_fluids.md`). Lin's arc is
the strongest independent validation that **the MPM direction is correct** and that the
remaining WP5 A/B is worth resolving rather than shelving.

But note the asymmetry he also concedes: **CA gets incompressibility for free**, which is
the thing MPM/APIC spend all their effort on. Our hybrid — CA bulk, MPM seam — is a
position he never occupied and arguably the best of both.

### 6.2 The full search, and where he landed

Heightfield/"pipe" method → multi-layer heightfields → pure Eulerian → SPH → PIC/FLIP →
APIC → **MPM**.

> *"the beauty of mpm and why i settled on it is **it's the only fluid sim model that doesn't
> require solving a linear system**."* (2020-11-24)

> *"the thing is, the fluid simulation part is easy if you're just simulating a general fluid,
> like a gas. but it becomes challenging when you try to do water, because you're actually
> simulating 2 fluids — air and water. and then there's boundaries to enforce. and then the
> gigantic linear system of equations to solve that can probably only be solved with jacobi
> because the system will be so large. and *then* there's mass conservation problems because
> you can't just track the surface using a density."* (2020-08-12)

> *"i'm becoming increasingly aware that pretty much every technique is either strictly
> lagrangian (SPH, PBF) or the combination of eulerian and lagrangian. i've only found 2
> papers that deal with free surface sim on just eulerian."*

> *"from what i've seen APIC is fantastic at preserving momentum and volume. **i care most
> about volume because it wouldn't make sense for lakes to just evaporate over time.**"*

### 6.3 Coarse simulation, fine detail — the resolution decision

> *"[the particles are] on a much, much coarser level — like **32x coarser**"* than his voxels.
> *"but like 16x that is just fine details — rocks, tree leaves, things like that."* (2020-08-09)

> *"by my estimations, the active water volume for my world in the average scene at the scale
> i'm predicting won't even top 100k particles. mobile phones can simulate 100k particles."*

> *"i had the idea to make the eulerian grid very coarse and use tiny particles, and as long as
> i used small time steps i could probably get away with a relatively small amount of
> particles — and use the tiny particles to do collision response at fine detail."*

Plus adaptivity:
> *"i've actually managed to get the simulation running adaptively. eg. processing fine
> particles near the surface, and only processing the world where there's water."* (2020-08-29)

**Compared to us:** our MPM seam runs at the voxel grid. Lin's argument is that fluid
*dynamics* needs far less resolution than fluid *appearance*, and that the two should be
decoupled. If the WP5 A/B ever concludes that full-resolution MPM is too expensive, running
the momentum field at ¼ or ⅛ and letting the CA carry the fine surface is a design he
validated.

### 6.4 The two-tier particle trick (his single best visual-per-cost idea)

> *"it's got some issues but experimenting with **'fast particles'** that add a ton of volume &
> detail for like **1/10 the cost**."* (2020-08-16)

> *"the splash particles are being run on the gpu. that can do millions no problem. the cpu
> could handle it too, but the cost of transferring a million particles is more expensive than
> just simulating them on the gpu."*

> *"these particles are like **1000x cheaper** than the volume 'true' water particles."*

> *"The particles are part of the simulation and spawn where a **high velocity surface** is
> detected."*

> *"all you have to do is detect where water is falling and add some particles. then it'll look
> great."*

And how they move — the derivation he had to work out by hand (2021-01-01):
> *"if i have a particle in a velocity field, how do i update the particle's velocity in a
> step? obviously you can't just add the velocity field or it would accelerate infinitely...
> `F = (m0 * v0 - m1 * v1) / dt` — there's the equation i needed."*
> *(i.e. drag toward the field via a momentum difference, not an acceleration.)*

**Compared to us [corrected]:** the first draft said we don't spawn spray from
high-velocity surfaces and that doing so would be hash-excluded. Wrong on both counts for
MPM liquids, right on the first for CA liquids. The MPM G2P already emits **splash
droplets** keyed on particle speed and low local density (`sim_fluid.wgsl:1150-1207`) and
**foam / spray / bubbles** classified by density (`:1320-1390`, `PPAY_FOAM`) — Lin's
two-tier trick, implemented. But they are *sim* particles: they land, and where they land
enters the world hash, so they are not free of determinism cost; they are merely keyed
deterministically on `(slot, position, tick)`.

The CA liquid has no velocity field, so "detect a high-velocity surface" needs a proxy.
The cheap and correct version is **render-only**, in the style of `siltMotes`
(procedural, no buffer, no hash): a liquid cell with air beneath it and open sides is a
*falling column*; draw mist and spray around such cells in the fragment shader from a
noise field animated by time. It costs a few neighbour reads on liquid hits only, cannot
touch the hash by construction, and is what makes a CA waterfall read as a waterfall.
Real ballistic spray at the impact point — where the falling column meets a surface — can
come later through the existing spawn stream if the render-only version isn't enough.

### 6.5 Threading: pad the chunks, sync the padding

> *"the trick comes from **localizing particles to their chunks and padding the chunks**, and
> then having a stage that syncs the padded data."*
>
> *"i benchmarked color-coding the chunk updates and just having particles update neighbor data
> directly but it was **very very very very verrrryyyyy slow**. the neighbor sync instead only
> makes up maybe **2% of the total time**."* (2021-01-01)

> *"pad your chunks with a 1 block thick layer, simulate the chunks independently, then sync up
> the padded values with the real neighbor values. that is what i do in my water sim, even
> though it's not cellular automata it's the same principle."* (2021-03-15)

And, notably:
> *"nope, **single buffer for everything**. the sim and gpu just play ping pong."*

**Compared to us [corrected — the first draft had this backwards]:** the draft said "we
should not be tempted by colouring schemes". **Our CA *is* a colouring scheme, and it is
the determinism mechanism**: 3×3×3 cell colouring, 27 colour passes × 2 gravity substeps
= **54 serialised indirect dispatches per tick** (`pass_table.def:207`, DESIGN §4), each
over the compacted dirty list, with the pass table refusing to let two colours overlap
because adjacent colours' writes collide. Lin's "verrrryyyyy slow" colouring was CPU
thread scheduling between colour groups in a particle sim; it says nothing about 54 GPU
dispatches. (Mark+apply two-phase is used by *other* kernels; the CA itself is colours plus
the tick stamp.)

What his padded-chunk design *is* relevant to is the known fixed cost of those 54
dispatches: on a small active set (a few hundred chunks) each dispatch under-fills the
GPU and the barriers between them dominate (`project-perf-audit-2026-08`: "54 serialised
step dispatches/tick fixed overhead — would need a shared-memory tile redesign"). His
shape *is* that redesign: one workgroup per chunk loads the chunk plus a 1-cell halo into
workgroup memory, runs all 27 colours in-workgroup with `workgroupBarrier` between them,
writes back, and a **separate reconciliation pass fixes the halo** — cells that moved
*across* a chunk boundary. His measurement (sync stage ≈ 2%) says the halo pass is cheap.
The hard part, which he never had, is making the reconciliation **order-independent**:
two chunks that both want to move a grain into the same border cell must resolve by a
rule that is a pure function of pre-tick state (the border cell's own colour pass
decides; lowest chunk index wins), and the result must either hash identically to the
54-dispatch version or be accepted as a hash move under rule 1. A real project, not a
tweak — and the only structural lever left on CA tick cost, so it belongs on the list
with that label (§13.2).

### 6.6 Sand and snow via MPM: the timestep wall

> *"the snow is so numerically sensitive that it has to be run at ultra low timesteps to be even
> remotely stable (**dt = 1/1000**), and on top of that it has a ton of matrix math."*
> *">maximum allowed dt: 0.0001 in most cases"*
> *"i don't understand why mpm is so numerically sensitive, but i wonder if there's a way to
> eliminate some of the factors like hardening. if the singular value/polar decomposition could
> be thrown out and replaced with some approximation it'd be great — even though the polar
> decomposition is what allows stuff like breaking apart and hardening together in the first
> place."* (2020-11-23/24)

> *"material point method sand and snow requires extremely small time steps, like 1/10,000 and
> uses matrix decomposition. so it's omega slow. the problem with proper sand where they behave
> like rigid particles with friction is it's too volumetric. **so i might just do vertical
> sand.**"* (2021-01-27)

**Compared to us — this is a load-bearing warning.** Our whole engine is *falling sand*. Lin
independently concluded that MPM sand is not real-time-feasible and that he would fall back
to trivial "vertical sand" — i.e. **cellular automata**. That is a strong argument that our
CA-for-granular / MPM-for-liquid split is not a compromise, it is the correct decomposition,
and that we should **not** be tempted to extend the MPM seam to powders.

He also flags the corollary: *"depending on how they're simulated, sure — but then there
would be no sleeping for the sand."* Rule 2 (cost scales with activity) is incompatible with
a continuum granular solver.

### 6.7 The unsolved problems he names

- **The water cycle.** *"waterfalls would be awesome but that water cycle problem is actually a
  real problem. i think the only way to solve it is to have water evaporate or something. but
  then there's spawning from the waterfall, and for that ¯\\\_(ツ)\_/¯. there's no such thing as
  a perfect solution besides making an infinite spring that spawns particles that will
  despawn guaranteed."* — cf. our `project-water-bodies` and its evaporation rules.
- **Two-way coupling.** One-way (fluid pushes objects) he got working in a day. Two-way *"is
  going to have to be off the table for now."* Buoyancy, though: *"volumetric buoyancy... it's
  **super trivial to compute with voxels**."*
- **Small puddles.** The reason he refused pure 3D sim at his voxel scale: *"i'd do 3d fluid
  sim if it weren't for the fact that small puddles would be impossible — to be realtime the
  particles would need to be very large."*
- **Sticking.** *"a solution where it reflects off of surfaces, but is allowed to leak through
  dirt, and stops at rock... the only way to truly fix it was to multiply the particle count by
  like 100. but you can't really tell. **water naturally sticks to surfaces anyway.**"*

### 6.8 Measured fluid performance (his numbers, for calibration)

| Date | Result |
|---|---|
| 2020-08-16 | 100k+ particles, **one CPU thread**, no SIMD |
| 2020-08-19 | ~15 ms peak sim time, constant dt = 1/30 |
| 2020-09-08 | ~8M particles (rendering optimised) |
| 2021-01-01 | **1.5 ms sim, 666 fps**, 100% multithreaded, single-buffered |
| 2021-01-03 | 200k particles @ 30 fps on 4 threads (with the full engine running) |
| 2021-01-04 | Whole stress-test lake *"still simulating in under 6 ms"* |

The thing that actually limited him: *"the stutters are from the terrain sculpting. it's
actually like **100x more expensive than the fluid sim** and runs on the same thread."*

---

## 7. Physics

### 7.1 He wrote his own solver, then bought one

He implemented projected Gauss-Seidel / sequential impulses from Erin Catto's paper, with
`mikola`'s help, over roughly three weeks (June–July 2020) — contact constraints, Jacobians,
friction, warm starting, contact reduction via QEF/k-means clustering. Then:

> *"i have a bug in my physics system related to collision testing, because i'm only using a
> single collision point per cell. for big objects it's generally fine, but for small objects
> that point changes and so they sink through the ground or jitter... aka **i need collision
> manifolds**, which is not trivial to generate and usually involves some temporal
> construction. so i implemented voxel-voxel collisions, but it generates like a bajillion
> contact points and is slow, and so i'm considering just using physx."* (2020-10-22)

> *"imagine a cube lying flat on the ground and the single contact point it generates is at one
> corner. it's going to generate an impulse at that corner, which produces a torque and rotates
> the other side into the ground, and then next frame the collision point isn't at the corner...
> **it basically defeats the sequential impulse model.**"*

And the verdict:

> *"tbh writing a physics engine that just uses boxes is as complex as a physics engine that
> uses generalized convex shapes. solvers work off of consistent contact points or contact
> manifolds, which is very very non-trivial to implement."* (2020-11-03)

> *"back when i started this i thought there were fancy optimizations that could be done with
> voxels... but in reality, they don't really work for anything. **the only real benefit i get
> in my world is a O(1) spatial broad phase but that's like the cheapest part of the
> system.**"*

> *"the only thing per-grid voxel physics is good for is fluid sim."*

> *"> the thing is, using the existing voxel world for terrain collision is more efficient than
> uploading a mesh to a lib — **as it turns out it's not.** ... 100% just use an existing lib.
> as soon as you need collisions more advanced than 'does this overlap', it's not worth it,
> because then you gotta do gjk/epa and some other stuff, which all the libraries already
> do."*

**Compared to us:** we use Jolt. Vindicated. Lin's specific findings that still apply:

- **Static → triangle mesh, dynamic → convex hulls.** Both PhysX and Bullet (and Jolt)
  refuse dynamic concave meshes. *"non-convex vs non-convex interactive continuous collision
  detection is an unsolved problem."*
- **Collision meshes at half voxel resolution.** *"no noticeable difference in quality, but
  good lord did memory usage go down — 15gb on my far distance to 7gb."* This is exactly our
  `project-skin-collider-split` (skin authoritative, collider derived by majority-fill).
  Independent confirmation that half-res is the right ratio.
- **Don't block on the solver.** *"call simulate, `fetchResults(false)`, if it returns true then
  copy the results into your objects / add and delete pending actors / call simulate again.
  when the physics lags your main loop won't."* Fixed timestep, run sooner if it overruns —
  *"you don't want to adapt your timestep to fix lag. it's better to just have laggy
  physics."*
- **XPBD is the future.** *"pbd, now that it can handle rigid bodies, is going to become the
  defacto in physics sims because of its stability and couple-ability. **impulses are what
  cause explosions.**"* (Müller et al. SCA2020; PhysX 5 is built on it.)

### 7.2 Player movement — the hack he defends

> *"you loop over all the voxels inside of the player aabb, push the player aabb against what
> it's colliding [with], then push it over vertically if the place is free 4 blocks or
> something, then smooth the camera. if it's not free they can't walk. **it's the smoothest
> method for movement. sounds crude in theory, but far better than any slope estimation stuff
> i tried.**"* (2020-04-17)

> *"fake for static, true for dynamic, at least for corrections."*

**Compared to us:** this is precisely our player collision + step-up + camera smoothing.
Convergent. Also `gotcha-unknown-footing-is-walkable` (blocked-on-unknown is an invisible
wall) is the failure mode of his approach when the mirror is incomplete — a refinement he
never needed because his world was fully CPU-resident.

### 7.3 Fracturing

Two candidate schemes, weighed:

> *"how important is it that fractured objects follow a voronoi/cellular-like pattern? vs
> breaking the result into **cube-based pieces** — so like cutting off branches from an octree.
> **the performance difference could be staggering** for a similar/still cool quality. i think
> the result would be similar to teardown."* (2020-10-04)

Later, having implemented both: *"it's using the voronoi method vs cubic, there's really no
immediate visual difference on the house and the complexity ended up being pretty similar."*

The insight that Voronoi is free:
> *"i just realized that a voronoi pattern is implicit if you just take a set of points and for
> any point in space, assign it a cell index based on the closest point. i mean that's
> *literally* the definition of a voronoi diagram. cool how the cells are always convex though."*

The problems he never fully solved: **islands and slivers** (*"the slivers and islands left
behind are very ugly"*), **spawning fractures collision-free**, and **convex hulls of fractured
pieces still enclosing the removed volume** (a cube minus a bite still has a cube hull).

**Compared to us:** DESIGN.md §7 flags island detection as "budget time for it" — Lin
confirms it stays hard. His convex-hull-of-remainder problem is why our microbody bricks
being COW is the right call.

---

## 8. Procedural generation

### 8.1 The thesis: detail comes from *things*, not from octaves

> *"noise is terrible for terrain gen — which is why you use noise as a basis and then
> supplement it with other procgen."* (2020-08-27)

> *"the right answer is to only use noise for the basic shape and to use something else to add
> detail: whether that's just placing a bunch of rock meshes, using displacement maps, or
> growing a ton of grass."*

> *"that's something a lot of voxel devs don't understand, is that **broad terrain is almost
> entirely flat. your detail shouldn't come from more octaves of simplex noise.**"* (2021-04-03)

> *"**small details + good lighting are what make a great procedural environment.** NdotL +
> shadows + point lights + flat textures + LOD'd simplex noise is not what makes a great
> procedural environment."* (2021-02-28)

> *"the earth is pretty much flat for the most part — it's just everything on the surface that
> makes it interesting."*

> *"i don't want to build a house on a plain. i want to dig tunnels, build bridges, build a
> porch that oversees a valley, build stairs across layers."*

**Compared to us:** `gotcha-slope-gate-must-read-the-landform` (gate on the coarse octaves)
and the terrain overhaul's attenuated octave ladder are half the lesson. The other half —
that the *interesting* detail should come from placed instances (rocks, ruins, ponds, trees)
rather than higher-frequency noise — is a direction the terrain overhaul packages A–C
already started (ruins, ponds, shores) and Lin says is the whole game.

### 8.2 Curves, not raw noise

> *"curves are so awesome. very very easy to add vertical layers."* — posted with the code:

```c
const glm::vec2 curve_points[] = {
  {0.0f,0.0f},{0.5f,0.15f},{0.7f,0.27f},{0.75f,0.35f},{0.8f,0.36f},{0.9f,0.65f},{1.0f,1.0f} };

inline float interpolate(float y0, float y1, float mu) {
  float mu2 = (1.0f - cos(mu * 3.1415926f)) * 0.5f;   // cosine interpolation
  return y0 * (1.0f - mu2) + y1 * mu2;
}
// feed the noise value through the piecewise curve
```

> *"i feed the noise value into the interpolate function, it returns the curve-fitted noise...
> that's why i wanted curves, it makes layering trivial. also not really my idea — i discovered
> it in world machine."*
>
> *"it makes defining plains, mountains, and lakes easily trivial. so trivial to go from plains
> to mountains."*
>
> *"i wonder what procedural curves would be like. i guess that's what biomes are huh."*

Biome design that follows from it:
> *"why not just have a voronoi/noise for all your biome properties — one layer for
> precipitation, one for altitude, one for temperature... then program in biomes that have a
> range that they can spawn in, use that range as weights, **and the biomes spit out terrain
> curves, vegetation, trees, etc.** that way it's super easy to program in new ones, blend
> across biomes, store in files, and have new terrain blend with old."* (2021-01-26)

**Compared to us [verified]:** our height is five octaves of Q14 value noise with iq
gradient attenuation (`worldgen.wgsl:480-593`, `fbmAtten`), and the only remaps are
special-purpose ramps (spawn plain, pond berm, sediment wedge); **no authored curve
exists**. Biomes do exist — one low-frequency noise plus an edge-break octave, thresholded
into desert/pine/meadow with per-biome trees, cover and marsh fringe (`biomeAt`, `:613`) —
so the hook for per-biome curves is already there. A per-biome **curve** (a short
polyline in `tuning.json`, interpolated in Q14 integer arithmetic so worldgen stays
hashed and deterministic; a cosine LUT is fine) remapping the *continental* octave sum
gives exactly the plains ↔ mountains control tuned by hand today, and blends across biome
boundaries by lerping the two curves' outputs. Small, cheap, and a natural editor in the
Worldgen tab, which already draws the heightmap. Moves the world hash once.

### 8.3 Column generation with structure-aware terrain

> *"**the key is generating entire columns of the world at once.** start by generating a
> heightmap, add some 3d noise to it × some threshold — this way you can guarantee it's ± the
> heightmap by that amount — determine where structures are in the world and their size, smooth
> out the noise values based on the distance to those points."* (2020-04-22)

> *"quasi random points projected down onto the highest terrain locations. if you want to really
> get fancy you can add a strength to the terrain where the closer it is to a tree, the more
> empty it's going to be — so you don't have trees growing into an overhang. so once you know
> where the trees are, when generating a column, if you determine that a tree (or any structure)
> intersects part of that chunk, you look it up; if it doesn't exist, you generate it.*
>
> ***way easier to generate the terrain around those points than it is to do it the other way
> around**, because then you'd have to make sure this chunk and that chunk and neighbors are all
> loaded, and then go through a painfully slow growth stage fitting it to what's there."*

> *"i'm just glad i finally figured out how to do procgen while having the terrain be a
> **constant lookup at any point**, proc generated structures and having the terrain adapt to
> them ahead of time... also **no neighbor accesses**."*

**Compared to us [checked]:** the "constant lookup, no neighbour access" property is
exactly what makes `genCell` parallel-safe and deterministic, and it is what the far
sieve depends on. Against his ordering, our three structure kinds split: **ponds do it
right** — the bowl carve and berm are inside the height contract (`landColumn`,
`worldgen.wgsl:2126-2158`), and `pondInfo` refuses sites whose slope exceeds
`pondMaxSlope`; **trees fit the terrain** (`treeInfoAt` takes `land` in, `t.base =
land.h`); **ruins don't** — `worldgen.wgsl:3099-3115` stamps the shell at
`baseHeight(centre)` with no flattening, so a ruin on a slope floats on one side and is
buried on the other. Lin's ordering (the structure is known when the column is generated;
the terrain adapts) is exactly the pond code. Apply it to ruins with a pad radius and a
blend — one hash move.

### 8.4 Ray-trace the world to place the vegetation

> *"i generate a base terrain, then **ray trace every point** essentially and store
> space/sunlight and use that for spawning grass, and as an extension down the line, caves and
> such."* (2021-05-08)

> *"the last of the world gen features is working, which allows growing grass only in sunlit
> upwards-facing areas, or vines hanging downwards in skylight-reached areas, or **placing
> crystals on the walls of dark open caves**."* (2021-03-06)

> *"there's a pipeline that does a bunch of ray tracing to calculate light and space for
> spawning grass/trees/crystals on cave walls/spawning additional rocks/stalag(m/t)ites."*

**Compared to us [corrected]:** the first draft said "we already compute sunlight as a
sim input" and could reuse it. We don't have a field to reuse. The sim's sunlight is a
day-phase scalar × `seesSky`, which reads **the single cell above** (`sim_step.wgsl:229`);
the column walk was tried and broke determinism at tick 1 (`:200-228`). Worldgen has no
notion of openness at all — the only "shade" is a tree's authored canopy cover value
(`TA_S_SHADE`) driving undergrowth density.

But worldgen doesn't need the sim's field, because **`genCell` is a pure function**: at
a candidate site it may sample the column above directly — that is what the far sieve
does, and it is exactly Lin's "constant lookup, no neighbour access" property making an
upward probe legal. Surface openness = the column above the surface is clear of
tree/ruin/overhang; cave openness = a short upward walk of the cave carve function. Cost
only at candidate sites, integer, deterministic, one hash move. Then: grass only under
open sky, moss on the shadowed side of trunks and walls, mushrooms and crystals only in
dark caves, vines where skylight reaches a cave ceiling. That is most of the "small
details" of §8.1, and it composes with the openness grid of §2.4 at render time.

### 8.5 Rivers, and the answer he settled on

> *"the answer is: you don't [flow-simulate them]. you instead use something like **voronoi
> noise, place the rivers on jittered cellular boundaries**, and make the terrain height follow
> something similar."* (2020-10-08)

### 8.6 Trees — he gave up and bought them

He designed an envelope-growth / space-colonisation tree generator, talked about it for a
year, and then:

> *"speed and memory usage are one thing, writing a powerful tree gen system is another, there's
> only a handful of papers, and **seeing a tree that's slightly different than another doesn't
> really add anything to gameplay**. a walk through a detailed forest with nice lighting is just
> as good as a walk through a detailed forest made of trees that are 99% similar with only
> slight variations in their branch placements. the only compelling part is growing trees with
> an environmental response but good luck making that in realtime at a decent scale."* (2021-03-08)

He voxelised assets from TheGrove3D / PlantFactory instead.

**Compared to us:** we went the other way — `assets/editor/treegen.js` authored trees baked
to `.svtree`. Given the tuner and the bake step, we get the variety cheaply and the atlas
gives us Lin's "99% similar" instancing anyway. His warning is about *runtime* tree growth,
which we correctly do not do.

---

## 9. Audio (a whole subsystem he built in three weeks, and we have one)

Not asked for, but too directly applicable to skip.

**The technique:** path-trace a small number of rays from the listener, and use the *results*
to drive OpenAL's EFX reverb parameters — not to synthesise the signal.

> *"there's *real* audio path tracing where you trace thousands of rays and process the signal
> yourself, which is stupid, and then there's what i do, which is trace a handful of rays and
> calculate values to feed into openal's reverb."*

> *"the openal eax reverb effect... contains a few very important reverb properties: reflections
> gain/delay, late reverb gain/delay/pan, air absorption gain, source attenuation, and high/low
> frequency filters for muffling."*

> *"i've also added reflective properties to different materials. eg. grass absorbs more than
> dirt, which absorbs more than stone, which absorbs more than something like glass. so caves
> are more sensitive to reverb."*

**Two ideas we could take directly:**

1. **Wind sound placement.** *"casting rays from the player to determine if it can 'hear' the
   sky, and if not, bouncing rays until it finds a point that can hear the sky and play from
   there. in an open field it'd result in the sound being played right in front of the player's
   face; in a cave it'd make it play at the opening."* That is a genuinely lovely idea and
   costs a handful of rays.
2. **Water sound by ray count and velocity.** *"do ray tracing and calculate how many rays hit
   water, then do a mix of sounds between gentle river flowing to waterfalls, depending on the
   velocity of the water hit."*

Also worth knowing: *"the tracing is barebones, in the thousands of rays per second — for sound
anyway"*, and *"the sound processing is fast enough on the cpu."* This is not expensive.

**Compared to us [checked]:** `Cues::` is a slot-based event API over a vendored
`xyzpan` spatialiser, and more of his design is already here than the first draft said:
per-source occlusion is a **material ray** through the CPU mirror with a per-material
acoustics table (`audio/occlusion.h`), and each voice carries a **reverb send** (`wetGain`,
kept partly alive through occlusion so a muffled source still excites the room). What is
missing is the *room itself*: nothing estimates the space the listener is in, so the send
has no environment to feed. His probe — a handful of rays from the listener, weighted by
material absorption, driving reverb size/decay — is the missing half, and the openness
grid of §2.4 P0 would answer "can I hear the sky" without any rays at all.
Presentation-only, no determinism risk.

---

## 10. Process and engineering philosophy

### 10.1 "Why every voxel engine dies" (2020-06-12) — the essay

Quoted in full because it is the best thing in the corpus and it is about us:

> *"i think voxel rendering, global illumination, procedural generation, physics simulation,
> game engine development and game content development are all very different areas of computer
> science, which a lot of people don't really take into consideration. it's a miracle minecraft
> ever made it into an actual game... he also didn't get stuck up on rendering a hundred kms
> worth of view distance, and was relatively conserved with the directions he took the game.*
>
> *pretty much every indie voxel dev starts out following minecraft's footsteps — some voxel
> terrain using perlin noise, editing, maybe some voxel lighting, water if they're lucky enough
> to get to it, and then it hits a dead end. **every voxel engine dies because no one has sat
> down and figured out all 20 different elements that have to come together** in order to make
> the voxel engine every indie dev wants — planetary generation, teardown physics, finite
> fluids, accurate global illumination, rendering billions of voxels per frame, multiplayer, to
> run on 2010 hardware, this feature, that feature. instead they all start the same way: 'what
> method can render voxels with level of detail', and hope to find answers on everything else
> down the road or think it's already solved and will be easy.*
>
> *the truth is, if you try and make 'minecraft 2.0' that has all these features, it'll be
> hacked together, ugly, run like ass, have a nightmare codebase and be a lame and boring game
> that in the end brings nothing new to the table because there's no true innovation and it's
> overly ambitious even for the most seasoned scholar/programmer/game developer.*
>
> *anyway that's my ted talk on why every voxel engine dies."*

### 10.2 Build top-down, not bottom-up

> *"Try to write your engine starting from the **top down**, vs bottom up. Eg. say you have
> trees — write a tree generator or loader in the beginning and get them on the screen asap. Or
> rigid body physics, just add them to the game. **You'll learn what works and doesn't work this
> way** rather than trying to sit down and design the entire engine on paper and then implementing
> every system perfectly from the ground up. The two engines I've gotten the furthest on used
> this method."* (2020-09-27)

> *"the more i develop my engine, the more i see it becoming a general engine like unreal/unity,
> just tailored to voxels, vs this whole idea i had at the start of 'i'm going to reinvent
> literally everything and need to start from scratch'."*

> *"writing high performing code is easy. **writing flexible, organized and scaling code is
> extremely challenging** and making it fast on top of that is even harder."*

> *"Designs are way more important than code. Code barely means anything. I'd rather read a
> white paper than just browse a github repo."*

### 10.3 Publish to debug

> *"posting this here since it **helps me find bugs instantly 90% of the time**"* — he
> screenshots the bug to Discord and the act of describing it solves it.

Also, the practical debugging note we've been bitten by:

> *"heh my game doesn't even run in debug mode... noise and lots of large array work is just so
> slow, plus i use stl containers a lot. i have to run in release and turn optimization off
> per-function if i want to debug"* — `#pragma optimize("", off)` — *"and forgetting about it"*
> is the trap. He later found *"my collision detection was running in debug mode"* after weeks.

**Compared to us:** `gotcha-verify-the-binary-you-measure` is the same scar. His version
(per-function optimisation disable, left on) is a variant we should watch for in any
`#pragma`/`#if 0` that survives a session.

### 10.4 On tools and dependencies

> *"i try to avoid writing any code that i don't have to. if i sat and wrote everything, like the
> convex hull calculator for that little mesh empty space skipping concept, god that would've
> taken forever."*

> *"well have fun writing your own image loading lib + model loading lib + noise generation lib +
> socket lib + convex hull calculator lib since you want physics + audio loading lib..."*

He nonetheless recommends **ImGui as a vector graphics library**, not just a UI library:
> *"imgui is 2 libraries in 1 — a vector graphics library, and a UI library... you have access to
> that vector part and so you can use it to draw all the shapes you want and handle the UI part
> yourself. **my game's UI uses it and it looks nothing like imgui.**"* Via
> `ImGui::GetWindowDrawList()` and `AddRectFilledHq`/`AddRect`/paths, with
> `SetNextWindowBgAlpha(0)` + `NoTitleBar|NoResize|NoMove` and invisible buttons for hit-testing.

**Compared to us:** we use ImGui for the overlay and a browser tuner for authoring. If we
ever want in-game UI beyond debug, this is the path that avoids adding a dependency.

---

## 11. Twelve things worth doing, ranked by leverage (re-ranked 2026-09-01)

The first draft's list is kept below with its fate; the ranking that survived the review
is in §13, with costs and verification commands. Of the original twelve: four were
**already done or refuted** (3 partly, 4, 9, 10), one was **wrong as stated** (5 — MPM
already does it; the CA version must be render-only), one was **misfiled** (2 — the
skip is refuted, the *terminate* variant is a small experiment), and the rest stand
with corrections.

1. **Irradiance grid + final gather (§2.4).** Stands, with the design corrected: six
   face values per 4³ block, injected from the shadow resolve pass, cone gather, rolling
   refresh for a moving sun. Phase 0 is a sky-openness grid. → §13.4.
2. **LOD the secondary rays (§3.3).** *Skipping* at 4³ was built and refuted;
   *terminating* on a set blocker bit past a distance is a one-branch experiment in
   `shadowMarch`. Modest gain now that the cache dedups rays 7x. → §13.2.
3. **Dynamic-index audit (§1.1).** The constant-trip arrays are fine; the `[axis]`
   component accesses are the real candidates, and only a register/spill readout
   (`VK_KHR_pipeline_executable_properties`) can say which. → §13.1 (`--shader-stats`).
4. **Page-pool fragmentation metric (§4.2).** **Dropped** — no mechanism at one page per
   chunk. Replaced by a `--soak` creep harness that catches the whole class. → §13.6.
5. **Spray from high-velocity surfaces (§6.4).** MPM already spawns it (sim particles,
   hashed on landing). The CA waterfall version must be **render-only**. → §13.3.
6. **Per-biome terrain curve (§8.2).** Stands; biomes already exist to hang it on;
   integer Q14 interpolation. → §13.5.
7. **Openness as a worldgen placement predicate (§8.4).** Stands, but not by reusing the
   sim's sunlight (a one-cell probe); sample the pure `genCell` column at candidate sites.
   → §13.5.
8. **Wind and water audio from ray probes (§9).** Stands. → §13.7.
9. **`mix(tmin, tmax, 0.5)` flooring (§2.10).** **Already handled** — the DDA carries the
   cell and the chunk-skip landing forces the exit axis. Nothing to do.
10. **Stamp instead of clear (§1.4).** **Audited, nothing found** — ~1.3 MiB of per-tick
    fills, microseconds; the shadow cache already stamps.
11. **Distance-widened blur for roughness (§2.8).** Stands, deferred until glossy
    reflections exist. → §13.4.
12. **Don't extend MPM to powders (§6.6).** Stands. Written down so nobody re-derives it.

Not on the first draft's list and now above most of it: the **two remaining inlined
`trace()` copies** in the fragment shader (§1.8, ~3.5 ms measured), the **per-step
shadow ray in underwater god rays** (§2.7, Lin's own 100x case), and the **dusk /
submerged arms** missing from `--render-budget`.

---

## 12. Where he is wrong, or wrong *for us*

Worth stating explicitly so this document isn't read as scripture.

- **"Cellular automata water has no soul."** True of pure level-averaging CA; not true of a CA
  with a fullness fraction, stamped substeps, and an MPM momentum seam. He never tried the
  hybrid. And he concedes CA gets incompressibility free — the exact thing his chosen method
  spends all its budget on.
- **"Memory should be the last thing a design is based on."** He then spent 14 months fighting
  memory. For us it is a *rule* (rule 2 and the page pool), not a late optimisation, because
  our costs must scale with activity rather than world size.
- **No determinism constraint.** He uses non-atomic racy accumulation (*"i just add without
  atomic ops and hope for the best"*), GPU-order-dependent blends, and float sim throughout.
  Several of his best tricks are simply closed to us. When a Lin technique looks free, check
  whether it is order-dependent before believing it.
- **"Octrees are extremely terrible for ray tracing."** This is a conclusion about *his* access
  pattern (a per-frame-mutating world with fine voxels and a fat attribute payload), reached
  after also concluding the opposite twice. Take the *reasoning* — update cost dominates,
  register pressure dominates, memory latency dominates — not the verdict.
- **He never shipped.** The corpus ends in May 2021 with an engine rewrite in progress, and
  voxely.net has been dormant since 2023. His own essay in §10.1 is, read one way, a prophecy
  about himself. The lesson CLAUDE.md already encodes — bound every emergent process, gate
  every invariant, keep the thing runnable — is the discipline he describes wanting and never
  quite imposed.
- **His world does not evolve without input.** Terrain is static between edits; only
  fluids and rigid bodies move, and both are small, bounded sets. That is why op-based
  saves work for him (§4.3), why "re-light where matter changed" is enough for his GI and
  not for ours (§2.4, the sun moves and the CA moves), and why his rendering numbers are
  measured against a world that never wakes up under the camera. Every one of his costs
  should be read as a floor for us, not an estimate.
- **"Colouring is slow."** True of CPU thread scheduling between colour groups; false of
  27 GPU dispatches over a compacted list, which is what our determinism rests on (§6.5).
  The transferable part is the padded-chunk shape as a *future* redesign, not a verdict
  on the present one.
- **His hardware and API era.** Vulkan 1.1, GTX 1080 → RTX 2080 Ti → 3090, GLSL. The
  buffer-vs-image and host-visible traps (§1.2, §1.3) were real for him and are
  already absent here; the Ampere integer note (§1.6) is about a datapath he was
  reading a whitepaper about, not a measurement. Take the *reasoning* — memory layout,
  latency, register pressure — and re-measure every number on the 3060 Ti with
  `--render-budget` before believing it.

---

## 13. To do — what this review says the engine should try next

Each item: what, why (with the measurement it rests on), cost, hash impact, and the ONE
command that verifies it. "Clear win" means the mechanism is verified in our code and the
gain is measured or arithmetic; "experiment" means it may be refuted and is cheap enough
to refute. Nothing here needs a C++ rebuild unless it says so. Items that touch a file
under another session's board claim (`shadow_resolve.wgsl`, `raymarch.wgsl`,
`common.wgsl`, `perfsuite.cpp` are claimed by the shadow-cache work as of 2026-09-01)
wait for that to land.

### 13.1 Performance — clear wins

1. **Delete the reflection and refraction `trace()` call sites from `fs`.** The shadow
   half of the register-footprint finding is being fixed by `shadow_resolve`; the other
   half — `traceReflection` (`raymarch.wgsl:3127`) and `traceRefraction` (`:5930`) — is
   measured at ~3.5 ms of the 14.66 ms frame (`noreflect` 3.58 ms vs `reflgate` 0.09 ms:
   the traversal is nothing, the footprint is everything). Fix: a `traceOpaque()` that
   returns `(hit, t, cell, axis, sgn, mat)` and carries none of `Hit`'s media, micro or
   water fields; both call sites and `shadeSecondaryHit` move to it. Cost: a day. Hash:
   none. Verify: `--render-budget`, the `noreflect`−`reflgate` gap must close to <0.5 ms.
   *(WGSL only; waits for the shadow-cache claim on `raymarch.wgsl`.)*
2. **`--shader-stats`: per-pipeline register/spill readout.** Enable
   `VK_KHR_pipeline_executable_properties` at device creation (it is an optional
   extension; NVIDIA and AMD expose it) and, behind a flag, print each pipeline's
   executable statistics (registers, local-memory bytes, spill stores/loads) and record
   them in `last_run.json`. This is the instrument §1.1 needs and the one that would have
   caught the 10.8x by-value-uniform bug in a single run. Cost: half a day, C++ in
   `rhi_vulkan.cpp` (rebuild). Hash: none. Verify: run it once and read the line for
   `raymarch` — if `trace()` spills, it says so.
3. **Then, and only then, the `[axis]` sites.** `d1[a1] = s1` (`voxelAO`), `n[h.axis]`,
   `hp[a1]`, `nn[a1] +=`, `rd[axis]`, `nLocal[axis]` (`microbody.wgsl`) — rewrite as
   `select` chains only where item 2 shows local memory. Cost: hours. Hash: none.
   Verify: `--shader-stats` delta, then `--render-budget`.
4. **Underwater god rays: stop casting a `trace()` per march step.** `godRays`
   (`raymarch.wgsl:3347`) is Lin's 16 ms case verbatim. Cheapest correct replacement for
   the near-vertical sun: a **per-column highest-blocker height** over the window (one u16
   per (x,z) column = 512 KiB), rebuilt over dirty chunks in `sim_occupancy`'s walk, sampled
   per step instead of traced. Cost: a day. Hash: none (render data). Verify: add a
   submerged camera arm to `--render-budget` first (there is none), then measure.
5. **Dusk and submerged arms in `--render-budget`.** `FindNoonTick()` is the only sun
   angle measured, and no arm is under water. Shadow rays near the horizon cross ~2x the
   chunks; god rays only exist under water. Cost: an hour in `perfsuite.cpp` (rebuild).
   Hash: none. Verify: the harness prints two more cameras.

### 13.2 Performance — experiments (cheap to refute)

1. **Coarse termination for distant shadow rays** (§3.3). In `shadowMarch`, once
   `tCur > D` (say 8 m), treat a set 4³ **blocker** bit as a hit instead of stepping fine
   cells inside the block. Not the refuted *skip*; this deletes the steps inside occupied
   blocks and makes canopy opaque to secondary rays. Cost: one branch. Hash: none. Verify:
   `--render-budget` shadow arms; the `shadow-cache` gate must still pass inside D. Expect a
   modest number — the cache already cut ray count ~7x.
2. **A conservative "any blocker" bit per cascade cell** (§3.4), beside the point-sampled
   material byte, read by `farShadowed` and the cascade hit test. Thin walls stop
   vanishing at level ≥2; fences read as walls at distance. Cost: a day across
   `worldgen.wgsl far/fardown` and `raymarch.wgsl`. Hash: none. Verify: `far-fog` gate
   plus a `--shot` of a ruin at 300 m.
3. **Half-resolution primary with depth-aware upsample.** `halfres` saved 10.2 of
   14.66 ms — cost is cleanly pixel-linear — and Lin got 260 fps from a quarter-rate
   checkerboard. Needs the G-buffer split DESIGN §9 *describes* but `fs` does not
   implement (shading is inline at `raymarch.wgsl:6416/6548`), plus a history buffer
   (§2.6). Cost: a week; a real architecture change. Hash: none. Verify: `--render-budget`
   `halfres` arm becomes the default and a `--shot` diff against full-res.
4. **Shared-memory tiled CA with halo reconciliation** (§6.5). One workgroup per chunk,
   27 colours in workgroup memory, a border pass whose outcome is a pure function of
   pre-tick state. Replaces 54 serialised dispatches with 2. Cost: weeks, and a
   determinism proof — the result either hashes identically or is a rule-1 hash move
   with the twice-run gate still green. Verify: `--selftest --gate determinism` twice-run,
   `--perf` CA row on a 200-chunk active set. **Only structural lever left on CA tick
   cost; do not start it for less than a measured CA-bound scenario.**
5. **Multi-region upload copies** (§1.3). Not now — the inline threshold covers every
   per-tick write today. Trigger: `kFetchPerTick` or the page size crossing 64 KiB.

### 13.3 Visuals — clear wins

1. **Render-only waterfall spray and mist for CA liquids** (§6.4). A liquid cell with air
   below and open sides is a falling column; draw procedural mist around it in the
   fragment shader (the `siltMotes` pattern: noise field, thresholded, time-animated).
   Cannot touch the hash. Cost: a day. Verify: `--shot` of a pond overflow; no gate.
2. **Ruins flatten their pad** (§8.3). Apply the pond's structure-adapts-terrain ordering
   to `worldgen.wgsl:3099-3115`: pad radius, height blend, site refused above a slope.
   Cost: hours. Hash: moves once — `--selftest --rebaseline`. Verify: the Worldgen tab's
   voxel view (`check_worldview.sh`) on a hillside ruin.
3. **Per-biome height curve** (§8.2). A polyline per biome in `tuning.json` remapping the
   continental octave in Q14 integer interpolation, lerped across biome boundaries, edited
   in the Worldgen tab. Cost: a day. Hash: moves once. Verify: `--sweep` on a curve knot
   proves reach; `--heightmap` shows the shape.
4. **Openness-driven placement** (§8.4). Sample the pure `genCell` column at candidate
   sites: grass only under open sky, moss on shaded faces, mushrooms/crystals in dark
   caves, vines under lit cave ceilings. Cost: two days. Hash: moves once. Verify:
   `--voxdump` a cave box and count placements by openness class.
5. **Zero-jitter test** (§2.3). Set the palette jitter amplitude to zero and look. Costs
   nothing; tells whether variety is doing lighting's job. If the world reads flat, that is
   the argument for 13.4.

### 13.4 Visuals — the big one: indirect light

Write `docs/PLAN_gi.md` from §2.4 with these phases, each judged by eye and by
`--render-budget` before the next starts:

- **P0 — openness grid.** Six face values per 4³ block (sparse over surface-bearing
  blocks), sky visibility only, written by the shadow resolve pass with a short upward
  march for each patch it already touches, plus a rolling world-space refresh. Replaces the
  `n.y` lerp in `ambientAt`. Caves go dark, overhangs shade, interiors dim. Render data,
  no hash.
- **P1 — direct injection + one-bounce gather.** The resolve pass deposits
  `albedo × sunlight` into the patch's block-face; the primary hit gathers 3–4 blocks
  along the normal hemisphere. Colour bleed, window fill light.
- **P2 — write-back.** The gathered result feeds the receiver's own block-face at low
  weight: multi-bounce over frames, his "unlimited bounces" trick. Needs the rolling
  refresh to be sun-aware (the sun moves; the grid ages even when the world is idle).
- **P3 — emissives.** Lava and fire inject from the `sim_occupancy` dirty walk; `heatSpill`
  becomes a special case and is deleted.
- **Later:** distance-widened blur roughness (§2.8) once anything glossy exists; a
  history buffer (§2.6) the moment any of the above is stochastic.

### 13.5 Worldgen and content

Covered by 13.3 items 2–4. One more from §8.5: **rivers on jittered Voronoi boundaries**
with the height following the same field, instead of flow simulation. Fits `biomeAt`'s
noise machinery; a hash move. Park until the water-bodies `currentMode` decision lands.

### 13.6 Instrumentation and process

1. **`--soak N`** (§4.2): the adversarial flight for N minutes, logging p50/p99 frame time,
   resident pages, retire-queue depth, dirty count and shadow-request overflow per 1,000
   frames to `last_run.json`; gate asserts no monotonic slope. Catches every session-length
   creep at once instead of one hypothesis per run. Cost: a day (rebuild).
2. **`--shader-stats`** (13.1.2).
3. **Fix DESIGN §9's pipeline bullet.** It describes "fullscreen ray pass → G-buffer →
   deferred lighting"; the engine is one fragment shader with shading inline. Either
   correct the doc (an hour, needs the `DESIGN.md` board claim) or build the split
   (13.2.3). The gap matters because every pass-splitting argument in this document
   assumes the doc's version exists.
4. **Persist the MutationQueue as a save sidecar** (§4.3): ops since the last snapshot.
   Prerequisite for the network stream and the replay log; not a replacement for the
   snapshot.

### 13.7 Audio (presentation-only, no hash)

1. **Wind at the cave mouth** (§9). A handful of rays from the listener; if none reach the
   sky, bounce until one does and place the wind emitter there. Once P0 of 13.4 exists,
   the openness grid answers "can I hear the sky" without rays.
2. **A room estimate for the reverb send that already exists.** `audio/occlusion.h`
   has per-material acoustics and a per-voice `wetGain`; nothing sets the room. Trace a
   few rays from the listener, weight by material absorption (grass > dirt > stone >
   glass), and derive size/decay for a single environment reverb. Caves get long tails,
   meadows none.
3. **Water sound by ray count.** Count rays hitting liquid; blend gentle → torrent by
   whether the hit column is falling (the same predicate as 13.3.1).

---

## 14. In plain words — what each recommended change should do

One line each, no jargon, in the order §13 lists them. If a change doesn't do the thing
in its line, it isn't done.

**Performance — clear wins (13.1)**

1. *Move reflections out of the main shader.* Makes every pixel cheaper to draw, even pixels with no reflection in them, because the shader stops carrying the baggage for reflections it isn't doing.
2. *Shader stats readout.* Tells us, in one line per shader, whether the GPU is running out of fast registers and falling back to slow memory. Right now we guess.
3. *Fix the axis-indexed vector writes.* If the readout says a shader is spilling, this removes the spill. Free speed with zero visual change.
4. *Stop tracing a full ray for every step of underwater light shafts.* Makes underwater scenes many times cheaper to draw by looking the answer up instead of re-searching for it.
5. *Add sunset and underwater cameras to the benchmark.* Makes sure we measure the expensive cases too, not only the easy noon-on-a-hill one.

**Performance — experiments (13.2)**

1. *Distant shadow rays stop at coarse blocks.* Makes shadows through trees and grass cheaper and calmer by treating a leafy block as solid once it's far enough away.
2. *"Something is here" bit for far-away blocks.* Stops thin walls disappearing in the distance, and lets them cast shadows there.
3. *Render at half resolution and upscale smartly.* Could cut the frame cost by more than half, but needs the renderer split into two stages first.
4. *Simulate each chunk in one workgroup instead of 54 small passes.* Makes the sand simulation cheaper when only a few chunks are active. Big, risky, only worth it once we prove the sim is the bottleneck.
5. *Batch uploads into one copy.* Not needed yet; noted for when uploads get bigger.

**Visuals — clear wins (13.3)**

1. *Mist and spray on waterfalls.* Makes falling water look like falling water, drawn on top with no effect on the simulation.
2. *Ruins flatten the ground under them.* Stops ruins floating on one side and sinking into a hill on the other.
3. *A height curve per biome.* Lets you say "this biome is flat plains, that one is jagged mountains" with a few draggable points in the tuner instead of hand-tuning noise numbers.
4. *Place plants by how open the sky is.* Grass only where the sun reaches, moss on the shady side, mushrooms and crystals only in dark caves.
5. *Turn the colour speckle off and look.* Costs nothing, and tells us whether the world's colour variety is doing the job lighting should be doing.

**Visuals — indirect light (13.4)**

- *Phase 0, openness.* Caves get dark, overhangs get shade, rooms get dim. The engine finally knows how much sky each surface can see.
- *Phase 1, one bounce.* Sunlight hitting green grass tints the rock next to it green. A window lights up the room it looks into.
- *Phase 2, write-back.* Bounced light keeps bouncing over a few frames, so rooms fill in naturally instead of one hard bounce.
- *Phase 3, glowing things.* Lava and fire light up their surroundings properly instead of the current fake "warm the nearby rock" trick.
- *Later.* Blurry reflections for rough surfaces, done cheaply; and a frame-to-frame smoothing buffer once anything starts to look noisy.

**Worldgen and content (13.5)**

- *Rivers along the cracks between biome cells.* Rivers that look placed by geography instead of simulated, at almost no cost.

**Instrumentation and process (13.6)**

1. *Soak test.* Flies around for a long time and complains if the game gets slowly slower. Catches the kind of bug no short test can see.
2. *Shader stats.* Same as 13.1.2.
3. *Fix the design doc's renderer description.* The doc describes a two-stage renderer we don't have. Either fix the doc or build the stage.
4. *Save the edit log next to the save file.* Lets us replay or stream what the player did, without replacing the normal save.

**Audio (13.7)**

1. *Wind plays from the cave mouth.* In the open, wind is all around you; inside a cave, it comes from the entrance.
2. *Rooms get echo.* Caves sound big and stony, fields sound dry, using the reverb the audio engine already has but never sets.
3. *Water sounds by how much water you can "see".* A gentle stream and a waterfall sound different, and louder the more of it is around you.

---

## Appendix — the extracted corpus

Everything is under `docs/refs/lin/`:

| File | Contents |
|---|---|
| `lin_Programming_fluids.txt` | 641 msgs, full conversational context — **the richest single file** |
| `lin_Programming_raytracing.txt` | 388 msgs |
| `lin_Programming_physics.txt` | 385 msgs |
| `lin_Programming_lighting.txt` | 263 msgs |
| `lin_Programming_procedural_generation.txt` | 214 msgs |
| `lin_Programming_graphics.txt` | 207 msgs |
| `lin_Programming_meshing.txt` | 114 msgs |
| `lin_Programming_data_storage.txt` | 94 msgs (ESVO / SVDAG / two-level AS) |
| `lin_Programming_programming.txt`, `lin_Programming_math.txt`, `lin_Programming_networking.txt` | small |
| `L01..L05.txt` | `#voxels` main channel, his messages ≥150 chars with one line of context — 2,896 entries, chronological, the dev log |
| `_short.txt` | `#voxels`, 45–150 char messages, 2,200 entries — grep target for terse claims |

Source: `F:\discord scrape\txt\VoxelGameDev.com - *.txt`. Regeneration scripts were ad-hoc;
the extraction rule was: parse `[date time] author` headers, keep messages authored by
`lin5159`, strip Discord embed/attachment/reaction blocks and bare URLs, merge consecutive
runs with `||`, and keep one preceding message as context.
