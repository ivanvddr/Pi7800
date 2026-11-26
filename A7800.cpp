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
 
#include "A7800.h"
#include "Video.h"
#include "Input.h"
#include "Audio.h"
#include <string.h>
#include <stdio.h>

static uint8_t s_open[256];                       // bus aperto: legge $FF
static uint8_t s_sink[256];                       // scritture ignorate (aree senza callback)
static uint8_t s_scratch[FB_W] __attribute__((aligned(4)));

void A7800::init() {
  Cpu6502::initTables();
  memset(s_open, 0xFF, sizeof(s_open));
  cpu.rdp = rdp; cpu.wrp = wrp; cpu.ioRead = ioReadCb; cpu.ioWrite = ioWriteCb; cpu.ctx = this;
  cpu.pcRing = cpu.pcRingStore;      // anello diagnostico PC (autopsia jam); nullptr = spento
  maria.rdp = rdp; maria.ram = ram;
  _scanLines = SCANLINES;                         // NTSC: 262 righe (setTV(true) → 312 per il PAL)
  remap();                                        // mappa iniziale: senza setCart() è tutto bus aperto
}

void A7800::setTV(bool pal) {
  _scanLines = pal ? (SCANLINES + 50) : SCANLINES;    // 262 → 312 righe/frame
}

void A7800::setCart(const uint8_t* const* rd, uint8_t* const* wr, uint16_t pBase) {
  cartRd = rd; cartWr = wr; pokeyBase = pBase;
  bool want = (pBase != 0);
  if (want && !pokeyOn) pokeyOn = pokey.begin();
  else if (!want && pokeyOn) { pokey.end(); pokeyOn = false; }
  remap();
}

void A7800::remap() {
  for (int p = 0; p < 256; p++) { rdp[p] = s_open; wrp[p] = s_sink; }
  for (int p = 0; p < 4; p++) { rdp[p] = nullptr; wrp[p] = nullptr; }          // TIA, MARIA, RAM zp/stack, RIOT: via callback
  for (int p = 0x18; p < 0x28; p++) { rdp[p] = ram + ((p - 0x18) << 8); wrp[p] = ram + ((p - 0x18) << 8); }   // RAM 4KB $1800-$27FF
  // $2800-$3FFF = bus aperto: NON è uno specchio della RAM! È l'area dell'High Score Cart
  // (ROM HSC a $3000-$3FFF, NVRAM a $1000-$17FF; il mirror RAM fino a $3FFF della
  // Software Guide è evidentemente sbagliato. I giochi con supporto HSC (Ms. Pac-Man,
  // Galaga, Centipede, Joust, Xevious, Robotron...) leggono la firma $C6/$FE a $3900/$3904:
  // se lì ci fosse lo specchio della RAM di sistema ($1900/$1904, dove vivono i puntatori
  // delle display list!) la firma può corrispondere per caso → "HSC fantasma" → il gioco
  // salta ai vettori HSC $3FEB-$3FFD ed esegue spazzatura → crash.
  for (int p = 0x28; p < 0x40; p++) { rdp[p] = s_open; wrp[p] = s_sink; }
  // Cartuccia $4000-$FFFF: le LETTURE solo dove il loader mappa; le SCRITTURE invece arrivano
  // SEMPRE al loader (cartWrite) tranne dove c'è RAM di cartuccia. Così gli hotspot di bank
  // switching NON possono perdersi (es. $FF80-$FF87 di Double Dragon: sono nella pagina ROM
  // $FF00 — prima, con wr==nullptr, quelle scritture finivano nel sink e il bank non cambiava MAI).
  for (int p = 0x40; p < 0x100; p++) {
    int i = p - 0x40;
    if (cartRd && cartRd[i]) rdp[p] = cartRd[i];
    wrp[p] = (cartWr && cartWr[i]) ? cartWr[i] : nullptr;                      // nullptr > ioWrite > cartWrite
  }
  // POKEY: se la base è >= $4000 (SuperGame+POKEY / 16K+POKEY) i suoi 16 registri sono
  // specchiati in TUTTO $4000-$7FFF: il loader NON deve mappare ROM lì.
  if (pokeyBase >= 0x4000) { for (int p = 0x40; p < 0x80; p++) { rdp[p] = nullptr; wrp[p] = nullptr; } }
  else if (pokeyBase) { int p = pokeyBase >> 8; rdp[p] = nullptr; wrp[p] = nullptr; }
}

void A7800::reset() {
  memset(ram, 0, sizeof(ram));
  maria.reset();
  tch[0] = TiaCh(); tch[1] = TiaCh();
  timRunning = false; timValue = 0; timFlagRead = false; timStart = 0;
  inptctrl = 0x07; clkBase = 0; carry = 0; curT = 0; dcX = dcY = 0;
  wsyncReq = false; cpu.stop = false; cpu.nmiPending = false;
  cpu.pcRingPos = 0;                       // anello diagnostico pulito a ogni reset
  dbgLastCartA = 0; dbgLastCartV = 0;
  if (pokeyOn) pokey.reset();
  remap();                        // il CPU qui sotto legge $FFFC/$FFFD: setCart() deve essere GIA' stata chiamata!
  cpu.irqLine = false;
  cpu.resetCPU();
}

// ── I/O ─────────────────────────────────────────────────────────────────────
bool A7800::pokeyAt(uint16_t a) {
  if (!pokeyBase) return false;
  if (pokeyBase >= 0x4000) return (a >= 0x4000 && a < 0x8000);    // 16 registri ripetuti in tutto $4000-$7FFF
  return (a >> 4) == (pokeyBase >> 4);                            // base $0450/$0440: finestra da 16 byte
}

uint8_t A7800::ioRead(uint16_t a) {
  uint8_t pg = a >> 8;
  if (pg <= 1) {
    uint8_t lo = a & 0xFF;
    if (lo >= 0x40) return ram[0x800 + (a & 0x1FF)];            // zero page / stack = $2040-$20FF / $2140-$21FF
    return (lo < 0x20) ? tiaRead(lo) : maria.readReg(lo);       // $28 = MSTAT (bit 7 = VBLANK)
  }
  if (pg <= 3) return riotRead(a);
  if (pokeyAt(a)) return pokey.read(a & 0x0F);
  return 0xFF;
}

void A7800::ioWrite(uint16_t a, uint8_t v) {
  uint8_t pg = a >> 8;
  if (pg <= 1) {
    uint8_t lo = a & 0xFF;
    if (lo >= 0x40) { ram[0x800 + (a & 0x1FF)] = v; return; }
    if (lo < 0x20) { tiaWrite(lo, v); return; }                 // $02 = WSYNC del TIA: inattivo in modo 7800
    maria.writeReg(lo, v);
    if (lo == 0x24) { wsyncReq = true; cpu.stop = true; }       // WSYNC della MARIA ($24!): CPU ferma fino a fine riga
    return;
  }
  if (pg <= 3) { riotWrite(a, v); return; }
  if (pokeyAt(a)) { pokey.write(a & 0x0F, v); return; }
  if (a >= 0x4000 && host) { dbgLastCartA = a; dbgLastCartV = v; host->cartWrite(*this, a, v); }   // bank switching
}

uint8_t A7800::tiaRead(uint8_t r) {
  // Modo 2 tasti (fonti: sorgente Commando 7800 "LDA INPT0/BMI" = premuto, e 7800.8bitdev.org,
  // Hardware Interfacing Guide 1.5.2): INPT0-3 usano la convenzione INVERTITA rispetto a
  // INPT4/5 — bit7=1 -> PREMUTO. Il valore di riposo delle linee NON collegate (INPT2/3)
  // e' quindi 0x00 (NON 0x80: sarebbe "premuto sempre" e bloccherebbe i menu dei giochi
  // a 2 tasti).
  switch (r & 0x0F) {
    case 0x08: return g_inpt0;          // INPT0: P0 pulsante DESTRO (modo 2 tasti): bit7=1 PREMUTO
    case 0x09: return g_inpt1;          // INPT1: P0 pulsante SINISTRO (modo 2 tasti): bit7=1 PREMUTO
    case 0x0A: return 0x00;             // INPT2: P1 destro (porta 2 non collegata): 0x00 = NON premuto
    case 0x0B: return 0x00;             // INPT3: P1 sinistro (porta 2 non collegata): 0x00 = NON premuto
    case 0x0C: return g_fire0;          // INPT4: fire P0, linea 1 tasto (FIRE o FIRE2): bit7=1 NON premuto
    case 0x0D: return 0x80;             // INPT5: fire P1 (porta 2 non collegata): bit7=1 NON premuto
    default: return 0x80;               // altri registri TIA leggibili: come bus aperto
  }
}

void A7800::tiaWrite(uint8_t r, uint8_t v) {
  switch (r) {
    case 0x01: inptctrl = v; break;                                       // INPTCTRL (modo 7800 / BIOS: HLE, nessun effetto)
    case 0x15: tch[0].audc = v & 15; break;   case 0x16: tch[1].audc = v & 15; break;
    case 0x17: tch[0].audf = v & 31; break;   case 0x18: tch[1].audf = v & 31; break;
    case 0x19: tch[0].audv = v & 15; break;   case 0x1A: tch[1].audv = v & 15; break;
    default: break;
  }
}

// ── RIOT 6532: porte + timer (calcolato in modo pigro) ───────────────────────
uint8_t A7800::intim() {
  if (!timRunning) return timValue;
  uint32_t el = nowCycles() - timStart;
  uint32_t ticks = el / (uint32_t)timInterval;
  if (ticks <= timValue) return (uint8_t)(timValue - ticks);
  uint32_t after = el - (uint32_t)(timValue + 1) * (uint32_t)timInterval;   // dopo l'underflow scende di 1 a ciclo
  return (uint8_t)(0xFF - (after & 0xFF));
}

uint8_t A7800::riotRead(uint16_t a) {
  if ((a & 0x280) != 0x280) return 0xFF;
  if (!(a & 0x04)) {
    switch (a & 3) { case 0: return g_swcha; case 2: return g_swchb; default: return 0; }
  }
  if (!(a & 1)) { timFlagRead = true; return intim(); }               // INTIM (la lettura azzera il flag IRQ)
  bool under = timRunning && ((nowCycles() - timStart) / (uint32_t)timInterval) > timValue;
  return (under && !timFlagRead) ? 0x80 : 0x00;                        // TIMINT
}

void A7800::riotWrite(uint16_t a, uint8_t v) {
  if ((a & 0x280) != 0x280) return;
  if ((a & 0x14) == 0x14) {                                            // TIM1T/TIM8T/TIM64T/T1024T
    static const int iv[4] = {1, 8, 64, 1024};
    timInterval = iv[a & 3]; timValue = v; timStart = nowCycles(); timRunning = true; timFlagRead = false;
  }
}

// ── TIA audio ────────────────────────────────────────────────────────────────
static const uint8_t P4t[15] = {1,1,0,1,1,1,0,0,0,0,1,0,1,0,0};
static const uint8_t P5t[31] = {0,0,1,0,1,1,0,0,1,1,1,1,1,0,0,0,1,1,0,1,1,1,0,1,0,1,0,0,0,0,0};
static const uint8_t D31t[31] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1};

void A7800::tiaTick(TiaCh& c) {
  if (c.div < c.audf) { c.div++; return; }                             // divisore (AUDF+1)
  c.div = 0;
  switch (c.audc) {
    case 0: case 11: c.out = true; break;
    case 1: c.out = P4t[c.p4]; c.p4 = (c.p4 + 1) % 15; break;
    case 2: if (c.d31 == 0) { c.out = P4t[c.p4]; c.p4 = (c.p4 + 1) % 15; } c.d31 = (c.d31 + 1) % 31; break;
    case 3: if (P5t[c.p5]) { c.out = P4t[c.p4]; c.p4 = (c.p4 + 1) % 15; } c.p5 = (c.p5 + 1) % 31; break;
    case 4: case 5: c.out = !c.out; break;
    case 6: case 10: c.out = D31t[c.d31]; c.d31 = (c.d31 + 1) % 31; break;
    case 7: case 9: c.out = P5t[c.p5]; c.p5 = (c.p5 + 1) % 31; break;
    case 8: { uint16_t b = (c.p9 ^ (c.p9 >> 4)) & 1; c.p9 = (c.p9 >> 1) | (b << 8); c.out = c.p9 & 1; } break;
    case 12: case 13: if (++c.d3 >= 3) { c.d3 = 0; c.out = !c.out; } break;
    case 14: if (++c.d3 >= 3) { c.d3 = 0; c.out = D31t[c.d31]; c.d31 = (c.d31 + 1) % 31; } break;
    case 15: if (++c.d3 >= 3) { c.d3 = 0; c.out = P5t[c.p5]; c.p5 = (c.p5 + 1) % 31; } break;
  }
}

// 2 campioni per riga: il clock audio del TIA e' 2 x frequenza di riga (31.4 kHz)
void A7800::audioLine() {
  for (int k = 0; k < 2; k++) {
    tiaTick(tch[0]); tiaTick(tch[1]);
    int x = ((tch[0].out ? tch[0].audv : 0) + (tch[1].out ? tch[1].audv : 0)) * 512;       // 0..15360
    if (pokeyOn) { int n = LINE_CLOCKS / 8 + k; x += pokey.advance(n) * 128 / n; }
    x *= AUDIO_GAIN;
    int y = x - dcX + ((dcY * 251) >> 8); dcX = x; dcY = y;                                // filtro DC
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    audioPush((int16_t)y);
  }
  // IRQ di livello: underflow del timer RIOT (si azzera leggendo INTIM o scrivendo un timer) + POKEY.
  // (Prima la linea IRQ era pilotata solo dal POKEY: i giochi che usano l'IRQ del timer si bloccavano.)
  bool under = timRunning && ((nowCycles() - timStart) / (uint32_t)timInterval) > timValue;
  cpu.irqLine = (under && !timFlagRead) || (pokeyOn && pokey.irq());
}

// ── Ciclo principale ─────────────────────────────────────────────────────────
void A7800::runLine(int s) {
  scan = s; cpu.stop = false; wsyncReq = false;
  const int row = s - A7800_VBLANK_LINES;
  // Finestra display MARIA = area "MARIA attempts display" del Software Guide:
  // raster 16-258 NTSC (243 righe), raster 16-308 PAL (293 righe). NON una finestra fissa
  // di 240 righe: i kernel a catena di DLI (Ms. Pac-Man, Galaga, Double Dragon...) completano
  // la sequenza di NMI di zona solo se TUTTE le voci del loro DLL vengono processate. Con la
  // finestra a 240 le DLI delle ultime zone andavano perse: il contatore di zona del gioco
  // desincronizzava e TUTTE le zone mostravano la stessa display list (bande orizzontali
  // ripetute ~16 px + flicker, con audio regolare perche' il frame-work gira lo stesso).
  const int dispLines = _scanLines - 19;                 // 262 -> 243, 312 -> 293
  const bool active = (row >= 0 && row < dispLines);
  const bool lastDispLine = (row == dispLines - 1);      // ultima riga: niente read-ahead
  maria.vblank = !active;
  if (s == A7800_VBLANK_LINES) maria.frameStart();

  int dma = 0;
  if (active) {
    // Oltre il framebuffer (PAL: righe 240-292) le zone/DLI vengono comunque processate,
    // solo i pixel non finiscono nel fb (il TFT mostra le prime FB_H righe).
    const bool draw = drawFrame && (row < FB_H);
    dma = maria.line(draw ? s_scratch : nullptr, !lastDispLine);
    if (draw) {                                                   // scrive nel framebuffer solo le righe cambiate
      uint32_t* d = (uint32_t*)(g_fb + row * FB_W); const uint32_t* n = (const uint32_t*)s_scratch;
      int i = 0; while (i < FB_W / 4 && d[i] == n[i]) i++;
      if (i < FB_W / 4) { memcpy(d, n, FB_W); g_fbDirty |= 1u << (row / TFT_CHUNK_LINES); }
    }
    if (maria.dliNow) { cpu.nmiPending = true; statDli++; }        // DLI: a fine ultima riga della zona PRECEDENTE
  }
  int limit = LINE_CLOCKS - (dma > LINE_CLOCKS - 40 ? LINE_CLOCKS - 40 : dma);
  int t = carry;
  while (t < limit) {
    curT = t;
    t += cpu.execCPU((limit - t + 3) / 4) * 4;                          // 1 ciclo CPU = 4 clock MARIA
    if (cpu.stop) { cpu.stop = false; if (wsyncReq) { wsyncReq = false; t = limit; } }   // WSYNC: salta a fine riga
  }
  carry = t - limit;
  curT = limit; clkBase += LINE_CLOCKS;
  audioLine();
}

void A7800::runFrame() {
  drawFrame = ((frameNo % (VIDEO_FRAMESKIP + 1)) == 0) && (g_fbState == 0);
  frameNo++; statTotal++; statFrames++; if (drawFrame) statDrawn++;
  statDli = 0;
  for (int s = 0; s < _scanLines; s++) runLine(s);
  if (drawFrame) { __sync_synchronize(); g_fbState = 1; }
}

void A7800::debugState(char* b, int n) {
  int k = 0;
  k += snprintf(b + k, n - k, "PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X jam=%d\n", cpu.pc, cpu.a, cpu.x, cpu.y, cpu.p, cpu.sp, cpu.jammed);
  k += snprintf(b + k, n - k, "MARIA: CTRL=%02X (DMA %s) DPP=%02X%02X CHBASE=%02X BG=%02X MSTAT=%02X INPTCTRL=%02X DLI/frame=%d DLLend=%d\n",
                maria.reg[0x1C], maria.dmaOn() ? "on" : "off", maria.reg[0x0C], maria.reg[0x10], maria.reg[0x14], maria.reg[0], maria.readReg(0x28), inptctrl, statDli, maria.dllEndOff / 3);
  k += snprintf(b + k, n - k, "PAL0: %02X %02X %02X  PAL1: %02X %02X %02X  PAL2: %02X %02X %02X\n", maria.reg[1], maria.reg[2], maria.reg[3],
                maria.reg[5], maria.reg[6], maria.reg[7], maria.reg[9], maria.reg[10], maria.reg[11]);
  k += snprintf(b + k, n - k, "TIA: AUDC=%d,%d AUDF=%d,%d AUDV=%d,%d  pokey=%d  RIOT intim=%02X\n", tch[0].audc, tch[1].audc, tch[0].audf, tch[1].audf,
                tch[0].audv, tch[1].audv, (int)pokeyOn, intim());
  // Diagnostica loader: vettori letti dalla mappa ATTUALE, opcode a PC e ultima hotspot
  // consegnata a cartWrite(). Verifica immediata del bankswitching:
  //   - "rst=" deve dare il vettore di reset REALE della cartuccia (per Double Dragon 78AC legge
  //     la PRIMA metà del bank 7, cioè rom+7*16384+$1FFC — non la coda del bank!);
  //   - "BANKW=" deve mostrare le scritture $FF80-$FF87 del gioco (78AC) o $8000-$BFFF (SuperGame);
  //   - "op@PC=00" con PC=$01xx significa che il CPU esegue BRK nel mirror TIA/MARIA = vettori/bank sbagliati.
  const uint8_t* vp = rdp[0xFF];
  k += snprintf(b + k, n - k, "VEC: rst=%02X%02X nmi=%02X%02X irq=%02X%02X op@PC=%02X BANKW=%04X<=%02X\n",
                vp[0xFD], vp[0xFC], vp[0xFB], vp[0xFA], vp[0xFF], vp[0xFE],
                rdp[cpu.pc >> 8] ? rdp[cpu.pc >> 8][cpu.pc & 0xFF] : ioRead(cpu.pc), dbgLastCartA, dbgLastCartV);
  // — AUTOPSIA DEL JAM: la CPU ha eseguito spazzatura
  // le ultime PC eseguite mostrano il percorso ESATTO: il salto che è entrato nel garbage
  // e il codice valido appena prima. Lo stack (le parole in cima sono gli indirizzi
  // di ritorno) e la riga JAM del loader (byte al PC del jam in TUTTI i banchi) dicono
  // quale bank il gioco intendeva lì.
  if (cpu.jammed && k < n - 32) {
    k += snprintf(b + k, n - k, "JAM @%04X: anello PC (ultime 48, vecchie -> nuove):\n", cpu.pc);
    const uint32_t tot = cpu.pcRingPos;
    const int cnt = (tot < 48) ? (int)tot : 48;
    for (int i = 0; i < cnt; i += 12) {
      if (k >= n - 80) break;
      k += snprintf(b + k, n - k, "  ");
      for (int j = 0; j < 12 && i + j < cnt; j++)
        k += snprintf(b + k, n - k, "%04X ", cpu.pcRing[(tot - cnt + (uint32_t)(i + j)) & (Cpu6502::PC_RING_N - 1)]);
      k += snprintf(b + k, n - k, "\n");
    }
    k += snprintf(b + k, n - k, "JAM stack da $01%02X:", (uint8_t)(cpu.sp + 1));
    for (int i = 0; i < 10 && k < n - 10; i++) {
      uint8_t lo = cpu.rd((uint16_t)(0x100 + ((cpu.sp + 1 + 2 * i) & 0xFF)));
      uint8_t hi = cpu.rd((uint16_t)(0x100 + ((cpu.sp + 2 + 2 * i) & 0xFF)));
      k += snprintf(b + k, n - k, " %02X%02X", hi, lo);
    }
    k += snprintf(b + k, n - k, "\n");
  }
  // riga diagnostica del loader (bank Activision selezionato, errori di
  // ricarica LRU, impronta finestra $A000-$DFFF; in caso di JAM anche anello switch e
  // tabella dei byte al PC del jam in tutti i banchi).
  if (host && k > 0 && k < n - 2) host->cartDebug(*this, b + k, n - k);
  (void)k;
}
