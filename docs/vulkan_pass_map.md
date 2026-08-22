# GPU pass / resource dependency map (Vulkan port seed)

Surveyed 2026-08-22 from the live tree (post limb-library merge). This is the
input document for `docs/vulkan_barrier_graph.md`: every producer→consumer edge
Dawn currently synchronizes automatically, which a Vulkan backend must
synchronize by hand. A missed edge here is a timing-dependent determinism bug
later, so treat edits to the tick path as edits to this file (and vice versa).

Buffer names are the C++ member names in `src/sim/world.h` (labels in
`World::Init` match).

---

## 1. TICK PASS CHAIN

**Entry point:** `Simulation::EncodeTick` (`src/sim/simulation.cpp:614-765`).
Driver: `sandvox::SubmitTick` (`src/test/support.cpp:104-173`). **One
`CommandEncoder`, one `Submit` per tick.** Encoder contains, in order:
EncodeTick → EncodeFarFill → EncodeReadbacks → EncodeDirtyCopy.

All shaders are `LoadShader(device, dir, name)` = `ShaderConstantPrelude() +
TuningWgslBlock() + common.wgsl + <body>` (`src/gpu/resources.cpp:89-114`).

### 1.0 — Pre-pass `ClearBuffer` ops (outside any pass, `simulation.cpp:617-621`)
| Clear | Condition |
|---|---|
| `dirty[1 - page_]` (dirtyOut) | every tick |
| `argsStage` (12 B) | every tick |
| `claim` (1 MiB) | `particlesActive` |
| `expMask` (~2.2 MiB) | `expCount > 0` |
| `hash` (16 B) | `hashEnable` |

### 1.1 — PASS A: "prep" (one `ComputePassEncoder`, `simulation.cpp:627-659`)
Multiple dispatches, no explicit barriers between them — Dawn inserts them.
Order inside the pass matters (RAW on `voxels`, `dirtyIn`, `counts`).

| # | Pipeline | WGSL | Entry | Dispatch | WG size | Cond. | What |
|---|---|---|---|---|---|---|---|
| A1 | `mutate_` | `sim_mutate.wgsl` | `main` | Direct `(4*opsCount,4,4)` | `(4,4,4)` | `opsCount > 0` | Brush ops into `voxels`, marks both dirty pages |
| A2 | `mutateCells_` | `sim_mutate.wgsl` | `cells` | Direct `((cellCount+63)/64,1,1)` | `(64)` | `cellCount > 0` | Exact-cell ops (island removal, prefabs, rubble) |
| A3 | `explodeMark_` | `sim_explode.wgsl` | `mark` | Direct `(11*expCount,11,11)` | `(4,4,4)` | `expCount > 0` | Pure read of `voxels` + occlusion trace; writes `expMask` |
| A4 | `explodeApply_` | `sim_explode.wgsl` | `apply` | Direct `(11*expCount,11,11)` | `(4,4,4)` | `expCount > 0` | Reads `expMask`, zeroes `voxels`, atomicAdd `counts[page]`, appends ejecta to `particles[page]` |
| A5 | `pSpawn_` | `sim_particle.wgsl` | `spawn` | Direct `((spawnCount+63)/64,1,1)` | `(64)` | `spawnCount > 0` | CPU shatter spawns appended to `particles[page]` |
| A6 | `compact_` | `sim_compact.wgsl` | `main` | Direct `(64,1,1)` | `(64)` | always | Compacts `dirty[page]` → `dirtyList`, count into `argsStage` |

`kExplosionWg = 11` (`world.h:123`) = `EXP_WG` in `common.wgsl`. A3/A4 use
`simPL2_` (slim group 0 + particleBGL_) while A1/A2/A6 use `simPL_` — bind
group swap mid-pass.

### 1.2 — Copy: `argsStage → dispatchArgs` (`simulation.cpp:664`, 12 B)
Outside the pass. The compaction→indirect staging hop: a buffer used as
`Indirect` must not be bound in any bind group of the same pass (WebGPU rule).

### 1.3 — PASS B: the 27-color × 2-substep CA (`simulation.cpp:667-679`)
- ONE `ComputePassEncoder`, ONE `SetPipeline(step_)`.
- **54 iterations**, each: `SetBindGroup(0, simBG_[page_], 1, &offset)` with
  `offset = k * 256` (`kPassStride = 256`) then
  `DispatchWorkgroupsIndirect(world_->dispatchArgs, 0)`.
- `passUBO` is 54 slices × 256 B, uploaded once at Init; slice k holds
  `{colorPhase.xyz, substep}` with `c = k % 27`, `substep = k / 27`.
- Shader `sim_step.wgsl:main`, `@workgroup_size(6,6,6)` (216 threads; one cell
  of the current color per thread in a 16³ chunk). One workgroup per dirty chunk.

**Vulkan implication:** the 54 dispatches are sequentially dependent — pass k+1
reads voxels pass k wrote. Each needs a full compute→compute barrier on
`voxels`, `dirtyOut`, `supportFlags`. `dispatchArgs` needs
`INDIRECT_COMMAND_READ` visibility once before the loop, not per iteration.

`sim_step.wgsl:25-27` deliberately does not declare binding 13 so that
`dispatchArgs` stays outside the pass usage scope — a WebGPU-only constraint,
but the staging copy stays for ordering correctness in v1.

### 1.4 — Particle chain (`simulation.cpp:684-714`, `if (particlesActive)`)
Runs after the CA so flights see settled ground.

| # | Pipeline | Entry | Dispatch | Pass |
|---|---|---|---|---|
| C1 | `pArgs1_` | `args1` | Direct `(1,1,1)` | own pass |
| — | copy `pArgsStage[16..28) → pDispatchArgs[0..12)` (`:693`) | | | |
| C2 | `pIntegrate_` | `integrate` | Indirect `pDispatchArgs` | pass 2 |
| C3 | `pArgs2_` | `args2` | Direct `(1,1,1)` | same pass 2 |
| — | copies `pArgsStage[16..28) → pDispatchArgs`, `pArgsStage[0..16) → drawArgs` (`:704-705`) | | | |
| C4 | `pResolve_` | `resolve` | Indirect `pDispatchArgs` | pass 3 |

`pArgs` layout (`sim_particle.wgsl:60-62`): `[0..3]` = indirect draw args
`{36, instances, 0, 0}`; `[4..6]` = indirect dispatch args `{groups,1,1}`.
- `args1` sizes integrate from `counts[page]` (read page).
- `args2` sizes resolve from `counts[1-page]` (write page).
- `integrate`: reads `particles[page]`, appends `particles[1-page]`, atomicMax
  `claim`, reads `voxels`.
- `resolve`: r/w `particles[1-page]`, reads `claim`, **writes `voxels`**
  (reinsertion + stain), marks `dirtyOut`.

### 1.5 — PASS D: occupancy / hash / pick — branch on `hashEnable` (`tick % 15 == 0`, `main.cpp:2359`)

**Hash tick (`simulation.cpp:716-724`, one pass):**
| # | Pipeline | Entry | Dispatch |
|---|---|---|---|
| D1 | `occupancy_` | `main` | Direct `(4096,1,1)` — whole-world scan, rewrites all `occupancy`, atomicAdd into `hash` |
| D2 | `pick_` | `main` | Direct `(1,1,1)` — DDA down `renderUBO` camera ray → `pick` |

**Dirty tick (`simulation.cpp:728-763`):**
| # | Step |
|---|---|
| — | `ClearBuffer(argsStage)` |
| D3 | `compactNext_` (`sim_compact:mainNext`) own pass — compacts dirtyOut → `dirtyList` + `argsStage` |
| — | copy `argsStage → dispatchArgs` (12 B) |
| D4 | `occupancyDirty_` (`sim_occupancy:mainDirty`) Indirect `dispatchArgs` — pass 2 |
| D5 | `pick_` — same pass 2 |
| D6 | `farDown_` (`worldgen.wgsl:fardown`) Indirect `dispatchArgs` (same args) — **own pass** (`farPL_` layout) |

Hash ticks skip fardown entirely (documented one-tick-late far propagation).

### 1.6 — Far-field fill (`Simulation::EncodeFarFill`, `simulation.cpp:570-578`)
`farFill_` (`worldgen.wgsl:far`), direct `(count,1,1)`, `count ≤ kFarListCap =
4096`, own pass, `farPL_`. Writes `farVox` (atomicStore) + `farOcc`.

### 1.7 — Non-tick / conditional encoders (separate Submits)
| Function | Passes | Trigger |
|---|---|---|
| `EncodeWorldgen` (`:542-558`) | 7 ClearBuffers + `worldgen_:main` direct `(4096,1,1)` | startup, `--shot`, regen |
| `EncodeGenList` (`:560-568`) | `worldgenList_:list` direct `(count,1,1)` | streaming shift — from `Stream::FillSlots` (`stream.cpp:274-278`), own encoder+Submit, mid-frame |
| `EncodeLoadReset` (`:580-593`) | 5 ClearBuffers + `occupancy_:main` `(4096,1,1)` | `LoadWorld` |
| `EncodeHashOnly` (`:595-603`) | ClearBuffer(hash) + `occupancy_:main` `(4096,1,1)` | selftest `HashWorldNow` |
| `EncodeWakeAll` (`:605-612`) | no pass — bare `queue.WriteBuffer(dirty[page_], ones, 16 KB)` | day/night gate crossing |

---

## 2. RENDER PASS CHAIN

One `RenderPassEncoder` per frame — `Simulation::BeginRenderPass`
(`simulation.cpp:903-925`). Color = swapchain view, Clear `{0.1,0.15,0.25,1}`;
depth = `Depth32Float` RenderAttachment, Clear `depthClearValue = 0.0f`
(**reversed-Z**). Frame order (`main.cpp:2813-2825`):

| # | Method | Pipeline | WGSL vs/fs | Draw | Depth | Notes |
|---|---|---|---|---|---|---|
| R1 | `DrawWorld` | `raymarch_` | `raymarch.wgsl` vs/fs | `Draw(3)` | write on, Always | writes `frag_depth`; `renderBG_` + `renderPartBG_[page_]` |
| R2 | `DrawParticles` | `particleDraw_` | `debris.wgsl` vsParticle | **`DrawIndirect(drawArgs, 0)`** | GreaterEqual | |
| R3 | `DrawBodies` | `bodyDraw_` | vsBody | `Draw(36, n)` | GreaterEqual | CPU-known count |
| R4 | `DrawMicroBodies` | `microBodyDraw_` | `microbody.wgsl` | `Draw(36, n)` | GreaterEqual, **CullMode::Front** | ⚠️ does `queue.WriteBuffer(mbInstBuf_)` **inside the open render pass** (`simulation.cpp:976`) |
| R5 | `DrawSprites` | `spriteDraw_` | vsSprite | `Draw(36, n)` | GreaterEqual | |
| R6 | `DrawDebugBoxes` | `debugBoxDraw_` | `debug_lines.wgsl` | `Draw(72, n)` | no test/write, alpha blend | |
| R7 | `Overlay::Render` | ImGui | — | `ImGui_ImplWGPU_RenderDrawData(..., pass.Get())` | ImGui's own | shares the pass; only `.Get()` use in tree |

No compute passes in the frame path — all compute is on the tick path.
Headless: `--shot` = R1 only; `--shot-mob` = R1+R3+R4 + CopyTextureToBuffer;
selftest render gate = R1+R2.

---

## 3. BUFFER INVENTORY

### 3a. `World::Init` (`src/sim/world.cpp:25-90`)
`kWorldN=512`, `kNumChunks=4096` (per compaction word-scan; 32768 chunks total),
`kVoxelCount=512³`, `kParticleCap=262144`, `kFarLevels=8`, `kFarN=256`.

| Buffer | Size | Usage | Notes |
|---|---|---|---|
| `voxels` | **512 MiB** | Storage \| CopySrc \| CopyDst | ★ the world |
| `dirty[0]`, `dirty[1]` | 16 KiB ea | Storage \| CopySrc \| CopyDst | |
| `dirtyList` | 16 KiB | Storage | compaction output |
| `argsStage` | 12 B | Storage \| CopySrc \| CopyDst | staging half of indirect pair |
| `dispatchArgs` | 12 B | **Indirect \| CopyDst** | never bound |
| `occupancy` | 16 KiB | Storage \| CopySrc \| CopyDst | `(blockers<<16)\|nonAir` |
| `support` | 16 KiB | Storage \| CopySrc \| CopyDst | one-shot, cleared after readback |
| `hash` | 16 B | Storage \| CopySrc \| CopyDst | |
| `tickUBO` | 64 B | Uniform \| CopyDst | |
| `passUBO` | 54×256 B | Uniform \| CopyDst | dynamic-offset windows |
| `opsBuf` | 2 KiB | Storage \| CopyDst | |
| `renderUBO` | sizeof(RenderParams) | Uniform \| CopyDst | read by raymarch AND `sim_pick` |
| `pick` | 32 B | Storage \| CopySrc \| CopyDst | |
| `particles[0/1]` | **8 MiB ea** | Storage | |
| `particleCounts` | 16 B | Storage \| CopySrc \| CopyDst | |
| `claim` | **1 MiB** | Storage \| CopyDst | |
| `pArgsStage` | 32 B | Storage \| CopySrc | staging half |
| `pDispatchArgs` | 12 B | **Indirect \| CopyDst** | |
| `drawArgs` | 16 B | **Indirect \| CopyDst** | crosses submit boundary → DrawIndirect |
| `expOps` | 256 B | Storage \| CopyDst | |
| `expMask` | **2.10 MiB** | Storage \| CopyDst | `EXP_MASK_STRIDE=68928` |
| `cellOps` | 512 KiB | Storage \| CopyDst | |
| `spawnOps` | 128 KiB | Storage \| CopyDst | |
| `sprites` | 2 KiB | Storage \| CopyDst | |
| `debugBoxes` | 64 KiB | Storage \| CopyDst | |
| `bodyInstances` | **4 MiB** | Storage \| CopyDst | |
| `bodyXforms` | 16 KiB | Storage \| CopyDst | |
| `genList` | 16 KiB | Storage \| CopyDst | |
| `farVox` | **128 MiB** | Storage \| CopySrc | ★ CopySrc selftest-only |
| `farOcc` | 128 KiB | Storage | relied on zero-init |
| `farList` | 16 KiB | Storage \| CopyDst | |
| `farUBO` | 128 B | Uniform \| CopyDst | |
| `slots_[0..2].buf` | ~1.51 MiB ea | **MapRead \| CopyDst** | readback ring, depth 3 |

`kSlotBytes` layout (`world.cpp:11-23`): mirror 27×16 KiB @0; dirty 16 KiB;
occupancy 16 KiB; hash (256-padded); pick (256-padded); particleCounts
(256-padded); support 16 KiB; `kFetchPerTick=64` × 16 KiB chunk fetches.

### 3b. `Simulation::Init` (`simulation.cpp:20-51`)
`materialBuf_` (4096×MaterialGpu), `reactionBuf_`, `microTableBuf_` (64 KiB),
`microPoolBuf_` (**4 MiB**), `mbModelBuf_` (4 KiB), `mbPoolBuf_` (**4 MiB**),
`mbInstBuf_` (8 KiB) — all Storage | CopyDst.

### 3c. Transient
- `Stream::AcquireStaging` — `evictStaging`, 256×16 KiB = 4 MiB,
  MapRead|CopyDst, pooled (`stream.cpp:184-195`), `kMaxPendingEvicts = 4`.
- Screenshot staging (W*H*4), selftest blocking reads (up to 512 MiB voxel
  snapshots in `selftest_sim.cpp`).
- Textures: `depthTex_` (Depth32Float RenderAttachment), offscreen RGBA8
  targets per headless mode.

---

## 4. DEPENDENCY EDGES

### Bind group slot map (group 0, `simBGL_`, `simulation.cpp:79-97`)
`0=voxels 1=dirty[page] 2=dirty[1-page] 3=materials 4=tickUBO 5=passUBO(dyn)
6=opsBuf 7=occupancy 8=hash 9=pick 10=renderUBO 11=reactions 12=dirtyList
13=argsStage 14=cellOps 15=support 16=genList`

`simSlimBGL_` = bindings 0–4. `particleBGL_` (group 1) = `0=particles[page]
1=particles[1-page] 2=particleCounts 3=claim 4=pArgsStage 5=expOps 6=expMask
7=spawnOps`. `farBGL_` (group 1) = `0=farVox 1=farOcc 2=farList 3=farUBO
4=dirtyList`.

### Per-pass R/W table
| Pass | Reads | Writes |
|---|---|---|
| worldgen main/list | materials, tickUBO, (genList) | voxels, occupancy, dirty[0], dirty[1] |
| mutate main/cells | voxels, materials, tickUBO, opsBuf/cellOps | voxels, dirtyIn, dirtyOut |
| explode mark | voxels, materials, tickUBO, expOps | expMask, dirty both |
| explode apply | voxels, materials, tickUBO, expOps, expMask | voxels, dirty both, counts (atomic), particles[page] |
| particle spawn | spawnOps, tickUBO | counts (atomic), particles[page] |
| compact main | dirty[page] | dirtyList, argsStage |
| step ×54 | voxels, materials, tickUBO, passUBO, reactions, dirtyList, dispatchArgs (indirect) | voxels, dirtyOut, support |
| particle args1 | counts, tickUBO | pArgsStage[4..6] |
| particle integrate | particles[page], counts, voxels, materials, tickUBO, pDispatchArgs | particles[1-page], counts, claim |
| particle args2 | counts, tickUBO | pArgsStage[0..6] |
| particle resolve | particles[1-page], counts, claim, voxels, materials, pDispatchArgs | voxels, particles[1-page], dirtyOut |
| compact mainNext | dirty[1-page] | dirtyList, argsStage |
| occupancy mainDirty | voxels, materials, dirtyList, dispatchArgs | occupancy |
| occupancy main | voxels, materials, tickUBO | occupancy, hash (atomic) |
| pick main | voxels, materials, renderUBO | pick |
| worldgen fardown | voxels, materials, tickUBO, farUBO, dirtyList, dispatchArgs | farVox (atomic), farOcc (atomic) |
| worldgen far | materials, tickUBO, farList, farUBO | farVox, farOcc |
| raymarch | voxels, occupancy, materials, renderUBO, farVox, farOcc, farUBO, microTable, microPool | color + frag_depth |
| debris vsParticle | materials, renderUBO, particles[page], drawArgs (indirect draw) | color, depth |
| debris vsBody/vsSprite | materials, renderUBO, bodyInstances+bodyXforms / sprites | color, depth |
| microbody | materials, renderUBO, bodyXforms, mbModel, mbPool, mbInst | color, depth |
| debug_lines | materials, renderUBO, debugBoxes | color (blend) |

### RAW chains within one tick (barrier-critical)
1. **`voxels`**: mutate → cells → explodeMark(read) → explodeApply → step×54
   (k → k+1) → resolve → occupancy → pick → fardown. **~60 serial RAW hops on
   the 512 MiB buffer.**
2. `dirty[page]`: cleared/written by mutate+explode → read by compact (WAR+RAW).
3. `dirty[1-page]`: cleared → written by mutate/explode/step/resolve → read by
   compactNext.
4. `dirtyList`: compact writes → step×54 reads; compactNext writes →
   occupancyDirty + fardown read. **Two generations in one tick.**
5. `argsStage`: cleared → compact → copied out → cleared again → compactNext →
   copied out (WAW at the second clear).
6. `counts`: CPU zeroes [1-page] → apply/spawn add [page] → args1 reads [page]
   → integrate adds [1-page] → args2 reads [1-page] → resolve reads [1-page].
7. `claim`: cleared → integrate atomicMax → resolve atomicLoad.
8. `particles[1-page]`: integrate appends → resolve RMW.
9. `support`: step×54 writes → copy to readback slot → **ClearBuffer(support)**
   (`world.cpp:159-160`) — transfer-read then transfer-write, same encoder.
10. `farVox`/`farOcc`: fardown then far, same encoder, different passes (WAW,
    both atomic).

### Indirect-args chains (producer → staging → copy → indirect buffer → consumer)
| Producer | Staging | Indirect | Consumer |
|---|---|---|---|
| `compact:main` | argsStage | dispatchArgs | step ×54 |
| `compact:mainNext` | argsStage | dispatchArgs | occupancyDirty, farDown |
| `particle:args1` | pArgsStage@16 | pDispatchArgs | integrate |
| `particle:args2` | pArgsStage@16 | pDispatchArgs | resolve |
| `particle:args2` | pArgsStage@0 | drawArgs | **DrawIndirect, next frame's submit** |

No buffer is both Indirect and Storage (WebGPU rule; `world.h:591-593`). In
Vulkan the copies could collapse (STORAGE|INDIRECT is legal) but v1 keeps them
verbatim — dropping them changes barrier scopes and is a separate, hash-gated
change.

---

## 5. CPU↔GPU TRAFFIC

### 5a. `queue.WriteBuffer` sites
Per-tick, before Submit (`support.cpp:126-140`): `tickUBO` 64 B always;
`opsBuf` ≤2 KiB; `expOps` ≤256 B; `cellOps` ≤512 KiB; `spawnOps` ≤128 KiB;
`particleCounts` 4 B @(1-page)*4; `dirty[page_]` 16 KiB on day/night crossing.
Per-tick from `FarField::PrepareTick` (`farfield.cpp:101-127`): `farUBO` 128 B,
`farList` ≤16 KiB.
Per-frame (`main.cpp`): `renderUBO`; `sprites` ≤2 KiB; `debugBoxes` ≤64 KiB
(debug only); `bodyInstances` up to 4 MiB when dirty; `bodyXforms` ≤16 KiB;
`mbInstBuf_` ≤8 KiB **inside the render pass** (`simulation.cpp:976`).
Streaming (`Stream::FillSlots`, `stream.cpp:234-279`, own Submit mid-frame):
per slot `voxels` @slot*16 KiB, `occupancy` @slot*4, `dirty[0/1]` @slot*4;
`genList`; **`tickUBO` overwritten with streaming's own TickParams**
(`stream.cpp:273`) before the genList submit.
Tables: `materialBuf_` full or 128-entry partial (`SetArtPalette`,
`simulation.cpp:403` — partial write into a live buffer), `reactionBuf_`,
`microTableBuf_`/`microPoolBuf_` (hot reload), `mbModelBuf_`/`mbPoolBuf_`
(every frame when `debris.MicroDirty()`), `passUBO` once at Init.

### 5b. `MapAsync` readback sites
| Site | Buffer | Semantics |
|---|---|---|
| `World::KickReadback` (`world.cpp:170-235`) | slot ~1.51 MiB | **ring depth 3**, `AllowProcessEvents`, drained by `ProcessEvents()` at `main.cpp:2833`; skips copies when all in flight; snapshot is one-tick-latent by design (`world.h:477`) |
| `Stream::EvictSlots` (`stream.cpp:175-181`) | evictStaging ≤4 MiB | `WaitAnyOnly`, polled timeout-0; pooled ring of 4; `CompleteOldest` blocks when full |
| `ReadHashSync` (`support.cpp:217-236`) | 16 B | the one sanctioned blocking read |
| `ReadCountsSync` / `ReadActiveChunksSync` | 16 B / 16 KiB | selftest only |
| selftest voxel dumps | up to 512 MiB | blocking, selftest only |
| screenshot grabs | ≤8 MiB | blocking, headless modes |

### 5c. Readback encode order (`World::EncodeReadbacks`, `world.cpp:105-163`)
Inside the tick encoder after all sim passes: ≤64 chunk fetch copies, 27 mirror
chunk copies, occupancy/hash/pick/particleCounts/support copies, then
**ClearBuffer(support)**. Then `EncodeDirtyCopy` copies `dirty[1-page]`.
`KickReadback()` runs after Submit and after `FlipPage` (`support.cpp:170-172`).

---

## 6. IMPLICIT ORDERING ASSUMPTIONS (Vulkan breakage list)

1. Automatic inter-dispatch barriers inside one ComputePassEncoder (prep A1→A6;
   the 54-step CA loop — 53 barriers; integrate→args2; occupancy→pick).
2. Automatic barriers between passes in one encoder (every pass transition).
3. `ClearBuffer` ordering vs subsequent passes/copies; `ClearBuffer(support)`
   immediately after copying it (transfer WAW).
4. `CopyBufferToBuffer` → `DispatchWorkgroupsIndirect` visibility (5 sites) —
   `TRANSFER_WRITE → INDIRECT_COMMAND_READ` at DRAW_INDIRECT stage.
5. `queue.WriteBuffer` ordered before later Submit (all of §5a) — Vulkan needs
   staging + HOST_WRITE barriers or queue-ordered upload path.
6. `WriteBuffer` inside an open render pass (`mbInstBuf_`,
   `simulation.cpp:976`) — illegal in Vulkan; must hoist before BeginRenderPass.
7. `Stream::EvictSlots` relies on cross-submit queue order: eviction copy
   submitted before `FillSlots` rewrites the same slots (`stream.cpp:172-174`).
8. `Stream::FillSlots` overwrites `tickUBO` and submits `worldgenList` between
   ticks — correct only via queue order vs the tick submit.
9. Partial `WriteBuffer` into a live buffer (`SetArtPalette`).
10. `MapAsync` callback modes: `AllowProcessEvents` fires only in
    `ProcessEvents()`; eviction uses `WaitAnyOnly` + timeout-0 poll;
    `Slot::inFlight` is the only fence. Vulkan needs real fences/timeline
    semaphores per ring slot.
11. `GpuContext::WaitIdle` = OnSubmittedWorkDone drain — used by `--shot`,
    save/load, selftests.
12. `page_` flips on CPU after Submit with no GPU sync — correct only because
    submits serialize.
13. WebGPU-only constraints that shaped the code: binding 13 omitted from
    `sim_step.wgsl`; `simSlimBGL_` exists for the 16-storage-buffers-per-stage
    layout limit. Vulkan lifts both — but removing them changes barrier needs.
14. **Zero-initialized buffers**: `farVox`/`farOcc` rely on zero = air/sky
    (`world.cpp:71-74`). Vulkan does not zero-init — needs explicit
    `vkCmdFillBuffer` (recommendation: zero-fill ALL buffers at creation to
    match WebGPU semantics wholesale).
15. `drawArgs` written on tick N's submit, read by DrawIndirect on a later
    frame's submit (cross-submit hazard).

---

## 7. PORT SURFACE (wgpu:: usage per file outside src/gpu/)

| File | Uses |
|---|---|
| `sim/simulation.h/.cpp` (166) | the core: all pipeline/bindgroup/pass creation and encoding, PushErrorScope, dynamic offsets, ClearBuffer/CopyBufferToBuffer, render pipelines, textures |
| `sim/world.h/.cpp` (44) | Buffer ×34 members, CreateBuffer, copies, ClearBuffer, MapAsync ring |
| `sim/stream.h/.cpp` (15) | own encoder+Submit, WriteBuffer, MapAsync + Future polling |
| `sim/farfield.*` (2) | Queue::WriteBuffer only |
| `sim/worldio.cpp` (2) | encoder+Submit wrapping EncodeLoadReset |
| `main.cpp` (43) | offscreen textures, CopyTextureToBuffer, screenshot MapAsync, frame loop, WriteBuffer sites |
| `test/support.h/.cpp` (32) | SubmitTick, WriteBuffer sites, blocking readbacks |
| `test/selftest*.cpp` (~150) | staging buffers, copies, blocking MapAsync, offscreen render targets |
| `ui/overlay.*` (6) | ImGui_ImplWGPU_* — swaps to imgui_impl_vulkan; only `.Get()` use in tree |

**RHI seam sizing: ~10 concepts** — Buffer (create/write/map), CommandEncoder
(clear/copy/begin passes), ComputePassEncoder (pipeline/bindgroup+dynoffset/
dispatch/indirect), RenderPassEncoder (pipeline/bindgroup/draw/indirect),
pipeline+layout+bindgroup creation, Queue (WriteBuffer/Submit/WorkDone),
Texture/TextureView, surface acquire/present, ProcessEvents/WaitAny, Future
plumbing. `src/gpu/{context,resources}` already hold the natural hook points.

---

## 8. BUILD INTEGRATION

- `CMakeLists.txt:44-74`: Dawn FetchContent, `DAWN_ENABLE_VULKAN=ON`, all other
  backends OFF — the engine already runs on Dawn's Vulkan backend today. The
  port is Dawn-out, not API-in.
- Linked: `dawn::webgpu_dawn`, `webgpu_cpp`, `webgpu_glfw`, glfw, imgui_lib
  (`IMGUI_IMPL_WEBGPU_BACKEND_DAWN`), Jolt, nlohmann_json.
- **Tint is available as linkable libraries in `C:/sv-deps/dawn-build/src/tint/`**
  including `tint_lang_spirv_writer` — runtime WGSL→SPIR-V is a
  `target_link_libraries` change + `tint::Program` → spirv writer call. The
  prelude concatenation in `LoadShader` is the natural hook.
- `TINT_BUILD_CMD_TOOLS=ON` builds `tint.exe` for `scripts/check_shaders.sh`,
  which reproduces `ShaderConstantPrelude()` by scraping `world.h` — a
  hand-maintained mirror; any new prelude constant must be added there too.
