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
 * ============================================================================
 * Atari 7800 per Raspberry Pi Pico (RP2040) - configurazione
 * ============================================================================
 */
#include <stdint.h>

// ---- Clock ---------------------------------------------------------------
#define PICO_CLOCK_KHZ        300000
#define PICO_VREG_VOLTAGE     VREG_VOLTAGE_1_30
#define PICO_PERI_FROM_SYS    1        // clk_peri = clk_sys (altrimenti SPI limitato a 24MHz)

// ---- Standard video (unita' di tempo = clock MARIA, 7.16 MHz; 1 ciclo CPU = 4 clock) ----
#define ATARI_PAL             0        // 0 = NTSC (262 righe), 1 = PAL (312 righe)
#if ATARI_PAL
  #define SCANLINES           312
  #define LINE_CLOCKS         456      // clock MARIA per riga (114 cicli CPU)
  #define CPU_HZ              1773448
  #define AUDIO_SAMPLE_RATE   31112    // 2 campioni per riga: il clock audio del TIA e' 2 x frequenza di riga
#else
  #define SCANLINES           262
  #define LINE_CLOCKS         454      // 113.5 cicli CPU per riga
  #define CPU_HZ              1789773
  #define AUDIO_SAMPLE_RATE   31540
#endif
#define A7800_VBLANK_LINES    16       // righe prima dell'inizio della zona attiva (MSTAT bit7 = 1)
#define A7800_ACTIVE_LINES    240      // righe disegnate = righe del display (FB_H)

// ---- Audio (MAX98357 I2S) ------------------------------------------------
#define PIN_I2S_BCLK          26
#define PIN_I2S_DIN           28
#define AUDIO_DMA_BUFFERS     6
#define AUDIO_DMA_WORDS       128
#define AUDIO_GAIN            2
#define POKEY_ULTRA_PER       ((int)(CPU_HZ / (0.8 * AUDIO_SAMPLE_RATE)))

// ---- Video ---------------------------------------------------------------
#define FB_W                  320
#define FB_H                  240
#define VIDEO_FRAMESKIP       0
#define TFT_CHUNK_LINES       8

// ---- Input ---------------------------------------------------------------
#define PIN_JOY_UP            2
#define PIN_JOY_DOWN          3
#define PIN_JOY_LEFT          4
#define PIN_JOY_RIGHT         5
#define PIN_JOY_FIRE          6
#define PIN_JOY_FIRE2         7
#define PIN_BTN_RESET         8       // pulsanti console opzionali (-1 = non usato)
#define PIN_BTN_SELECT        1
#define PIN_BTN_PAUSE         0
#define PIN_PS2_CLK           15
#define PIN_PS2_DATA          14

// ---- Loader (SD / LittleFS) ----------------------------------------------
#define SD_CS                 13
#define SD_MISO               12
#define SD_MOSI               11
#define SD_SCLK               10
#define SD_SPI_HZ             10000000
#define GAME_DIR              "/Pi7800/games"      // + anteprime .jpg 200x150 e noImage.jpg
#define ATARI_BROWSER_JPG     1
#define MAX_SORTED_GAMES      200
#define A7800_CART_POOL_BANKS 6                    // pool di bank da 16KB (allocato solo quando serve)
#define MENU_BACK_FRAMES      50                   // RESET+SELECT tenuti ~1 s = ritorno al browser
// #define A7800_AUTOLOAD     "/Pi7800/games/gioco.a78"

// ---- Diagnostica ---------------------------------------------------------
#define A7800_DEBUG           1

#ifndef A7800_CART_POOL_BANKS
#define A7800_CART_POOL_BANKS 12
#endif
