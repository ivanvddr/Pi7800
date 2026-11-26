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
 * Formato delle voci, identico per DLL e DL a quanto documentato nel Software Guide:
 *
 *   DLL (3 byte per zona, puntata da DPPH:DPPL, letta all'inizio del frame):
 *     byte0: bit7 DLI, bit6 H16 (holey su blocchi 4K), bit5 H8 (blocchi 2K), bit4 = 0,
 *            bit3-0 offset = altezza zona - 1 (l'offset viene sommato al byte alto
 *            dell'indirizzo grafica e DECREMENTA a ogni riga: prima riga offset N … ultima 0)
 *     byte1: indirizzo ALTO della Display List
 *     byte2: indirizzo BASSO della Display List
 *
 *   DL, header a 4 byte (se byte1 & 0x1F != 0):
 *     byte0: indirizzo BASSO dei dati grafici
 *     byte1: bit7-5 palette, bit4-0 larghezza in complemento a 2 (byte, non pixel)
 *     byte2: indirizzo ALTO dei dati grafici
 *     byte3: HPOS (0-159; unità da 2 pixel)
 *
 *   DL, header a 5 byte (se byte1 & 0x1F == 0 e byte1 & 0x5F != 0, cioè fine lista solo
 *   quando byte1 & 0x5F == 0):
 *     byte0: indirizzo BASSO (dei dati in modo diretto, della mappa caratteri in indiretto)
 *     byte1: bit7 write mode (0=160x2/320x1, 1=160x4/320x2), bit6 = 1 (sempre),
 *            bit5 modo indiretto/caratteri, bit4-0 = 0
 *     byte2: indirizzo ALTO (dei dati o della mappa caratteri)   
 *     byte3: bit7-5 palette, bit4-0 larghezza (0 = 32)           
 *     byte4: HPOS
 *
 *   Fine DL: byte1 & 0x5F == 0 
 */
 
#include "Maria.h"
#include <string.h>

void Maria::reset() {
  memset(reg, 0, sizeof(reg));
  vblank = false; dliNow = false;
  dllPtr = dlPtr = 0; zoneLeft = 0; curOffset = 0;
  dllBase = 0; dllEndOff = 0;
  zH16 = zH8 = false; wmode = false;
  firstLine = true; dllPending = true;
  palIdx = 0; hPos = 0;
  memset(lineRam, 0, sizeof(lineRam));
}

void Maria::writeReg(uint8_t r, uint8_t v) {
  if (r < 0x20 || r > 0x3F) return;
  reg[r - 0x20] = v;                 // WSYNC ($24): l'halt lo gestisce la macchina
}

uint8_t Maria::readReg(uint8_t r) const {
  if (r == 0x28) return vblank ? 0x80 : 0x00;     // MSTAT: solo lettura
  if (r < 0x20 || r > 0x3F) return 0;
  return reg[r - 0x20];
}

void Maria::frameStart() {
  firstLine = true;                 // "First DMA of the frame": +8 clock PER FRAME (misurato su hardware)
  dllPending = true;                // DPPH/DPPL vengono agganciati alla prima riga con DMA attivo
}

// Legge la voce DLL corrente (dllPtr) e prepara la zona: DL, offset, holey, DLI.
void Maria::readDllEntry() {
  uint8_t f = mrd(dllPtr);
  zH16 = (f & 0x40) != 0;
  zH8  = (f & 0x20) != 0;
  curOffset = f & 0x0F;
  dlPtr = (uint16_t)((mrd((uint16_t)(dllPtr + 1)) << 8) | mrd((uint16_t)(dllPtr + 2)));
  dliNow = (f & 0x80) != 0;          // NMI: la macchina lo serve dopo questa riga
  zoneLeft = curOffset + 1;
}

// Una cella "semplice" (160A, 320A, 320D): cc = 2 bit di colore.
inline void Maria::cell(uint8_t cc) {
  if (hPos < 160) {
    if (cc) lineRam[hPos] = (uint8_t)(palIdx | cc);
    else if (reg[R_CTRL] & 0x04) lineRam[hPos] = 0;   // kangaroo: colore 0 dipinge lo sfondo
  }
  hPos++;
}

// Una cella "ricca" (160B, 320B, 320C): pp32 = bit di palette già in posizione 3-2,
// cc10 = bit di colore già in posizione 1-0; il bit 4 (palette 4-7) viene dall'header.
inline void Maria::cell2(uint8_t pp32, uint8_t cc10) {
  if (hPos < 160) {
    if (pp32 | cc10) lineRam[hPos] = (uint8_t)((palIdx & 0x10) | pp32 | cc10);
    else if (reg[R_CTRL] & 0x04) lineRam[hPos] = 0;   // kangaroo
  }
  hPos++;
}

// Legge un byte grafico all'indirizzo ga e lo scrive in Line RAM (2 o 4 celle).
void Maria::storeGraphic(uint16_t ga) {
  uint8_t d = holey(ga) ? 0 : mrd(ga);      // holey DMA: dato "buco" = celle trasparenti
  if (wmode) {                              // 2 celle per byte: 160B / 320B / 320C
    cell2((uint8_t)(d & 0x0C), (uint8_t)((d & 0xC0) >> 6));         // pixel sx: pal = bit 3-2, col = bit 7-6
    cell2((uint8_t)((d & 0x03) << 2), (uint8_t)((d & 0x30) >> 4));  // pixel dx: pal = bit 1-0, col = bit 5-4
  } else {                                  // 4 celle per byte: 160A / 320A / 320D
    cell((uint8_t)((d & 0xC0) >> 6));
    cell((uint8_t)((d & 0x30) >> 4));
    cell((uint8_t)((d & 0x0C) >> 2));
    cell((uint8_t)(d & 0x03));
  }
}

// Scorre la Display List della zona corrente disegnando in lineRam e accumulando il costo DMA.
// Costi = Software Guide, Appendice 3 "DMA TIMING" (MISURATI su hardware):
//   header 4 byte = 8, header 5 byte = 10, byte grafico diretto = 3,
//   carattere indiretto = 6 (1 byte) / 9 (2 byte)
void Maria::renderList(int& cost) {
  const uint8_t chbase = reg[R_CHBASE];
  const bool cwidth = (reg[R_CTRL] & 0x10) != 0;     // caratteri a 2 byte
  uint16_t p = dlPtr;
  uint8_t mode = mrd((uint16_t)(p + 1));
  for (int guard = 0; guard < 128 && (mode & 0x5F); guard++) {   // guardia anti-DL corrotte
    uint16_t addr = (uint16_t)(mrd(p) | (mrd((uint16_t)(p + 2)) << 8));   // byte0 = lo, byte2 = hi (sempre)
    uint8_t width;
    bool indirect = false;
    bool holed = false;
    if (mode & 0x1F) {                       // header a 4 byte
      cost += 8;
      palIdx = (uint8_t)((mode & 0xE0) >> 3);            // palette << 2
      hPos = mrd((uint16_t)(p + 3));
      width = (uint8_t)(((~mode) & 0x1F) + 1);
      p += 4;
    } else {                                 // header a 5 byte (esteso)
      cost += 10;
      uint8_t b3 = mrd((uint16_t)(p + 3));                // byte3 = palette + width
      palIdx = (uint8_t)((b3 & 0xE0) >> 3);
      hPos = mrd((uint16_t)(p + 4));
      indirect = (mode & 0x20) != 0;
      wmode = (mode & 0x80) != 0;                         // il write mode persiste (non si azzera mai da solo)
      uint8_t wf = b3 & 0x1F;
      width = (wf == 0) ? 32 : (uint8_t)(((~wf) & 0x1F) + 1);
      p += 5;
    }
    if (!indirect) {
      uint16_t ga = (uint16_t)(addr + ((uint16_t)curOffset << 8));   // offset sommato al byte alto
      for (uint8_t i = 0; i < width; i++, ga++) {
        if (holey(ga)) { holed = true; continue; }        // buco: nessuna lettura, nessun costo per byte
        cost += 3; storeGraphic(ga);
      }
      if (holed) cost += 3;                              // costo statico del buco (direct)
    } else {
      uint16_t cm = addr;                                            // mappa caratteri: senza offset
      for (uint8_t i = 0; i < width; i++, cm++) {
        uint16_t ga = (uint16_t)((((uint16_t)(uint8_t)(chbase + curOffset)) << 8) | mrd(cm));
        if (holey(ga)) { holed = true; continue; }        // buco: costo statico, niente letture
        cost += cwidth ? 9 : 6;                          // carattere indiretto: 6 (1 byte) / 9 (2 byte), tutto incluso
        storeGraphic(ga);
        if (cwidth) storeGraphic((uint16_t)(ga + 1));    // carattere a 2 byte
      }
      if (holed) cost += cwidth ? 9 : 6;                 // costo statico del buco (indirect)
    }
    mode = mrd((uint16_t)(p + 1));
  }
}

int Maria::line(uint8_t* out, bool advance) {
  dliNow = false;
  if (!dmaOn()) {                                          // DMA spento: solo sfondo, CPU libera
    if (out) memset(out, reg[R_BKGRD], 320);
    return 0;
  }
  if (dllPending) {                                        // prima riga utile del frame (o DMA attivato dopo)
    dllPtr = (uint16_t)((reg[R_DPPH] << 8) | reg[R_DPPL]);
    dllBase = dllPtr;                                      // diagnostica: inizio DLL del frame
    readDllEntry();
    dllPending = false;
  }
  // Costo base di riga = Software Guide App.3 (misurato su hardware): 16 per una
  // riga normale di zona, 24 sull'ULTIMA riga della zona (include la lettura della
  // voce DLL successiva), +8 per il primo DMA del frame ("First DMA of the frame").
  int cost = 16 + (firstLine ? 8 : 0) + ((zoneLeft == 1 && advance) ? 8 : 0);
  firstLine = false;

  // Il puntatore DL viene riletto dalla DLL a OGNI riga: un DLI può
  // cambiare la DL della zona in corso e la riga successiva la usa.
  dlPtr = (uint16_t)((mrd((uint16_t)(dllPtr + 1)) << 8) | mrd((uint16_t)(dllPtr + 2)));

  memset(lineRam, 0, sizeof(lineRam));
  renderList(cost);

  if (out) {
    const uint8_t rmode = reg[R_CTRL] & 3;
    if (rmode == 0) {                    // 160A / 160B: 1 cella = 1 pixel largo (2 px nel buffer 320)
      for (int i = 0; i < 160; i++) { uint8_t c = colorOf(lineRam[i]); out[i * 2] = c; out[i * 2 + 1] = c; }
    } else if (rmode == 2) {             // 320B / 320D: 1 cella = 2 pixel
      for (int i = 0; i < 160; i++) {
        uint8_t v = lineRam[i];
        out[i * 2]     = colorOf((uint8_t)((v & 0x10) | ((v & 0x08) >> 3) | (v & 0x02)));
        out[i * 2 + 1] = colorOf((uint8_t)((v & 0x10) | ((v & 0x04) >> 2) | ((v & 0x01) << 1)));
      }
    } else if (rmode == 3) {             // 320A / 320C: 1 cella = 2 pixel
      for (int i = 0; i < 160; i++) {
        uint8_t v = lineRam[i];
        out[i * 2]     = colorOf((uint8_t)(v & 0x1E));
        out[i * 2 + 1] = colorOf((uint8_t)((v & 0x1C) | ((v & 0x01) << 1)));
      }
    }
    // rmode == 1: non definito ("N/A" nel Software Guide): la riga non
    // viene riscritta. Nessun gioco lo usa.
  }

  // Fine riga: la zona consuma una riga; quando è esaurita si legge la voce DLL SUCCESSIVA
  // (read-ahead): l'eventuale DLI di quella zona scatta adesso, cioè "following DMA on the
  // last line of the previous zone" come da Software Guide.
  // advance=false (ultima riga display del frame): la voce successiva NON viene letta
  if (advance) {
    if (zoneLeft > 0) zoneLeft--;
    if (zoneLeft <= 0) {
      dllPtr = (uint16_t)(dllPtr + 3);
      readDllEntry();
    } else if (curOffset > 0) {
      curOffset--;
    }
  } else {
    dllEndOff = (uint16_t)(dllPtr - dllBase);               // diagnostica: consumo DLL a fine finestra
  }
  return cost;
}
