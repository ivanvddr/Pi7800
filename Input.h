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
 */
 
#include <stdint.h>
// Stato letto dalla macchina (core0)
extern uint8_t g_swcha;    // SWCHA: bit7 destra, bit6 sinistra, bit5 giu', bit4 su (giocatore 0); bit3..0 giocatore 1. 0 = premuto
extern uint8_t g_fire0;    // INPT4 ($0C), linea del pulsante SINGOLO (modo 1 tasto): 0x00 = premuto, 0x80 = rilasciato.
                           // Come sul controller originale (Pro-Line/CX78, tasti a doppio polo): attivo se FIRE *o*
                           // FIRE2 e' premuto -> ai giochi a 1 tasto (Ms. Pac-Man) e al browser rispondono entrambi.
extern uint8_t g_inpt1;    // INPT1 ($09): P0 pulsante SINISTRO nel modo 2 tasti. ATTENZIONE: convenzione INVERTITA
extern uint8_t g_inpt0;    // INPT0 ($08): P0 pulsante DESTRO   nel modo 2 tasti. rispetto a INPT4: bit7=1 -> PREMUTO,
                           // 0 -> rilasciato (sorgente Commando 7800 "LDA INPT0/BMI" + guida 8bitdev.org sez. 1.5.2)
extern uint8_t g_swchb;    // SWCHB: bit0 RESET, bit1 SELECT, bit3 PAUSE (0 = premuto); gli altri bit a 1

enum { EV_RESET, EV_SELECT, EV_PAUSE, EV_WARM, EV_BROWSER };
struct InEvent { uint8_t type, code, down; };

void inputBegin();
void inputUpdate();               // legge joystick/pulsanti (1 volta per frame)
bool inputPoll(InEvent& e);       // eventi della tastiera PS/2
void inputConsoleKb(uint8_t bit, bool down);   // bit 0 RESET, 1 SELECT, 2 PAUSE

typedef uint8_t (*NavReadFn)();   // lettura della navigazione (stesso formato di FunduinoJoystick::read)

// Navigazione per browser: STESSO formato di FunduinoJoystick::read() (0 = premuto):
//   bit0 SU, bit1 GIU', bit2 DESTRA, bit3 SINISTRA, bit4 FIRE. Unisce joystick e frecce/Invio/Spazio.
uint8_t inputNavState();
