# hatari-fork

**English** | [Polski](#polski)

A fork of the [Hatari](https://github.com/hatari/hatari) Atari ST/STE/TT/Falcon emulator
with a few small usability improvements. The original project readme is kept in `readme.txt`.

Based on upstream `hatari/hatari`, commit `74fb997`
("Use "IPF_Caps_" prefix for all global Capslib-related functions").

## Changes compared to the original

### 1. Volume control (0–20)
- New `nVolume` option in the `[Sound]` section of `hatari.cfg` (default `20` = maximum).
- New `<` / `>` buttons in the SDL GUI **Sound** dialog.
- Volume is applied in the audio callback (`src/sdl/audio.c`).
- The value is saved to the config file right after leaving the Sound dialog
  (`Configuration_SaveVolume()`) without rewriting the rest of the configuration.
- Out-of-range values are reset to 20.

### 2. Adjustable "Fast floppy" speed factor
- New `nFastFloppyFactor` option in the `[Floppy]` section (default `100`).
- The **Floppy disks** dialog has a speed selector (`<` / `>`) with steps:
  x1, x2, x5, x10, x20, x50, x100, x200, x500, x1000, x2000, x5000.
- The speed-up is applied consistently in the FDC <-> CPU cycle conversion (`src/fdc.c`)
  instead of only dividing the timer at start, so rotation, index pulses, timers and
  byte transfers all scale together.
- The value is saved to the config file (`Configuration_SaveFastFloppyFactor()`).
- Values below 1 are reset to 1.

### 3. Ctrl+Alt as shortcut modifier
- `Ctrl+Alt` (either side) works as the emulator shortcut modifier (`src/sdl/keymap.c`),
  which helps on laptops without an AltGr key.

### Modified files
`src/configuration.c`, `src/includes/configuration.h`, `src/fdc.c`, `src/sdl/audio.c`,
`src/sdl/keymap.c`, `src/gui-sdl/dlgSound.c`, `src/gui-sdl/dlgFloppy.c`.

### Building
As upstream (CMake): `cmake -S . -B build && cmake --build build`

License: GPL v2 or later, same as the original (`gpl.txt`).

---

## Polski

Fork emulatora Atari ST/STE/TT/Falcon [Hatari](https://github.com/hatari/hatari)
z kilkoma drobnymi poprawkami użytkowymi. Oryginalny README projektu jest w pliku `readme.txt`.

Baza: upstream `hatari/hatari`, commit `74fb997`
("Use "IPF_Caps_" prefix for all global Capslib-related functions").

## Zmiany względem oryginału

### 1. Regulacja głośności (0–20)
- Nowa opcja `nVolume` w sekcji `[Sound]` pliku `hatari.cfg` (domyślnie `20` = maksimum).
- Nowe przyciski `<` / `>` w oknie **Sound** (GUI SDL).
- Głośność jest skalowana w callbacku audio (`src/sdl/audio.c`).
- Wartość jest zapisywana do pliku konfiguracji zaraz po wyjściu z okna Sound
  (`Configuration_SaveVolume()`), bez nadpisywania reszty konfiguracji.
- Wartości spoza zakresu są korygowane do 20.

### 2. Regulowany współczynnik "Fast floppy"
- Nowa opcja `nFastFloppyFactor` w sekcji `[Floppy]` (domyślnie `100`).
- Okno **Floppy disks** ma wybór prędkości (`<` / `>`) z krokami:
  x1, x2, x5, x10, x20, x50, x100, x200, x500, x1000, x2000, x5000.
- Przyspieszenie działa spójnie w konwersji cykli FDC <-> CPU (`src/fdc.c`),
  a nie tylko przez dzielenie przy starcie timera, więc rotacja, impulsy indeksu,
  timery i transfer bajtów skalują się razem.
- Wartość jest zapisywana do konfiguracji (`Configuration_SaveFastFloppyFactor()`).
- Wartości mniejsze niż 1 są korygowane do 1.

### 3. Ctrl+Alt jako modyfikator skrótów
- `Ctrl+Alt` (dowolna strona) działa jako modyfikator skrótów emulatora
  (`src/sdl/keymap.c`), co ułatwia pracę na laptopach bez klawisza AltGr.

### Zmienione pliki
`src/configuration.c`, `src/includes/configuration.h`, `src/fdc.c`, `src/sdl/audio.c`,
`src/sdl/keymap.c`, `src/gui-sdl/dlgSound.c`, `src/gui-sdl/dlgFloppy.c`.

### Budowanie
Jak w oryginale (CMake): `cmake -S . -B build && cmake --build build`

Licencja: GPL v2 lub nowsza, tak jak w oryginale (`gpl.txt`).
