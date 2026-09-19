# xyzpan — vendored spatializer engine

Copied from `C:\Users\Luke\Desktop\programming\audio_webgame\engine\`
(itself a verbatim snapshot of the `xyzpan` VST plugin's engine at commit
`e5782fb3f3f17ede70bac43c2d66017373609f70`, 2026-06-10).

Copied into sandvox: 2026-08-20.

## What this is

A per-voice binaural spatializer. **One `XYZPanEngine` instance renders exactly
one mono source** — the game creates a pool of them (`src/audio/world.h`) and
each playing sound owns one for its lifetime. Signal chain per voice:

    doppler delay -> comb bank -> pinna/ear-canal EQ -> ITD/ILD binaural split
    -> chest bounce -> floor bounce -> distance gain + air absorption
    -> early reflections (image source) -> FDN reverb

It is pure C++20 with **zero third-party dependencies** — the only includes
across all 33 files are `<algorithm> <array> <atomic> <cmath> <cstdint>
<cstring> <random> <vector>`. That is an invariant worth keeping: it is what
makes the engine trivially portable and testable. Do not add a dependency here;
game concepts (voxels, materials, `World`) belong in `src/audio/*.cpp`, never
inside `xyzpan/`.

`obfuscate.h` from the upstream snapshot was DROPPED — a compile-time string
obfuscator for shipping VST license keys, referenced by nothing.

## Coordinate convention — READ THIS BEFORE TOUCHING `MakeParams`

The engine is **Z-up, Y-forward, right-handed** (Blender-style):

    x = right      y = FORWARD      z = UP

sandvox is **Y-up**, and its camera basis is **LEFT-handed**: `Camera::Right()`
is `Forward() × +Y`, so right is −X when forward is +Z (and that is genuinely
screen-right — the renderer marches `camRight * ndc.x`).

`AudioWorld::MakeParams` (`src/audio/world.cpp`) therefore maps sandvox
`(x, y_up, z)` to engine `(−x, z, y_up)` with `listenerYaw = π/2 − cam.yaw` and
`listenerPitch = cam.pitch` unchanged. Everything the engine is fed is in
**meters**, not voxels; sandvox positions are voxels, so they are multiplied by
`kVoxelMeters` at the same boundary. All of it happens in exactly one function on
purpose.

**Do not drop the minus.** Until 2026-09-19 this was a bare `(x, z, y)` swizzle —
determinant −1, so it REFLECTED the sound field instead of rotating it, and no
listener yaw can undo a reflection. Front came out of the right ear at every
yaw; the player's right came out of the centre. The pitch was negated to
"compensate", which fixed nothing and inverted elevation as well. Derivation:

    camera forward (flat)   = ( cos a, 0, sin a )        sandvox
    engine forward at yaw Y = ( -sin Y, cos Y, 0 )       engine (inverse yaw about Z)
    engine(camera forward)  = ( -cos a, sin a, 0 )
    => sin Y = cos a, cos Y = sin a  =>  Y = pi/2 - a

Azimuth is `atan2(x, y)` and elevation `atan2(z, horizontal)` (`Coordinates.cpp`),
and the listener transform applies inverse yaw about Z then inverse pitch about
X (`Engine.cpp`) — so positive yaw is counter-clockwise seen from above and
positive pitch is nose-UP, which is what sandvox's `pitch` already means.

The `audio-spatial` selftest gate renders real audio through a real engine and
asserts which ear it comes out of at twelve camera poses. Run it after ANY edit
to this conversion: `--selftest --gate audio-spatial`, no GPU, no device.

## Local modifications

Both are inherited from audio_webgame and must be re-applied if this is ever
re-synced from upstream. Grep `LOCAL MOD`.

1. **`EngineParams::floorBounceFactor`** (`Types.h`, `Engine.cpp`). Upstream
   drives the floor bounce from the listener-relative elevation angle, which is
   wrong for a game with real terrain: a sound on the ground 30 m away is at ~0°
   elevation and would get no bounce. The override (default `-1` keeps upstream
   behaviour; `[0,1]` is caller-supplied "how close to the floor is this") lets
   the game pass the source's actual height above the voxel ground.

## Upstream quirk the game works around, rather than patching

`Engine.cpp` computes the propagation-delay fraction as `clamp(dist / sqrt(3))`
— a leftover `[-1,1]`-unit-cube assumption that saturates at 1.73 m and would
cap doppler/propagation delay at that distance. Rather than fork the engine,
`MakeParams` drives `distDelayMaxMs = max(dist, sqrt(3)) * 1000/343` so the
product is exactly `dist / 343 s` at every distance. Params-only, so the engine
stays a clean vendored copy. See the comment at that assignment.

## Build

Compiled directly into the `sandvox` target (see the `src/audio/xyzpan/src/*.cpp`
entries in `CMakeLists.txt`) rather than as a separate static library — sandvox
has no other sub-libraries and one target keeps the build simple.

Upstream builds this with `/fp:fast /arch:AVX2`. **sandvox does not**, because
the whole project compiles as one target and relaxed float must not reach the
sim (CLAUDE.md rule 1 — bit-determinism). The audio path is float-heavy but
entirely presentation, so it is unconstrained by that rule; it simply doesn't
get the extra vectorization. If audio CPU ever matters, the fix is to split
`xyzpan` back into its own static library with its own flags, NOT to loosen
flags on the sandvox target.
