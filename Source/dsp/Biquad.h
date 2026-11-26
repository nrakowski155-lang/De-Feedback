#pragma once

#include <algorithm>
#include <cmath>

namespace defb
{

// Transposed direct form II biquad, float state, double coefficient design.
struct BiquadCoeffs
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

struct BiquadState
{
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept { z1 = z2 = 0.0f; }
};

inline float processBiquad (const BiquadCoeffs& c, BiquadState& s, float x) noexcept
{
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

// RBJ peaking EQ. gainDb < 0 produces the suppression notch used here.
inline BiquadCoeffs makePeaking (double sampleRate, double freq, double q, double gainDb)
{
    BiquadCoeffs c;

    const double nyquist = sampleRate * 0.5;
    freq = std::clamp (freq, 10.0, nyquist * 0.98);
    q    = std::clamp (q, 0.1, 100.0);

    const double A     = std::pow (10.0, gainDb / 40.0);
    const double w0    = 2.0 * 3.14159265358979323846 * freq / sampleRate;
    const double cosw  = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);

    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * cosw;
    const double b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * cosw;
    const double a2 = 1.0 - alpha / A;

    const double inv = 1.0 / a0;
    c.b0 = (float) (b0 * inv);
    c.b1 = (float) (b1 * inv);
    c.b2 = (float) (b2 * inv);
    c.a1 = (float) (a1 * inv);
    c.a2 = (float) (a2 * inv);
    return c;
}

} // namespace defb
