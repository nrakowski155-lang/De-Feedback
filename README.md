# De-Feedback

Adaptive Rückkopplungs-Unterdrückung („De-Feedback") als **Audio Unit (AU)** für
Logic Pro, plus VST3 und Standalone-App. Das Plugin hört dem Signal zu, erkennt
pfeifende Koppelfrequenzen und setzt genau dort schmale, tiefe Kerbfilter – ohne
den Rest des Programms anzufassen.

Vorbild ist der Arbeitsablauf von *Waves Feedback Hunter* bzw. *De-Feedback*:
live einschleifen, Monitoranlage ausklingeln lassen, Notches setzen, fertig.

Die Oberfläche zeigt das Live-Spektrum, markiert jede gesetzte Frequenz mit
Frequenzlabel und bietet sechs Regler plus Frequenzbereich, Music-Guard-Schalter
und einen CLEAR-Button.

---

## Was das Plugin macht

**Analyse**

* 2048-Punkt-FFT, 75 % Überlappung (~21 ms Auflösung, 23 Hz Bin-Breite bei 48 kHz)
* lokaler Spektralboden als 1/3-Oktav-Mittelwert, damit auch im vollen Mix ein
  schmaler Peak auffällt
* Peak-Picking mit parabolischer Interpolation (Frequenzgenauigkeit ~1–2 Hz)
* Prüfung auf **Schmalbandigkeit** (ein Howl ist wenige Hz breit)
* **Harmonischen-Test**: hat der Peak Oberton-Verwandte (f/2, f/3, 2f, 3f), ist
  es mit hoher Wahrscheinlichkeit ein Instrument und kein Feedback
* **Wachstums-Test**: ein Howl baut sich über die Zeit auf, ein gehaltener Ton
  nicht; gemessen über die halbe Attack-Zeit
* **Pegel-Gate** gegen Rauschen und **Music Guard** (Dominiert der Peak das
  Programm? Liegt er über dem Programm-RMS?)

**Unterdrückung**

* bis zu 16 Kerbfilter, gekoppelt stereo (links = rechts)
* RBJ-Peaking-Biquads, Q einstellbar (schmal bis breit)
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
Universal-Binary (arm64 + x86_64), führt die DSP-Tests aus, prüft das AU mit
`auval` und lädt zwei ZIPs als Artifact hoch:

* `De-Feedback-AU-macOS-universal.zip`
* `De-Feedback-VST3-macOS-universal.zip`

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

Im letzten Abschnitt gemessen: **17,9 dB** bei 640 Hz, **18,3 dB** bei 1180 Hz,
**18,2 dB** bei 1730 Hz. Das Musikbett bleibt erhalten (Peak trocken 0,762,
nass 0,604). Die ersten Sekunden des nassen Teils zeigen bewusst den Hunt-Vorgang,
bis die Notches greifen.

Neu rendern:

```bash
g++ -O2 -std=c++17 -I Source/dsp demo/render_demo.cpp \
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
| Notches | 1–16 | 8 | Maximale Anzahl Kerbfilter |
| Depth | 3–36 dB | 18 dB | Absenkung pro Notch |
| Width (Q) | 2–48 | 16 | Höher = schmaler |
| Attack | 10–2000 ms | 250 ms | Wie lange muss ein Peak bestehen? |
| Release | 0,1–20 s | 2,5 s | Nur im Dynamic-Modus |
| Low / High | 30–800 Hz / 2–20 kHz | 40 Hz / 16 kHz | Analyse- und Filterbereich |
| Music Guard | an/aus | an | Harmonischen-/Wachstums-/Pegelprüfung |

Bei Rückkopplung im Bass (z. B. Kontrabass, Bass-Amp) ggf. Low auf 60–80 Hz
setzen, damit musikalische Grundtöne nicht als Feedback gelten.

---

## Tests

Der DSP-Kern ist framework-frei und lässt sich überall bauen:

```bash
g++ -O2 -std=c++17 -I Source/dsp \
    tests/dsp_test.cpp Source/dsp/DeFeedbackEngine.cpp -o defb_tests
./defb_tests
```

Geprüft werden unter anderem:

* FFT-Amplitude und Fenster-Normierung
* bit-exakter Bypass
* eine Koppelfrequenz bei 1 kHz, die exponentiell aufbaut → Notch sitzt auf
  ±25 Hz, der Ton wird um **17,9 dB** abgesenkt, das Programm bleibt erhalten
* drei gleichzeitige Howls (350 / 1450 / 3200 Hz) → drei Notches
* 12 s realistisches Programmmaterial (Oberton-Instrumente, Akkordwechsel,
  Drums) → **keine** Fehlauslösung
* Dynamic-Modus gibt den Notch frei, wenn die Kopplung weg ist
* Extremwerte (16 Notches, −36 dB, Q 48, 96 kHz) bleiben stabil und endlich
* Reset entfernt alle Notches

Aktueller Stand: 15 Checks, 0 Fehler.

---

## Wie es intern funktioniert

`Source/dsp/DeFeedbackEngine.*` ist der komplette Algorithmus, ohne JUCE.
`Source/PluginProcessor.*` ist nur die Host-Anbindung (Parameter, Busse, State),
`Source/PluginEditor.*` die Oberfläche.

Signalfluss pro Block:

```
Eingang ──┬─► FFT-Analyse (nur lesen) ─► Kandidaten ─► Tracker ─► Notch-Ziele
          └─► Kerbfilter (max. 16) ─► Ausgang
```

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
  getestet und die JUCE-Plugin-Quellen gegen JUCE 8.0.9 übersetzt
  (Syntax-/Semantikprüfung); ein AU-Bundle wurde dort nicht gelinkt, dafür ist
  Xcode nötig.

## Lizenz

MIT (siehe `LICENSE`). JUCE steht unter der AGPLv3 bzw. einer kommerziellen
JUCE-Lizenz; für eine kommerzielle Veröffentlichung des Plugins ist eine
JUCE-Lizenz erforderlich.
