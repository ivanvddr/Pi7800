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
 */
#include "A78Loader.h"
#include <strings.h>
#include <stdlib.h>

A78Loader::A78Loader() {
  memset(_sink, 0xFF, sizeof(_sink));
  memset(_poolBank, 0xFF, sizeof(_poolBank)); memset(_poolStamp, 0, sizeof(_poolStamp));
  memset(_slotBank, 0, sizeof(_slotBank)); memset(_slotPool, 0, sizeof(_slotPool)); memset(_slotOn, 0, sizeof(_slotOn));
  _romName[0] = '\0'; _romPath[0] = '\0';
  clearPages();
}
void A78Loader::clearPages() { for (int i = 0; i < 192; i++) { _rd[i] = nullptr; _wr[i] = nullptr; } }

// ── Filesystem ──────────────────────────────────────────────────────────────
bool A78Loader::initFilesystem(FSType preferred, int sdCsPin) {
  if (_fsReady) return true;
  _sdCsPin = sdCsPin;
  auto trySD = [&]() -> bool {
    Serial.println("[Loader] SD...");
    if (initSD()) { _currentFS = FSType::FS_SD; _fsReady = true; Serial.println("[Loader] SD OK"); return true; }
    Serial.println("[Loader] SD not available"); return false;
  };
  auto tryLFS = [&]() -> bool {
    Serial.println("[Loader] LittleFS...");
    if (LittleFS.begin()) { _currentFS = FSType::FS_LITTLEFS; _fsReady = true; Serial.println("[Loader] LittleFS OK"); return true; }
    Serial.println("[Loader] LittleFS not available"); return false;
  };
  if (preferred == FSType::FS_SD) { if (trySD()) return true; if (tryLFS()) return true; }
  else                            { if (tryLFS()) return true; if (trySD()) return true; }
  Serial.println("[Loader] ERROR: no filesystem found");
  return false;
}
bool A78Loader::initSD() {
  SPI1.setRX(SD_MISO); SPI1.setTX(SD_MOSI); SPI1.setSCK(SD_SCLK); SPI1.setCS(SD_CS);
  SPI1.begin(); delay(100);
  return SD.begin(_sdCsPin, SD_SPI_HZ, SPI1);
}
void A78Loader::closeFilesystem() {
  closeFile();
  if (_currentFS == FSType::FS_SD) { SD.end(); Serial.println("[Loader] SD closed"); }
  _fsReady = false; _currentFS = FSType::FS_NONE;
}
bool A78Loader::openPath(const char* path) {
  if (_fileOpen) closeFile();
  if (!_fsReady) return false;
  _file = (_currentFS == FSType::FS_SD) ? SD.open(path, FILE_READ) : LittleFS.open(path, "r");
  _fileOpen = (bool)_file;
  return _fileOpen;
}
void A78Loader::closeFile() { if (_fileOpen) { _file.close(); _fileOpen = false; } }
bool A78Loader::readAt(uint32_t offset, uint8_t* dst, uint32_t len) {
  if (!_fileOpen || !_file.seek(offset)) return false;
  return (uint32_t)_file.read(dst, len) == len;
}
File A78Loader::openFile(const char* path, const char* mode) {
  if (_currentFS == FSType::FS_SD) return SD.open(path, mode);
  if (_currentFS == FSType::FS_LITTLEFS) return LittleFS.open(path, mode);
  return File();
}
bool A78Loader::fileExists(const char* path) {
  if (_currentFS == FSType::FS_SD) return SD.exists(path);
  if (_currentFS == FSType::FS_LITTLEFS) return LittleFS.exists(path);
  return false;
}
void A78Loader::setSelectedROM(const char* path) {
  if (!path) return;
  strncpy(_romPath, path, sizeof(_romPath) - 1); _romPath[sizeof(_romPath) - 1] = '\0';
  Serial.printf("A78Loader: ROM selected > %s\n", _romPath);
}
void A78Loader::extractName(const char* path) {
  const char* slash = strrchr(path, '/'); const char* base = slash ? slash + 1 : path;
  strncpy(_romName, base, sizeof(_romName) - 1); _romName[sizeof(_romName) - 1] = '\0';
  char* dot = strrchr(_romName, '.'); if (dot) *dot = '\0';
}
bool A78Loader::isMediaFile(const char* name) {
  const char* e = strrchr(name, '.'); if (!e) return false;
  return !strcasecmp(e, ".a78") || !strcasecmp(e, ".bin") || !strcasecmp(e, ".rom");
}

// ── Caricamento ─────────────────────────────────────────────────────────────
void A78Loader::freeMem() {
  if (_flat) { free(_flat); _flat = nullptr; }
  if (_pool) { free(_pool); _pool = nullptr; }
  if (_sgRam) { free(_sgRam); _sgRam = nullptr; }
  _poolN = 0; memset(_poolBank, 0xFF, sizeof(_poolBank));
}
void A78Loader::unmount() {
  closeFile(); freeMem(); clearPages();
  _cartType = CartType::NONE; _loaded = false; _romSize = 0; _numBanks = 0; _flags = 0; _pokeyBase = 0;
  _actSel = 0; _actSwap = false;
  _actErr = 0; _actSw = 0; _actSeen = 0; _swRingPos = 0;
  if (_m) _m->setCart(nullptr, nullptr, 0);
}
void A78Loader::unloadROM() { unmount(); _romName[0] = '\0'; _romPath[0] = '\0'; }

bool A78Loader::loadROM(const char* path) {
  if (!path || !path[0] || !_m) return false;
  if (!_fsReady && !initFilesystem()) { Serial.println("[Loader] ERROR: filesystem not ready"); return false; }
  if (!isMediaFile(path)) { Serial.printf("[Loader] ERROR: unknown extension: %s\n", path); return false; }
  extractName(path);
  Serial.printf("\n[Loader] Loading: %s\n", path);

  // Avvio "sicuro" (il problema del suono stridolo + freeze al via) ──
  // Il caricamento e' il momento di massimo stress per la SD (burst di seek/letture:
  // certificazione CRC/MD5 + banchi fissi). Se una lettura "riesce" ma consegna
  // spazzatura, il vettore di reset risulta assurdo: la CPU parte dentro la ROM
  // corrotta, scrive valori casuali nei registri TIA (AUDV fino a 15 = STRIDIO
  // assordante) e va in jam. Ora: (1) pausa di assestamento dopo l'apertura del
  // file; (2) il vettore di reset viene VERIFICATO prima di avviare la macchina,
  // con fino a 3 tentativi (file riaperto da zero); (3) dopo 3 tentativi andati
  // male: LOAD ERROR pulito.
  for (int attempt = 1; attempt <= 3; attempt++) {
    unmount();
    strncpy(_romPath, path, sizeof(_romPath) - 1); _romPath[sizeof(_romPath) - 1] = '\0';
    if (!openPath(path)) { Serial.printf("[Loader] ERROR: file non apribile (tentativo %d/3)\n", attempt); delay(100); continue; }
    delay(50);                                   // la SD si assesta dopo l'open
    if (!parse()) break;                         // header/dimensione non validi: deterministico, niente retry
    applyTitleFixups();                          // PRIMA del CRC — Double Dragon/Rampage
    checkHeaderFixups();                         // diventano ACTIVISION qui: si salta il CRC (128K di letture)
    bool ok = (_cartType == CartType::STANDARD) ? setupStandard()
             : (_cartType == CartType::ACTIVISION) ? setupActivision()
             : setupSuper();
    if (!ok) { delay(100); continue; }           // RAM o lettura fallita: un altro tentativo non costa
    const uint8_t* vpage = _rd[0xBF];            // pagina $FF00 (vettori): mappata da OGNI cartuccia valida
    const uint16_t rstv = vpage ? (uint16_t)(vpage[0xFC] | (vpage[0xFD] << 8)) : 0x0000;
    if (!vpage || rstv < 0x4000 || rstv >= 0xFFFA) {
      Serial.printf("[Loader] WARNING: vettore di reset $%04X insensato (tentativo %d/3): lettura SD corrotta? Ritento\n", rstv, attempt);
      delay(100);
      continue;
    }
    _loaded = true;
    _m->setHost(this);
    _m->setTV(_tvPal);                           // PAL: 312 righe/frame (il ritmo lo da' l'audio I2S)
    _m->setCart(_rd, _wr, _pokeyBase);
    _m->reset();                                 // la CPU parte dal vettore di reset della cartuccia
    const char* tn = (_cartType == CartType::STANDARD) ? "STANDARD"
                   : (_cartType == CartType::SUPERGAME9) ? "SUPERGAME9"
                   : (_cartType == CartType::ACTIVISION) ? "ACTIVISION" : "SUPERGAME";
    Serial.printf("[Loader] %s: %lu KB, %s%s\n", _romName, (unsigned long)(_romSize / 1024), tn, _pokeyBase ? " + POKEY" : "");
    {   // diagnostica di avvio: vettore di reset REALE e primi opcode (verifica immediata del mapping)
      const uint8_t* cp = (rstv >= 0x4000) ? _rd[(rstv - 0x4000) >> 8] : nullptr;
      Serial.printf("[Loader] rst=$%04X op=%02X %02X %02X\n", rstv,
                    cp ? cp[rstv & 0xFF] : 0xFF, cp ? cp[(rstv + 1) & 0xFF] : 0xFF, cp ? cp[(rstv + 2) & 0xFF] : 0xFF);
    }
    return true;
  }
  unmount();
  Serial.println("[Loader] LOAD ERROR (file illeggibile o SD instabile: riavvia la scheda e riprova)");
  return false;
}

// header .a78 (versioni 1-4) oppure ROM grezza
bool A78Loader::parse() {
  uint32_t fileSize = _file.size();
  uint8_t h[128] = {0};
  _hdr = 0; _flags = 0; _tvPal = false;
  memset(_cartTitle, 0, sizeof(_cartTitle));
  if (fileSize >= 128 && readAt(0, h, 128) && !memcmp(h + 1, "ATARI7800", 9)) {
    _hdr = 128;
    memcpy(_cartTitle, h + 17, 32);                           // titolo (byte 17-48): per applyTitleFixups()
    _cartTitle[32] = '\0';
    _flags = (uint16_t)((h[53] << 8) | h[54]);                 
    _tvPal = (h[57] & 1) != 0;                                 // bit 0 = PAL (bit 1-2: composite/multi-region)
    Serial.printf("[Loader] A78 v%u \"%.32s\" flags=%04X tv=%s\n", h[0], (const char*)(h + 17), _flags, _tvPal ? "PAL" : "NTSC");
  }
  _romSize = fileSize - _hdr;
  if (_flags & (0x0200 | 0x0800 | 0x1000 | 0x2000 | 0x4000 | 0x8000)) {
    // Absolute, YM2151, SOUPER, bankset, halt-RAM, POKEY@800
    Serial.printf("[Loader] ERROR: cartridge type not supported (flags %04X)\n", _flags); return false;
  }
  // normalizza i conflitti su $4000:
  if (_flags & 0x0010) _flags &= ~0x0008;                      // bank6@4000 vince su rom@4000
  if (_flags & 0x0018) _flags &= ~0x0004;                      // la ROM a $4000 vince sulla RAM
  if (_flags & 0x003C) _flags &= ~0x0001;                      // RAM/ROM a $4000 vince sul POKEY@4000
  if (_flags & 0x003C) _flags |= 0x0002;                       // hardware a $4000 senza bit SG: tratta come SG
  _pokeyBase = (_flags & 0x0001) ? 0x4000 : (_flags & 0x0040) ? 0x0450 : (_flags & 0x0400) ? 0x0440 : 0;
  if (_flags & 0x0100) {                                       // ACTIVISION (Double Dragon, Rampage)
    if (_romSize < 8 * A78_BANK_SIZE || (_romSize % A78_BANK_SIZE) != 0) {
      Serial.printf("[Loader] ERROR: Activision size %lu not valid\n", (unsigned long)_romSize); return false;
    }
    _cartType = CartType::ACTIVISION;
    _numBanks = (uint16_t)(_romSize / A78_BANK_SIZE);
    _flags = 0x0100; _pokeyBase = 0;                           // i bit bassi non contano
    return true;
  }
  bool sg  = (_flags & 0x0002) != 0;
  bool sg9 = sg && (_flags & 0x0008) != 0;                     // rom@4000: SuperGame a 9+ banchi (144K, 272K...)
  if (!_hdr) {                                                 // grezza: dalla dimensione
    sg  = (_romSize >= 65536);
    sg9 = sg && (((_romSize / A78_BANK_SIZE) & 1) != 0);       // numero di banchi dispari = layout SG9
  }
  if (!sg) {
    if (_romSize == 0 || _romSize > 49152) { Serial.printf("[Loader] ERROR: size %lu not valid for a standard cartridge\n", (unsigned long)_romSize); return false; }
    _cartType = CartType::STANDARD;
  } else {
    if (_romSize < 2 * A78_BANK_SIZE || (_romSize % A78_BANK_SIZE) != 0) { Serial.println("[Loader] ERROR: SuperGame size must be a multiple of 16 KB"); return false; }
    _cartType = sg9 ? CartType::SUPERGAME9 : CartType::SUPERGAME;
    _numBanks = (uint16_t)(_romSize / A78_BANK_SIZE);
  }
  return true;
}

// ── Header .a78 noti come errati (fixup su CRC, tabella vettori MAME a7800.xml) ──
// Summer Games / Winter Games girano con flags 0002 invece di 0006 (SG + RAM a $4000):
// il jailbreak NT-Mini documenta la correzione (byte 0x36 del file: 02 -> 06).
struct A78KnownFixup { uint32_t crc; const char* name; };
static const A78KnownFixup s_fixups[] = {
  { 0x65C6DF3F, "Summer Games" },        // 128K, a78_sg_ram
  { 0x8981B531, "Winter Games" },        // 128K, a78_sg_ram
  { 0x994AF6E0, "Impossible Mission" },  // 128K, a78_sg_ram
  { 0x0F87FD7B, "Tower Toppler" },       //  64K, a78_sg_ram
};

bool A78Loader::knownHeaderFixup(uint32_t crc, uint16_t& flags) {
  for (const auto& f : s_fixups)
    if (f.crc == crc) {
      flags |= 0x0004;                    // RAM 16 KB su $4000-$7FFF
      Serial.printf("[Loader] Header fixup: %s e' SuperGame+RAM (flags -> %04X)\n", f.name, flags);
      return true;
    }
  return false;
}

void A78Loader::checkHeaderFixups() {
  // solo SuperGame "semplice" (0002) senza nulla a $4000: gli altri header non li tocchiamo
  if (_cartType != CartType::SUPERGAME || (_flags & 0x003C) != 0) return;
  uint32_t crc = computeCRC();            // una passata sul file (header escluso), solo in questo caso
  knownHeaderFixup(crc, _flags);
}

// ── Fixup per TITOLO ───────────────────────────────────────
// Il CRC del dump DELL'UTENTE puo' differire da quello No-Intro della tabella (revisioni,
// regioni, rip): il titolo nell'header .a78 (byte 17-48) o nel nome del file e' piu' robusto.
// Vale SOLO per SuperGame "semplice" (0002) senza nulla a $4000, come la via CRC.
static bool a78CiSub(const char* hay, const char* needle) {   // sottostringa case-insensitive
  if (!hay) return false;
  auto lo = [](char c) -> char { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
  for (const char* p = hay; *p; p++) {
    int i = 0;
    while (needle[i] && p[i] && lo(p[i]) == lo(needle[i])) i++;
    if (!needle[i]) return true;            // needle interamente consumato
  }
  return false;
}

void A78Loader::applyTitleFixups() {
  if (_cartType != CartType::SUPERGAME || (_flags & 0x003C) != 0) return;
  // 1) Activision 78AC con header che mente: i dump storici di Double Dragon e Rampage
  //    (cartuccia 78AC da 128 KB) hanno flags 0002 (SuperGame). Con la mappatura SuperGame il
  //    vettore di reset viene letto dalla coda del bank 7 invece che dalla sua prima meta':
  //    la CPU parte da $0000, esegue spazzatura e va in jam (rst=0000, PC=$01xx).
  if (_numBanks == 8 && (a78CiSub(_cartTitle, "double dragon") || a78CiSub(_romName, "double dragon") ||
                         a78CiSub(_cartTitle, "rampage") || a78CiSub(_romName, "rampage"))) {
    _cartType = CartType::ACTIVISION;
    _flags = 0x0100; _pokeyBase = 0;                         
    Serial.printf("[Loader] Header fixup: %s e' Activision 78AC (l'header diceva SuperGame)\n", _romName);
    return;
  }
  // 2) SuperGame + RAM (0002 invece di 0006): senza la RAM di lavoro a $4000-$7FFF le display
  //    list del gioco finiscono nella ROM → rendering a pezzi con audio regolare.
  if (_flags & 0x0004) return;                                // il CRC ha gia' provveduto
  if (a78CiSub(_cartTitle, "summer games") || a78CiSub(_romName, "summer games") ||
      a78CiSub(_cartTitle, "winter games") || a78CiSub(_romName, "winter games")) {
    _flags |= 0x0004;
    Serial.printf("[Loader] Header fixup: %s e' SuperGame+RAM (flags -> %04X)\n", _romName, _flags);
  }
}

uint32_t A78Loader::computeCRC() {
  static uint32_t table[256]; static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    built = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  uint8_t buf[512];
  uint32_t off = _hdr, end = _hdr + _romSize;
  while (off < end) {
    uint32_t n = end - off; if (n > sizeof(buf)) n = sizeof(buf);
    if (!readAt(off, buf, n)) return 0;
    for (uint32_t i = 0; i < n; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    off += n;
  }
  return crc ^ 0xFFFFFFFFu;
}

bool A78Loader::setupStandard() {
  const uint32_t size = (_romSize + 255) & ~255u;
  _flat = (uint8_t*)malloc(size);
  if (!_flat) { Serial.println("[Loader] ERROR: not enough RAM"); return false; }
  memset(_flat, 0xFF, size);
  if (!readAt(_hdr, _flat, _romSize)) { Serial.println("[Loader] ERROR reading ROM"); return false; }
  closeFile();
  const uint32_t base = 0x10000 - size;                       // la ROM e' mappata in alto
  for (uint32_t i = 0; i < size / 256; i++) { _rd[((base - 0x4000) >> 8) + i] = _flat + i * 256; _wr[((base - 0x4000) >> 8) + i] = _sink; }
  _numBanks = 1;
  Serial.printf("[Loader] STANDARD: $%04X-$FFFF\n", (unsigned)base);
  return true;
}

bool A78Loader::readBank(uint16_t bank, uint8_t* dst) {
  uint32_t off = _hdr + (uint32_t)bank * A78_BANK_SIZE;
  if (readAt(off, dst, A78_BANK_SIZE)) return true;
  // la lettura LRU avviene DENTRO l'emulazione (execCPU -> cartWrite): un
  // fallimento silenzioso lascerebbe la finestra sul bank vecchio e il gioco esegue
  // spazzatura fino al jam. Riapro il file (handle "stanco") e ritento UNA volta;
  // se fallisce ancora lo scrivo sul log e lo conto (visibile in debugState).
  if (openPath(_romPath) && readAt(off, dst, A78_BANK_SIZE)) return true;
  memset(dst, 0xFF, A78_BANK_SIZE);
  _actErr++;
  Serial.printf("[Loader] ERRORE lettura bank %u dal file (errori=%u)!\n", (unsigned)bank, (unsigned)_actErr);
  return false;
}

void A78Loader::mapSlot(uint8_t s) {                           // slot 0 = $4000, 1 = $8000, 2 = $C000 (64 pagine ciascuno)
  const uint8_t* base = _slotOn[s] ? _pool + (uint32_t)_slotPool[s] * A78_BANK_SIZE : nullptr;
  for (int i = 0; i < 64; i++) {
    _rd[s * 64 + i] = base ? base + i * 256 : nullptr;
    _wr[s * 64 + i] = base ? (s == 1 ? nullptr : _sink) : nullptr;   // scritture in $8000-$BFFF = selezione del bank
  }
  if (s == 0 && _sgRam)                                        // RAM in cartuccia: 16 KB su tutto $4000-$7FFF
    for (int i = 0; i < 64; i++) { _rd[i] = _sgRam + (i - 0) * 256; _wr[i] = _sgRam + (i - 0) * 256; }
}

bool A78Loader::setupSuper() {
  // RAM in cartuccia (bit 2): 16 KB su tutto $4000-$7FFF. Allochiamo PRIMA del pool: se il pool
  // si prendesse tutta la RAM libera (128K nei giochi SG+RAM), la calloc fallirebbe e addio gioco.
  if (_flags & 0x0004) {
    _sgRam = (uint8_t*)calloc(0x4000, 1);
    if (!_sgRam) { Serial.println("[Loader] ERROR: not enough RAM for the $4000 cart RAM"); return false; }
    _slotOn[0] = false;                                        // niente ROM in $4000-$7FFF
  }
  uint8_t want = (_numBanks < A7800_CART_POOL_BANKS) ? (uint8_t)_numBanks : A7800_CART_POOL_BANKS;
  if (want > A78_POOL_MAX) want = A78_POOL_MAX;
  while (want >= 3 && !(_pool = (uint8_t*)malloc((size_t)want * A78_BANK_SIZE))) want--;
  if (!_pool) { Serial.println("[Loader] ERROR: not enough RAM for cart pool"); return false; }
  _poolN = want;
  const bool sg9 = (_cartType == CartType::SUPERGAME9);
  Serial.printf("[Loader] %s: %u banks, pool %u x 16 KB%s\n", sg9 ? "SUPERGAME9" : "SUPERGAME",
                (unsigned)_numBanks, (unsigned)_poolN, _numBanks <= _poolN ? " (all in RAM)" : " (LRU)");
  memset(_poolBank, 0xFF, sizeof(_poolBank)); memset(_poolStamp, 0, sizeof(_poolStamp)); _stamp = 0;

  if (_sgRam) {
    // già allocata e mappata sopra
  } else if (_pokeyBase == 0x4000) {                           // POKEY in cartuccia (bit 0): occupa tutto $4000-$7FFF
    _slotOn[0] = false;                                        // (es. Commando: a $4000 non c'e' ROM)
  } else {
    _slotOn[0] = true;
    _slotBank[0] = sg9 ? 0 : (uint16_t)(_numBanks - 2);        // SG9: bank 0 del file; SG: penultimo (= 6 nei 128K)
  }
  _slotOn[1] = true; _slotOn[2] = true;                           // $8000 (finestra) e $C000 (ultimo bank) sempre attivi
  _slotBank[1] = sg9 ? 1 : 0;                                  // stato dopo il reset
  _slotBank[2] = (uint16_t)(_numBanks - 1);                    // ultimo bank, sempre fisso in $C000
  for (uint8_t s = 0; s < 3; s++) _slotPool[s] = s;            // assegnazione provvisoria prima del caricamento
  if (_numBanks <= _poolN) {
    for (uint16_t b = 0; b < _numBanks; b++) { if (!readBank(b, _pool + (uint32_t)b * A78_BANK_SIZE)) return false; _poolBank[b] = b; _poolStamp[b] = ++_stamp; }
    _slotPool[0] = (uint8_t)(_slotBank[0] < _numBanks ? _slotBank[0] : 0); _slotPool[1] = (uint8_t)_slotBank[1]; _slotPool[2] = (uint8_t)_slotBank[2];
    closeFile();
    for (uint8_t s = 0; s < 3; s++) mapSlot(s);
  } else {
    if (!loadBank(2, _slotBank[2]) || !loadBank(1, _slotBank[1])) return false;
    if (_slotOn[0] && !loadBank(0, _slotBank[0])) return false;
    for (uint8_t s = 0; s < 3; s++) mapSlot(s);
  }
  return true;
}

bool A78Loader::loadBank(uint8_t slot, uint16_t bank) {
  if (slot > 2 || _numBanks == 0) return false;
  bank %= _numBanks;
  if (_poolBank[_slotPool[slot]] == bank) { _slotBank[slot] = bank; mapSlot(slot); return true; }
  int idx = -1;
  for (int i = 0; i < _poolN; i++) if (_poolBank[i] == bank) { idx = i; break; }
  if (idx < 0) {
    uint32_t best = 0xFFFFFFFFUL;
    for (int i = 0; i < _poolN; i++) {
      bool used = false;
      for (uint8_t s2 = 0; s2 < 3; s2++) if (s2 != slot && _slotOn[s2] && _slotPool[s2] == i && _poolBank[i] != 0xFFFF) used = true;
      if (used) continue;
      if (_poolBank[i] == 0xFFFF) { idx = i; break; }
      if (_poolStamp[i] < best) { best = _poolStamp[i]; idx = i; }
    }
    if (idx < 0) return false;
    if (!readBank(bank, _pool + (uint32_t)idx * A78_BANK_SIZE)) { _poolBank[idx] = 0xFFFF; return false; }
    _poolBank[idx] = bank;
  }
  _poolStamp[idx] = ++_stamp;
  _slotPool[slot] = (uint8_t)idx; _slotBank[slot] = bank;
  mapSlot(slot);
  return true;
}

// ── ACTIVISION 78AC (Double Dragon, Rampage) ────────────────────────────────
// Il dump No-Intro (MD5 543484c0…) è LINEARE — le metà 8K NON vanno mai
// invertite NEL FILE — ma le finestre FISSE della cartuccia mostrano le metà 8K
// INVERTITE rispetto alla finestra switchabile (A13 invertita). "ATARI 7800
// BANKSWITCHING GUIDE" (E. Stolberg, 7800.8bitdev.org), alla lettera:
//   $4000-$5fff second 8kb of bank 6   →  $4000-$5FFF = bank6 + 0x2000 (SECONDA metà)
//   $6000-$7fff first  8kb of bank 6   →  $6000-$7FFF = bank6 + 0x0000 (PRIMA metà)
//   $8000-$9fff second 8kb of bank 7   →  $8000-$9FFF = bank7 + 0x2000 (SECONDA metà)
//   $e000-$ffff first  8kb of bank 7   →  $E000-$FFFF = bank7 + 0x0000 (PRIMA metà:
//     i vettori $FFFA-$FFFF stanno a rom+7*16384+$1FFC, NON in coda al file!)
//   $A000-$DFFF = bank selezionato (16 KB, LINEARE), hotspot: write >= $E000 con
//   bank = indirizzo & 7 ($FF80→0 … $FF87→7; il gioco usa STA $FF80,X).
// Nessun layout viene forzato via MD5: l'MD5 resta puramente diagnostico.

static bool a78SaneVectors(const uint8_t* v) {
  // v[0..1]=NMI, v[2..3]=RESET, v[4..5]=IRQ (ordine fisico $FFFA/$FFFC/$FFFE!).
  // RST deve puntare nella finestra fissa $E000-$FFFF
  // (da lì la cartuccia parte sempre); NMI e IRQ possono puntare in una QUALSIASI
  // finestra mappata ($4000-$FFFF: DD li punta a $4000, bank 6, finestra $4000-$5FFF).
  uint16_t rst = (uint16_t)(v[2] | (v[3] << 8));
  if (rst < 0xE000) return false;
  for (int i = 0; i < 3; i++) {
    uint16_t w = (uint16_t)(v[i * 2] | (v[i * 2 + 1] << 8));
    if (w < 0x4000) return false;
  }
  return true;
}

void A78Loader::detectActLayout() {
  uint8_t va[6] = {0}, vb[6] = {0};
  uint32_t last = _hdr + (uint32_t)(_numBanks - 1) * A78_BANK_SIZE;
  bool aOk = readAt(last + 0x1FFA, va, 6) && a78SaneVectors(va);   // No-Intro (lineare): vettori nel PRIMO 8K del bank 7
  bool bOk = readAt(last + 0x3FFA, vb, 6) && a78SaneVectors(vb);   // vettori in coda al file

  _actSwap = (!aOk && bOk);
  Serial.printf("[Loader] ACTIVISION: dump %s\n", _actSwap ? "(meta' 8K scambiate nel file: normalizzo)" : "No-Intro (lineare)");
  if (!aOk && !bOk)
    Serial.println("[Loader] ACTIVISION: tabella vettori non riconosciuta alle due posizioni note: "
                   "provo il layout No-Intro");
}

bool A78Loader::readActBank(uint16_t bank, uint8_t* dst) {
  uint32_t off = _hdr + (uint32_t)bank * A78_BANK_SIZE;
  auto readHalves = [&]() -> bool {
    if (!_actSwap) return readAt(off, dst, A78_BANK_SIZE);
    return readAt(off + 0x2000, dst, 0x2000) && readAt(off, dst + 0x2000, 0x2000);   // meta' invertite nel file
  };
  if (readHalves()) return true;
  // come readBank: riapro il file e ritento
  // un fallimento qui DENTRO execCPU lascerebbe la finestra sul bank vecchio
  if (openPath(_romPath) && readHalves()) return true;
  _actErr++;
  Serial.printf("[Loader] ACT: ERRORE lettura bank %u dal file (errori=%u)!\n", (unsigned)bank, (unsigned)_actErr);
  return false;
}

int A78Loader::actFind(uint16_t bank) const {
  for (int i = 0; i < _poolN; i++) if (_poolBank[i] == bank) return i;
  return -1;
}

const uint8_t* A78Loader::actEnsure(uint16_t bank, uint16_t evictable) {
  if (bank >= _numBanks) bank %= _numBanks;
  int idx = actFind(bank);
  if (idx < 0) {
    const uint16_t p1 = (uint16_t)(_numBanks - 2), p2 = (uint16_t)(_numBanks - 1);   // bank 6 e 7: fissi, mai rimossi
    uint32_t best = 0xFFFFFFFFUL; idx = -1;
    for (int i = 0; i < _poolN; i++) {
      uint16_t b = _poolBank[i];
      if (b != 0xFFFF && b != evictable && (b == p1 || b == p2)) continue;
      if (b == 0xFFFF) { idx = i; break; }
      if (_poolStamp[i] < best) { best = _poolStamp[i]; idx = i; }
    }
    if (idx < 0) return nullptr;
    if (!readActBank(bank, _pool + (uint32_t)idx * A78_BANK_SIZE)) return nullptr;
    _poolBank[idx] = bank;
    if (bank < 16 && !(_actSeen & (1u << bank))) {             // log al PRIMO carico di ogni bank
      _actSeen |= (uint16_t)(1u << bank);
      Serial.printf("[Loader] ACT: bank %u caricato da file (slot %d)\n", (unsigned)bank, idx);
    }
  }
  _poolStamp[idx] = ++_stamp;
  return _pool + (uint32_t)idx * A78_BANK_SIZE;
}

bool A78Loader::actSelect(uint16_t bank) {
  const uint8_t* p = actEnsure(bank, _actSel);                 // il bank sostituito e' sacrificabile
  if (!p) return false;
  _actSel = (uint16_t)(bank % _numBanks);
  mapAct();
  return true;
}

void A78Loader::mapAct() {
  const int i6 = actFind((uint16_t)(_numBanks - 2)), i7 = actFind((uint16_t)(_numBanks - 1)), is = actFind(_actSel);
  if (i6 < 0 || i7 < 0 || is < 0) return;                      // (non dovrebbe mai accadere)
  const uint8_t* b6 = _pool + (uint32_t)i6 * A78_BANK_SIZE;
  const uint8_t* b7 = _pool + (uint32_t)i7 * A78_BANK_SIZE;
  const uint8_t* bs = _pool + (uint32_t)is * A78_BANK_SIZE;
  auto win = [&](int idx, int pages, const uint8_t* base, uint32_t off, bool hot) {
    for (int i = 0; i < pages; i++) { _rd[idx + i] = base + off + (uint32_t)i * 256; _wr[idx + i] = hot ? nullptr : _sink; }
  };
  win(  0, 32, b6, 0x2000, false);   // $4000-$5FFF = SECONDA metà del bank 6 (A13 invertita!)
  win( 32, 32, b6, 0x0000, false);   // $6000-$7FFF = PRIMA metà del bank 6
  win( 64, 32, b7, 0x2000, false);   // $8000-$9FFF = SECONDA metà del bank 7
  win( 96, 64, bs, 0x0000, false);   // $A000-$DFFF = bank selezionato (16 KB, LINEARE)
  win(160, 32, b7, 0x0000, true);    // $E000-$FFFF = PRIMA metà del bank 7 (VETTORI! scritture = hotspot)
}

bool A78Loader::setupActivision() {
  uint8_t want = (_numBanks < A7800_CART_POOL_BANKS) ? (uint8_t)_numBanks : A7800_CART_POOL_BANKS;
  if (want > A78_POOL_MAX) want = A78_POOL_MAX;
  while (want >= 3 && !(_pool = (uint8_t*)malloc((size_t)want * A78_BANK_SIZE))) want--;
  if (!_pool) { Serial.println("[Loader] ERROR: not enough RAM for cart pool"); return false; }
  _poolN = want;
  memset(_poolBank, 0xFF, sizeof(_poolBank)); memset(_poolStamp, 0, sizeof(_poolStamp)); _stamp = 0;
  _actSel = 0;                                                 // bank 0 a $A000-$DFFF dopo il reset
  _actErr = 0; _actSw = 0; _actSeen = 0; _swRingPos = 0;
  detectActLayout();
  {   // MD5 dei dati ROM (header escluso): e' l'identificatore dei
      // dump Activision. 
      // L'unico giudice e' la detection della tabella vettori (detectActLayout).
    char md[33];
    if (computeMD5(md)) {
      const char* note = "non in tabella: se il gioco si blocca, mandaci questo MD5";
      if      (!strcmp(md, "543484c00ba233736bcaba2da20eeea9")) note = "Double Dragon NTSC No-Intro (lineare)";
      else if (!strcmp(md, "de2ebafcf0e37aaa9d0e9525a7f4dd62")) note = "Double Dragon PAL No-Intro (lineare)";
      else if (!strcmp(md, "ac03806cef2558fc795a7d5d8dba7bc0")) note = "Rampage No-Intro (lineare)";
      Serial.printf("[Loader] ACTIVISION: MD5=%s (%s)\n", md, note);
    }
  }
  Serial.printf("[Loader] ACTIVISION: %u banks, pool %u x 16 KB%s\n", (unsigned)_numBanks, (unsigned)_poolN,
                _numBanks <= _poolN ? " (all in RAM)" : " (LRU: 6/7 fissi)");
  if (_poolN >= _numBanks) {                                   // tutta la ROM in RAM: zero ricarichi dalla SD
    for (uint16_t b = 0; b < _numBanks; b++) {
      if (!readActBank(b, _pool + (uint32_t)b * A78_BANK_SIZE)) return false;
      _poolBank[b] = b; _poolStamp[b] = ++_stamp;
    }
    closeFile();
    _actSeen = (uint16_t)((_numBanks >= 32) ? 0xFFFFu : ((1u << _numBanks) - 1));   // gia' tutti caricati
  } else {
    if (!actEnsure(_numBanks - 2, 0xFFFF) || !actEnsure(_numBanks - 1, 0xFFFF) || !actEnsure(_actSel, 0xFFFF)) return false;
    _actSeen = (uint16_t)((1u << 0) | (1u << (_numBanks - 2)) | (1u << (_numBanks - 1)));   // 0, 6 e 7 gia' in pool
  }
  mapAct();
  return true;
}

// scrittura in $8000-$BFFF: seleziona il bank da 16 KB visibile in $8000 (SuperGame)
// scrittura in $E000-$FFFF: seleziona il bank in $A000-$DFFF (Activision, bank = indirizzo & 7)
void A78Loader::cartWrite(A7800& m, uint16_t addr, uint8_t value) {
  if (_cartType == CartType::ACTIVISION) {
    if (addr < 0xE000) return;                                 // hotspot: TUTTE le scritture >= $E000
    uint16_t bank = (uint16_t)(addr & 0x07);                   // il bank sta nel nibble basso dell'INDIRIZZO
    // anello delle ultime 16 scritture hotspot (comprese le no-op)
    _swRingA[_swRingPos & 15] = addr; _swRingV[_swRingPos & 15] = value;
    _swRingB[_swRingPos & 15] = (uint8_t)(bank % _numBanks); _swRingPos++;
    if (bank != _actSel) {
      // log dei primi switch
      const bool log = (_actSw < 12);
      _actSw++;
      const bool ok = actSelect(bank);
      if (log) Serial.printf("[Loader] ACT switch $%04X -> bank %u%s\n", addr, (unsigned)bank, ok ? "" : " FALLITO!");
      if (ok) m.remap();
    }
    return;
  }
  if (addr < 0x8000 || addr >= 0xC000) return;
  // maschera banchi pari -> numBanks-1, dispari -> numBanks-2
  const uint16_t mask = (_numBanks & 1) ? (uint16_t)(_numBanks - 2) : (uint16_t)(_numBanks - 1);
  uint16_t bank;
  if (_cartType == CartType::SUPERGAME9) bank = (uint16_t)((value & mask) + 1);   // il bank 0 del file sta fisso a $4000
  else if (_cartType == CartType::SUPERGAME) bank = (uint16_t)(value & mask);
  else return;                                                                    // STANDARD: nessun hotspot
  if (bank != _slotBank[1] && loadBank(1, bank)) m.remap();
}

// ── Riga diagnostica per il blocco al secondo (debugState) ──────────────────
// Per ACTIVISION: bank selezionato, slot nel pool, impronta della finestra
// $A000-$DFFF (somma dei primi 16 byte alle pagine $A000/$C000/$DF00: cambia col
// bank e rivela se le metà 8K sono nell'ordine giusto), switch visti ed errori
// di lettura. Per SUPERGAME: banchi alle tre finestre + errori.
// Se la CPU è in JAM: AUTOPSIA — anello delle ultime 16 scritture
// hotspot (indirizzo <= valore -> bank: mostra la sequenza reale del gioco) e
// tabella del byte al PC del jam in TUTTI i banchi (pool o file): se un ALTRO
// bank contiene codice valido lì, il gioco intendeva quello e la finestra era
// rimasta sul bank sbagliato > la divergenza è nel codice PRIMA del salto.
uint8_t A78Loader::actByteAt(uint16_t bank, uint16_t off) {
  if (bank >= _numBanks || off >= A78_BANK_SIZE) return 0xFF;
  int idx = actFind(bank);
  if (idx >= 0) return _pool[(uint32_t)idx * A78_BANK_SIZE + off];
  if (!_fileOpen) return 0xFF;
  // dal file, con la stessa normalizzazione delle metà 8K di readActBank
  uint32_t fo = _hdr + (uint32_t)bank * A78_BANK_SIZE;
  if (_actSwap) fo += (off < 0x2000) ? (0x2000 + off) : (off - 0x2000);
  else          fo += off;
  uint8_t v;
  return readAt(fo, &v, 1) ? v : 0xFF;
}

void A78Loader::cartDebug(A7800& m, char* b, int n) {
  if (!_loaded || n < 96) { if (n > 0) b[0] = 0; return; }
  int k = 0;
  if (_cartType == CartType::ACTIVISION) {
    const uint8_t* wA = _rd[0x60];            // pagina $A000  (offset 0x0000 del bank)
    const uint8_t* wC = _rd[0x80];            // pagina $C000  (offset 0x2000)
    const uint8_t* wD = _rd[0x9F];            // pagina $DF00  (offset 0x3F00)
    uint16_t fA = 0, fC = 0, fD = 0;
    if (wA) for (int i = 0; i < 16; i++) fA += wA[i];
    if (wC) for (int i = 0; i < 16; i++) fC += wC[i];
    if (wD) for (int i = 240; i < 256; i++) fD += wD[i];
    k += snprintf(b + k, n - k, "\nACT: bank=%u slot=%d fp=%03X/%03X/%03X ($A000/$C000/$DF00) switch=%u errori=%u\n",
                  (unsigned)_actSel, actFind(_actSel), fA, fC, fD, (unsigned)_actSw, (unsigned)_actErr);
    if (m.cpu.jammed) {
      // anello switch: ultime 16 scritture hotspot (vecchie -> nuove)
      const uint8_t cnt = (_swRingPos < 16) ? _swRingPos : 16;
      if (cnt && k < n - 64) {
        k += snprintf(b + k, n - k, "JAM switch (ultime %u):", cnt);
        for (uint8_t i = 0; i < cnt && k < n - 16; i++) {
          uint8_t idx = (uint8_t)((_swRingPos - cnt + i) & 15);
          k += snprintf(b + k, n - k, " %04X<=%02X>b%u", _swRingA[idx], _swRingV[idx], (unsigned)_swRingB[idx]);
        }
        k += snprintf(b + k, n - k, "\n");
      }
      // byte al PC del jam in tutti i banchi (solo se il jam è nella finestra switchabile)
      if (m.cpu.pc >= 0xA000 && m.cpu.pc < 0xE000 && k < n - 80) {
        uint16_t off = (uint16_t)(m.cpu.pc - 0xA000);
        k += snprintf(b + k, n - k, "JAM byte @off=%04X nei banchi:", off);
        for (uint16_t bnk = 0; bnk < _numBanks && k < n - 10; bnk++)
          k += snprintf(b + k, n - k, " b%u=%02X", (unsigned)bnk, actByteAt(bnk, off));
        k += snprintf(b + k, n - k, "\n");
      }
    }
  } else if (_cartType == CartType::SUPERGAME || _cartType == CartType::SUPERGAME9) {
    snprintf(b, n, "\nBANK: $4000=b%u $8000=b%u $C000=b%u errori=%u\n",
             (unsigned)_slotBank[0], (unsigned)_slotBank[1], (unsigned)_slotBank[2], (unsigned)_actErr);
  } else {
    b[0] = 0;
  }
}

// ── MD5 (dati ROM, header escluso) ─────────────────────────────────────────
struct A78Md5 { uint32_t a, b, c, d; uint64_t len; uint8_t buf[64]; int blen; };
static void a78md5block(A78Md5& s, const uint8_t* p) {
  static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
  static const uint8_t S[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                                 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                                 4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                                 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
  uint32_t w[16];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
  uint32_t a = s.a, b = s.b, c = s.c, d = s.d;
  for (int i = 0; i < 64; i++) {
    uint32_t f, g;
    if (i < 16)      { f = (b & c) | (~b & d); g = (uint32_t)i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (uint32_t)(5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d;          g = (uint32_t)(3 * i + 5) & 15; }
    else             { f = c ^ (b | ~d);       g = (uint32_t)(7 * i) & 15; }
    uint32_t t = d; d = c; c = b;
    b += ((a + f + K[i] + w[g]) << S[i]) | ((a + f + K[i] + w[g]) >> (32 - S[i]));
    a = t;
  }
  s.a += a; s.b += b; s.c += c; s.d += d;
}
bool A78Loader::computeMD5(char out[33]) {
  if (!_fileOpen) return false;
  A78Md5 s = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0, {0}, 0 };
  auto feed = [&](uint8_t v) {
    s.buf[s.blen++] = v; s.len++;
    if (s.blen == 64) { a78md5block(s, s.buf); s.blen = 0; }
  };
  uint8_t buf[512];
  uint32_t off = _hdr, end = _hdr + _romSize;
  while (off < end) {
    uint32_t len = end - off; if (len > sizeof(buf)) len = sizeof(buf);
    if (!readAt(off, buf, len)) return false;
    for (uint32_t i = 0; i < len; i++) feed(buf[i]);
    off += len;
  }
  feed(0x80);
  while (s.blen != 56) feed(0);                                 // padding fino a 56 mod 64
  uint64_t bits = (uint64_t)_romSize * 8;                       // lunghezza in bit del solo messaggio
  for (int i = 0; i < 8; i++) { feed((uint8_t)(bits & 0xFF)); bits >>= 8; }
  static const char hex[] = "0123456789abcdef";
  const uint32_t r[4] = { s.a, s.b, s.c, s.d };
  for (int i = 0; i < 16; i++) {
    uint8_t v = (uint8_t)(r[i >> 2] >> ((i & 3) * 8));
    out[i * 2] = hex[v >> 4]; out[i * 2 + 1] = hex[v & 15];
  }
  out[32] = 0;
  return true;
}

// ── Info / debug / sfoglia ──────────────────────────────────────────────────
void A78Loader::debugStatus() const {
  if (!_loaded) { Serial.println("[Loader] No cartridge loaded"); return; }
  const char* tn = (_cartType == CartType::STANDARD) ? "STANDARD"
                 : (_cartType == CartType::SUPERGAME9) ? "SUPERGAME9"
                 : (_cartType == CartType::ACTIVISION) ? "ACTIVISION" : "SUPERGAME";
  Serial.println("\n=== A78Loader Status ===");
  Serial.printf("Cart   : %s (%lu KB) %s flags=%04X pokey=$%04X\n", _romName, (unsigned long)(_romSize / 1024), tn, _flags, _pokeyBase);
  if (_cartType == CartType::SUPERGAME || _cartType == CartType::SUPERGAME9) {
    Serial.printf("Banks  : %u, pool=%u, file=%s, RAM $4000=%s\n", (unsigned)_numBanks, (unsigned)_poolN, _fileOpen ? "open (LRU)" : "closed", _sgRam ? "yes" : "no");
    for (int s = 0; s < 3; s++) Serial.printf(" Slot %d ($%04X-$%04X) -> bank %u (pool %u) %s\n", s, 0x4000 + s * 0x4000, 0x7FFF + s * 0x4000, (unsigned)_slotBank[s], (unsigned)_slotPool[s], _slotOn[s] ? "" : "(off)");
  }
  if (_cartType == CartType::ACTIVISION)
    Serial.printf("Windows: $4000=b%u.hi $6000=b%u.lo $8000=b%u.hi $A000=b%u (sel) $E000=b%u.lo, pool=%u, file=%s, dump %s\n",
                  (unsigned)(_numBanks - 2), (unsigned)(_numBanks - 2), (unsigned)(_numBanks - 1), (unsigned)_actSel,
                  (unsigned)(_numBanks - 1), (unsigned)_poolN, _fileOpen ? "open (LRU)" : "closed", _actSwap ? "(meta' normalizzate)" : "No-Intro (lineare)");
  Serial.println("========================");
}

void A78Loader::listROMs(const char* folder) {
  if (!_fsReady) { Serial.println("[Loader] Filesystem not ready"); return; }
  Serial.printf("Cartridges in %s:\n==========================================\n", folder);
  if (_currentFS == FSType::FS_SD) {
    File root = SD.open(folder);
    if (!root) { Serial.println(" ERROR: folder not found"); return; }
    File e = root.openNextFile();
    while (e) { if (!e.isDirectory() && isMediaFile(e.name())) Serial.printf(" %-40s %6lu KB\n", e.name(), (unsigned long)(e.size() / 1024)); e.close(); e = root.openNextFile(); }
    root.close();
  } else {
    Dir dir = LittleFS.openDir(folder);
    while (dir.next()) if (isMediaFile(dir.fileName().c_str())) { File f = dir.openFile("r"); if (f) { Serial.printf(" %-40s %6lu KB\n", dir.fileName().c_str(), (unsigned long)(f.size() / 1024)); f.close(); } }
  }
  Serial.println("==========================================");
}

static int scanFolder(FSType fs, const char* folder, int index, char* out, size_t outLen) {
  int n = 0;
  auto visit = [&](const char* name) {
    if (!A78Loader::isMediaFile(name)) return;
    if (n == index && out) snprintf(out, outLen, "%s/%s", folder, (name[0] == '/') ? name + 1 : name);
    n++;
  };
  if (fs == FSType::FS_SD) {
    File root = SD.open(folder); if (!root) return 0;
    File e = root.openNextFile();
    while (e) { if (!e.isDirectory()) visit(e.name()); e.close(); e = root.openNextFile(); }
    root.close();
  } else if (fs == FSType::FS_LITTLEFS) {
    Dir dir = LittleFS.openDir(folder);
    while (dir.next()) visit(dir.fileName().c_str());
  }
  return n;
}

int A78Loader::countROMs(const char* folder) { 
  return _fsReady ? scanFolder(_currentFS, folder, -1, nullptr, 0) : 0; 
}

bool A78Loader::loadIndex(int index, const char* folder) {
  if (!_fsReady) return false;
  char path[128] = {0};
  int n = scanFolder(_currentFS, folder, index, path, sizeof(path));
  if (index < 0 || index >= n || !path[0]) return false;
  _browseIdx = index;
  return loadROM(path);
}

bool A78Loader::loadNext(int delta, const char* folder) {
  int n = countROMs(folder);
  if (n <= 0) { Serial.printf("[Loader] No cartridge in %s\n", folder); return false; }
  int i = (_browseIdx < 0) ? (delta > 0 ? 0 : n - 1) : (_browseIdx + delta + n) % n;
  return loadIndex(i, folder);
}
