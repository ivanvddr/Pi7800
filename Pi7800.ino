/*
 * Questo file fa parte del progetto Pi7800.
 *
 * Copyright (C) 2026 Ivan Vettori
 *
 * Questo file è opera originale dell'autore e viene rilasciato
 * sotto licenza GNU General Public License v3.0.
 * Puoi ridistribuirlo e/o modificarlo secondo i termini della GPLv3.
 *
 * Questo programma è distribuito nella speranza che sia utile,
 * ma SENZA ALCUNA GARANZIA; senza neppure la garanzia implicita
 * di COMMERCIABILITÀ o IDONEITÀ PER UN PARTICOLARE SCOPO.
 * Vedi la licenza GPLv3 per maggiori dettagli.
 *
 *  Atari 7800 su Raspberry Pi Pico (RP2040) - Arduino IDE (core arduino-pico)
 *  core0: browser cartucce / emulazione (Sally 6502C + MARIA + TIA/RIOT [+ POKEY]) + audio + rendering
 *  core1: solo invio del framebuffer al TFT via DMA (non blocca mai core0)
 *
 *  Flusso (come PiSMS): BROWSER --FIRE--> carica la cartuccia e avvia l'emulazione
 *  Ritorno al browser: F12 oppure RESET+SELECT premuti 1 s.   Console: F2 RESET, F3 SELECT, F4 PAUSE, F5 reset macchina.
 */
#include <Arduino.h>
#include "config.h"
#include "A7800.h"
#include "Video.h"
#include "Audio.h"
#include "Input.h"
#include "A78Loader.h"
#include "GameBrowser.h"
#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

static A7800 machine;
static A78Loader loader;
static volatile bool canStartVideo = false;       // core1 aspetta la fine dell'overclock prima di inizializzare SPI/TFT
static void serialCommands();

enum AppState { APP_BROWSER, APP_EMU };
static AppState app = APP_BROWSER;
static GameBrowser browser(videoTFT(), &loader);
static int menuHold = 0;

static void waitVideoIdle() { while (g_fbState != 0) tight_loop_contents(); }
static void enterEmu() { waitVideoIdle(); g_fbDirty = FB_DIRTY_ALL; menuHold = 0; app = APP_EMU; }
static void showBrowser() { waitVideoIdle(); app = APP_BROWSER; browser.begin(); }

void setup() {
#if PICO_CLOCK_KHZ
  vreg_set_voltage(PICO_VREG_VOLTAGE);
  delay(10);
  set_sys_clock_khz(PICO_CLOCK_KHZ, true);
#endif
#if PICO_PERI_FROM_SYS
  { uint32_t f = clock_get_hz(clk_sys); clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, f, f); }
#endif
  __sync_synchronize();
  canStartVideo = true;
  Serial.begin(115200);
  inputBegin();
  machine.init();
  loader.setMachine(&machine);
  loader.initFilesystem(FSType::FS_SD, SD_CS);    // SD, con fallback su LittleFS
  machine.setHost(&loader);
  audioBegin();
  while (!g_videoReady) { tight_loop_contents(); }
#ifdef A7800_AUTOLOAD
  if (loader.loadROM(A7800_AUTOLOAD)) { enterEmu(); return; }
#endif
  showBrowser();
}

void loop() {
  inputUpdate();
  InEvent e;

  if (app == APP_BROWSER) {
    while (inputPoll(e)) {}
    if (browser.update()) {                                 // FIRE: cartuccia scelta
      String path = browser.getPrgSelected();
      if (loader.loadROM(path.c_str())) enterEmu();         // monta, riavvia la macchina e parte
      else browser.notify("LOAD ERROR!");
    }
    serialCommands();
    return;
  }

  while (inputPoll(e)) {
    switch (e.type) {
      case EV_RESET:   inputConsoleKb(0, e.down); break;
      case EV_SELECT:  inputConsoleKb(1, e.down); break;
      case EV_PAUSE:   inputConsoleKb(2, e.down); break;
      case EV_WARM:    machine.reset(); break;
      case EV_BROWSER: showBrowser(); return;
    }
  }
  if ((g_swchb & 0x03) == 0) { if (++menuHold >= MENU_BACK_FRAMES) { showBrowser(); return; } } else menuHold = 0;

  machine.runFrame();     // ~16.6 ms (NTSC): il ritmo lo da' il blocco su I2S (audioPush)
  serialCommands();
#if A7800_DEBUG
  static uint32_t t0 = 0;
  if (millis() - t0 >= 1000) {
    static char buf[700];
    uint32_t blocked = g_audioBlockedUs; g_audioBlockedUs = 0;
    Serial.printf("--- %lu ms | frame emulati/s=%lu disegnati/s=%lu (reale=%d) | carico core0 ~%lu%% ---\n",
                  (unsigned long)millis(), (unsigned long)machine.statTotal, (unsigned long)machine.statDrawn, ATARI_PAL ? 50 : 60,
                  (unsigned long)(blocked >= 1000000 ? 0 : 100 - blocked / 10000));
    Serial.printf("    TFT: invio=%lu us, blocchi=%lu/%d\n", (unsigned long)g_videoSendUs, (unsigned long)g_videoChunks, FB_CHUNKS);
    machine.statTotal = machine.statDrawn = 0;
    machine.debugState(buf, sizeof(buf));
    Serial.print(buf);
    t0 = millis();
  }
#endif
}

// Comandi sulla seriale: ls | load <percorso> | status | unload
static void serialCommands() {
  static char line[128]; static int n = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = ch; continue; }
    line[n] = 0; n = 0;
    if (!strcmp(line, "ls")) loader.listROMs();
    else if (!strncmp(line, "load ", 5)) { if (loader.loadROM(line + 5)) enterEmu(); }
    else if (!strcmp(line, "status")) loader.debugStatus();
    else if (!strcmp(line, "unload")) loader.unloadROM();
  }
}

void setup1() {
  while (!canStartVideo) { tight_loop_contents(); }
  videoBegin();
}
void loop1() { videoLoop(); }
