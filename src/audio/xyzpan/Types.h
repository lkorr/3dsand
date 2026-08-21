#pragma once
#include "xyzpan/Constants.h"

namespace xyzpan {

// Waveform selector for the dev-tool test tone oscillator.
enum class TestToneWaveform : int {
    Saw               = 0,
    Square            = 1,
    WhiteNoise        = 2,
    PulsingSaw        = 3,
    PulsingSquare     = 4,
    PulsingWhiteNoise = 5,
    StereoNoiseSaw    = 6,
    Sine              = 7,
    Click             = 8,
    Triangle          = 9,
};

// Parameters passed to the engine each processBlock.
// Grows per phase as DSP features are added.
struct EngineParams {
    // Spatial position (Phase 1)
    float x = 0.0f;  // [-1, 1]: X=1 = right, X=-1 = left
    float y = 1.0f;  // [-1, 1]: Y=1 = front, Y=-1 = behind
    float z = 0.0f;  // [-1, 1]: Z=1 = above, Z=-1 = below

    // Dev panel parameters (Phase 2 — binaural panning)
    // All have defaults from Constants.h matching physical/perceptual baselines.
    // Exposed via APVTS in the plugin layer for real-time tuning.
    float maxITD_ms         = kDefaultMaxITD_ms;      // ITD maximum in ms
    float headShadowMinHz   = kHeadShadowMinHz;        // head shadow LPF minimum cutoff
    float ildMaxDb          = kDefaultILDMaxDb;        // maximum ILD attenuation in dB
    float rearShadowMinHz   = kRearShadowMinHz;        // rear shadow LPF minimum cutoff
    float smoothMs_ITD      = kDefaultSmoothMs_ITD;    // ITD smoother time constant
    float smoothMs_Filter   = kDefaultSmoothMs_Filter; // SVF cutoff smoother time constant
    float smoothMs_Gain     = kDefaultSmoothMs_Gain;   // ILD gain smoother time constant

    bool binauralEnabled    = true;  // false = ITD off + hardpan mode

    // =========================================================================
    // Phase 3: Depth (DEPTH-01, DEPTH-02, DEPTH-04, DEPTH-05)
    // =========================================================================
    // Per-filter delay times (ms) and feedback gains for the series comb bank.
    // Defaults mirror kCombDefaultDelays_ms / kCombDefaultFeedback in Constants.h.
    // Arrays use in-place initializers because constexpr arrays cannot be used
    // as default member initializers in C++17/20 without extra boilerplate.
    float combDelays_ms[kMaxCombFilters] = {
        0.21f, 0.37f, 0.54f, 0.68f, 0.83f,
        0.97f, 1.08f, 1.23f, 1.38f, 1.50f
    };
    float combFeedback[kMaxCombFilters] = {
        0.15f, 0.14f, 0.16f, 0.13f, 0.15f,
        0.14f, 0.16f, 0.13f, 0.15f, 0.14f
    };
    float combWetMax = kCombMaxWet;  // 0.30f — max wet blend at Y=-1

    // =========================================================================
    // Phase 3: Elevation (ELEV-01, ELEV-02, ELEV-03, ELEV-04, ELEV-05)
    // =========================================================================
    float pinnaNotchFreqHz = kPinnaNotchFreqHz;  // 8000 Hz — pinna notch center
    float pinnaNotchQ      = kPinnaNotchQ;        // 2.0   — ~0.5 octave bandwidth
    float pinnaShelfFreqHz = kPinnaShelfFreqHz;   // 4000 Hz — high shelf knee
    float chestDelayMaxMs  = kChestDelayMaxMs;    // 2.0 ms — chest bounce max delay
    float chestGainDb      = kChestGainDb;        // -8 dB  — chest bounce attenuation
    float floorDelayMaxMs  = kFloorDelayMaxMs;    // 20.0 ms — floor bounce max delay
    float floorGainDb      = kFloorGainDb;        // -5 dB  — floor bounce attenuation
    float chestHPFHz       = kChestHPFHz;         // 700 Hz — chest bounce HP cascade cutoff
    float chestLPHz        = kChestLPHz;          // 1000 Hz — chest bounce LP cutoff
    float floorAbsHz       = kFloorAbsHz;         // 5000 Hz — floor HF absorption LPF

    // Floor bounce factor override (LOCAL MOD, see VENDORED_FROM.md).
    // < 0 (default): floor bounce is driven by the listener-relative elevation
    // factor, the original VST behavior. >= 0: caller-supplied closeness of
    // the SOURCE to the floor beneath it, in [0,1]: 1 = on the floor (max
    // bounce, min delay), 0 = far above it (no bounce). The game runtime
    // computes this from world geometry per source per block.
    float floorBounceFactor = -1.0f;

    // Pinna P1 fixed peak
    float pinnaP1FreqHz  = kPinnaP1FreqHz;    // 5000 Hz — fixed peak center
    float pinnaP1GainDb  = kPinnaP1GainDb;    // +4 dB   — peak boost
    float pinnaP1Q       = kPinnaP1Q;          // 1.5     — moderate width

    // Pinna N2 secondary notch (offset from N1)
    float pinnaN2OffsetHz = kPinnaN2OffsetHz;  // 3000 Hz — N2 = N1 + offset
    float pinnaN2GainDb   = kPinnaN2GainDb;    // -8 dB   — notch depth
    float pinnaN2Q        = kPinnaN2Q;          // 2.0     — bandwidth

    // Pinna N1 range limits (elevation-dependent sweep)
    float pinnaN1MinHz = kPinnaN1MinHz;        // 6500 Hz — at Z=-1 (below)
    float pinnaN1MaxHz = kPinnaN1MaxHz;        // 10000 Hz — at Z=+1 (above)

    // Near-field LF boost
    float nearFieldLFHz    = kNearFieldLFHz;      // 200 Hz — low-shelf frequency
    float nearFieldLFMaxDb = kNearFieldLFMaxDb;   // 6 dB   — max boost at close range

    // Head shadow fully-open cap
    float headShadowFullOpenHz = kHeadShadowFullOpenHz;  // 16000 Hz — safe SVF range

    // =========================================================================
    // Phase 4: Distance Processing (DIST-01 through DIST-06)
    // =========================================================================
    float distDelayMaxMs = kDistDelayMaxMs;   // 300.0f — max propagation delay
    float distSmoothMs   = kDistSmoothMs;     // 30.0f  — delay smoother time constant
    bool  dopplerEnabled = true;              // DIST-05: toggle delay+doppler
    float airAbsMaxHz    = kAirAbsMaxHz;      // 22000.0f — LPF cutoff at min distance
    float airAbsMinHz    = kAirAbsMinHz;      // 8000.0f  — LPF cutoff at max distance
    float airAbs2MaxHz   = kAirAbs2MaxHz;     // 22000.0f — stage 2 close (flat)
    float airAbs2MinHz   = kAirAbs2MinHz;     // 12000.0f — stage 2 far (extra HF loss)
    float distGainFloorDb = kDistGainFloorDb;  // -72 dB  — gain at sphere boundary
    float distGainMax     = kDistGainMax;      // 2.0     — max +6dB boost at close range
    float distCurveSteep  = kDistCurveSteepDefault; // 0=uniform slope, 1=flat center/cliff edge

    // =========================================================================
    // Phase 5: Reverb (VERB-01 through VERB-04)
    // =========================================================================
    float verbSize        = kVerbDefaultSize;
    float verbDecay       = kVerbDefaultDecay;
    float verbDamping     = kVerbDefaultDamping;
    float verbWet         = kVerbDefaultWet;
    float verbPreDelayMax = kVerbPreDelayMaxMs;
    float verbModDepth    = kVerbDefaultModDepth;
    float verbDiffusion   = kVerbDefaultDiffusion;

    // =========================================================================
    // Phase 5: LFO (LFO-01 through LFO-05)
    // =========================================================================
    // Per-axis: rate (Hz), depth (position units [0,1]), phase offset ([0,1]),
    // waveform int (0=Sine 1=Triangle 2=Saw 3=RampDown 4=Square 5=SampleHold).
    float lfoXRate      = kLFODefaultRate;  float lfoYRate      = kLFODefaultRate;  float lfoZRate      = kLFODefaultRate;
    float lfoXDepth     = 0.0f;             float lfoYDepth     = 0.0f;             float lfoZDepth     = 0.0f;
    float lfoXPhase     = 0.0f;             float lfoYPhase     = 0.0f;             float lfoZPhase     = 0.0f;
    int   lfoXWaveform  = 0;                int   lfoYWaveform  = 0;                int   lfoZWaveform  = 0;
    float lfoXSmooth    = 0.0f;             float lfoYSmooth    = 0.0f;             float lfoZSmooth    = 0.0f;
    // Tempo sync (per-axis)
    bool  lfoXTempoSync = false;
    bool  lfoYTempoSync = false;
    bool  lfoZTempoSync = false;
    float hostBpm       = 120.0f;           // passed from processBlock AudioPlayHead
    int   hostTimeSigNum = 4;               // time signature numerator (beats per bar)
    int   hostTimeSigDen = 4;               // time signature denominator (beat unit)
    double hostPpqPosition = -1.0;          // quarter notes from song start; -1 = unavailable
    bool   hostIsPlaying   = false;         // true when DAW transport is rolling
    float lfoXBeatDiv   = 1.0f;             float lfoYBeatDiv   = 1.0f;             float lfoZBeatDiv   = 1.0f;

    // =========================================================================
    // Phase 5: LFO extensions
    // =========================================================================
    float lfoSpeedMul    = kLFOSpeedMulDefault;
    float lfoDepthMul    = 1.0f;
    bool  lfoXResetPhase = false;
    bool  lfoYResetPhase = false;
    bool  lfoZResetPhase = false;

    // =========================================================================
    // Geometry
    // =========================================================================
    float sphereRadius           = kSphereRadiusDefault;
    float vertMonoCylinderRadius = kVertMonoCylinderRadius;  // DEPRECATED — unused, kept for preset compat

    // =========================================================================
    // Virtual ear offsets (distance-difference spatial cue model)
    // =========================================================================
    float azimuthEarOffset    = kAzimuthEarOffset;
    float rearEarOffset       = kRearEarOffset;
    float elevEarOffset       = kElevEarOffset;
    float presenceShelfFreqHz = kPresenceShelfFreqHz;
    float presenceShelfMaxDb  = kPresenceShelfMaxDb;
    float earCanalFreqHz      = kEarCanalFreqHz;
    float earCanalQ           = kEarCanalQ;
    float earCanalMaxDb       = kEarCanalMaxDb;

    // =========================================================================
    // Aux reverb send
    // =========================================================================
    float auxSendGainMaxDb  = kAuxSendGainMaxDb;

    // =========================================================================
    // Stereo source node splitting
    // =========================================================================
    float stereoWidth        = 0.0f;
    bool  stereoFaceListener = false;
    float stereoOrbitPhase   = 0.0f;
    float stereoOrbitOffset  = 0.0f;

    // =========================================================================
    // Swarm configuration (N-node generalization)
    // =========================================================================
    int   swarmNodeCount     = 1;       // clamped to [1, kMaxSwarmNodes] in process()
    int   swarmMovementMode  = 0;       // SwarmMovementMode enum (0 = StereoOrbit)
    float swarmParam1        = 0.0f;    // mode-specific parameter
    float swarmParam2        = 0.0f;    // mode-specific parameter
    float swarmParam3        = 0.0f;    // mode-specific parameter
    float swarmDelayOffset   = 0.0f;    // max delay stagger across nodes (ms)
    int   swarmDelayMode     = 0;       // 0=Linear,1=Exponential,2=Logarithmic,3=Steps,4=Custom
    int   swarmDelayOrder    = 0;       // same enum as swarmPitchOrder
    float customNodeDelay[16] = {};     // per-node delay ms for Custom delay mode
    float swarmNodeGainDb    = 0.0f;    // per-node gain (fixed at unity, no UI)
    bool  swarmERShared      = true;    // true = single ER at centroid position
    float swarmPitchSpread   = 0.0f;   // 0-64 semitones pitch spread (interpretation per mode)
    int   swarmPitchMode     = 0;      // 0=Detune,1=Chromatic,2=Octaves,3=Fifths,4=Harmonics,5=Custom
    int   swarmPitchOrder    = 0;      // 0=Linear,1=Random,2=Interleave,3=Reverse,4=Mirror,5=Pairs,6=Converge
    int   swarmPitchSeed     = 0;      // 0-99 seed for Random order
    float customNodePitch[16] = {};    // per-node semitone offsets for Custom mode
    // Per-option variation values (see ParamIDs.h for per-selection meaning)
    float swarmPitchModeVar  = 1.0f;   // Chromatic: quantize step (st); Harmonics: stack ratio
    int   swarmPitchOrderVar = 2;      // Interleave: divisions; Pairs: group size
    int   swarmDelayModeVar  = 2;      // Exp/Log: curve power; Steps: step count
    int   swarmDelayOrderVar = 2;      // Interleave: divisions; Pairs: group size

    // =========================================================================
    // Stereo orbit LFOs (XY, XZ, YZ planes)
    // =========================================================================
    int   stereoOrbitXYWaveform   = 0;                       int   stereoOrbitXZWaveform   = 0;                       int   stereoOrbitYZWaveform   = 0;
    float stereoOrbitXYRate       = kStereoOrbitDefaultRate;  float stereoOrbitXZRate       = kStereoOrbitDefaultRate;  float stereoOrbitYZRate       = kStereoOrbitDefaultRate;
    float stereoOrbitXYBeatDiv    = 1.0f;                    float stereoOrbitXZBeatDiv    = 1.0f;                    float stereoOrbitYZBeatDiv    = 1.0f;
    float stereoOrbitXYPhase      = 0.0f;                    float stereoOrbitXZPhase      = 0.0f;                    float stereoOrbitYZPhase      = 0.0f;
    bool  stereoOrbitXYResetPhase = false;                   bool  stereoOrbitXZResetPhase = false;                   bool  stereoOrbitYZResetPhase = false;
    float stereoOrbitXYDepth      = 0.0f;                    float stereoOrbitXZDepth      = 0.0f;                    float stereoOrbitYZDepth      = 0.0f;
    float stereoOrbitXYSmooth     = 0.0f;                    float stereoOrbitXZSmooth     = 0.0f;                    float stereoOrbitYZSmooth     = 0.0f;
    bool  stereoOrbitXYTempoSync  = false;
    bool  stereoOrbitXZTempoSync  = false;
    bool  stereoOrbitYZTempoSync  = false;
    float stereoOrbitSpeedMul     = 1.0f;
    float stereoOrbitDepthMul     = 1.0f;

    // =========================================================================
    // Dev tool: Test tone oscillator
    // =========================================================================
    bool             testToneEnabled  = false;
    float            testToneGainDb   = -12.0f;
    float            testTonePitchHz  = kTestTonePitchDefault;
    float            testTonePulseHz  = kTestTonePulseDefault;
    TestToneWaveform testToneWaveform = TestToneWaveform::Saw;

    // =========================================================================
    // Expanded Pinna EQ (P5) — 4 additional bands
    // =========================================================================
    float conchaNotchFreqHz  = kConchaNotchFreqHz;   // 4000 Hz
    float conchaNotchQ       = kConchaNotchQ;         // 3.0
    float conchaNotchMaxDb   = kConchaNotchMaxDb;     // -8.0 dB

    float upperPinnaFreqHz   = kUpperPinnaFreqHz;     // 12000 Hz
    float upperPinnaQ        = kUpperPinnaQ;           // 2.0
    float upperPinnaMinDb    = kUpperPinnaMinDb;       // -4.0 dB
    float upperPinnaMaxDb    = kUpperPinnaMaxDb;       // +3.0 dB

    float shoulderPeakFreqHz = kShoulderPeakFreqHz;   // 1500 Hz
    float shoulderPeakQ      = kShoulderPeakQ;         // 1.0
    float shoulderPeakMaxDb  = kShoulderPeakMaxDb;     // +2.0 dB

    float tragusNotchFreqHz  = kTragusNotchFreqHz;    // 8500 Hz
    float tragusNotchQ       = kTragusNotchQ;          // 3.5
    float tragusNotchMaxDb   = kTragusNotchMaxDb;      // -5.0 dB

    // =========================================================================
    // Early Reflections (Image Source Method)
    // =========================================================================
    bool  erEnabled     = true;
    float erRoomSize    = kERRoomSizeDefault;   // half-dimension in meters
    float erDamping     = kERDampingDefault;     // 0–1: wall absorption
    float erLevel       = kERLevelDefault;       // 0–1: overall ER wet level
    float erReverbSend  = kERReverbSendDefault;  // 0–1: volume send to FDN reverb

    // =========================================================================
    // Listener head orientation
    // =========================================================================
    float listenerYaw   = 0.0f;  // radians
    float listenerPitch = 0.0f;  // radians
    float listenerRoll  = 0.0f;  // radians

    // =========================================================================
    // Walker mode — movable listener position (scaled by R in processor)
    // =========================================================================
    float listenerX = 0.0f;
    float listenerY = 0.0f;
    float listenerZ = 0.0f;

    // =========================================================================
    // Dev tool: Per-feature bypass toggles
    // =========================================================================
    bool bypassER         = false;
    bool bypassITD        = false;
    bool bypassHeadShadow = false;
    bool bypassILD        = false;
    bool bypassNearField  = false;
    bool bypassRearShadow = false;
    bool bypassPinnaEQ    = false;
    bool bypassExpandedPinna = false;
    bool bypassComb       = false;
    bool bypassChest      = false;
    bool bypassFloor      = false;
    bool bypassDistGain   = false;
    bool bypassDoppler    = false;
    bool bypassAirAbs     = false;
    bool bypassReverb     = false;
};

// Result of XYZ-to-spherical coordinate conversion.
// Y-forward convention: azimuth=0 means directly in front (Y=1).
struct SphericalCoord {
    float azimuth;    // radians: 0 = front, +PI/2 = right (X=1), clockwise positive
    float elevation;  // radians: +PI/2 = directly above, -PI/2 = directly below
    float distance;   // Euclidean distance from origin, floored at kMinDistance
};

} // namespace xyzpan
