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
 * A7800.h — macchina Atari 7800 (Sally 6502C + MARIA + TIA audio/ingressi + RIOT 6532 [+ POKEY di cartuccia])
 *
 * Mappa REGISTRI (verificata contro il BIOS PAL originale, disassemblato 1:1 — NON cambiarla!):
 *   TIA modo 7800:  INPTCTRL=$01 (controllo mapping BIOS/cart: HLE, nessun effetto qui),
 *                   WSYNC del TIA=$02 (attivo solo in modo 2600; in 7800 non fa nulla),
 *                   AUDC0=$15 AUDC1=$16 AUDF0=$17 AUDF1=$18 AUDV0=$19 AUDV1=$1A,
 *                   lette: INPT4=$0C (fire P0, linea pulsante SINGOLO = FIRE o FIRE2,
 *                   bit7=1 non premuto), INPT5=$0D (fire P1, non collegato),
 *                   INPT0=$08 (P0 destro) / INPT1=$09 (P0 sinistro) nel modo 2 tasti e
 *                   INPT2=$0A / INPT3=$0B (P1, non collegati): convenzione INVERTITA,
 *                   bit7=1 = PREMUTO (sorgente Commando 7800 + guida 8bitdev.org 1.5.2)
 *   MARIA ($20-$3F, reg[x] = indirizzo & 0x1F):
 *                   BACKGRND=$20, P0C1-3=$21-$23, WSYNC=$24 (FERMA LA CPU), P1C1-3=$25-$27,
 *                   MSTAT=$28 (letta, bit7=VBLANK), P2C1-3=$29-$2B, DPPH=$2C, P3C1-3=$2D-$2F,
 *                   DPPL=$30, P4C1-3=$31-$33, CHBASE=$34, P5C1-3=$35-$37, OFFSET=$38,
 *                   P6C1-3=$39-$3B, CTRL=$3C, P7C1-3=$3D-$3F
 *
 * Mappa MEMORIA:
 *   $0040-$00FF RAM (mirror $2040-$20FF), $0100-$013F mirror TIA/MARIA, $0280-$029F RIOT,
 *   $1800-$27FF RAM 4KB, $2800-$3FFF bus aperto (e' l'area dell'High Score Cart: ROM HSC a
 *   $3000-$3FFF e NVRAM a $1000-$17FF — senza HSC si legge $FF: i giochi HSC come Ms. Pac-Man
 *   e Galaga cercano la firma $C6/$FE a $3900/$3904 e NON devono trovarla!), $4000-$FFFF cartuccia.
 *   Senza BIOS (HLE): la CPU parte dal vettore di reset della cartuccia.
 *
 * CONTRATTO con il loader — rispettare ESATTAMENTE quest'ordine e queste regole:
 *   1) init() → setHost() → setCart() → reset().  reset() legge $FFFC/$FFFD dalla mappa corrente:
 *      se setCart() arriva dopo reset(), la CPU parte da $FFFF (bus aperto) ed esegue spazzatura.
 *   2) setCart(rd, wr, pokeyBase): rd[192] = puntatori di pagina per $4000-$FFFF (nullptr = bus aperto).
 *      wr[192] = RAM di cartuccia pagina per pagina; wr==nullptr (array o voce) = la scrittura
 *      arriva a A7800Host::cartWrite() → è lì che DEVE avvenire il bank switching.
 *   3) In cartWrite(), dopo il cambio bank: aggiornare rd[] e chiamare setCart()/remap().
 *      (la CPU e la MARIA leggono la tabella condivisa rdp[] ad ogni accesso: remap() a metà
 *      istruzione è sicuro, NON tenere puntatori di pagina in cache nella CPU.)
 *
 * MAPPING CARTUCCE:
 *   SuperGame standard 128K: $8000-$BFFF = bank 0-7 selezionato SCRIVENDO il NUMERO di bank
 *     (value & 7) in un qualsiasi indirizzo $8000-$BFFF; bank 7 FISSO a $C000-$FFFF;
 *     $4000-$7FFF dipende dalla variante (bank 6 fisso / nulla / RAM 16K / POKEY specchiato).
 *   "Activision" 78AC (DOUBLE DRAGON, RAMPAGE — NON è SuperGame standard! Il vero "Absolute"
 *     78AB è solo F-18 HORNET e SUPER SKATEBOARDIN': 2 banchi da 16K a $4000-$7FFF selezionati
 *     scrivendo $01/$02 su $8000, con $8000-$FFFF fisso — header bit 9, rifiutato dal loader):
 *     $A000-$DFFF = bank 0-7 selezionato SCRIVENDO a $FF80-$FF87: il numero di bank sta nel
 *       NIBBLE BASSO dell'INDIRIZZO ($FF80→bank0, $FF81→bank1, … $FF87→bank7);
 *     $4000-$5FFF = SECONDA metà (8K) del bank 6;   $6000-$7FFF = PRIMA metà del bank 6;
 *     $8000-$9FFF = SECONDA metà (8K) del bank 7;   $E000-$FFFF = PRIMA metà del bank 7
 *       (i vettori $FFF8-$FFFF leggono la metà INIZIALE del bank 7, NON la sua coda!).
 *     Nota: con 8 banchi switchabili conviene tenere TUTTA la ROM (128KB) in RAM: occupa
 *     esattamente la stessa memoria del pool LRU 6x16K + 2 banchi fissi, con zero ricarichi.
 */
#include "config.h"
#include "Cpu6502.h"
#include "Maria.h"
#include "Pokey.h"

class A7800;
class A7800Host {                                         // il loader: bank switching e RAM di cartuccia
public:
  virtual ~A7800Host() {}
  virtual void cartWrite(A7800& m, uint16_t addr, uint8_t value) = 0;    // TUTTE le scritture ROM $4000-$FFFF
  // Diagnostica per debugState() (blocco al secondo): il loader puo' aggiungere righe
  // (con \n iniziale) o lasciare la stringa vuota. Riceve la macchina: quando la CPU e'
  // in JAM il loader stampa l'AUTOPSIA completa (anello degli switch + byte al PC del
  // jam in tutti i banchi: rivela quale bank il gioco INTENDEVA eseguire lì).
  virtual void cartDebug(A7800& m, char* b, int n) { (void)m; if (n > 0) b[0] = 0; }
};

class A7800 {
public:
  Cpu6502 cpu;
  Maria maria;
  Pokey pokey;
  uint8_t ram[4096];

  void init();
  void reset();
  void runFrame();
  void setHost(A7800Host* h) { host = h; }
  // PAL/NTSC dal byte 57 dell'header .a78 (il loader la chiama in loadROM): PAL = 312 righe/frame.
  // Il ritmo lo da' il blocco su I2S (audioPush): 312 righe x 2 campioni = ~50 fps, come il PAL reale.
  void setTV(bool pal);
  int  scanLines() const { return _scanLines; }
  // rd/wr: 192 pagine da 256 B per $4000-$FFFF. rd[i]==nullptr → bus aperto.
  // wr==nullptr (voce o array) → la scrittura va a cartWrite() (bank switching). wr[i]!=nullptr → RAM di cartuccia.
  void setCart(const uint8_t* const* rd, uint8_t* const* wr, uint16_t pokeyBase);
  void remap();
  uint32_t frames() const { return statFrames; }
  uint32_t statDrawn = 0, statTotal = 0;
  int statDli = 0;              // DLI (NMI MARIA) generati nell'ULTIMO frame: Ms. Pac-Man = 32/frame
  void debugState(char* buf, int n);                      // usa un buffer da almeno 512 byte

private:
  A7800Host* host = nullptr;
  const uint8_t* rdp[256]; uint8_t* wrp[256];
  const uint8_t* const* cartRd = nullptr; uint8_t* const* cartWr = nullptr;
  uint16_t pokeyBase = 0; bool pokeyOn = false;
  uint32_t statFrames = 0, frameNo = 0;
  bool drawFrame = false, wsyncReq = false;
  int scan = 0, carry = 0, curT = 0;
  int _scanLines = 0;                                     // righe per frame: 262 NTSC / 312 PAL (setTV)
  uint8_t inptctrl = 0;
  uint16_t dbgLastCartA = 0; uint8_t dbgLastCartV = 0;    // ultima hotspot consegnata al loader

  static uint8_t ioReadCb(void* c, uint16_t a)             { return ((A7800*)c)->ioRead(a); }
  static void    ioWriteCb(void* c, uint16_t a, uint8_t v) { ((A7800*)c)->ioWrite(a, v); }
  uint8_t ioRead(uint16_t a);
  void    ioWrite(uint16_t a, uint8_t v);
  bool    pokeyAt(uint16_t a);                            // finestra POKEY (specchiata se base >= $4000)
  uint8_t tiaRead(uint8_t r);
  void    tiaWrite(uint8_t r, uint8_t v);
  uint8_t riotRead(uint16_t a);
  void    riotWrite(uint16_t a, uint8_t v);
  void    runLine(int s);
  void    audioLine();

  // RIOT: il timer si calcola in modo pigro dal contatore assoluto di cicli
  uint32_t clkBase = 0;                                   // clock MARIA trascorsi all'inizio della riga corrente
  inline uint32_t nowCycles() const { return (clkBase + (uint32_t)(curT + cpu.cycNow * 4)) / 4; }
  uint32_t timStart = 0; int timInterval = 1024; uint8_t timValue = 0; bool timFlagRead = false, timRunning = false;
  uint8_t intim();

  // TIA audio (2 canali)
  struct TiaCh { uint8_t audc = 0, audf = 0, audv = 0, div = 0, p4 = 0, p5 = 0, d31 = 0, d3 = 0; uint16_t p9 = 0x1FF; bool out = false; };
  TiaCh tch[2];
  void tiaTick(TiaCh& c);
  int dcX = 0, dcY = 0; int audHalf = 0;
};
