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
 * Maria.h — chip grafico MARIA dell'Atari 7800
 *
 * Riferimenti: "7800 Software Guide" (Atari, edizione 8bitdev.org) e ProSystem 1.3
 * (Greg Stanton, GPLv2+, Core/Maria.cpp) da cui è ripreso il modello a "celle" del
 * Line RAM (160 celle da 5 bit: palette<<2 | colore) e il timing DMA.
 *
 * Mappa registri $20-$3F (indice in reg[] = indirizzo - $20), confermata da entrambe le fonti:
 *   $20 BKGRD   $21-$23 P0C1-3   $24 WSYNC   $25-$27 P1C1-3   $28 MSTAT(r)
 *   $29-$2B P2C1-3   $2C DPPH    $2D-$2F P3C1-3   $30 DPPL    $31-$33 P4C1-3
 *   $34 CHBASE $35-$37 P5C1-3   $38 OFFSET   $39-$3B P6C1-3   $3C CTRL   $3D-$3F P7C1-3
 *   Trucco (ProSystem): palette n colore c = reg[(n << 2) | c].
 *
 *   - frameStart() una volta per frame; line(out) una volta per riga "visibile"
 *     (out = buffer da 320 byte con i colori finali, oppure nullptr = solo costo DMA).
 *   - advance = false sull'ULTIMA riga display del frame: 
 *     la voce DLL successiva NON viene letta in read-ahead e la sua eventuale DLI NON
 *     scatta (apparterrebbe al frame dopo).
 *   - dliNow va controllato DOPO ogni line(): se è true la macchina deve generare l'NMI
 *     (vettore $FFFA/$FFFB). line() riporta il flag a false all'inizio della riga dopo.
 *   - MSTAT/vblank, WSYNC ($24), INPTCTRL e il ritmo delle righe restano alla macchina.
 */
#include <stdint.h>

class Maria {
public:
  // memoria vista da MARIA: tabella pagine a 64 KB (nullptr = non leggibile → 0) + RAM per gli
  // alias zero-page/stack: ram[0] = $1000, quindi $0040-$00FF = ram[0x840..], $0140-$01FF = ram[0x940..]
  const uint8_t* const* rdp = nullptr;
  const uint8_t* ram = nullptr;

  uint8_t reg[0x20];                      // $20..$3F (indice = indirizzo - $20)
  bool vblank = false;                    // MSTAT bit 7
  bool dliNow = false;                    // voce DLL appena letta con flag DLI → NMI
  // Diagnostica (sessione 6): quanta DLL è stata consumata nella finestra display.
  // dllEndOff/3 = numero di voci processate: se il DLL del gioco è più lungo della
  // finestra (tipico: gioco PAL con header NTSC bugiardo) le ultime voci (e le loro
  // DLI!) restano fuori → il contatore di zona del kernel desincronizza.
  uint16_t dllBase = 0;                   // prima voce DLL del frame (DPP agganciato)
  uint16_t dllEndOff = 0;                 // byte di DLL consumati a fine finestra display

  void reset();
  void writeReg(uint8_t r, uint8_t v);    // r = 0x20..0x3F
  uint8_t readReg(uint8_t r) const;
  bool dmaOn() const { return ((reg[R_CTRL] >> 5) & 3) == 2; }   // CTRL bit 6-5 = 10

  void frameStart();                      // inizio frame: la DLL viene agganciata alla prima riga con DMA attivo
  // elabora una riga; ritorna il costo DMA in clock MARIA. advance=false sull'ultima riga
  // display del frame: niente read-ahead della voce DLL successiva (e niente sua DLI).
  int line(uint8_t* out, bool advance = true);

  // indici dei registri in reg[]
  static constexpr uint8_t R_BKGRD  = 0x00;   // $20
  static constexpr uint8_t R_WSYNC  = 0x04;   // $24 (l'halt CPU lo gestisce la macchina)
  static constexpr uint8_t R_MSTAT  = 0x08;   // $28 (solo lettura)
  static constexpr uint8_t R_DPPH   = 0x0C;   // $2C
  static constexpr uint8_t R_DPPL   = 0x10;   // $30
  static constexpr uint8_t R_CHBASE = 0x14;   // $34
  static constexpr uint8_t R_OFFSET = 0x18;   // $38
  static constexpr uint8_t R_CTRL   = 0x1C;   // $3C
  // PnCm = reg[(n << 2) | m], n = 0..7, m = 1..3

private:
  uint16_t dllPtr = 0, dlPtr = 0;
  int zoneLeft = 0;
  uint8_t curOffset = 0;
  bool zH16 = false, zH8 = false;
  bool wmode = false;                     // write mode: persiste finché un header a 5 byte non lo ridefinisce
  bool firstLine = true, dllPending = true;
  uint8_t palIdx = 0;                     // palette << 2, pronto per indicizzare reg[]
  int hPos = 0;
  uint8_t lineRam[160];                   // celle: (palette<<2)|colore; 0 = non scritto → sfondo

  inline uint8_t mrd(uint16_t a) const {
    const uint8_t* p = rdp ? rdp[a >> 8] : nullptr;
    if (p) return p[a & 0xFF];
    if ((a >> 8) <= 1 && (a & 0xFF) >= 0x40) return ram[0x800 + (a & 0x1FF)];
    return 0;
  }
  inline uint8_t colorOf(uint8_t cell) const { return (cell & 3) ? reg[cell & 31] : reg[R_BKGRD]; }
  inline bool holey(uint16_t a) const {
    if (a > 0x7FFF) {                                   // holey DMA: solo sopra $8000 (ProSystem)
      if (zH16 && (a & 0x1000)) return true;
      if (zH8 && (a & 0x0800)) return true;
    }
    return false;
  }
  inline void cell(uint8_t cc);           // 1 cella: cc = colore 2 bit (160A/320A/320D)
  inline void cell2(uint8_t pp32, uint8_t cc10);   // 1 cella "ricca": pp32 = bit 3-2, cc10 = bit 1-0
  void storeGraphic(uint16_t ga);
  void readDllEntry();
  void renderList(int& cost);
};
