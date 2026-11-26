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
 * A78Loader.h — Loader per cartucce Atari 7800.
 *
 *   .a78      header da 128 byte (versioni 1-4): titolo, dimensione, tipo cartuccia (POKEY, SuperGame, RAM)
 *   .bin .rom cartuccia grezza: 16/32/48 KB = standard (mappata in alto),
 *             >= 64 KB multiplo di 16 KB = SuperGame (banchi pari) o SuperGame9 (banchi dispari, ipotesi)
 *
 * Tipi supportati:
 *   STANDARD    16/32/48 KB: ROM mappata da $FFFF in giù, resto open bus (0xFF)
 *   SUPERGAME   banchi da 16 KB: $4000 = penultimo bank, $8000 = bank selezionato con una scrittura
 *               in $8000-$BFFF, $C000 = ultimo bank. RAM opzionale da 16 KB su $4000-$7FFF (bit 2).
 *   SUPERGAME9  variante da 9+ banchi (144/272 KB, bit 3 "rom@4000"): $4000 = bank 0 del file, la
 *               scrittura N in $8000 mostra il bank N+1, $C000 = ultimo bank (es. Alien Brigade, Crossbow).
 *   ACTIVISION  78AC (Double Dragon, Rampage — bit 8, 128 KB): il dump No-Intro e' LINEARE (le
 *               metà 8K non si toccano nel file) ma le finestre FISSE della cartuccia mostrano le
 *               metà 8K INVERTITE rispetto alla finestra switchabile (A13 invertita — "ATARI 7800
 *               BANKSWITCHING GUIDE" di E. Stolberg;
 *               $4000-$5FFF = SECONDA metà (8K) del bank 6;  $6000-$7FFF = PRIMA metà del bank 6;
 *               $8000-$9FFF = SECONDA metà (8K) del bank 7;  $E000-$FFFF = PRIMA metà del bank 7
 *               (i vettori $FFFA-$FFFF stanno a rom+7*16384+$1FFC, NON in coda al file!);
 *               $A000-$DFFF = bank 0-7 da 16 KB selezionato SCRIVENDO in $E000-$FFFF: il numero di
 *               bank sta nel nibble basso dell'INDIRIZZO. Bank iniziale: 0. Le immagini della software list MAME
 *               hanno le metà 8K scambiate NEL FILE (vettori a fine file): rilevate dalla tabella vettori e normalizzate
 *               al volo leggendo dalla SD (readActBank). MD5 dei dump noti: SOLO diagnostico, NESSUN
 *               layout forzato. In loadROM(): avvio "sicuro" — pausa SD dopo l'open +
 *               verifica del vettore di reset con retry.
 *   POKEY       base $4000 (tutta l'area $4000-$7FFF, specchiata ogni 16 byte), $0450 oppure $0440.
 *
 * Rifiutati: Absolute, YM2151, SOUPER, bankset/halt-RAM, POKEY@800, banked/mirror RAM.
 *
 * Header .a78 "rotti" noti — DOPPIA fixup automatica in loadROM():
 *   1. per CRC;
 *   2. per TITOLO: il CRC del dump DELL'UTENTE puo' differire da
 *      quello No-Intro della tabella (revisioni/regioni/rip) — il titolo nell'header .a78
 *      (byte 17-48) o nel nome del file e' piu' robusto. Vale SOLO per SuperGame "semplice"
 *      (flags 0002, niente hardware a $4000):
 *        - "Double Dragon" / "Rampage" a 128 KB → forzati ACTIVISION 78AC: i dump storici hanno
 *          l'header SuperGame (flags 0002) anche se la cartuccia e' 78AC; con la mappatura
 *          SuperGame il vettore di reset viene letto nel punto sbagliato → la CPU parte da
 *          $0000, esegue spazzatura e va in jam (rst=0000, PC=$01xx): il freeze del log.
 *        - "Summer Games" / "Winter Games" → flags 0006 (SuperGame + RAM 16 KB a $4000-$7FFF,
 *          come il jailbreak NT-Mini che corregge il byte 0x36 del file da 02 a 06): senza
 *          quella RAM le display list del gioco vivono nella ROM → rendering a pezzi, audio ok.
 *
 * Sorgente: SD (SPI1) con fallback su LittleFS. Senza BIOS (HLE).
 */
#include <Arduino.h>
#include <SD.h>
#include <LittleFS.h>
#include "config.h"
#include "A7800.h"

enum class FSType { FS_NONE, FS_SD, FS_LITTLEFS };
enum class CartType : uint8_t { NONE, STANDARD, SUPERGAME, SUPERGAME9, ACTIVISION };

static constexpr uint32_t A78_BANK_SIZE = 0x4000;    // 16 KB
static constexpr uint8_t  A78_POOL_MAX  = 12;

class A78Loader : public A7800Host {
public:
  A78Loader();
  ~A78Loader() { unloadROM(); }
  void setMachine(A7800* m) { _m = m; }

  // ── Filesystem (come SMSLoader) ──
  bool initFilesystem(FSType preferred = FSType::FS_SD, int sdCsPin = SD_CS);
  FSType currentFS() const { return _currentFS; }
  bool fsReady() const { return _fsReady; }
  void closeFilesystem();

  // ── Cartuccia ──
  bool loadROM(const char* path);          // monta la cartuccia e RIAVVIA la macchina (avvio automatico)
  void unloadROM();
  bool isLoaded() const { return _loaded; }
  bool isPal() const { return _tvPal; }    // dal byte 57 dell'header: cablato a setTV() in loadROM()

  inline uint8_t read(uint16_t addr) const {
    const uint8_t* p = (addr >= 0x4000) ? _rd[(addr - 0x4000) >> 8] : nullptr;
    return p ? p[addr & 0xFF] : 0xFF;
  }
  inline void write(uint16_t, uint8_t) {}
  void writeSlow(uint16_t, uint8_t) {}
  const uint8_t* const* readPages() const { return _rd; }    // 192 pagine da 256 B ($4000-$FFFF)
  uint8_t* const* writePages() { return _wr; }

  uint32_t romSize() const { return _romSize; }
  uint8_t numBanks() const { return (uint8_t)_numBanks; }
  CartType mapperType() const { return _cartType; }
  const char* romName() const { return _romName; }
  uint8_t slotBank(int s) const { return (s >= 0 && s < 3) ? (uint8_t)_slotBank[s] : 0; }

  void debugStatus() const;
  void listROMs(const char* folder = GAME_DIR);
  bool fileExists(const char* path);
  void setSelectedROM(const char* path);
  File openFile(const char* path, const char* mode);
  FSType getCurrentFilesystem() const { return _currentFS; }
  const char* getSelectedROM() const { return _romPath; }
  static bool isMediaFile(const char* name);      // .a78 .bin .rom

  int  countROMs(const char* folder = GAME_DIR);
  bool loadIndex(int index, const char* folder = GAME_DIR);
  bool loadNext(int delta, const char* folder = GAME_DIR);

  // Correzione header noti: true se il CRC è nella tabella (e aggiorna flags).
  // Pubblico/statico per poterlo verificare anche fuori dal loader.
  static bool knownHeaderFixup(uint32_t crc, uint16_t& flags);

  // ── A7800Host ──
  void cartWrite(A7800& m, uint16_t addr, uint8_t value) override;
  void cartDebug(A7800& m, char* b, int n) override;         // riga diagnostica per debugState()
                                                                             // (in JAM: autopsia completa)

private:
  A7800* _m = nullptr;
  FSType _currentFS = FSType::FS_NONE; bool _fsReady = false; int _sdCsPin = SD_CS;
  bool initSD();

  CartType _cartType = CartType::NONE; bool _loaded = false;
  uint32_t _romSize = 0; uint16_t _numBanks = 0; uint32_t _hdr = 0; uint16_t _flags = 0, _pokeyBase = 0;
  bool _tvPal = false;
  char _romName[64]; char _romPath[128]; char _cartTitle[33] = {0};   // titolo dall'header .a78 (byte 17-48)
  File _file; bool _fileOpen = false;
  bool openPath(const char* path);
  void closeFile();
  bool readAt(uint32_t offset, uint8_t* dst, uint32_t len);
  void extractName(const char* path);
  void unmount();

  const uint8_t* _rd[192]; uint8_t* _wr[192]; uint8_t _sink[256];
  bool parse();
  bool setupStandard();
  bool setupSuper();
  void clearPages();

  // STANDARD: ROM piatta in un buffer
  uint8_t* _flat = nullptr;
  // SUPERGAME: pool di bank da 16 KB (LRU) + RAM opzionale da 16 KB su $4000-$7FFF
  uint8_t* _pool = nullptr; uint8_t _poolN = 0;
  uint16_t _poolBank[A78_POOL_MAX]; uint32_t _poolStamp[A78_POOL_MAX]; uint32_t _stamp = 0;
  uint16_t _slotBank[3]; uint8_t _slotPool[3]; bool _slotOn[3];
  uint8_t* _sgRam = nullptr;
  bool loadBank(uint8_t slot, uint16_t bank);
  void mapSlot(uint8_t slot);
  bool readBank(uint16_t bank, uint8_t* dst);
  void freeMem();

  // ACTIVISION 78AC: stesse finestre per tutti i bank; pool condiviso con LRU.
  // Banchi fissi 6 e 7 mai eliminabili; il bank selezionato è sostituibile.
  bool setupActivision();
  void mapAct();                                             // rimappa le 5 finestre ($4000/$6000/$8000/$A000/$E000)
  int  actFind(uint16_t bank) const;                         // indice nel pool, -1 se assente
  const uint8_t* actEnsure(uint16_t bank, uint16_t evictable);  // carica il bank (LRU), restituisce il puntatore
  bool actSelect(uint16_t bank);                             // cambia il bank a $A000-$DFFF
  bool readActBank(uint16_t bank, uint8_t* dst);              // legge normalizzando le metà 8K se _actSwap
  void detectActLayout();                                    // dump No-Intro
  uint16_t _actSel = 0;                                      // bank visibile in $A000-$DFFF (0 dopo il reset)
  bool _actSwap = false;                                     // metà 8K scambiate nel file (normalizzate in lettura)
  uint8_t _actErr = 0;                                       // errori di lettura banchi dal file
  uint8_t _actSw = 0;                                        // switch Activision ricevuti (log limitato)
  uint16_t _actSeen = 0;                                     // banchi già caricati: log solo al primo carico
  // Sessione 7 (autopsia jam): anello delle ULTIME scritture hotspot (comprese le
  // no-op: rivelano l'intento del gioco), con indirizzo, valore e bank risultante.
  uint16_t _swRingA[16]; uint8_t _swRingV[16]; uint8_t _swRingB[16]; uint16_t _swRingPos = 0;
  uint8_t actByteAt(uint16_t bank, uint16_t off);             // byte a un offset di un bank (pool o file)

  // MD5 dei dati ROM (header escluso): è l'identificatore con cui la community
  // cataloga i dump Activision per distinguere
  // i dump "lineari" No-Intro da quelli con le metà 8K scambiate)
  bool computeMD5(char out[33]);

  // CRC32 della ROM (header escluso): usato solo per i fixup degli header noti
  uint32_t computeCRC();
  void checkHeaderFixups();
  void applyTitleFixups();                                   // fixup per titolo

  int _browseIdx = -1;
};
