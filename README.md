# Pi7800

Pi7800 is an Atari 7800 emulator developed for the Raspberry Pi Pico (RP2040) using the Arduino IDE environment (arduino-pico core).
The project integrates support for the ILI9341 SPI display, game loading from SD or flash, joystick / PS2 keyboard input and i2s audio output.

# 🚀 Features

* Core: Emulation of the Sally 6502C CPU (NMOS 6502: all documented opcodes, the most common undocumented ones — SLO/RLA/SRE/RRA/SAX/LAX/DCP/ISC — and decimal mode), the MARIA graphics chip (Line RAM modelled as 160 5-bit cells with DMA timing based on ProSystem 1.3; DLL/DLI handled as NMI, holey DMA, 8/16-line zones) plus TIA audio/input registers and RIOT (MOS 6532) RAM-I/O-timer. The console BIOS is not required (HLE): the CPU boots straight from the cartridge reset vector. Cartridge POKEY included for the audio-enhanced titles (Ballblazer, Commando, ...): 4 channels with polynomial counters, timers with IRQ and RANDOM, adapted from an 800XL POKEY emulation.
* Display: ILI9341 320×240 SPI with DMA (TFT_eSPI); 256-entry Atari palette generated in YIQ and sent as RGB565. Dual-core architecture: core0 runs the emulator and marks the 8-line chunks of the framebuffer that changed, core1 converts and pushes only those chunks to the panel — it never blocks core0.
* Storage: Loading .a78 / .bin / .rom ROMs from MicroSD (dedicated SPI1 bus) or from flash via LittleFS. Supported mappers: STANDARD 16/32/48 KB, SUPERGAME (16 KB banks, optional 16 KB cartridge RAM, LRU bank pool), SUPERGAME9 (9+ bank variant: Alien Brigade, Crossbow), ACTIVISION 78AC 128 KB (Double Dragon, Rampage — both No-Intro linear dumps and MAME dumps with the 8K halves swapped in file are normalized on the fly) and POKEY cartridges ($4000 / $0450 / $0440). Automatic fixups for the well-known broken .a78 headers (CRC- and title-based). Rejected types: Absolute 78AB, YM2151, SOUPER, bankset/halt-RAM, POKEY@800, banked/mirror RAM.
* Audio: i2s sound generation through a MAX98357 amplifier. TIA (2 channels) and POKEY (4 channels) are mixed and duplicated on L/R, sampled at the native line rate (~31.5 kHz: 2 samples per scanline) and paced in real time by the I2S ring buffer.
* Input: One joystick port (via GPIO) with two independent action buttons (Pro-Line/CX78 style), console buttons (RESET / SELECT / PAUSE) and a PS/2 keyboard: F2 RESET, F3 SELECT, F4 PAUSE, F5 machine reset, F12 back to browser, arrows + Enter/Space as joystick/fire.
* Browser: Alphabetical game list with 200×150 JPG cover art (or noImage.jpg fallback) and file-type tag (A78/ROM), up to 200 ROMs from SD and flash.

# 🛠️ Hardware Requirements

To build the project, you will need the following components:

* Microcontroller: Raspberry Pi Pico (RP2040).
* Display: 2.8" ILI9341 SPI (320×240).
* Audio:
  + MAX98357 amplifier.
  + 3W / 4–8Ω speaker
* Input:
  + 1× directional joystick board with 5-way switch + 2 buttons (FIRE = left button, FIRE2 = right button).
  + Optional console buttons: RESET, SELECT, PAUSE.
  + Optional PS/2 keyboard (mini-DIN socket or bare connector).
* Storage: a MicroSD module (on SPI1) or the Pico's internal flash (LittleFS).

# 🔌 Wiring

The Raspberry Pi Pico is the core of the system. Below are the detailed connections, divided by module.

## 1. ILI9341 Display

The display pins are not fixed by the sketch: they are configured in TFT_eSPI's User_Setup.h.
The suggested wiring below matches the Pi2600 / PiSMS reference hardware (same modules and pins):

| Component | Module Pin | Pico Pin (GPIO) | Note |
| --- | --- | --- | --- |
| Display | VCC | VSYS | Powered directly from Pico |
|  | GND | GND |  |
|  | SDO (MISO) | GP16 | SPI0 |
|  | LED | 3V3 | Backlight |
|  | SCK | GP18 | SPI0 |
|  | SDI (MOSI) | GP19 | SPI0 |
|  | DC | GP21 |  |
|  | RESET | GP20 |  |
|  | CS | GP17 |  |

## 2. MicroSD Card (SPI1)

The ROM card runs on its own SPI1 bus, so it does not share the display bus:

| Component | Module Pin | Pico Pin (GPIO) | Note |
| --- | --- | --- | --- |
| SD Card | SD_CS | GP13 | Chip select on SPI1 |
|  | SD_SCK | GP10 | SPI1 |
|  | SD_MOSI | GP11 | SPI1 |
|  | SD_MISO | GP12 | SPI1 |

## 3. Audio (i2s Amplifier)

| Component | Module Pin | Pico Pin (GPIO) | Note |
| --- | --- | --- | --- |
| Ampl. | VCC | VBUS | 5V |
|  | GND | GND |  |
|  | BCLK | GP26 |  |
|  | LRC | GP27 | LRC is BCLK+1 (PIO limitation) |
|  | DIN | GP28 |  |

## 4. Joystick, Console Buttons and PS/2 Keyboard

Joystick and buttons use internal pull-up resistors.
Each button closes the contact to GND.

| Function | Pico Pin (GPIO) | Note |
| --- | --- | --- |
| Joystick UP | GP2 |  |
| Joystick DOWN | GP3 |  |
| Joystick LEFT | GP4 |  |
| Joystick RIGHT | GP5 |  |
| FIRE (left button) | GP6 |  |
| FIRE2 (right button) | GP7 | Pro-Line/CX78 style |
| Console RESET | GP8 | Optional (-1 = unused) |
| Console SELECT | GP1 | Optional (-1 = unused) |
| Console PAUSE | GP0 | Optional (-1 = unused) |
| PS/2 CLK | GP15 | Keyboard clock, falling-edge interrupt |
| PS/2 DATA | GP14 | Keyboard data |
| Keyboard VCC | VBUS | 5V, as per PS/2 spec |
| Keyboard GND | GND |  |

# 📝 Technical Notes

* Power:
  Pico powered via USB.
  Audio amplifier requires 5V (VBUS).
* Dual-core: core0 emulates the console (Sally CPU + MARIA + TIA/RIOT + cartridge POKEY + audio samples) and never waits for core1; core1 only converts the framebuffer chunks marked dirty and sends them via DMA. The browser and menus draw through the same TFT object, but only when no frame is in flight.
* No BIOS (HLE): the emulator does not need the 7800 BIOS. The High Score Cart area is left as open bus ($FF): HSC-aware games (Ms. Pac-Man, Galaga) look for the $C6/$FE signature at $3900/$3904 and must not find it.
* Video standards: ATARI_PAL in config.h selects the base timing (0 = NTSC: 262 lines, CPU 1.79 MHz; 1 = PAL: 312 lines, CPU 1.77 MHz; 1 CPU cycle = 4 MARIA clocks @ 7.16 MHz). At load time the PAL bit of the .a78 header (byte 57) is honored: PAL games run at 312 lines/frame (~50 fps). The visible window (240 lines) is rendered into the 320×240 framebuffer.
* Audio pacing: the emulator runs as fast as it can and blocks only when the I2S ring buffer is full — this is what locks emulation speed to real time. Native sample rate is 31540 Hz (NTSC) / 31112 Hz (PAL), 2 samples per scanline.
* Frameskip: VIDEO_FRAMESKIP in config.h — 0 = render every frame, N = render 1 frame out of N+1. A frame is also skipped automatically whenever core1 is still sending the previous one.
* Cartridge mapping: standard cartridges are loaded flat into RAM; SuperGame/Activision cartridges are served by an LRU pool of 16 KB banks (A7800_CART_POOL_BANKS, default 6) streamed from SD/flash, so even 272 KB ROMs run without loading the whole file into RAM. Activision 78AC bank switching happens by writing to $FF80–$FF87 (bank number in the low nibble of the address). Known broken headers are fixed automatically at load (e.g. Double Dragon/Rampage dumps with a SuperGame header are forced to 78AC; Summer/Winter Games get their missing 16 KB cartridge RAM flag).
* TFT_eSPI Configuration:
  Replace User_Setup.h with:
  * Driver: ILI9341
  * DMA: Enabled
  The sketch sets the display rotation itself.
  If you notice graphical glitches, reduce the SPI frequency.
* Debug: set A7800_DEBUG 1 to print on serial, once per second: emulated/drawn frames per second, core0 load, TFT send time and chunk count, plus the full machine state (CPU registers, MARIA DLL status, cartridge mapper info). If the CPU jams, an "autopsy" of the last executed PCs and cartridge bank switches is printed.
* Serial console: 115200 baud — ls | load \<path\> | status | unload.

# 📂 Software Setup

The project is developed for Arduino IDE using the RP2040 core (arduino-pico).
In the Tools menu: Board = Raspberry Pi Pico, Optimize = -O3 (recommended).

## Required Libraries

Make sure you have installed:

* TFT_eSPI (configured for ILI9341 on Pico)
* SD (for the MicroSD on SPI1)
* TJpg_Decoder (JPG cover art in the browser)
* LittleFS and I2S (bundled with the arduino-pico core)

## File System Structure

The emulator looks for ROMs inside the /Pi7800/games directory, both on the MicroSD and in the Pico's flash filesystem.
Organize your FS like this:

```
Pi7800/
└─ games/
   ├─ game1.a78
   ├─ game1.jpg
   ├─ game2.bin
   └─ noImage.jpg
```

* Supported extensions: .a78 (128-byte header, versions 1–4), .bin and .rom (raw dumps: 16/32/48 KB standard, ≥ 64 KB multiple of 16 KB = SuperGame / SuperGame9)
* Optional cover art: a 200×150 non-progressive .jpg with the same base name as the ROM; noImage.jpg is shown as fallback
* Up to 200 ROMs are listed in the browser (MAX_SORTED_GAMES in config.h)
* Optional: uncomment A7800_AUTOLOAD in config.h to boot directly into a ROM

## Using LittleFS Instead of a Physical SD Card

Instead of using a physical SD card, you can store the whole Pi7800 folder directly inside the Pico's onboard flash memory.
To do this, create a /data folder next to your sketch with the same structure (data/Pi7800/games/...) and use the LittleFS Upload Tool for Arduino IDE, which uploads the contents of your local /data folder into the Pico's flash filesystem.
If a MicroSD is present it takes priority; LittleFS is used as automatic fallback.

Official documentation:
Arduino-Pico LittleFS guide:
[https://arduino-pico.readthedocs.io/en/latest/filesystems.html](https://arduino-pico.readthedocs.io/en/latest/filesystems.html)
LittleFS Upload Tool (arduino-littlefs-upload):
[https://github.com/earlephilhower/arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)

# 🎮 Usage

1. **Startup**: On boot, the ROM browser appears ("Pi7800", "@IV ATARI 7800 emulator"), listing alphabetically the ROMs found on SD and flash with their cover art (type tag A78/ROM when no preview is available).
2. **Navigation**: Use the joystick (or the keyboard arrows) to move between ROMs:
   **GAMES BROWSER**: LEFT/RIGHT = previous/next, DOWN = jump 10 entries forward. Press FIRE (or Enter/Space) to load the selected game and start the emulation. FIRE2 works as FIRE too.
3. **In game**:
   * FIRE / FIRE2 = the two action buttons of the Pro-Line/CX78 controller (both respond on one-button games, like the real two-pole buttons; to swap left/right assignments edit Input.cpp)
   * Physical RESET / SELECT / PAUSE buttons, or F2 / F3 / F4 on the PS/2 keyboard
   * F5 = machine reset (warm restart)
   * Hold RESET + SELECT for about 1 second, or press F12, to go back to the ROM browser
4. **Serial console** (115200 baud): ls (list ROMs), load \<path\> (load and start), status (loader/machine state), unload (eject cartridge).

# ⚡ Performance & Overclock

To achieve acceptable emulation speed, the Raspberry Pi Pico is pushed beyond its factory specifications.
In config.h:

```
#define PICO_CLOCK_KHZ    300000              // Overclock to 300MHz
#define PICO_VREG_VOLTAGE VREG_VOLTAGE_1_30   // vreg 1.30V
```

The clock is changed at boot, before I2S / SPI are configured (core1 waits for it before initializing the TFT), with the voltage regulator raised to 1.30V.
clk_peri is derived from clk_sys (PICO_PERI_FROM_SYS), otherwise the SPI peripheral would be capped at 24 MHz.
This frequency allows:

* Real-time processing of CPU cycles, MARIA rendering and audio samples (TIA + POKEY)
* Hot emulation code running from SRAM (ATARI_RAMFUNC), avoiding flash XIP cache misses
* Palette conversion and framebuffer push done in .time_critical sections, with partial (8-line chunk) panel updates to cut DMA time

You may still notice slowdowns in some games, graphical glitches, or audio ticks due to timing desynchronization.

# ⚠️ Disclaimer

This project is provided "as is", for hobby and experimental use.
Overclock: May reduce microcontroller lifespan or cause thermal instability.
Hardware: The author is not responsible for damage to components (Pico, display, speaker, keyboard), overheating, or malfunctions caused by incorrect wiring or use of the provided software.
Risks: Any hardware or software modification is performed at the user's own risk.

# 📜 License

Pi7800 is released under the GNU General Public License v3.0. See the LICENSE file for details.
Author: Ivan Vettori ivanvddr

