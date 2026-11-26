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
 */
#include <Arduino.h>
#include "config.h"
#include "Input.h"

uint8_t g_swcha = 0xFF, g_fire0 = 0x80, g_inpt0 = 0x00, g_inpt1 = 0x00, g_swchb = 0xFF;
// riposo: g_fire0 = 0x80 (INPT4, 1-tasto, attivo-basso); g_inpt0/g_inpt1 = 0x00
// (modo 2 tasti, attivo-alto: 0x00 = NON premuto e' il valore di riposo corretto!)

// ---- coda eventi (ISR PS/2 -> loop core0) ----
static volatile InEvent q[16];
static volatile uint8_t qh = 0, qt = 0;
static void qpush(uint8_t type, uint8_t code, uint8_t down) {
  uint8_t n = (qh + 1) & 15; if (n == qt) return;
  q[qh].type = type; q[qh].code = code; q[qh].down = down; qh = n;
}
bool inputPoll(InEvent& e) {
  if (qt == qh) return false;
  e.type = q[qt].type; e.code = q[qt].code; e.down = q[qt].down; qt = (qt + 1) & 15; return true;
}

static volatile uint8_t navKb = 0xFF;
static uint8_t kbConsole = 0, btnConsole = 0;      // bit = premuto

// F2 RESET, F3 SELECT, F4 PAUSE (momentaneo), F5 reset della macchina, F12 torna al browser
static void onKey(uint8_t code, bool ext, bool down) {
  uint8_t nb = 0;
  if (ext) { if (code == 0x75) nb = 0x01; else if (code == 0x72) nb = 0x02; else if (code == 0x74) nb = 0x04; else if (code == 0x6B) nb = 0x08; }
  else if (code == 0x5A || code == 0x29) nb = 0x10;
  if (nb) { if (down) navKb &= ~nb; else navKb |= nb; }
  if (ext) return;
  if (code == 0x06) qpush(EV_RESET, 0, down);
  else if (code == 0x04) qpush(EV_SELECT, 0, down);
  else if (code == 0x0C) qpush(EV_PAUSE, 0, down);
  else if (code == 0x03 && down) qpush(EV_WARM, 0, 1);
  else if (code == 0x07 && down) qpush(EV_BROWSER, 0, 1);
}

// ---- ISR clock PS/2 ----
static volatile uint8_t bitn = 0; static volatile uint16_t shreg = 0; static volatile uint32_t lastUs = 0;
static bool fExt = false, fBreak = false;
static void ps2Isr() {
  uint32_t now = micros();
  if (now - lastUs > 1500) { bitn = 0; shreg = 0; }
  lastUs = now;
  int d = digitalRead(PIN_PS2_DATA);
  if (bitn == 0) { if (d) return; }
  else if (bitn <= 8) shreg |= (uint16_t)d << (bitn - 1);
  else if (bitn == 10) {
    uint8_t c = shreg & 0xFF; bitn = 0; shreg = 0;
    if (c == 0xE0) fExt = true;
    else if (c == 0xF0) fBreak = true;
    else if (c != 0xAA && c != 0xFA) { onKey(c, fExt, !fBreak); fExt = false; fBreak = false; }
    return;
  }
  bitn++;
}

void inputBegin() {
  const int pins[] = {PIN_JOY_UP, PIN_JOY_DOWN, PIN_JOY_LEFT, PIN_JOY_RIGHT, PIN_JOY_FIRE, PIN_JOY_FIRE2,
                       PIN_BTN_RESET, PIN_BTN_SELECT, PIN_BTN_PAUSE};
  for (int p : pins) if (p >= 0) pinMode(p, INPUT_PULLUP);
  pinMode(PIN_PS2_CLK, INPUT_PULLUP); pinMode(PIN_PS2_DATA, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_PS2_CLK), ps2Isr, FALLING);
}

void inputConsoleKb(uint8_t bit, bool down) { if (down) kbConsole |= 1 << bit; else kbConsole &= ~(1 << bit); }

void inputUpdate() {
  uint8_t a = 0xFF;                                   // giocatore 0: bit7 destra, 6 sinistra, 5 giu', 4 su
  if (!digitalRead(PIN_JOY_UP))    a &= ~0x10;
  if (!digitalRead(PIN_JOY_DOWN))  a &= ~0x20;
  if (!digitalRead(PIN_JOY_LEFT))  a &= ~0x40;
  if (!digitalRead(PIN_JOY_RIGHT)) a &= ~0x80;
  g_swcha = a;

  // Pulsanti, come il controller Pro-Line a 2 tasti: ogni tasto ha DUE poli, uno sulla
  // linea del pulsante singolo (INPT4, attiva-bassa) e uno sulla propria linea del modo
  // 2 tasti (INPT0/1, attiva-alta). FIRE = pulsante SINISTRO, FIRE2 = pulsante DESTRO:
  // per invertirli scambia le due assegnazioni qui sotto.
  g_fire0 = (digitalRead(PIN_JOY_FIRE) && digitalRead(PIN_JOY_FIRE2)) ? 0x80 : 0x00;  // INPT4: FIRE o FIRE2
  g_inpt1 = digitalRead(PIN_JOY_FIRE)  ? 0x00 : 0x80;   // INPT1 (P0 sinistro): 0x80 = PREMUTO (conv. invertita)
  g_inpt0 = digitalRead(PIN_JOY_FIRE2) ? 0x00 : 0x80;   // INPT0 (P0 destro):   0x80 = PREMUTO (conv. invertita)

  btnConsole = 0;
  if (PIN_BTN_RESET  >= 0 && !digitalRead(PIN_BTN_RESET))  btnConsole |= 1;
  if (PIN_BTN_SELECT >= 0 && !digitalRead(PIN_BTN_SELECT)) btnConsole |= 2;
  if (PIN_BTN_PAUSE  >= 0 && !digitalRead(PIN_BTN_PAUSE))  btnConsole |= 4;
  uint8_t c = kbConsole | btnConsole;
  uint8_t b = 0xFF;
  if (c & 1) b &= ~0x01;                              // RESET
  if (c & 2) b &= ~0x02;                              // SELECT
  if (c & 4) b &= ~0x08;                              // PAUSE
  g_swchb = b;
}

uint8_t inputNavState() {
  uint8_t s = 0xFF;
  if (!(g_swcha & 0x10)) s &= ~0x01;                  // su
  if (!(g_swcha & 0x20)) s &= ~0x02;                  // giu'
  if (!(g_swcha & 0x80)) s &= ~0x04;                  // destra
  if (!(g_swcha & 0x40)) s &= ~0x08;                  // sinistra
  if (!g_fire0)          s &= ~0x10;                  // fire (FIRE o FIRE2: basta g_fire0, che li ORa)
  return s & navKb;
}
