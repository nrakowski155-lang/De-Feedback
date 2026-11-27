// Offline verification of the De-Feedback / ResonanceAI DSP core.
// Build:  see CMake target defb_dsp_tests, or compile tests/dsp_test.cpp together
//         with Source/dsp/DeFeedbackEngine.cpp and Source/ai/MaskModel.cpp (C++20).
// Run:    /tmp/defb_tests

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "DeFeedbackEngine.h"
#include "MaskModel.h"

using namespace defb;

static constexpr double kPi = 3.14159265358979323846;

static int gChecks = 0;
static int gFailures = 0;

static void check (bool ok, const std::string& what, const std::string& detail = {})
{
    ++gChecks;
    if (! ok)
        ++gFailures;
    std::printf ("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                 detail.empty() ? "" : "  -> ", detail.c_str());
}

// Amplitude of a single frequency (Goertzel) over the whole buffer.
static double toneAmplitude (const std::vector<float>& x, double freq, double sr)
{
    const int n = (int) x.size();
    const double w = 2.0 * kPi * freq / sr;
    const double coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double s0 = (double) x[(size_t) i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    const double re = s1 - s2 * std::cos (w);
    const double im = s2 * std::sin (w);
    return std::sqrt (re * re + im * im) * 2.0 / (double) n;
}

static double rms (const std::vector<float>& x)
{
    double sum = 0.0;
    for (float v : x)
        sum += (double) v * v;
    return std::sqrt (sum / std::max<size_t> (1, x.size()));
}

static void processStereo (Engine& engine, std::vector<float>& l, std::vector<float>& r, int block = 128)
{
    for (size_t i = 0; i < l.size(); i += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, l.size() - i);
        engine.process (l.data() + i, r.data() + i, n);
    }
}

struct Howl
{
    double freq;
    double startTime;
    double rampTime;
    double startAmp;
    double endAmp;
};

// Deterministic "programme material": three harmonic-rich voices with a slow
// chord change plus drums, i.e. something a feedback detector has to leave alone.
static std::vector<float> musicBed (int n, double sr, std::mt19937& rng, bool drums)
{
    std::vector<float> out ((size_t) n, 0.0f);
    std::normal_distribution<float> noise (0.0f, 1.0f);

    const double chords[3][3] = { { 110.0, 164.8, 220.0 },
                                  {  98.0, 146.8, 196.0 },
                                  { 123.5, 185.0, 246.9 } };

    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const int bar = (int) (t / 2.0) % 3;
        const double barPhase = std::fmod (t, 2.0);
        const double env = std::min (1.0, std::min (barPhase / 0.12, (2.0 - barPhase) / 0.12));

        double s = 0.0;

        for (int v = 0; v < 3; ++v)
        {
            const double f0 = chords[bar][v];
            const double vib = 1.0 + 0.0015 * std::sin (2.0 * kPi * 4.7 * t + v);

            for (int h = 1; h <= 6; ++h)
                s += (0.030 / (double) h) * env * std::sin (2.0 * kPi * f0 * (double) h * vib * t + 0.7 * h);
        }

        s += 0.004 * noise (rng);
        out[(size_t) i] = (float) s;
    }

    if (drums)
    {
        const double bpm = 120.0;
        const int hitEvery = (int) (sr * 60.0 / bpm * 0.5);

        for (int start = 0; start < n; start += hitEvery)
        {
            for (int i = 0; i < (int) (sr * 0.06) && start + i < n; ++i)
            {
                const double env = std::exp (-(double) i / (sr * 0.012));
                out[(size_t) (start + i)] += (float) (0.16 * env * noise (rng));
            }
        }
    }

    return out;
}

static std::vector<float> addHowls (int n, double sr, const std::vector<Howl>& howls)
{
    std::vector<float> out ((size_t) n, 0.0f);

    for (const auto& h : howls)
    {
        const int start = (int) (h.startTime * sr);
        const int ramp  = std::max (1, (int) (h.rampTime * sr));

        for (int i = start; i < n; ++i)
        {
            const double k = std::min (1.0, (double) (i - start) / (double) ramp);
            const double amp = h.startAmp * std::pow (h.endAmp / h.startAmp, k); // exponential build-up
            const double t = (double) i / sr;
            out[(size_t) i] += (float) (amp * std::sin (2.0 * kPi * h.freq * t));
        }
    }

    return out;
}

static void mixInto (std::vector<float>& dst, const std::vector<float>& src)
{
    for (size_t i = 0; i < dst.size() && i < src.size(); ++i)
        dst[i] += src[i];
}

// ---------------------------------------------------------------- tests ----

static void testBypassIsExact()
{
    std::printf ("\nBypass ist bit-exakt\n");

    Engine engine;
    Parameters p;
    p.mode = Mode::Off;
    engine.setParameters (p);
    engine.prepare (48000.0, 256);

    std::mt19937 rng (7);
    std::normal_distribution<float> dist (0.0f, 0.2f);
    std::vector<float> l (4096), r (4096);
    for (size_t i = 0; i < l.size(); ++i) { l[i] = dist (rng); r[i] = dist (rng); }

    auto original = l;
    processStereo (engine, l, r, 256);

    check (l == original, "Eingang bleibt bei Modus OFF unveraendert");
}

static void testSingleHowl()
{
    std::printf ("\nEinzelne Koppelfrequenz (1 kHz, exponentiell aufbauend)\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 5.0);

    std::mt19937 rng (11);
    auto bed = musicBed (n, sr, rng, false);
    const auto bedOnly = bed; // reference for "how much programme survives"
    auto howl = addHowls (n, sr, { { 1000.0, 0.5, 2.0, 0.0008, 0.30 } });
    mixInto (bed, howl);
    bed.resize ((size_t) n);

    auto l = bed, r = bed;

    Engine engine;
    Parameters p;
    p.mode        = Mode::Hunt;
    p.sensitivityDb = 9.0f;
    p.depthDb     = 18.0f;
    p.q           = 16.0f;
    p.attackMs    = 250.0f;
    p.musicGuard  = true;
    engine.setParameters (p);
    engine.prepare (sr, 128);

    processStereo (engine, l, r);

    NotchInfo info[16] {};
    const int count = engine.getNotchInfo (info, 16);

    check (count >= 1, "mindestens ein Notch aktiv", "count=" + std::to_string (count));

    bool found = false;
    for (int i = 0; i < count; ++i)
    {
        if (std::abs (info[i].freq - 1000.0) < 25.0)
            found = true;
    }
    check (found, "Notch sitzt bei 1 kHz");

    // attenuation of the howl in the last second
    const int tail = (int) (sr * 1.0);
    std::vector<float> inTail (bed.end() - tail, bed.end());
    std::vector<float> outTail (l.end() - tail, l.end());

    const double ampIn  = toneAmplitude (inTail, 1000.0, sr);
    const double ampOut = toneAmplitude (outTail, 1000.0, sr);
    const double reductionDb = 20.0 * std::log10 (std::max (ampIn, 1e-9) / std::max (ampOut, 1e-9));

    char buf[160];
    std::snprintf (buf, sizeof buf, "in=%.4f out=%.5f  (%.1f dB)", ampIn, ampOut, reductionDb);
    check (reductionDb > 12.0, "Koppelton wird um > 12 dB abgesenkt", buf);

    // the musical bed itself must survive (compare against the bed without the howl)
    std::vector<float> bedTail (bedOnly.end() - tail, bedOnly.end());
    const double bedRef = rms (bedTail);
    const double bedOut = rms (outTail);
    std::snprintf (buf, sizeof buf, "bed=%.4f out=%.4f", bedRef, bedOut);
    check (bedOut > bedRef * 0.80, "restliches Programm bleibt erhalten (< 2 dB Verlust)", buf);

    check (std::isfinite (rms (l)), "Ausgang ist endlich");
}

static void testResponseTime()
{
    std::printf ("\nAnsprechzeit: wie schnell wird ein aufbauender Howl abgesenkt\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 3.0);

    std::mt19937 rng (91);
    auto bed = musicBed (n, sr, rng, false);
    const int onset = (int) (sr * 1.0);
    auto howl = addHowls (n, sr, { { 1500.0, 1.0, 0.05, 0.01, 0.30 } });
    mixInto (bed, howl);

    const auto input = bed;
    auto l = bed, r = bed;

    Engine engine;
    Parameters p; // defaults: Attack 60 ms, Auto Q, Music Guard an
    engine.setParameters (p);
    engine.prepare (sr, 128);

    processStereo (engine, l, r);

    // 10 ms windows: first window after the onset where the howl is 6 dB down
    const int win = (int) (sr * 0.010);
    int firstCut = -1;
    double ampIn = 0.0, ampOut = 0.0;

    for (int start = onset; start + win <= n; start += win)
    {
        std::vector<float> wi (input.begin() + start, input.begin() + start + win);
        std::vector<float> wo (l.begin() + start, l.begin() + start + win);
        const double a = toneAmplitude (wi, 1500.0, sr);
        const double b = toneAmplitude (wo, 1500.0, sr);

        if (b <= a * 0.5)
        {
            firstCut = start;
            ampIn = a;
            ampOut = b;
            break;
        }
    }

    const double latencyMs = firstCut < 0 ? -1.0
                                          : 1000.0 * (double) (firstCut - onset) / sr;

    char buf[160];
    std::snprintf (buf, sizeof buf, "latency=%.0f ms (in=%.3f out=%.3f)", latencyMs, ampIn, ampOut);
    check (firstCut >= 0, "Howl wird innerhalb von 3 s abgesenkt", buf);
    check (latencyMs >= 0.0 && latencyMs < 150.0, "Reaktion schneller als 150 ms", buf);
}

static void testMultipleHowls()
{
    std::printf ("\nDrei gleichzeitige Koppelfrequenzen\n");

    const double sr = 44100.0;
    const int n = (int) (sr * 6.0);

    std::mt19937 rng (23);
    auto bed = musicBed (n, sr, rng, true);
    auto howls = addHowls (n, sr, { { 350.0, 0.4, 2.2, 0.0005, 0.22 },
                                    { 1450.0, 0.9, 1.8, 0.0005, 0.20 },
                                    { 3200.0, 1.4, 1.6, 0.0005, 0.18 } });
    mixInto (bed, howls);

    auto l = bed, r = bed;

    Engine engine;
    Parameters p;
    p.mode         = Mode::Hunt;
    p.sensitivityDb = 8.0f;
    p.depthDb      = 15.0f;
    p.q            = 14.0f;
    p.attackMs     = 300.0f;
    p.musicGuard   = true;
    p.maxNotches   = 8;
    engine.setParameters (p);
    engine.prepare (sr, 512);

    processStereo (engine, l, r, 512);

    NotchInfo info[16] {};
    const int count = engine.getNotchInfo (info, 16);

    for (double f : { 350.0, 1450.0, 3200.0 })
    {
        bool found = false;
        for (int i = 0; i < count; ++i)
            if (std::abs (info[i].freq - f) < f * 0.03)
                found = true;

        char buf[96];
        std::snprintf (buf, sizeof buf, "count=%d", count);
        check (found, "Notch bei " + std::to_string ((int) f) + " Hz", buf);
    }
}

static void testMusicOnlyNoFalsePositives()
{
    std::printf ("\nNur Programmmaterial (kein Feedback) -> keine Notches\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 12.0);

    std::mt19937 rng (31);
    auto bed = musicBed (n, sr, rng, true);

    auto l = bed, r = bed;

    Engine engine;
    Parameters p;
    p.mode          = Mode::Hunt;
    p.sensitivityDb = 9.0f;
    p.attackMs      = 300.0f;
    p.musicGuard    = true;
    engine.setParameters (p);
    engine.prepare (sr, 128);

    processStereo (engine, l, r);

    const int count = engine.getNumNotches();
    char buf[96];
    std::snprintf (buf, sizeof buf, "count=%d", count);
    check (count == 0, "keine Fehlausloesung durch Musik", buf);
}

static void testDynamicRelease()
{
    std::printf ("\nDynamic-Modus: Notch loest sich, wenn die Kopplung weg ist\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 8.0);

    std::mt19937 rng (37);
    auto bed = musicBed (n, sr, rng, false);

    // howl only during the first three seconds
    const int howlN = (int) (sr * 3.0);
    std::vector<float> part ((size_t) howlN, 0.0f);
    auto howl = addHowls (howlN, sr, { { 800.0, 0.2, 1.2, 0.001, 0.25 } });
    mixInto (part, howl);
    for (int i = 0; i < howlN; ++i)
        bed[(size_t) i] += part[(size_t) i];

    auto l = bed, r = bed;

    Engine engine;
    Parameters p;
    p.mode         = Mode::Dynamic;
    p.sensitivityDb = 9.0f;
    p.attackMs     = 200.0f;
    p.releaseMs    = 800.0f;
    p.depthDb      = 18.0f;
    p.musicGuard   = true;
    engine.setParameters (p);
    engine.prepare (sr, 128);

    processStereo (engine, l, r);

    char buf[96];
    std::snprintf (buf, sizeof buf, "count=%d", engine.getNumNotches());
    check (engine.getNumNotches() == 0, "Notch ist nach der Kopplung wieder freigegeben", buf);
}

static void testExtremeSettingsStayStable()
{
    std::printf ("\nExtremeinstellungen bleiben stabil (12 Notches)\n");

    const double sr = 96000.0;
    const int n = (int) (sr * 4.0);

    std::mt19937 rng (41);
    std::normal_distribution<float> dist (0.0f, 0.6f);
    std::vector<float> l ((size_t) n), r ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        l[(size_t) i] = dist (rng) + 0.4f * std::sin (2.0f * 3.14159265f * 2200.0f * (float) i / (float) sr);
        r[(size_t) i] = dist (rng) - 0.2f * std::sin (2.0f * 3.14159265f * 55.0f * (float) i / (float) sr);
    }

    Engine engine;
    Parameters p;
    p.mode         = Mode::Hunt;
    p.sensitivityDb = 3.0f;
    p.maxNotches   = 12;
    p.depthDb      = 36.0f;
    p.q            = 48.0f;
    p.attackMs     = 10.0f;
    p.musicGuard   = false;
    engine.setParameters (p);
    engine.prepare (sr, 1024);

    processStereo (engine, l, r, 1024);

    float peak = 0.0f;
    bool finite = true;
    for (float v : l) { finite = finite && std::isfinite (v); peak = std::max (peak, std::abs (v)); }

    char buf[96];
    std::snprintf (buf, sizeof buf, "peak=%.3f notches=%d", peak, engine.getNumNotches());
    check (finite, "keine NaN/Inf-Werte", buf);
    check (peak < 4.0f, "kein Aufschaukeln", buf);

    engine.clearNotches();

    // separate input, so the filter tail is not fed back in as "new" audio
    std::vector<float> silence ((size_t) 4800, 0.0f);
    std::vector<float> out ((size_t) 4800, 0.0f);
    for (int i = 0; i < 40; ++i)
    {
        std::copy (silence.begin(), silence.end(), out.begin());
        engine.process (out.data(), nullptr, (int) out.size());
    }

    std::snprintf (buf, sizeof buf, "count=%d", engine.getNumNotches());
    check (engine.getNumNotches() == 0, "Reset entfernt alle Notches", buf);
}

static void testFineConfirmation()
{
    std::printf ("\n4096-Punkt-Bestaetigung: exakte Frequenz und Auto-Q\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 5.0);
    const double howlFreq = 1237.4; // deliberately off the 2048 bin grid

    std::mt19937 rng (53);
    auto bed = musicBed (n, sr, rng, false);
    auto howl = addHowls (n, sr, { { howlFreq, 0.4, 2.4, 0.0008, 0.28 } });
    mixInto (bed, howl);

    auto l = bed, r = bed;

    Engine engine;
    Parameters p;
    p.mode          = Mode::Hunt;
    p.sensitivityDb = 9.0f;
    p.autoQ         = true;
    p.attackMs      = 250.0f;
    p.musicGuard    = true;
    engine.setParameters (p);
    engine.prepare (sr, 128);

    processStereo (engine, l, r);

    NotchInfo info[Engine::kMaxNotches] {};
    const int count = engine.getNotchInfo (info, Engine::kMaxNotches);

    float bestError = 1.0e9f;
    float bestQ = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        const float error = std::abs (info[i].freq - (float) howlFreq);
        if (error < bestError) { bestError = error; bestQ = info[i].q; }
    }

    char buf[128];
    std::snprintf (buf, sizeof buf, "count=%d, error=%.2f Hz, Q=%.1f", count, (double) bestError, (double) bestQ);
    check (count >= 1, "Notch fuer die bestaetigte Frequenz", buf);
    check (bestError < 4.0f, "Frequenz auf < 4 Hz genau", buf);
    check (bestQ >= 6.0f && bestQ <= 32.0f, "automatischer Q im gueltigen Bereich", buf);
}

static void testAiMaskAssist()
{
    std::printf ("\nResonanceAI-Maske: zusaetzliche Evidenz, kein Veto\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 6.0);
    const double toneFreq = 1500.0;

    // steady, quiet resonance inside a loud noise floor: no growth, not
    // dominant. With sensitivity 20 dB the DSP alone has no candidate at all.
    std::mt19937 rng (71);
    std::normal_distribution<float> dist (0.0f, 1.0f);
    std::vector<float> bed ((size_t) n);

    for (int i = 0; i < n; ++i)
        bed[(size_t) i] = 0.10f * dist (rng)
                        + (float) (0.02 * std::sin (2.0 * kPi * toneFreq * (double) i / sr));

    Parameters p;
    p.mode          = Mode::Hunt;
    // deliberately above the tone's own prominence: without the mask there is
    // no candidate at all, with the mask the model lifts it over the threshold
    p.sensitivityDb = 20.0f;
    p.attackMs      = 250.0f;
    p.musicGuard    = true;

    auto run = [&] (bool withAi)
    {
        auto l = bed, r = bed;

        Engine engine;
        p.aiAssist = withAi;
        engine.setParameters (p);
        engine.prepare (sr, 128);

        const int band = engine.getBandForFrequency ((float) toneFreq);

        for (size_t offset = 0; offset < l.size(); offset += 512)
        {
            if (withAi)
            {
                ai::Mask mask {};
                mask.model = ai::kModelBuiltin;
                mask.timestampMs = ai::nowMs();
                mask.confidence[band] = 1.0f;
                engine.postAiMask (mask);
            }

            const int block = (int) std::min<size_t> (512, l.size() - offset);
            engine.process (l.data() + offset, r.data() + offset, block);
        }

        NotchInfo info[Engine::kMaxNotches] {};
        const int count = engine.getNotchInfo (info, Engine::kMaxNotches);
        bool found = false;

        for (int i = 0; i < count; ++i)
            if (std::abs (info[i].freq - (float) toneFreq) < 30.0f)
                found = true;

        return std::make_pair (count, found);
    };

    const auto dspOnly = run (false);
    const auto withAi  = run (true);

    char buf[128];
    std::snprintf (buf, sizeof buf, "DSP=%d AI=%d", dspOnly.first, withAi.first);
    check (dspOnly.first == 0, "ohne Maske bleibt der gehaltene Ton unangetastet", buf);
    check (withAi.second, "mit KI-Maske wird dieselbe Frequenz gesetzt", buf);
}

static void testAiMaskContract()
{
    std::printf ("\nAI-Patch- und Masken-Vertrag\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 2.0);

    std::mt19937 rng (61);
    auto bed = musicBed (n, sr, rng, false);

    auto l = bed, r = bed;

    Engine engine;
    Parameters p;
    p.mode = Mode::Hunt;
    engine.setParameters (p);
    engine.prepare (sr, 256);

    const uint32_t before = engine.getAiPatchRevision();
    processStereo (engine, l, r, 256);
    const uint32_t after = engine.getAiPatchRevision();

    std::vector<float> patch ((size_t) Engine::kPatchSize, -1.0f);
    const int copied = engine.copyAiPatch (patch.data(), (int) patch.size());

    bool inRange = true;
    float peak = 0.0f;

    for (float v : patch)
    {
        inRange = inRange && v >= 0.0f && v <= 1.0f;
        peak = std::max (peak, v);
    }

    char buf[128];
    std::snprintf (buf, sizeof buf, "rev %u->%u, copied=%d, peak=%.2f",
                   before, after, copied, (double) peak);
    check (after > before, "Patch-Revision steigt bei Audio", buf);
    check (copied == Engine::kPatchSize, "64 x 256 Patch wird geliefert", buf);
    check (inRange, "Patch-Werte liegen in 0 .. 1", buf);
    check (peak > 0.05f, "Patch enthaelt Signal", buf);

    const auto state = engine.getAiState();
    std::snprintf (buf, sizeof buf, "model=%d fresh=%d", state.model, state.maskFresh ? 1 : 0);
    check (state.patchRevision == after, "Status meldet die aktuelle Revision", buf);

    // a mask posted by the worker must become visible in the published state
    ai::Mask mask {};
    mask.model = ai::kModelBuiltin;
    mask.timestampMs = ai::nowMs();
    mask.confidence[10] = 0.9f;
    engine.postAiMask (mask);

    std::vector<float> tailL ((size_t) 2048, 0.0f), tailR ((size_t) 2048, 0.0f);
    engine.process (tailL.data(), tailR.data(), 2048);

    const auto live = engine.getAiState();
    std::snprintf (buf, sizeof buf, "model=%d fresh=%d conf=%.2f",
                   live.model, live.maskFresh ? 1 : 0, (double) live.confidence);
    check (live.maskFresh, "frische Maske wird uebernommen", buf);
    check (live.model == ai::kModelBuiltin, "Modell-Kennung wird gemeldet", buf);
    check (live.confidence > 0.85f, "Masken-Konfidenz wird gemeldet", buf);
}

static void testBuiltinMaskModel()
{
    std::printf ("\nBuilt-in-Modell (Fallback ohne Core ML)\n");

    auto model = ai::createBuiltinMaskModel();
    check (model != nullptr && model->ready(), "Built-in-Modell ist verfuegbar");

    if (model == nullptr)
        return;

    std::vector<float> patch ((size_t) Engine::kPatchSize, 0.0f);
    std::vector<float> mask ((size_t) Engine::kNumBands, 0.0f);
    float clip = 0.0f;

    check (model->infer (patch.data(), mask.data(), clip), "Inferenz auf Stille");
    check (clip < 0.2f, "Stille ergibt keine Feedback-Konfidenz");

    const int band = 24;

    for (int t = 0; t < Engine::kPatchFrames; ++t)
    {
        for (int b = 0; b < Engine::kNumBands; ++b)
        {
            const int distance = std::abs (b - band);
            patch[(size_t) b * Engine::kPatchFrames + t] = distance == 0 ? 0.8f
                                                         : distance <= 4 ? 0.40f : 0.30f;
        }
    }

    check (model->infer (patch.data(), mask.data(), clip), "Inferenz auf schmalbandigem Peak");

    char buf[96];
    std::snprintf (buf, sizeof buf, "conf=%.2f", (double) mask[(size_t) band]);
    check (mask[(size_t) band] > 0.55f, "schmaler Peak erhaelt hohe Konfidenz", buf);
}

static void testBuiltinModelOnMusic()
{
    std::printf ("\nBuilt-in-Maske im Musikbett (keine Fehlausloesung)\n");

    const double sr = 48000.0;
    const int n = (int) (sr * 12.0);

    std::mt19937 rng (67);
    auto bed = musicBed (n, sr, rng, true);

    auto l = bed, r = bed;

    Engine engine;
    Parameters p;
    p.mode          = Mode::Hunt;
    p.sensitivityDb = 9.0f;
    p.attackMs      = 300.0f;
    p.musicGuard    = true;
    p.aiAssist      = true;
    engine.setParameters (p);
    engine.prepare (sr, 128);

    auto model = ai::createBuiltinMaskModel();

    std::vector<float> patch ((size_t) Engine::kPatchSize, 0.0f);
    std::vector<float> mask ((size_t) Engine::kNumBands, 0.0f);

    uint32_t lastRevision = 0;
    int inferences = 0;

    // mimics the plugin's AI worker: read the patch, score it, post the mask
    for (size_t offset = 0; offset < l.size(); offset += 128)
    {
        const int block = (int) std::min<size_t> (128, l.size() - offset);
        engine.process (l.data() + offset, r.data() + offset, block);

        const uint32_t revision = engine.getAiPatchRevision();

        if (revision != lastRevision && model != nullptr
            && engine.copyAiPatch (patch.data(), (int) patch.size()) == (int) patch.size())
        {
            lastRevision = revision;

            float clip = 0.0f;
            if (model->infer (patch.data(), mask.data(), clip))
            {
                ai::Mask message {};

                for (int b = 0; b < Engine::kNumBands; ++b)
                    message.confidence[b] = mask[(size_t) b];

                message.clipProbability = clip;
                message.model           = ai::kModelBuiltin;
                message.timestampMs     = ai::nowMs();

                engine.postAiMask (message);
                ++inferences;
            }
        }
    }

    char buf[128];
    std::snprintf (buf, sizeof buf, "inferences=%d, notches=%d", inferences, engine.getNumNotches());
    check (inferences > 100, "Worker-Pfad wurde durchlaufen", buf);
    check (engine.getNumNotches() == 0, "Built-in-Maske loest keine Notches auf Musik aus", buf);
}

static void testNotchBudget()
{
    std::printf ("\n12-Notch-Budget\n");

    Engine engine;
    Parameters p;
    p.maxNotches = 99; // clamped
    engine.setParameters (p);

    char buf[96];
    std::snprintf (buf, sizeof buf, "kMaxNotches=%d, param=%d",
                   Engine::kMaxNotches, engine.getParameters().maxNotches);
    check (Engine::kMaxNotches == 12, "Engine-Limit ist 12", buf);
    check (engine.getParameters().maxNotches == 12, "Parameter wird auf 12 begrenzt", buf);
}

static void testFftRoundTrip()
{
    std::printf ("\nAnalyse-Fenster / FFT-Grundfunktion\n");

    defb::Fft fft (11);
    std::vector<float> re (2048, 0.0f), im (2048, 0.0f);

    for (int i = 0; i < 2048; ++i)
        re[(size_t) i] = std::sin (2.0 * kPi * 64.0 * (double) i / 2048.0);

    fft.forward (re.data(), im.data());

    // single positive bin, amplitude 1 => |X| = N/2
    const float mag = std::sqrt (re[64] * re[64] + im[64] * im[64]);
    char buf[96];
    std::snprintf (buf, sizeof buf, "mag=%.1f", mag);
    check (std::abs (mag - 1024.0f) < 1.0f, "FFT-Peak bei Bin 64 mit korrekter Amplitude", buf);
}

int main()
{
    std::printf ("De-Feedback DSP-Tests\n=====================\n");

    testFftRoundTrip();
    testBypassIsExact();
    testSingleHowl();
    testResponseTime();
    testMultipleHowls();
    testMusicOnlyNoFalsePositives();
    testDynamicRelease();
    testExtremeSettingsStayStable();
    testFineConfirmation();
    testAiMaskContract();
    testBuiltinMaskModel();
    testAiMaskAssist();
    testBuiltinModelOnMusic();
    testNotchBudget();

    std::printf ("\n-----------------------------\n");
    std::printf ("%d Checks, %d Fehler\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
