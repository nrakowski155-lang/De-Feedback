# De-Feedback

Adaptive Rückkopplungs-Unterdrückung („De-Feedback") als **Audio Unit (AU)** für
Logic Pro, plus VST3 und Standalone-App. Das Plugin hört dem Signal zu, erkennt
pfeifende Koppelfrequenzen und setzt genau dort schmale, tiefe Kerbfilter – ohne
den Rest des Programms anzufassen.

Vorbild ist der Arbeitsablauf von *Waves Feedback Hunter* bzw. *De-Feedback*:
live einschleifen, Monitoranlage ausklingeln lassen, Notches setzen, fertig.

Die Oberfläche zeigt das Live-Spektrum, markiert jede gesetzte Frequenz mit
Frequenzlabel und bietet sechs Regler plus Frequenzbereich, Music-Guard-,
AI- und AUTO-Q-Schalter sowie einen CLEAR-Button.

Die zweite Ausbaustufe heißt **ResonanceAI**: ein kleines CNN+GRU-Modell
(ca. 1,21 Mio. Parameter, FP16, Core ML) liefert eine Bandmaske, die der
DSP-Engine als zusätzliche Evidenz dient. Ohne installiertes Modell läuft ein
konservativer Built-in-Scorer; ohne frische Maske fällt die Engine automatisch
auf den reinen DSP-Pfad zurück. Details in [training/README.md](training/README.md)
und [docs/resonance-ai.md](docs/resonance-ai.md).

---

## Download & automatische Installation

Das fertige macOS-Paket (AU + VST3) hängt an jedem GitHub-Release. Der
Bootstrap-Installer holt das neueste Release, prüft die SHA-256-Summe und
installiert systemweit:

```bash
curl -fsSL -o install-latest.sh \
  https://raw.githubusercontent.com/nrakowski155-lang/De-Feedback/main/scripts/install-latest.sh
bash install-latest.sh
```

Oder aus dem geklonten Repo:

```bash
./scripts/install-latest.sh                  # neuestes Release
./scripts/install-latest.sh --tag v1.1.0     # bestimmtes Release
./scripts/install-latest.sh --print-url      # nur die Download-URL
```

**Privates Repository:** Der Download braucht eine Anmeldung – auch für das
Skript selbst. Am einfachsten das Repo klonen und lokal starten:

```bash
git clone https://github.com/nrakowski155-lang/De-Feedback.git
cd De-Feedback
./scripts/install-latest.sh
```

Das Skript nutzt die GitHub CLI automatisch, wenn du einmalig `gh auth login`
ausgeführt hast; alternativ `GITHUB_TOKEN` mit Lesezugriff auf Releases setzen.
Das Paket lässt sich auch direkt auf der
[Releases-Seite](https://github.com/nrakowski155-lang/De-Feedback/releases)
herunterladen.

Ein Release wird automatisch gebaut, sobald ein Tag gepusht wird:

```bash
git tag v1.1.0
git push origin v1.1.0
```

Der CI-Lauf hängt dann `De-Feedback.pkg` (stabiler Name, daher funktioniert
`/releases/latest/download/…` dauerhaft), `De-Feedback-Uninstall.pkg`, beide
Plugin-ZIPs und `SHA256SUMS` an das Release.

---

## Was das Plugin macht

**Analyse**

* 2048-Punkt-FFT mit 256-Sample-Hop (5,33 ms Analyseintervall, 23 Hz
  Bin-Breite bei 48 kHz)
* 4096-Punkt-Bestätigungs-FFT alle 16 Hops (~85 ms): verfeinert die Frequenz
  (im Test 0,3 Hz Fehler bei 1237,4 Hz) und misst die Peak-Breite
* lokaler Spektralboden als 1/3-Oktav-Mittelwert, damit auch im vollen Mix ein
  schmaler Peak auffällt
* Peak-Picking mit parabolischer Interpolation
* Prüfung auf **Schmalbandigkeit** (ein Howl ist wenige Hz breit)
* **Harmonischen-Test**: hat der Peak Oberton-Verwandte (f/2, f/3, 2f, 3f), ist
  es mit hoher Wahrscheinlichkeit ein Instrument und kein Feedback
* **Wachstums-Test**: ein Howl baut sich über die Zeit auf, ein gehaltener Ton
  nicht; gemessen über die halbe Attack-Zeit
* **Pegel-Gate** gegen Rauschen und **Music Guard** (Dominiert der Peak das
  Programm? Liegt er über dem Programm-RMS?)
* **ResonanceAI-Bandmaske** (optional): 64 Bänder × 256 Frames, asynchron im
  Worker-Thread berechnet, kann die Evidenz um bis zu +8 dB verstärken und bei
  ≥ 70 % Konfidenz den Music Guard überstimmen – aber nie eine Erkennung
  verhindern

**Unterdrückung**

* bis zu **12** dynamische Kerbfilter, gekoppelt stereo (links = rechts)
* RBJ-Peaking-Biquads; **automatischer Q** aus der gemessenen Peak-Breite
  (6–32, der WIDTH-Regler ist dann die Obergrenze) oder fester Q
* **adaptive Tiefe**: 80–100 % der eingestellten Tiefe, je nachdem wie weit der
  Peak über der Schwelle liegt
* Tiefe bis −36 dB, Ein-/Ausblenden mit 30/80 ms (kein Klicken)
* Frequenzgrenzen (Low/High) gegen Bass und Hiss
* Notches werden bei Bedarf nachgeführt (langsame Retune) und bei vollem
  Speicher nur dann ersetzt, wenn der alte Howl nicht mehr aktiv ist

**Modi**

| Modus | Verhalten |
|---|---|
| `OFF` | bit-exakte Bypass, keine Analyse |
| `HUNT` | Live-Modus: erkennt und **hält** die Notches (kein Pumpen). Der klassische Soundcheck-/Bühnen-Modus. |
| `DYNAMIC` | Studio-Modus: gibt einen Notch wieder frei, wenn die Kopplung länger als die Release-Zeit weg ist. |

---

## Bauen (macOS)

Voraussetzungen:

* macOS 11 oder neuer, Xcode + Command Line Tools
* CMake ≥ 3.22
* Internetzugang für den JUCE-Download (oder eine lokale JUCE-Kopie)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

Mit lokalem JUCE-Checkout statt Download:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DJUCE_PATH=/pfad/zu/JUCE
cmake --build build -j 8
```

Ergebnis (JUCE legt die Konfiguration als Ordner ab, z. B. `Release/`):

```
build/DeFeedback_artefacts/Release/AU/De-Feedback.component
build/DeFeedback_artefacts/Release/VST3/De-Feedback.vst3
build/DeFeedback_artefacts/Release/Standalone/De-Feedback.app
```

Die verwendeten `juce_add_plugin`-Optionen sind gegen die CMake-API von
JUCE 8.0.9 geprüft: `FORMATS AU VST3 Standalone`, `AU_MAIN_TYPE
kAudioUnitType_Effect`, `VST3_CATEGORIES Fx`, `COPY_PLUGIN_AFTER_BUILD`, die
MIDI-/Editor-Flags und die 4-Zeichen-Codes (`Dfbk` / `Dfb1`, je genau ein
Großbuchstabe, damit auch GarageBand sie akzeptiert). Der AU-Export-Prefix wird
von JUCE automatisch zu `De_FeedbackAU` bereinigt.

Alternativ direkt in Xcode öffnen: `cmake -B build -G Xcode` und
`open build/DeFeedback.xcodeproj`.

Gepinnt ist JUCE 8.0.9 (siehe `CMakeLists.txt`). Falls eine sehr neue
Xcode-Version damit Probleme macht, zuerst `GIT_TAG` auf eine neuere
JUCE-Version heben und die Compiler-Meldungen prüfen – die hier genutzten
JUCE-APIs sind über 7/8/9 hinweg stabil.

`COPY_PLUGIN_AFTER_BUILD` ist aktiv, das AU landet also automatisch in
`~/Library/Audio/Plug-Ins/Components/`. Falls macOS das Bundle blockiert:

```bash
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/De-Feedback.component
codesign --force --deep -s - ~/Library/Audio/Plug-Ins/Components/De-Feedback.component
killall -9 AudioComponentRegistrar 2>/dev/null
```

Validieren:

```bash
auval -v aufx Dfb1 Dfbk
```

In Logic Pro: Spur → Audio FX → Audio Units → **De-Feedback Audio** →
**De-Feedback**. Logic cached Plugins; nach einem neuen Build Logic einmal
neu starten.

### Automatischer macOS-Build (GitHub Actions)

`.github/workflows/build-macos.yml` läuft bei jedem Push auf `main`, bei Pull
Requests und manuell (`workflow_dispatch`). Der Job baut auf `macos-15` ein
Universal-Binary (arm64 + x86_64), führt die DSP-Tests aus, installiert das AU
über `scripts/install-macos.sh`, validiert es mit `auval`, deinstalliert es
wieder über `scripts/uninstall-macos.sh`, baut Installer-/Deinstaller-Pakete und
lädt alles als Artifact hoch:

* `De-Feedback-AU-macOS-universal.zip`
* `De-Feedback-VST3-macOS-universal.zip`
* `De-Feedback-<version>.pkg` (Installer, systemweit)
* `De-Feedback-Uninstall-<version>.pkg` (Deinstaller)

Zusätzlich prüft der Lauf die Skripte (`bash -n`) und führt einen Smoke-Test der
Trainings-Pipeline aus: 12 synthetische Patches mit Shape-, Wertebereichs- und
Positiv-Label-Prüfung. Damit ist `training/synthesize.py` nicht nur syntaktisch
abgesichert.

Die Bundles werden ad-hoc signiert (`codesign -s -`), aber **nicht notarisiert**.
Nach dem Download aus GitHub die Quarantäne entfernen:

```bash
xattr -dr com.apple.quarantine De-Feedback.component
```

Lokal lässt sich derselbe Build ohne Installationsschritt nachstellen:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DDEFB_COPY_PLUGIN_AFTER_BUILD=OFF
cmake --build build -j 4
```

### Windows / Linux

Für VST3 und Standalone baut derselbe Befehl auch ohne Apple-Toolchain
(AU wird dann nicht erzeugt, weil Audio Units macOS-only sind):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

### Installation (Skript oder .pkg)

**Skript** – findet das gebaute Bundle automatisch, kopiert AU und VST3,
entfernt die Quarantäne, signiert ad-hoc und validiert das AU:

```bash
./scripts/install-macos.sh
./scripts/install-macos.sh --from ~/Downloads/De-Feedback-AU-macOS-universal.zip
./scripts/install-macos.sh --system          # /Library, mit sudo
```

Optionen: `--sign "Developer ID Application: …"` für eine echte Signatur,
`--skip-auval`, `--help`. Ohne `--system` landet alles in
`~/Library/Audio/Plug-Ins/…` – dafür ist kein sudo nötig.

**Paket** – aus demselben Build, inklusive Deinstaller:

```bash
./scripts/build-pkg.sh
# dist/De-Feedback-1.1.0.pkg
# dist/De-Feedback-Uninstall-1.1.0.pkg

sudo installer -pkg dist/De-Feedback-1.1.0.pkg -target /
```

Das Installer-Paket legt AU und VST3 systemweit unter
`/Library/Audio/Plug-Ins/…` ab, das payload-free Deinstaller-Paket entfernt
beide wieder. Mit `--sign "Developer ID Installer: …"` werden die Pakete
signiert; ohne Zertifikat sind sie unsigniert (lokal trotzdem installierbar).

**Deinstallieren:**

```bash
./scripts/uninstall-macos.sh            # Benutzerinstallation
./scripts/uninstall-macos.sh --system   # /Library
./scripts/uninstall-macos.sh --all      # beides
```

Ein trainiertes KI-Modell unter `~/Library/Application Support/ResonanceAI/`
bleibt bewusst erhalten – das sind Nutzerdaten, keine Plugin-Dateien.

---

## Demo (Vorher/Nachher)

[demo/de_feedback_before_after.wav](demo/de_feedback_before_after.wav) –
18,1 s, 48 kHz, 16 Bit, stereo:

| Zeit | Inhalt |
|---|---|
| 0:00.0–0:00.6 | Stille |
| 0:00.6–0:08.6 | **vorher**: Musikbett + drei aufbauende Rückkopplungen (640, 1180, 1730 Hz) |
| 0:08.6–0:10.1 | Pause |
| 0:10.1–0:18.1 | **nachher**: identisches Material durch die Engine (HUNT, 18 dB Tiefe, Q 16) |

Im letzten Abschnitt gemessen: **15,5 dB** bei 640 Hz, **16,8 dB** bei 1180 Hz,
**16,3 dB** bei 1730 Hz. Die Engine setzt die Notches auf 639,5 / 1179,3 /
1730,5 Hz (Auto-Q ≈ 32). Die ersten Sekunden des nassen Teils zeigen bewusst
den Hunt-Vorgang, bis die Notches greifen.

Neu rendern:

```bash
g++ -O2 -std=c++20 -I Source/dsp demo/render_demo.cpp \
    Source/dsp/DeFeedbackEngine.cpp -o demo_render
./demo_render
```

---

## Bedienung in Logic (Live)

1. Plugin auf den Kanal, in dem die Kopplung entsteht (meist Gesangsmikrofon
   bzw. Monitor-Bus), **nicht** auf den Master.
2. Modus **HUNT**, Depth ca. 18 dB, Q ca. 16, Attack 250 ms, Music Guard an.
3. Monitore auf Show-Pegel, Gain langsam aufdrehen, bis es pfeift. Das Plugin
   setzt pro Frequenz einen Notch und hält ihn.
4. Vor dem nächsten Song ggf. **CLEAR** – danach HUNT einfach stehen lassen.
5. Für Aufnahmen/Studio: **DYNAMIC**, dann lösen sich Notches wieder, wenn die
   Kopplung verschwindet.

Das Plugin hat **0 Latenz** (kein Lookahead), ist also auch im Monitoring
einsetzbar.

### Parameter

| Parameter | Bereich | Default | Bedeutung |
|---|---|---|---|
| Mode | Off / Hunt / Dynamic | Hunt | Betriebsart |
| Sensitivity | 3–24 dB | 9 dB | Wie weit muss ein Peak über dem lokalen Spektralboden liegen? |
| Notches | 1–12 | 8 | Maximale Anzahl Kerbfilter |
| Depth | 3–36 dB | 18 dB | Maximale Absenkung pro Notch (adaptiv 80–100 %) |
| Width (Q) | 2–48 | 16 | Höher = schmaler; bei Auto Q die Obergrenze |
| Attack | 10–2000 ms | 250 ms | Wie lange muss ein Peak bestehen? |
| Release | 0,1–20 s | 2,5 s | Nur im Dynamic-Modus |
| Low / High | 30–800 Hz / 2–20 kHz | 40 Hz / 16 kHz | Analyse- und Filterbereich |
| Music Guard | an/aus | an | Harmonischen-/Wachstums-/Pegelprüfung |
| AI Assist | an/aus | an | ResonanceAI-Bandmaske als zusätzliche Evidenz |
| Auto Q | an/aus | an | Q aus der gemessenen Peak-Breite (4096-FFT) |

Bei Rückkopplung im Bass (z. B. Kontrabass, Bass-Amp) ggf. Low auf 60–80 Hz
setzen, damit musikalische Grundtöne nicht als Feedback gelten.

---

## ResonanceAI (KI-Stufe)

**Architektur**

* Der DSP erzeugt alle 256 Samples ein 64-Band-Spektrumpatch (256 Frames,
  band-major, letzter Frame = aktuell) und veröffentlicht es über einen Seqlock.
* Ein Worker-Thread (`juce::Thread`) kopiert den Patch, führt das Modell aus und
  postet eine Bandmaske (0…1) über eine lock-free SPSC-Queue zurück.
* Die Engine liest die Maske im Analyseframe. Nur wenn sie jünger als 500 ms ist,
  erhöht sie die Prominenz um bis zu +8 dB; ab 70 % Konfidenz darf sie den
  Music Guard überstimmen. Ein Veto („Maske sagt kein Feedback“) gibt es bewusst
  nicht – ein unvalidiertes Modell darf keinen echten Howl unterdrücken.
* Modell-Laden und Warm-up passieren beim Plugin-Start, nie im Audio-Callback.

**Modell-Vertrag**

| | |
|---|---|
| Eingang | `spectrogram` float32 [1, 1, 64, 256], normalisierte dB (0…1) |
| Ausgänge | `mask` [1, 64], `clip` [1, 1] |
| Modell | CNN + GRU, ca. 1,21 Mio. Parameter, FP16, < 10 MB |
| Laufzeit | Core ML, CPU + Neural Engine (umschaltbar), Fallback Built-in |

**Modell installieren**

```bash
mkdir -p ~/Library/Application\ Support/ResonanceAI
cp -R ResonanceFeedback.mlmodelc ~/Library/Application\ Support/ResonanceAI/
```

Alternativ `RESONANCE_AI_MODEL=/pfad/ResonanceFeedback.mlmodelc` setzen. Die
Statuszeile zeigt `CORE ML` bzw. `BUILTIN`; `RESONANCE_AI_COMPUTE_UNITS=1|2|3`
wählt CPU / CPU+NE / alle Einheiten für den Benchmark.

**Training** (PyTorch → Core ML) ist in [training/README.md](training/README.md)
beschrieben. **Es ist noch kein trainiertes Modell im Repository** – Pipeline,
Vertrag und Fallback sind fertig; das Training braucht echte Raum- und
Mikrofonaufnahmen und läuft auf dem Mac.

---

## Tests

Der DSP-Kern ist framework-frei und lässt sich überall bauen:

```bash
g++ -O2 -std=c++20 -I Source/dsp -I Source/ai \
    tests/dsp_test.cpp Source/dsp/DeFeedbackEngine.cpp Source/ai/MaskModel.cpp \
    -o defb_tests
./defb_tests
```

Geprüft werden unter anderem:

* FFT-Amplitude und Fenster-Normierung
* bit-exakter Bypass
* eine Koppelfrequenz bei 1 kHz, die exponentiell aufbaut → Notch sitzt auf
  ±25 Hz, der Ton wird um **17,0 dB** abgesenkt, das Programm bleibt erhalten
* drei gleichzeitige Howls (350 / 1450 / 3200 Hz) → drei Notches
* 12 s realistisches Programmmaterial (Oberton-Instrumente, Akkordwechsel,
  Drums) → **keine** Fehlauslösung
* Dynamic-Modus gibt den Notch frei, wenn die Kopplung weg ist
* 4096er-Bestätigung: 1237,4 Hz auf **0,3 Hz** genau, Auto-Q im Bereich 6–32
* Extremwerte (12 Notches, −36 dB, Q 48, 96 kHz) bleiben stabil und endlich
* 64 × 256-Patch: Revision, Wertebereich 0…1, Signalanteil
* Masken-Vertrag: frische Maske wird übernommen, Modell und Konfidenz gemeldet
* Built-in-Modell: Stille → niedrige, schmaler Peak → hohe Konfidenz
* KI-Maske setzt eine gehaltene Resonanz, die der DSP allein verwirft
* Built-in-Maske über den Worker-Pfad auf 12 s Musik → **keine** Fehlauslösung
* Reset entfernt alle Notches

Aktueller Stand: **37 Checks, 0 Fehler**.

---

## Wie es intern funktioniert

`Source/dsp/DeFeedbackEngine.*` ist der komplette DSP-Algorithmus, ohne JUCE.
`Source/ai/*` enthält den Modell-Vertrag, die Built-in-Maske, die Lock-free-Queue
und die Core-ML-Bridge. `Source/PluginProcessor.*` ist die Host-Anbindung
(Parameter, Busse, State, AI-Worker), `Source/PluginEditor.*` die Oberfläche.

Signalfluss pro Block:

```
Eingang ──┬─► 2048-FFT (nur lesen) ─► Kandidaten ─► Tracker ─► Notch-Ziele
          │        │                                     ▲
          │        └─► 64×256-Patch (seqlock) ─► AI-Worker ─► Bandmaske (SPSC)
          └─► Kerbfilter (max. 12) ─► Ausgang
```

Der Audio-Thread alloziert nicht, wartet nie auf die KI und nimmt keine Locks:
Der Patch wird über einen Seqlock veröffentlicht, die Maske kommt über eine
lock-free Single-Producer/Single-Consumer-Queue zurück. Ist die Maske älter als
500 ms, arbeitet die Engine unverändert rein DSP-basiert.

Die Analyse läuft bewusst **vor** den Filtern auf dem echten Eingangssignal:
Das Plugin „hört" die Kopplung also weiter, auch wenn der Notch sie schon
absenkt. Deshalb bleibt der Notch im Hunt-Modus stabil stehen, statt
endlos auf- und zuzumachen.

---

## Grenzen

* Ein einzelner, sehr lauter, gehaltener Sinus (z. B. ein reiner Synth-Pad ohne
  Obertöne) ist physikalisch nicht von einem Howl zu unterscheiden. Music Guard,
  Sensitivität und der Frequenzbereich sind die Stellschrauben dagegen.
* Zwei Howls, die enger als ca. 1/40 Oktave beieinander liegen, teilen sich
  einen Notch.
* Das Plugin bekämpft die **Symp­tome** an der Insert-Stelle. Mikrofonierung,
  Monitor-EQ und Gain-Struktur bleiben die eigentliche Lösung.
* Audio Units lassen sich nur auf macOS bauen. In der Entwicklungsumgebung dieses
  Projekts (Linux-Container) wurde der DSP-Kern vollständig kompiliert und
  getestet; ein AU-Bundle wurde dort nicht gelinkt, dafür ist Xcode nötig. Die
  Core-ML-Bridge ist macOS-only und wurde hier nur reviewt, nicht kompiliert.
* Ohne installiertes, trainiertes Modell arbeitet die KI-Stufe mit dem
  Built-in-Scorer (Schmalbandigkeit + Wachstum). Der eigentliche Gewinn
  (Gesang vs. Feedback) entsteht erst mit dem trainierten Modell.
* Die Performance-Ziele (Inferenz p95 < 10 ms, < 150 MB, < 10 % eines Kerns,
  keine Dropouts, Latenz in Logic) sind **nicht gemessen**. Die GUI wurde nie
  gerendert, weil in dieser Umgebung kein macOS-Display verfügbar ist.

## Lizenz

MIT (siehe `LICENSE`). JUCE steht unter der AGPLv3 bzw. einer kommerziellen
JUCE-Lizenz; für eine kommerzielle Veröffentlichung des Plugins ist eine
JUCE-Lizenz erforderlich.
