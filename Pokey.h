#pragma once
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
 * Pokey.h — POKEY ridotto per le cartucce 7800 (Ballblazer, Commando, ...): 4 canali audio con polinomiali,
 * timer 1/2/4 con IRQ e RANDOM. Niente tastiera ne' seriale. Adattato dall'emulazione dell'800XL.
 */
#include <stdint.h>

class Pokey {
public:
  bool begin();                                   // costruisce le tabelle poly (alloca 16 KB per il poly17)
  void end();
  void reset();
  void write(uint8_t r, uint8_t v);
  uint8_t read(uint8_t r);
  // Avanza di n cicli CPU; ritorna somma(volume*cicli) in unita' doppie (come nell'800XL).
  int advance(int n);
  bool irq() const { return irqst != 0xFF; }

private:
  uint8_t audf[4] = {0}, audc[4] = {0}, audctl = 0, irqen = 0, irqst = 0xFF;
  int per[4] = {1, 1, 1, 1}, cnt[4] = {1, 1, 1, 1};
  bool outv[4] = {false, false, false, false};
  int b4 = 0, b5 = 0, b9 = 0, b17 = 0, curT = 0;
  void recalc();
};
