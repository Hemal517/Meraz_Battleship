#ifndef MERAZ_LOADOUT_H
#define MERAZ_LOADOUT_H

/* =====================================================================
   MERAZ_LOADOUT.H - the "choose your powers" step for a team board
   =====================================================================
   NO radio code in here. This only does what has to happen BEFORE you
   register with Central:

       1. the team picks 2 attack powers and 1 defence power
       2. the team locks the choice in
       3. you get ONE byte back - the loadout - to put at the end of your
          34-byte registration packet (see PARTICIPANT_GUIDE.md, section 3)

   Typical use:

       #include "meraz_loadout.h"
       uint8_t myLoadout;

       void setup() {
         Serial.begin(115200);
         myLoadout = selectLoadoutSerial();   // blocks until locked in
         // ...only now start ESP-NOW and register
       }

   If you build your own screen (touch, buttons, ...) you can skip
   selectLoadoutSerial() and just use loadoutIsValid() to check what you
   built before you send it.
   ===================================================================== */

#include <Arduino.h>

// Power ids (these go in the power-up packet) ...
#define POWER_SONAR   1
#define POWER_SALVO   2
#define POWER_MINE    3
#define POWER_REPAIR  4
#define POWER_SMOKE   5
#define POWER_SHIELD  6   // compulsory - always yours
#define POWER_DOUBLE  7

// ... and the loadout byte has bit (power id - 1) set for each chosen power.
#define LOADOUT_BIT(power)    ((uint8_t)(1u << ((power) - 1)))
#define LOADOUT_ATTACK_MASK   (LOADOUT_BIT(POWER_SONAR) | LOADOUT_BIT(POWER_SALVO) | LOADOUT_BIT(POWER_MINE) | LOADOUT_BIT(POWER_DOUBLE))   // 0x47
#define LOADOUT_DEFENCE_MASK  (LOADOUT_BIT(POWER_REPAIR) | LOADOUT_BIT(POWER_SMOKE))                                                       // 0x18
#define LOADOUT_SHIELD_BIT    LOADOUT_BIT(POWER_SHIELD)                                                                                    // 0x20

// How many bits are set in a byte.
inline int loadoutBitCount(uint8_t v) {
  int n = 0;
  for (int b = 0; b < 8; b++) if (v & (1u << b)) n++;
  return n;
}

// The same test Central runs: exactly 2 attack powers, exactly 1 defence
// power, no stray bits. The shield bit may be set or clear.
inline bool loadoutIsValid(uint8_t loadout) {
  const uint8_t known = (uint8_t)(LOADOUT_ATTACK_MASK | LOADOUT_DEFENCE_MASK | LOADOUT_SHIELD_BIT);
  if (loadout & (uint8_t)~known) return false;
  return loadoutBitCount(loadout & LOADOUT_ATTACK_MASK) == 2 &&
         loadoutBitCount(loadout & LOADOUT_DEFENCE_MASK) == 1;
}

// Prints e.g.  "Sonar Ping + Double Attack + Smoke Screen (+ Shield)"
inline void printLoadout(uint8_t loadout) {
  static const uint8_t ids[6] = { POWER_SONAR, POWER_SALVO, POWER_MINE, POWER_DOUBLE, POWER_SMOKE, POWER_REPAIR };
  static const char *names[6] = { "Sonar Ping", "Salvo", "Mine", "Double Attack", "Smoke Screen", "Repair" };
  bool first = true;
  for (int i = 0; i < 6; i++) {
    if (!(loadout & LOADOUT_BIT(ids[i]))) continue;
    if (!first) Serial.print(" + ");
    Serial.print(names[i]);
    first = false;
  }
  Serial.print(" (+ Shield)");
}

// Reads one line from the Serial Monitor (blocking). Needs "Newline" selected
// at the bottom of the Serial Monitor.
inline void readLineSerial(char *buf, size_t size) {
  size_t n = 0;
  for (;;) {
    while (!Serial.available()) delay(5);
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { if (n == 0) continue; break; }   // ignore empty lines
    if (n < size - 1) buf[n++] = c;
  }
  buf[n] = '\0';
}

// Digits in `line` -> bit mask of picked menu numbers (bit 0 = "1", bit 1 = "2", ...).
// Returns false if a number repeats or is outside 1..maxChoice. Other characters
// (spaces, commas) are ignored.
inline bool parsePicks(const char *line, int maxChoice, uint8_t &picks) {
  picks = 0;
  for (const char *p = line; *p; p++) {
    if (*p < '0' || *p > '9') continue;
    int n = *p - '0';
    if (n < 1 || n > maxChoice) return false;
    uint8_t bit = (uint8_t)(1u << (n - 1));
    if (picks & bit) return false;
    picks |= bit;
  }
  return true;
}

// The whole step: menu -> picks -> confirm. Returns the locked loadout byte
// (always a legal one, with the shield bit set). Blocks until the team confirms.
inline uint8_t selectLoadoutSerial() {
  static const uint8_t attackIds[4] = { POWER_SONAR, POWER_SALVO, POWER_MINE, POWER_DOUBLE };
  static const uint8_t defenceIds[2] = { POWER_SMOKE, POWER_REPAIR };
  char line[32];

  for (;;) {
    Serial.println();
    Serial.println("=== CHOOSE YOUR LOADOUT ===");
    Serial.println("Always yours: Single Strike (unlimited) and Shield (unlimited, never twice in a row).");
    Serial.println();
    Serial.println("ATTACK powers - pick TWO (2 uses each):");
    Serial.println("  1 = Sonar Ping     scan a 3x3 area of an opponent for ship cells");
    Serial.println("  2 = Salvo          fire at 3 cells in a straight row/column");
    Serial.println("  3 = Mine           hide a trap on one of your own water cells");
    Serial.println("  4 = Double Attack  fire at any 2 cells at once");
    Serial.println("Type two numbers, e.g.  1 3   then Enter:");

    uint8_t attackPicks = 0;
    readLineSerial(line, sizeof(line));
    if (!parsePicks(line, 4, attackPicks) || loadoutBitCount(attackPicks) != 2) {
      Serial.println("-> Please pick exactly TWO different numbers from 1-4. Starting again.");
      continue;
    }

    Serial.println();
    Serial.println("DEFENCE power - pick ONE (2 uses):");
    Serial.println("  1 = Smoke Screen   hides attacks on you until your next turn");
    Serial.println("  2 = Repair         flip one of your hit cells back to intact");
    Serial.println("Type 1 or 2, then Enter:");

    uint8_t defencePicks = 0;
    readLineSerial(line, sizeof(line));
    if (!parsePicks(line, 2, defencePicks) || loadoutBitCount(defencePicks) != 1) {
      Serial.println("-> Please pick exactly ONE number: 1 or 2. Starting again.");
      continue;
    }

    uint8_t loadout = LOADOUT_SHIELD_BIT;
    for (int i = 0; i < 4; i++) if (attackPicks & (1u << i)) loadout |= LOADOUT_BIT(attackIds[i]);
    for (int i = 0; i < 2; i++) if (defencePicks & (1u << i)) loadout |= LOADOUT_BIT(defenceIds[i]);

    Serial.println();
    Serial.print("Your loadout: "); printLoadout(loadout);
    Serial.print("   (byte 0x"); Serial.print(loadout, HEX); Serial.println(")");
    Serial.println("Lock it in? You can NOT change it once the match starts.  Y = lock in, N = choose again");

    readLineSerial(line, sizeof(line));
    if (line[0] == 'Y' || line[0] == 'y') {
      Serial.println("Loadout locked. You may now connect to Central.");
      return loadout;
    }
  }
}

#endif
