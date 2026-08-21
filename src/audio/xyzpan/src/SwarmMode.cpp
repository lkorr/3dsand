#include "xyzpan/SwarmMode.h"
#include "xyzpan/dsp/SineLUT.h"
#include <cmath>

namespace xyzpan {

static constexpr float kPI = 3.14159265358979323846f;
static constexpr float kTwoPI = 2.0f * kPI;

// Precomputed orbit rotation trig — compute once per call, apply per node.
struct OrbitRotation {
    float cosXZ = 1.0f, sinXZ = 0.0f;
    float cosYZ = 1.0f, sinYZ = 0.0f;
    bool hasXZ = false, hasYZ = false;
};

static inline OrbitRotation precomputeOrbitRotation(
    float orbitDepXZ, float orbitRawXZ, float orbitDepYZ, float orbitRawYZ)
{
    OrbitRotation r;
    if (std::abs(orbitDepXZ) > 1e-7f) {
        r.hasXZ = true;
        const float angXZ = orbitRawXZ * orbitDepXZ * kPI;
        r.cosXZ = dsp::SineLUT::cosLookupAngle(angXZ);
        r.sinXZ = dsp::SineLUT::lookupAngle(angXZ);
    }
    if (std::abs(orbitDepYZ) > 1e-7f) {
        r.hasYZ = true;
        const float angYZ = orbitRawYZ * orbitDepYZ * kPI;
        r.cosYZ = dsp::SineLUT::cosLookupAngle(angYZ);
        r.sinYZ = dsp::SineLUT::lookupAngle(angYZ);
    }
    return r;
}

static inline void applyOrbitRotation(float& offX, float& offY, float& offZ, const OrbitRotation& r)
{
    if (r.hasXZ) {
        const float tmpX = offX * r.cosXZ - offZ * r.sinXZ;
        const float tmpZ = offX * r.sinXZ + offZ * r.cosXZ;
        offX = tmpX;
        offZ = tmpZ;
    }
    if (r.hasYZ) {
        const float tmpY = offY * r.cosYZ - offZ * r.sinYZ;
        const float tmpZ = offY * r.sinYZ + offZ * r.cosYZ;
        offY = tmpY;
        offZ = tmpZ;
    }
}

static inline void applyOrbitRotationYZOnly(float& offX, float& offY, float& offZ, const OrbitRotation& r)
{
    if (r.hasYZ) {
        const float tmpY = offY * r.cosYZ - offZ * r.sinYZ;
        const float tmpZ = offY * r.sinYZ + offZ * r.cosYZ;
        offY = tmpY;
        offZ = tmpZ;
    }
}

// Simple integer hash for deterministic pseudo-random in Scatter mode.
static inline float hashToFloat(int seed) {
    unsigned int h = static_cast<unsigned int>(seed);
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;  // [0, 1]
}

void computeStereoOrbitOffsets(
    int nodeCount,
    float halfSpread,
    float spreadX, float spreadY,
    float orbitAngleXY,
    float smoothedOffset,
    float smoothedPhase,
    float orbitDepXZ, float orbitRawXZ,
    float orbitDepYZ, float orbitRawYZ,
    NodeOffset* outOffsets)
{
    if (nodeCount <= 0) return;

    const float angularStep = kTwoPI / static_cast<float>(nodeCount);
    const auto rot = precomputeOrbitRotation(orbitDepXZ, orbitRawXZ, orbitDepYZ, orbitRawYZ);

    for (int n = 0; n < nodeCount; ++n) {
        float nodeAngle;
        if (nodeCount == 2) {
            if (n == 0) {
                nodeAngle = orbitAngleXY + smoothedOffset;
            } else {
                nodeAngle = orbitAngleXY + smoothedOffset + kPI + smoothedPhase;
            }
        } else {
            nodeAngle = orbitAngleXY + smoothedOffset
                      + static_cast<float>(n) * angularStep
                      + (n > 0 ? smoothedPhase : 0.0f);
        }

        const float cosA = dsp::SineLUT::cosLookupAngle(nodeAngle);
        const float sinA = dsp::SineLUT::lookupAngle(nodeAngle);

        float offX = halfSpread * (spreadX * cosA - spreadY * sinA);
        float offY = halfSpread * (spreadX * sinA + spreadY * cosA);
        float offZ = 0.0f;

        applyOrbitRotation(offX, offY, offZ, rot);
        outOffsets[n] = { offX, offY, offZ };
    }
}

void computeSwarmOffsets(
    SwarmMovementMode mode,
    int nodeCount,
    float halfSpread,
    float spreadX, float spreadY,
    float orbitAngleXY,
    float orbitRawXY,
    float smoothedOffset,
    float smoothedPhase,
    float orbitDepXZ, float orbitRawXZ,
    float orbitDepYZ, float orbitRawYZ,
    float param1, float param2, float param3,
    NodeOffset* outOffsets)
{
    if (nodeCount <= 0) return;

    switch (mode) {
        case SwarmMovementMode::StereoOrbit:
            computeStereoOrbitOffsets(nodeCount, halfSpread, spreadX, spreadY,
                orbitAngleXY, smoothedOffset, smoothedPhase,
                orbitDepXZ, orbitRawXZ, orbitDepYZ, orbitRawYZ,
                outOffsets);
            break;

        case SwarmMovementMode::CongaLine:
            // CongaLine is handled inline in Engine.cpp using main-axis LFOs.
            // Fall through to default (zero offsets) — this case is never reached.
            break;

        case SwarmMovementMode::Helix:
        {
            const float turns = 0.5f + 3.5f * param1;
            const float helixR = halfSpread * (0.25f + 1.75f * param2);
            const float nMax = static_cast<float>(nodeCount > 1 ? nodeCount - 1 : 1);
            const auto rot = precomputeOrbitRotation(orbitDepXZ, orbitRawXZ, orbitDepYZ, orbitRawYZ);

            for (int n = 0; n < nodeCount; ++n) {
                const float t = static_cast<float>(n) / nMax;
                const float helixAngle = orbitAngleXY + smoothedOffset
                                       + t * turns * kTwoPI;
                const float cosA = dsp::SineLUT::cosLookupAngle(helixAngle);
                const float sinA = dsp::SineLUT::lookupAngle(helixAngle);

                float offX = helixR * (spreadX * cosA - spreadY * sinA);
                float offY = helixR * (spreadX * sinA + spreadY * cosA);
                float offZ = (nodeCount > 1) ? halfSpread * (t * 2.0f - 1.0f) : 0.0f;

                applyOrbitRotation(offX, offY, offZ, rot);
                outOffsets[n] = { offX, offY, offZ };
            }
            break;
        }

        case SwarmMovementMode::Lissajous:
        {
            const float harmonicStep = 1.0f + 3.0f * param1;
            const auto rot = precomputeOrbitRotation(orbitDepXZ, orbitRawXZ, orbitDepYZ, orbitRawYZ);

            for (int n = 0; n < nodeCount; ++n) {
                const float harmonic = 1.0f + static_cast<float>(n) * harmonicStep;
                const float nodeAngle = orbitAngleXY * harmonic + smoothedOffset;
                const float cosA = dsp::SineLUT::cosLookupAngle(nodeAngle);
                const float sinA = dsp::SineLUT::lookupAngle(nodeAngle);

                const float xyScale = 1.0f - param2 * 0.5f;
                float offX = halfSpread * xyScale * (spreadX * cosA - spreadY * sinA);
                float offY = halfSpread * xyScale * (spreadX * sinA + spreadY * cosA);
                float offZ = halfSpread * param2 * dsp::SineLUT::lookupAngle(nodeAngle * 1.5f);

                applyOrbitRotation(offX, offY, offZ, rot);
                outOffsets[n] = { offX, offY, offZ };
            }
            break;
        }

        case SwarmMovementMode::Scatter:
        {
            const float angularStep = kTwoPI / static_cast<float>(nodeCount);
            const auto rot = precomputeOrbitRotation(orbitDepXZ, orbitRawXZ, orbitDepYZ, orbitRawYZ);

            for (int n = 0; n < nodeCount; ++n) {
                const int seed = n * 7919;
                const float randAngleOff = (hashToFloat(seed) - 0.5f) * kTwoPI;
                const float randRadiusOff = hashToFloat(seed + 3571) - 0.5f;
                const float randZOff = (hashToFloat(seed + 7127) - 0.5f) * 2.0f;

                const float nodeAngle = orbitAngleXY + smoothedOffset
                                      + static_cast<float>(n) * angularStep
                                      + param1 * randAngleOff;
                const float nodeRadius = 1.0f + param1 * randRadiusOff;

                const float cosA = dsp::SineLUT::cosLookupAngle(nodeAngle);
                const float sinA = dsp::SineLUT::lookupAngle(nodeAngle);

                float offX = halfSpread * nodeRadius * (spreadX * cosA - spreadY * sinA);
                float offY = halfSpread * nodeRadius * (spreadX * sinA + spreadY * cosA);
                float offZ = halfSpread * param1 * randZOff;

                applyOrbitRotation(offX, offY, offZ, rot);
                outOffsets[n] = { offX, offY, offZ };
            }
            break;
        }

        case SwarmMovementMode::Breathe:
        {
            const float angularStep = kTwoPI / static_cast<float>(nodeCount);
            const float breathBase = std::abs(orbitRawXY);
            const float rotAngle = orbitRawXZ * orbitDepXZ * kPI;
            const auto rot = precomputeOrbitRotation(0.0f, 0.0f, orbitDepYZ, orbitRawYZ);

            for (int n = 0; n < nodeCount; ++n) {
                const float baseAngle = rotAngle
                                      + static_cast<float>(n) * angularStep
                                      + smoothedOffset;

                const float nodeBreath = (n & 1)
                    ? breathBase * (1.0f - param1) + (1.0f - breathBase) * param1
                    : breathBase;
                const float radius = halfSpread * nodeBreath;
                const float cosA = dsp::SineLUT::cosLookupAngle(baseAngle);
                const float sinA = dsp::SineLUT::lookupAngle(baseAngle);

                float offX = radius * (spreadX * cosA - spreadY * sinA);
                float offY = radius * (spreadX * sinA + spreadY * cosA);
                float offZ = halfSpread * param2
                    * dsp::SineLUT::lookupAngle(baseAngle * 0.5f + static_cast<float>(n) * 1.3f)
                    * nodeBreath;

                applyOrbitRotationYZOnly(offX, offY, offZ, rot);
                outOffsets[n] = { offX, offY, offZ };
            }
            break;
        }

        case SwarmMovementMode::BrownianDrift:
        {
            const float dampScale = 1.0f - param2 * 0.8f;
            const float timeBase = (orbitRawXY * 0.5f + 0.5f) * kTwoPI;
            const float rotAngle = orbitRawXZ * orbitDepXZ * kPI;
            const float cosRot = dsp::SineLUT::cosLookupAngle(rotAngle);
            const float sinRot = dsp::SineLUT::lookupAngle(rotAngle);
            const auto rot = precomputeOrbitRotation(0.0f, 0.0f, orbitDepYZ, orbitRawYZ);

            for (int n = 0; n < nodeCount; ++n) {
                const float fn = static_cast<float>(n);
                const float driftX = halfSpread * dsp::SineLUT::lookupAngle(
                    timeBase * (0.3f + fn * 0.17f) + fn * 2.1f) * param1 * dampScale;
                const float driftY = halfSpread * dsp::SineLUT::cosLookupAngle(
                    timeBase * (0.2f + fn * 0.13f) + fn * 3.7f) * param1 * dampScale;

                float offX = driftX * cosRot - driftY * sinRot;
                float offY = driftX * sinRot + driftY * cosRot;
                float offZ = halfSpread * dsp::SineLUT::lookupAngle(
                    timeBase * (0.15f + fn * 0.11f) + fn * 5.3f) * param1 * dampScale;

                applyOrbitRotationYZOnly(offX, offY, offZ, rot);
                outOffsets[n] = { offX, offY, offZ };
            }
            break;
        }

        case SwarmMovementMode::Bounce:
        {
            const float timeBase = orbitAngleXY;
            const float angularStep = kTwoPI / static_cast<float>(nodeCount);
            const auto rot = precomputeOrbitRotation(orbitDepXZ, orbitRawXZ, orbitDepYZ, orbitRawYZ);

            for (int n = 0; n < nodeCount; ++n) {
                const float fn = static_cast<float>(n);

                const float posAngle = fn * angularStep + smoothedOffset;
                const float cosA = dsp::SineLUT::cosLookupAngle(posAngle);
                const float sinA = dsp::SineLUT::lookupAngle(posAngle);

                float offX = halfSpread * param2 * (spreadX * cosA - spreadY * sinA);
                float offY = halfSpread * param2 * (spreadX * sinA + spreadY * cosA);

                const float heightScale = 0.4f + 0.6f * hashToFloat(n * 7919);
                const float nodeFreq = 1.0f + heightScale * (1.0f + param3 * 3.0f);
                const float phaseOff = fn * 1.83f;

                const float bounceRaw = dsp::SineLUT::lookupAngle(timeBase * nodeFreq + phaseOff);
                const float bounce = std::abs(bounceRaw);

                float offZ = halfSpread * param1 * heightScale * bounce;

                applyOrbitRotation(offX, offY, offZ, rot);
                outOffsets[n] = { offX, offY, offZ };
            }
            break;
        }

        default:
            for (int n = 0; n < nodeCount; ++n)
                outOffsets[n] = { 0.0f, 0.0f, 0.0f };
            break;
    }
}

} // namespace xyzpan
