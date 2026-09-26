#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

/* =====================================================================
   GAME_LOGIC.H - the actual battleship rules, with zero dependency on
   Arduino, WiFi, or ESP-NOW.
   =====================================================================
   This header holds every rule from the spec that doesn't care how a
   packet got here: ship validation, registration rules, attack
   validation, turn order, and elimination. central_node.ino #includes
   this and calls into it from handleRegistration()/handleAttack()/
   cmdSkipTurn() - it's the ONE place those rules are written down.

   Because it's plain, portable C++ (just <cstdint>/<cstring>), this
   exact same file also compiles on a regular PC with g++. That's what
   central_logic_test.cpp (the desktop test harness) uses it for - so
   when that harness passes, it's testing these exact rules, not a
   hand-copied approximation of them.

   Keep this file in the same folder as central_node.ino (Arduino IDE
   will show it as a second tab automatically), and in the same folder
   as central_logic_test.cpp when compiling the test harness.
   ===================================================================== */

#include <cstdint>
#include <cstring>

// =====================================================================
// CONSTANTS (must match participant_node.ino's copies of the same values)
// =====================================================================

#define MAX_TEAMS  4
#define GRID_SIZE  5

#define ORIENT_HORIZONTAL 0
#define ORIENT_VERTICAL   1

#define CELL_WATER      0
#define CELL_SHIP       1
#define CELL_MISS       2
#define CELL_HIT        3

#define RESULT_MISS     0
#define RESULT_HIT      1
#define RESULT_INVALID  2
#define RESULT_SUNK     3   // this hit eliminated the whole team

#define REG_OK                   0
#define REG_REJECTED             1
#define REG_RECONNECTED          2
#define REG_RECONNECT_REJECTED   3

// Internal-only - never sent over the wire. tryRegister() returns one
// of these instead of the generic REG_REJECTED so the caller can log
// *why* it failed; map all of them to REG_REJECTED before building
// the actual RegAckPacket, since that's all participant_node.ino
// understands.
#define REG_FAIL_MAC_MISMATCH   10
#define REG_FAIL_BAD_NAME       11
#define REG_FAIL_BAD_SHIPS      12
#define REG_FAIL_GAME_STARTED   13

// Same idea for tryAttack() - these never go on the wire either.
// central_node.ino maps all of them to RESULT_INVALID for the
// FeedbackPacket, and uses the specific value for its own log line.
#define ATTACK_FAIL_ID_MISMATCH     20
#define ATTACK_FAIL_NOT_RUNNING     21
#define ATTACK_FAIL_WRONG_TURN      22
#define ATTACK_FAIL_ATTACKER_DEAD   23
#define ATTACK_FAIL_BAD_TARGET      24
#define ATTACK_FAIL_SELF_ATTACK     25
#define ATTACK_FAIL_TARGET_INVALID  26
#define ATTACK_FAIL_BAD_COORDS      27
#define ATTACK_FAIL_ALREADY_HIT     28

#define STATE_SETUP     0
#define STATE_READY     1
#define STATE_RUNNING   2
#define STATE_GAMEOVER  3

// =====================================================================
// SHIP PLACEMENT
// =====================================================================

typedef struct __attribute__((packed)) {
  uint8_t ship_len;     // 1, 3, or 5
  uint8_t start_x;      // 0-4
  uint8_t start_y;      // 0-4
  uint8_t orientation;  // ORIENT_HORIZONTAL or ORIENT_VERTICAL
} ShipPlacement;

// Builds outGrid from 3 ship placements. Returns true only if the
// layout is fully legal: exactly one ship of length 1, one of 3, one
// of 5, straight lines only, fits in the 5x5 grid, no overlaps.
inline bool buildAndValidateGrid(ShipPlacement ships[3], uint8_t outGrid[GRID_SIZE][GRID_SIZE]) {
  for (int r = 0; r < GRID_SIZE; r++)
    for (int c = 0; c < GRID_SIZE; c++)
      outGrid[r][c] = CELL_WATER;

  bool sawLen1 = false, sawLen3 = false, sawLen5 = false;

  for (int s = 0; s < 3; s++) {
    uint8_t len = ships[s].ship_len;
    uint8_t sx = ships[s].start_x;
    uint8_t sy = ships[s].start_y;
    uint8_t orient = ships[s].orientation;

    if (len == 1) { if (sawLen1) return false; sawLen1 = true; }
    else if (len == 3) { if (sawLen3) return false; sawLen3 = true; }
    else if (len == 5) { if (sawLen5) return false; sawLen5 = true; }
    else return false;

    if (orient != ORIENT_HORIZONTAL && orient != ORIENT_VERTICAL) return false;
    if (sx >= GRID_SIZE || sy >= GRID_SIZE) return false;

    for (int i = 0; i < len; i++) {
      int x = sx + (orient == ORIENT_HORIZONTAL ? i : 0);
      int y = sy + (orient == ORIENT_VERTICAL ? i : 0);
      if (x >= GRID_SIZE || y >= GRID_SIZE) return false;
      if (outGrid[y][x] != CELL_WATER) return false;
      outGrid[y][x] = CELL_SHIP;
    }
  }

  return sawLen1 && sawLen3 && sawLen5;
}

inline bool shipsMatch(ShipPlacement a[3], ShipPlacement b[3]) {
  return memcmp(a, b, sizeof(ShipPlacement) * 3) == 0;
}

inline bool validTeamName(const char *name) {
  int len = 0;
  while (len < 20 && name[len] != '\0') len++;
  return len > 0;
}

// =====================================================================
// TURN ORDER
// =====================================================================

// A team counts as "in the game" for turn-order purposes only if it's
// both registered AND not eliminated - this is what lets a round be
// force-started with fewer than 4 teams (see tryForceStart() below)
// without turn order ever landing on a team that never showed up.
inline void advanceTurn(uint8_t &roundState, uint8_t &currentTurnIndex, bool eliminated[MAX_TEAMS], bool registered[MAX_TEAMS]) {
  int aliveCount = 0, lastAlive = -1;
  for (int i = 0; i < MAX_TEAMS; i++) {
    if (registered[i] && !eliminated[i]) { aliveCount++; lastAlive = i; }
  }
  (void)lastAlive;  // not needed here - central_node.ino logs the winner separately

  if (aliveCount <= 1) {
    roundState = STATE_GAMEOVER;
    return;
  }

  int idx = currentTurnIndex;
  for (int tries = 0; tries < MAX_TEAMS; tries++) {
    idx = (idx + 1) % MAX_TEAMS;
    if (registered[idx] && !eliminated[idx]) {
      currentTurnIndex = idx;
      return;
    }
  }
}

// =====================================================================
// REGISTRATION
// =====================================================================

inline void updateRoundStateAfterRegistration(uint8_t &roundState, bool registered[MAX_TEAMS]) {
  if (roundState == STATE_SETUP || roundState == STATE_READY) {
    bool allRegistered = true;
    for (int i = 0; i < MAX_TEAMS; i++) {
      if (!registered[i]) allRegistered = false;
    }
    roundState = allRegistered ? STATE_READY : STATE_SETUP;
  }
}

// Runs every registration rule from the spec and mutates the game
// arrays in place on success. Returns REG_OK / REG_REJECTED /
// REG_RECONNECTED / REG_RECONNECT_REJECTED - the caller (central_node.ino)
// is responsible for resolving the sender's MAC to macTeam (1-4, or a
// value outside that range if unknown) BEFORE calling this, and for
// all the I/O afterwards (sending the ack, logging, saving state).
inline uint8_t tryRegister(
  int macTeam,
  int claimedTeamId,
  const char *teamName,
  ShipPlacement ships[3],
  uint8_t &roundState,
  bool registered[MAX_TEAMS],
  char teamNames[MAX_TEAMS][20],
  ShipPlacement storedShips[MAX_TEAMS][3],
  uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE],
  int remainingShips[MAX_TEAMS],
  bool eliminated[MAX_TEAMS]
) {
  if (macTeam < 1 || macTeam > MAX_TEAMS) return REG_FAIL_MAC_MISMATCH;
  if (claimedTeamId != macTeam) return REG_FAIL_MAC_MISMATCH;
  if (!validTeamName(teamName)) return REG_FAIL_BAD_NAME;

  uint8_t candidateGrid[GRID_SIZE][GRID_SIZE];
  if (!buildAndValidateGrid(ships, candidateGrid)) return REG_FAIL_BAD_SHIPS;

  int idx = macTeam - 1;

  if (roundState == STATE_RUNNING || roundState == STATE_GAMEOVER) {
    // Game already started - this can only be a reconnection attempt,
    // never a fresh registration or a ship-layout change.
    if (!registered[idx]) return REG_FAIL_GAME_STARTED;

    bool nameMatches = (strncmp(teamNames[idx], teamName, 20) == 0);
    bool shipsAreSame = shipsMatch(storedShips[idx], ships);
    if (nameMatches && shipsAreSame) return REG_RECONNECTED;
    return REG_RECONNECT_REJECTED;  // never touch stored state on mismatch
  }

  // SETUP or READY: accept this as a (re)registration.
  registered[idx] = true;
  strncpy(teamNames[idx], teamName, 20);
  teamNames[idx][19] = '\0';
  memcpy(storedShips[idx], ships, sizeof(ShipPlacement) * 3);
  memcpy(grid[idx], candidateGrid, sizeof(candidateGrid));
  remainingShips[idx] = 9;
  eliminated[idx] = false;

  updateRoundStateAfterRegistration(roundState, registered);
  return REG_OK;
}

// Lets the organizer start a round without all 4 teams registered -
// e.g. one team never showed up. Requires at least 2 registered teams
// and that a round isn't already in progress. On success, starts the
// round with the first registered team going first and returns true;
// on failure, leaves everything untouched and returns false.
inline bool tryForceStart(uint8_t &roundState, uint8_t &currentTurnIndex, bool registered[MAX_TEAMS]) {
  if (roundState == STATE_RUNNING || roundState == STATE_GAMEOVER) return false;

  int registeredCount = 0;
  int firstIdx = -1;
  for (int i = 0; i < MAX_TEAMS; i++) {
    if (registered[i]) {
      registeredCount++;
      if (firstIdx == -1) firstIdx = i;
    }
  }

  if (registeredCount < 2) return false;

  roundState = STATE_RUNNING;
  currentTurnIndex = firstIdx;
  return true;
}

// =====================================================================
// ATTACKS
// =====================================================================

// Runs every attack-validation rule from the spec (all 11 checks) and,
// on a valid attack, updates the grid/remaining-ships/eliminated/turn
// state. Returns RESULT_MISS / RESULT_HIT / RESULT_SUNK / RESULT_INVALID.
// Same division of responsibility as tryRegister(): the caller resolves
// macTeam from the sender's MAC and handles all I/O afterwards.
inline uint8_t tryAttack(
  int macTeam,
  int claimedAttackerId,
  int targetId,
  int x,
  int y,
  uint8_t &roundState,
  uint8_t &currentTurnIndex,
  uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE],
  int remainingShips[MAX_TEAMS],
  bool eliminated[MAX_TEAMS],
  bool registered[MAX_TEAMS]
) {
  if (macTeam < 1 || macTeam > MAX_TEAMS) return ATTACK_FAIL_ID_MISMATCH;
  if (claimedAttackerId != macTeam) return ATTACK_FAIL_ID_MISMATCH;
  if (roundState != STATE_RUNNING) return ATTACK_FAIL_NOT_RUNNING;

  int attackerIdx = macTeam - 1;
  if (attackerIdx != currentTurnIndex) return ATTACK_FAIL_WRONG_TURN;
  if (eliminated[attackerIdx]) return ATTACK_FAIL_ATTACKER_DEAD;

  if (targetId < 1 || targetId > MAX_TEAMS) return ATTACK_FAIL_BAD_TARGET;
  int targetIdx = targetId - 1;
  if (targetIdx == attackerIdx) return ATTACK_FAIL_SELF_ATTACK;
  if (!registered[targetIdx] || eliminated[targetIdx]) return ATTACK_FAIL_TARGET_INVALID;

  if (x < 0 || x >= GRID_SIZE || y < 0 || y >= GRID_SIZE) return ATTACK_FAIL_BAD_COORDS;

  uint8_t cell = grid[targetIdx][y][x];
  if (cell == CELL_MISS || cell == CELL_HIT) return ATTACK_FAIL_ALREADY_HIT;

  uint8_t resultCode;
  if (cell == CELL_WATER) {
    grid[targetIdx][y][x] = CELL_MISS;
    resultCode = RESULT_MISS;
  } else {  // CELL_SHIP
    grid[targetIdx][y][x] = CELL_HIT;
    remainingShips[targetIdx]--;
    if (remainingShips[targetIdx] <= 0) {
      eliminated[targetIdx] = true;
      resultCode = RESULT_SUNK;
    } else {
      resultCode = RESULT_HIT;
    }
  }

  advanceTurn(roundState, currentTurnIndex, eliminated, registered);
  return resultCode;
}

#endif  // GAME_LOGIC_H
