# ResonanceAI – Umsetzungsstand

Diese Datei gleicht die technische Spezifikation (M3 / 16 GB, Logic Pro, AUv2)
mit dem ab, was im Repository tatsächlich implementiert und **verifiziert** ist.
Zielwerte sind als Ziel markiert; gemessene Werte stehen nur dort, wo sie in
diesem Workspace reproduzierbar waren.

## 1. Kurzfassung

| Bereich | Status |
|---|---|
| AU/VST3-Plugin (JUCE 8.0.9, C++20, Logic) | implementiert, macOS-Build in CI |
| DSP-Engine (Dual-FFT, 12 Notches, Auto-Q, adaptive Tiefe) | implementiert, **37 Checks grün** |
| AI-Vertrag (64 × 256 Patch, Bandmaske, Lock-free-Queue) | implementiert, **getestet** |
| Built-in-Maskenmodell (Fallback ohne Core ML) | implementiert, **getestet** |
| Core-ML-Bridge (FP16, CPU/NE/All, Warm-up, Fallback) | implementiert, **nicht kompiliert** (macOS-only) |
| Trainings-Pipeline (PyTorch → Core ML) | implementiert, Datengenerator **ausgeführt**, Training nicht |
| Trainiertes Modell | **nicht vorhanden** |
| M3-Benchmark (p95, Speicher, CPU, Dropouts) | **nicht gemessen** |
| Logic-Ladevorgang / GUI | **nicht geprüft** (kein macOS/Display hier) |

## 2. Signalweg

```
Eingang ─┬─► 2048-FFT, Hop 256 ─► Spektralboden ─► Peaks ─► 4096-FFT
         │      (5,33 ms)           (1/3 Oktave)            │
         │                                                  ├─► Frequenz (≈0,3 Hz)
         │                                                  └─► Breite ─► Auto-Q
         │
         │   64×256-Patch (Seqlock) ─► AI-Worker ─► Core ML / Built-in
         │                                  │
         │                                  └─► Bandmaske (SPSC-Queue)
         │                                                  │
         │                                                  ▼
         └─► Kandidaten ─► Tracker ─► Notch-Ziele (max. 12) ─► Kerbfilter ─► Ausgang
```

Der Audio-Thread:

* alloziert nicht, blockiert nicht, wartet nie auf die KI,
* veröffentlicht den Patch über ein Seqlock (nur Schreiben, kein Lock),
* liest die Maske über eine lock-free SPSC-Queue (nur Pop),
* filtert kausal mit RBJ-Peaking-Biquads ⇒ **0 Samples zusätzliche Latenz**.

## 3. Spezifikation → Implementierung

| Spezifikation | Umsetzung | Verifikation |
|---|---|---|
| 2048-FFT, Hop 256 | `Engine::kFftSize/kHop` | Tests, Patch-Contract |
| 4096-FFT zur Bestätigung | `Engine::refineTracks`, alle 16 Hops | 1237,4 Hz → 0,3 Hz Fehler |
| 12 dynamische Notches | `Engine::kMaxNotches = 12` | Budget-Test, Extremtest |
| Automatischer Q | −6-dB-Breite der 4096-FFT → Q 6…32 | Test: Q = 32 bei reinem Ton |
| Adaptive Absenkung | 80–100 % der Depth, abhängig von Prominenz | Demo: 15,5–16,8 dB bei Depth 18 |
| Attack/Release, Auto-Release | Tracker + `releaseMs` im Dynamic-Modus | Dynamic-Test |
| Music Guard | Harmonischen-/Wachstums-/Pegeltest | 12 s Programm ohne Fehlauslösung |
| 64 × 256 Features, band-major | `Engine::patch_`, `publishPatch` | Patch-Test: 16 384 Werte, 0…1 |
| CNN + GRU, ~1,2 Mio. Parameter | `training/model.py` | 1 210 513 Parameter (Konstruktion, `assert`) |
| FP16, < 10 MB | `export_coreml.py` (`ct.precision.FLOAT16`) | ~2,4 MB Gewichte (Konstruktion) |
| CPU + Neural Engine | `MLModelConfiguration.computeUnits` | nicht gemessen |
| Worker-Thread, kein RT-Zugriff | `AiWorkerThread` in `PluginProcessor.cpp` | Code-Review |
| Fallback bei Verzögerung/Ausfall | Maske > 500 ms ⇒ reiner DSP | Masken-/Status-Test |
| Modell vor Audio laden/wärmen | Laden im Konstruktor, Warm-up im Bridge-Load | Code-Review |
| Mono/Stereo, 44,1–96 kHz | Busse, `prepare(sampleRate)` | 44,1/48/96 kHz in Tests |
| 32-bit Float | JUCE-Pfad | – |
| Kein Kopieren von Konkurrenzsoftware | eigene DSP-/ML-Implementierung | – |

> Hinweis zum Kontextfenster: 64 Bänder × 256 Frames bei einem Hop von 256
> Samples ergeben 1,37 s bei 48 kHz. Die Spezifikation nennt zusätzlich ~341 ms,
> was 64 Frames entspräche. Implementiert ist die Tensorform **64 × 256**, weil
> sie zu Hop 256 und dem 2048er-Fenster passt; das Modell sieht damit gut eine
> Sekunde Vorgeschichte, was gehaltene Gesangsnoten von Feedback unterscheidbar
> macht.

## 4. Gemessene Werte (dieser Workspace)

| Messung | Wert |
|---|---|
| DSP-Tests | 37 Checks, 0 Fehler |
| Frequenzgenauigkeit (4096-FFT) | 0,30 Hz bei 1237,4 Hz |
| Demo-Absenkung (Depth 18, Auto-Q) | 15,5 / 16,8 / 16,3 dB bei 640 / 1180 / 1730 Hz |
| Demo-Notch-Frequenzen | 639,50 / 1179,34 / 1730,51 Hz |
| Notch-Q (reiner Howl) | 31,9–32,0 |
| Patch | 64 × 256 float32 = 64 KB, Revision alle ~85 ms |
| Modellparameter | 1 210 513 (Formel/Konstruktion, `training/model.py`) |
| Datengenerator | 60 Patches in < 3 s, 24 positive, Werte 0…1 |

Nicht gemessen: Core-ML-Inferenz, Speicherbedarf, CPU-Last, Audio-Dropouts,
Latenz in Logic, Klangurteil. Diese Werte gehören nach einem Lauf auf dem
Ziel-M3 in diese Tabelle – mit macOS- und Xcode-Version.

## 5. Bewusste Sicherheitsentscheidungen

1. **Kein KI-Veto.** Die Maske kann Erkennungen verstärken und den Music Guard
   überstimmen, aber nie eine DSP-Erkennung verwerfen. Solange kein auf echten
   Aufnahmen validiertes Modell existiert, wäre ein Veto ein Risiko für die
   Bühne.
2. **Strenge Aktualität.** Masken älter als 500 ms werden ignoriert; bei
   Host-Stopp, Thread-Stall oder fehlendem Modell läuft alles wie vorher.
3. **Keine Allokation im Callback.** Patch und Maske sind feste Puffer; die
   Queue verwirft im Vollfall die neueste Nachricht statt zu warten.
4. **Modellwahl zur Laufzeit.** Fehlt `ResonanceFeedback.mlmodelc`, wird die
   Built-in-Heuristik verwendet; die Statuszeile zeigt das offen an.

## 6. Offene Punkte

1. Modell auf echten Loops (Sprache, Gesang, Chor, Instrumente, Becken)
   trainieren und gegen den DSP-Baseline vergleichen.
2. `benchmark_coreml.py` auf dem M3 ausführen und p50/p95 dokumentieren.
3. AU in Logic laden, `auval` laufen lassen, Latenz und Dropouts messen.
4. GUI auf macOS rendern und visuell prüfen.
5. Optional: Modell im Bundle mitliefern (Resources) und signieren.

## 7. Reproduzieren

```bash
# DSP + AI-Vertrag
g++ -O2 -std=c++20 -I Source/dsp -I Source/ai \
    tests/dsp_test.cpp Source/dsp/DeFeedbackEngine.cpp Source/ai/MaskModel.cpp \
    -o /tmp/defb_tests && /tmp/defb_tests

# Demo neu rendern
g++ -O2 -std=c++20 -I Source/dsp demo/render_demo.cpp \
    Source/dsp/DeFeedbackEngine.cpp -o /tmp/demo_render && /tmp/demo_render

# Trainingsdaten (nur numpy)
python3 training/synthesize.py --patches 4000
```

Der macOS-Build läuft über `.github/workflows/build-macos.yml`
(Universal-Binary, DSP-Tests, `auval`, ZIP-Artefakte).
