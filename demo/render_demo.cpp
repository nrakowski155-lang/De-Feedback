// Renders a before/after listening example with the real De-Feedback engine.
//
//   dry section  -> programme material with feedback building up
//   1.5 s pause
//   wet section  -> the same material through a fresh engine in HUNT mode
//
// Build & run:
//   g++ -O2 -std=c++20 -I Source/dsp demo/render_demo.cpp
//       Source/dsp/DeFeedbackEngine.cpp -o demo_render
//   ./demo_render
//
// Writes demo/de_feedback_before_after.wav (48 kHz, 16 bit, stereo).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "DeFeedbackEngine.h"

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kSampleRate = 48000.0;
    constexpr double kSectionLength = 8.0;
    constexpr int    kSectionSamples = (int) (kSectionLength * kSampleRate);

    // ---------------------------------------------------------------- WAV ---
    void writeU32 (std::vector<uint8_t>& b, uint32_t v)
    {
        b.push_back ((uint8_t) (v & 0xff));
        b.push_back ((uint8_t) ((v >> 8) & 0xff));
        b.push_back ((uint8_t) ((v >> 16) & 0xff));
        b.push_back ((uint8_t) ((v >> 24) & 0xff));
    }

    void writeU16 (std::vector<uint8_t>& b, uint16_t v)
    {
        b.push_back ((uint8_t) (v & 0xff));
        b.push_back ((uint8_t) ((v >> 8) & 0xff));
    }

    bool writeWav (const std::string& path,
                   const std::vector<float>& left,
                   const std::vector<float>& right,
                   double sampleRate)
    {
        const int channels = 2;
        const int bits     = 16;
        const uint32_t frames = (uint32_t) std::min (left.size(), right.size());
        const uint32_t dataBytes = frames * channels * (bits / 8);

        std::vector<uint8_t> out;
        out.reserve (44 + dataBytes);

        out.insert (out.end(), { 'R', 'I', 'F', 'F' });
        writeU32 (out, 36 + dataBytes);
        out.insert (out.end(), { 'W', 'A', 'V', 'E' });

        out.insert (out.end(), { 'f', 'm', 't', ' ' });
        writeU32 (out, 16);
        writeU16 (out, 1);                 // PCM
        writeU16 (out, (uint16_t) channels);
        writeU32 (out, (uint32_t) sampleRate);
        writeU32 (out, (uint32_t) sampleRate * channels * (bits / 8));
        writeU16 (out, (uint16_t) (channels * (bits / 8)));
        writeU16 (out, (uint16_t) bits);

        out.insert (out.end(), { 'd', 'a', 't', 'a' });
        writeU32 (out, dataBytes);

        for (uint32_t i = 0; i < frames; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float sample = ch == 0 ? left[i] : right[i];
                const float clamped = std::clamp (sample, -1.0f, 1.0f);
                const int16_t value = (int16_t) std::lround (clamped * 32767.0f);
                writeU16 (out, (uint16_t) value);
            }
        }

        FILE* f = std::fopen (path.c_str(), "wb");
        if (f == nullptr)
            return false;

        const bool ok = std::fwrite (out.data(), 1, out.size(), f) == out.size();
        std::fclose (f);
        return ok;
    }

    // ------------------------------------------------------------- music ----
    double envelope (double t, double duration, double attack, double release)
    {
        if (t < 0.0 || t > duration)
            return 0.0;

        double e = 1.0;

        if (t < attack)
            e = t / attack;

        if (t > duration - release)
            e = std::min (e, (duration - t) / release);

        return std::max (0.0, e);
    }

    struct Howl
    {
        double freq;
        double start;
        double ramp;
        double amplitude;
    };

    void renderMusic (std::vector<float>& left, std::vector<float>& right)
    {
        const int n = kSectionSamples;
        left.assign ((size_t) n, 0.0f);
        right.assign ((size_t) n, 0.0f);

        std::mt19937 rng (20261126);
        std::normal_distribution<float> noise (0.0f, 1.0f);

        const double chords[4][3] = { { 110.00, 164.81, 220.00 },   // Am
                                      {  87.31, 130.81, 174.61 },   // F
                                      { 130.81, 196.00, 261.63 },   // C
                                      {  98.00, 146.83, 196.00 } }; // G
        const double bassNotes[4] = { 55.00, 43.65, 65.41, 49.00 };

        // pad + bass
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / kSampleRate;
            const int chord = ((int) (t / 2.0)) % 4;
            const double local = std::fmod (t, 2.0);
            const double env = envelope (local, 2.0, 0.18, 0.35);

            double l = 0.0, r = 0.0;

            for (int v = 0; v < 3; ++v)
            {
                const double f0 = chords[chord][v];
                const double vib = 1.0 + 0.0012 * std::sin (2.0 * kPi * 4.3 * t + v);
                const double pan = 0.5 + 0.18 * std::sin ((double) v * 2.0 + 0.7);

                for (int h = 1; h <= 6; ++h)
                {
                    const double partial = (0.040 / (double) h)
                                             * std::sin (2.0 * kPi * f0 * (double) h * vib * t + 0.8 * h);
                    l += partial * (1.0 - pan);
                    r += partial * pan;
                }
            }

            const double bass = bassNotes[chord];
            const double b = 0.085 * env * (std::sin (2.0 * kPi * bass * t)
                                            + 0.35 * std::sin (2.0 * kPi * bass * 2.0 * t));
            l += b;
            r += b;

            left[(size_t) i]  += (float) l;
            right[(size_t) i] += (float) r;
        }

        // drums
        const int kickEvery = (int) (kSampleRate * 0.5);

        for (int start = 0; start < n; start += kickEvery)
        {
            double phase = 0.0;
            for (int i = 0; i < (int) (kSampleRate * 0.28) && start + i < n; ++i)
            {
                const double x = (double) i / kSampleRate;
                const double f = 45.0 + 95.0 * std::exp (-x / 0.035);
                phase += 2.0 * kPi * f / kSampleRate;
                const double env = std::exp (-x / 0.11);
                const float s = (float) (0.20 * env * std::sin (phase));
                left[(size_t) (start + i)]  += s;
                right[(size_t) (start + i)] += s;
            }
        }

        for (int beat = 1; beat < (int) (kSectionLength / 1.0); beat += 2)
        {
            const int start = (int) ((double) beat * kSampleRate);
            for (int i = 0; i < (int) (kSampleRate * 0.12) && start + i < n; ++i)
            {
                const double env = std::exp (-(double) i / (kSampleRate * 0.045));
                const float s = (float) (0.10 * env * noise (rng));
                left[(size_t) (start + i)]  += s;
                right[(size_t) (start + i)] += s * 0.9f;
            }
        }

        for (int start = 0; start < n; start += (int) (kSampleRate * 0.25))
        {
            for (int i = 0; i < (int) (kSampleRate * 0.035) && start + i < n; ++i)
            {
                const double env = std::exp (-(double) i / (kSampleRate * 0.008));
                const float s = (float) (0.022 * env * noise (rng));
                left[(size_t) (start + i)]  += s;
                right[(size_t) (start + i)] += s * 0.8f;
            }
        }

        // ---- feedback that builds up ----
        const Howl howls[] = {
            { 1180.0, 1.0, 4.0, 0.20 },
            { 1730.0, 2.0, 4.0, 0.15 },
            {  640.0, 3.0, 4.5, 0.13 }
        };

        for (const auto& h : howls)
        {
            const int start = (int) (h.start * kSampleRate);
            const int ramp  = std::max (1, (int) (h.ramp * kSampleRate));

            for (int i = start; i < n; ++i)
            {
                const double k = std::min (1.0, (double) (i - start) / (double) ramp);
                const double amp = 0.0005 * std::pow (h.amplitude / 0.0005, k);
                const double t = (double) i / kSampleRate;
                const float s = (float) (amp * std::sin (2.0 * kPi * h.freq * t));
                left[(size_t) i]  += s;
                right[(size_t) i] += s;
            }
        }
    }

    double goertzel (const std::vector<float>& x, size_t begin, size_t end, double freq)
    {
        const double w = 2.0 * kPi * freq / kSampleRate;
        const double coeff = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;

        for (size_t i = begin; i < end; ++i)
        {
            const double s0 = (double) x[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        const double re = s1 - s2 * std::cos (w);
        const double im = s2 * std::sin (w);
        return std::sqrt (re * re + im * im) * 2.0 / (double) std::max<size_t> (1, end - begin);
    }

    void append (std::vector<float>& dstL, std::vector<float>& dstR,
                 const std::vector<float>& srcL, const std::vector<float>& srcR)
    {
        dstL.insert (dstL.end(), srcL.begin(), srcL.end());
        dstR.insert (dstR.end(), srcR.begin(), srcR.end());
    }

    void appendSilence (std::vector<float>& dstL, std::vector<float>& dstR, double seconds)
    {
        const int n = (int) (seconds * kSampleRate);
        dstL.insert (dstL.end(), (size_t) n, 0.0f);
        dstR.insert (dstR.end(), (size_t) n, 0.0f);
    }
}

int main()
{
    std::vector<float> dryL, dryR;
    renderMusic (dryL, dryR);

    // ---- process a copy with a fresh engine (live / cold start) ----
    std::vector<float> wetL = dryL, wetR = dryR;

    defb::Engine engine;
    defb::Parameters params;
    params.mode          = defb::Mode::Hunt;
    params.sensitivityDb = 9.0f;
    params.maxNotches    = 8;
    params.depthDb       = 18.0f;
    params.q             = 16.0f;
    params.attackMs      = 250.0f;
    params.musicGuard    = true;
    params.autoQ         = true;  // width from the 4096 pt confirmation FFT
    params.aiAssist      = false; // offline render: no AI worker attached

    engine.setParameters (params);
    engine.prepare (kSampleRate, 512);

    for (int offset = 0; offset < kSectionSamples; offset += 512)
    {
        const int n = std::min (512, kSectionSamples - offset);
        engine.process (wetL.data() + offset, wetR.data() + offset, n);
    }

    defb::NotchInfo info[defb::Engine::kMaxNotches] {};
    const int notchCount = engine.getNotchInfo (info, defb::Engine::kMaxNotches);

    std::printf ("Rendered 8.0 s, detected notches: %d\n", notchCount);
    for (int i = 0; i < notchCount; ++i)
        std::printf ("  notch %d: %8.2f Hz  %.1f dB  Q %.1f\n",
                     i + 1, info[i].freq, info[i].gainDb, info[i].q);

    // ---- assemble: dry, pause, wet ----
    std::vector<float> outL, outR;
    outL.reserve ((size_t) (kSectionSamples * 2 + kSampleRate * 3));
    outR.reserve ((size_t) (kSectionSamples * 2 + kSampleRate * 3));

    appendSilence (outL, outR, 0.6);
    append (outL, outR, dryL, dryR);
    appendSilence (outL, outR, 1.5);
    append (outL, outR, wetL, wetR);

    // safety margin, same gain for both sections
    float peak = 0.0f;
    for (float v : outL) peak = std::max (peak, std::abs (v));
    for (float v : outR) peak = std::max (peak, std::abs (v));

    if (peak > 0.95f)
    {
        const float gain = 0.95f / peak;
        for (float& v : outL) v *= gain;
        for (float& v : outR) v *= gain;
        std::printf ("Normalised by %.2f dB\n", 20.0 * std::log10 (gain));
    }

    const std::string path = "demo/de_feedback_before_after.wav";

    if (! writeWav (path, outL, outR, kSampleRate))
    {
        std::printf ("Could not write %s\n", path.c_str());
        return 1;
    }

    // ---- measurement ----
    const size_t tailStart = (size_t) (kSectionSamples * 0.55); // last ~3.6 s
    const size_t tailEnd   = (size_t) kSectionSamples;

    std::printf ("\nReduction in the last 3.6 s (dry -> wet):\n");
    const double freqs[] = { 640.0, 1180.0, 1730.0 };

    for (double f : freqs)
    {
        const double before = goertzel (dryL, tailStart, tailEnd, f);
        const double after  = goertzel (wetL, tailStart, tailEnd, f);
        const double db     = 20.0 * std::log10 (std::max (before, 1e-9) / std::max (after, 1e-9));
        std::printf ("  %6.0f Hz: %7.4f -> %7.4f  (%5.1f dB)\n", f, before, after, db);
    }

    std::printf ("\nWrote %s (%.1f s, 48 kHz, 16 bit stereo)\n",
                 path.c_str(), (double) outL.size() / kSampleRate);
    std::printf ("0:00.0-0:00.6 silence | 0:00.6-0:08.6 dry | 0:08.6-0:10.1 pause | 0:10.1-0:18.1 wet\n");
    return 0;
}
