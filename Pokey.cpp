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
 
#include "Pokey.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>

static uint8_t P4[15], P5[31], P9[511];
static uint8_t* P17 = nullptr;
static bool s_built = false;

bool Pokey::begin() {
  if (!s_built) {
    uint32_t r = 0xF;   for (int i = 0; i < 15; i++)  { P4[i] = r & 1; r = (r >> 1) | ((((r) ^ (r >> 1)) & 1) << 3); }
    r = 0x1F;           for (int i = 0; i < 31; i++)  { P5[i] = r & 1; r = (r >> 1) | ((((r) ^ (r >> 2)) & 1) << 4); }
    r = 0x1FF;          for (int i = 0; i < 511; i++) { P9[i] = r & 1; r = (r >> 1) | ((((r) ^ (r >> 4)) & 1) << 8); }
    s_built = true;
  }
  if (!P17) {
    P17 = (uint8_t*)calloc(16384, 1);
    if (!P17) return false;
    uint32_t r = 0x1FFFF;
    for (int i = 0; i < 131071; i++) { if (r & 1) P17[i >> 3] |= (1 << (i & 7)); r = (r >> 1) | ((((r) ^ (r >> 5)) & 1) << 16); }
  }
  reset();
  return true;
}
void Pokey::end() { if (P17) { free(P17); P17 = nullptr; } }

void Pokey::reset() {
  memset(audf, 0, 4); memset(audc, 0, 4); audctl = 0; irqen = 0; irqst = 0xFF;
  for (int i = 0; i < 4; i++) { outv[i] = false; per[i] = cnt[i] = 1; }
  b4 = b5 = b9 = b17 = 0; curT = 0;
  recalc(); for (int i = 0; i < 4; i++) cnt[i] = per[i];
}

void Pokey::recalc() {
  int base = (audctl & 1) ? 114 : 28;
  int c0 = (audctl & 0x40) ? 1 : base, c2 = (audctl & 0x20) ? 1 : base;
  auto p8 = [](int f, int clk) { return clk == 1 ? f + 4 : (f + 1) * clk; };
  if (audctl & 0x10) { int v = (audf[1] << 8) | audf[0]; per[1] = (c0 == 1) ? v + 7 : (v + 1) * c0; per[0] = 1; }
  else { per[0] = p8(audf[0], c0); per[1] = p8(audf[1], base); }
  if (audctl & 0x08) { int v = (audf[3] << 8) | audf[2]; per[3] = (c2 == 1) ? v + 7 : (v + 1) * c2; per[2] = 1; }
  else { per[2] = p8(audf[2], c2); per[3] = p8(audf[3], base); }
  for (int i = 0; i < 4; i++) if (cnt[i] > per[i]) cnt[i] = per[i];
}

void Pokey::write(uint8_t r, uint8_t v) {
  switch (r & 0x0F) {
    case 0: case 2: case 4: case 6: audf[(r & 7) >> 1] = v; recalc(); break;
    case 1: case 3: case 5: case 7: audc[(r & 7) >> 1] = v; break;
    case 8: audctl = v; recalc(); break;
    case 9: for (int i = 0; i < 4; i++) cnt[i] = per[i]; break;       // STIMER
    case 14: irqen = v; irqst |= ~v; break;
    default: break;
  }
}

uint8_t Pokey::read(uint8_t r) {
  r &= 0x0F;
  if (r < 8) return 228;                       // POT
  switch (r) {
    case 8: return 0;
    case 10: {                                  // RANDOM
      int rv = 0;
      if (audctl & 0x80) { int i = (b9 + curT) % 511; for (int k = 0; k < 8; k++) rv |= P9[(i + k) % 511] << k; }
      else if (P17) { int i = (b17 + curT) % 131071; for (int k = 0; k < 8; k++) { int j = (i + k) % 131071; rv |= ((P17[j >> 3] >> (j & 7)) & 1) << k; } }
      return (uint8_t)~rv;
    }
    case 14: return irqst;
    case 15: return 0xFF;
    default: return 0xFF;
  }
}

int Pokey::advance(int n) {
  int total = 0;
  for (int i = 0; i < 4; i++) {
    if ((i == 0 && (audctl & 0x10)) || (i == 2 && (audctl & 0x08))) continue;
    const int vol = audc[i] & 15; const bool vo = audc[i] & 0x10;
    const bool ultra = !vo && per[i] < POKEY_ULTRA_PER;      // oltre la banda audio: contributo nullo (evita aliasing)
    int rem = n, c = cnt[i], acc = 0;
    int lvl = vo ? vol * 2 : ultra ? 0 : (outv[i] ? vol * 2 : 0);
    while (rem >= c) {
      acc += lvl * c; rem -= c; c = per[i];
      const int off = n - rem;
      if (!vo && !ultra) {
        const uint8_t d = audc[i] & 0xE0;
        bool run = true;
        if (!(d & 0x80)) run = P5[(b5 + off) % 31];
        if (run) {
          if (d == 0x00 || d == 0x80) {
            if (audctl & 0x80) outv[i] = P9[(b9 + off) % 511];
            else if (P17) { int j = (b17 + off) % 131071; outv[i] = (P17[j >> 3] >> (j & 7)) & 1; }
          } else if (d == 0x40 || d == 0xC0) outv[i] = P4[(b4 + off) % 15];
          else outv[i] = !outv[i];
        }
        lvl = outv[i] ? vol * 2 : 0;
      }
      const int im = (i == 0) ? 1 : (i == 1) ? 2 : (i == 3) ? 4 : 0;
      if (im && (irqen & im)) irqst &= ~im;
    }
    cnt[i] = c - rem; acc += lvl * rem;
    total += acc;
  }
  b4 = (b4 + n) % 15; b5 = (b5 + n) % 31; b9 = (b9 + n) % 511; b17 = (b17 + n) % 131071;
  curT = 0;
  return total;
}
