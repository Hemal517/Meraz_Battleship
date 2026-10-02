#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

/* =====================================================================
   GAME_LOGIC.H - the actual battleship rules, with zero dependency on
   Arduino, WiFi, or ESP-NOW.
   =====================================================================
   This header holds every rule that doesn't care how a packet got here:
   ship validation, registration, attack validation, turn order,
   elimination and (v3) the POWER-UPS.

   Keep this file in the same folder as central_node.ino (Arduino IDE
   shows it as a second tab), and next to central_logic_test.cpp /
   powerup_test.cpp when compiling the test harnesses.

   v3 CHANGES (all backwards compatible):
     - tryAttack() got two OPTIONAL trailing parameters. Without them it
       behaves exactly as before, so central_logic_test.cpp still passes.
     - advanceTurn() is untouched; advanceTurnPS() is the power-up aware
       version (mine skips + shield/smoke expiry).
     - New: PowerState, fireAtCell(), tryPowerUp(), displayCell(),
       displayRemaining().
   ===================================================================== */

#include <cstdint>
#include <cstring>

// =====================================================================
// CONSTANTS (must match participant_node.ino's copies of the same values)
// =====================================================================

#define MAX_TEAMS  4
#define GRID_SIZE  7

// Ship directions. start_x/start_y is always the ship's FIRST cell and
// the ship grows from there:
//   HORIZONTAL : rightward       (dx=+1, dy= 0)
//   VERTICAL   : downward        (dx= 0, dy=+1)
//   DIAG_DOWN  : down-right      (dx=+1, dy=+1)  e.g. (0,0) -> (4,4)
//   DIAG_UP    : up-right        (dx=+1, dy=-1)  e.g. (0,4) -> (4,0)
#define ORIENT_HORIZONTAL 0
#define ORIENT_VERTICAL   1
#define ORIENT_DIAG_DOWN  2
#define ORIENT_DIAG_UP    3

#define CELL_WATER      0
#define CELL_SHIP       1
#define CELL_MISS       2
#define CELL_HIT        3

// FeedbackPacket.result_code
#define RESULT_MISS     0
#define RESULT_HIT      1
#define RESULT_INVALID  2
#define RESULT_SUNK     3   // this hit eliminated the whole team
#define RESULT_MINE     4   // v3: the cell held a MINE - attacker loses their next turn
#define RESULT_BLOCKED  5   // v3: the cell is under a SHIELD - nothing happened, turn used
#define RESULT_UNKNOWN  6   // v3: target is under SMOKE - attacker is not told the result

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
// POWER-UP CONSTANTS
// =====================================================================

#define POWER_SONAR   1   // 3x3 area on an opponent -> number of ship cells, private
#define POWER_SALVO   2   // 3 cells in a row/column on ONE opponent
#define POWER_MINE    3   // hidden trap on one of your own WATER cells (placed before the game)
#define POWER_REPAIR  4   // flip one of your hit cells back to intact
#define POWER_SMOKE   5   // until your next turn, attacks on you are not reported to anyone
#define POWER_SHIELD  6   // until your next turn, a 3x3 block of your grid can't be hit
#define POWER_DOUBLE  7   // 2 cells (any two) on ONE opponent
#define POWER_COUNT   7

// How many DIFFERENT power-ups one team may use per game (each can be
// used once). 7 = all of them. Set it to 3 to make teams CHOOSE.
// The mine counts toward this budget.
#define POWERUP_BUDGET 7

#define NO_CELL 255       // "no mine placed"

// PowerResultPacket.status (on the wire)
#define PWR_STATUS_OK        0
#define PWR_STATUS_REJECTED  1
#define PWR_STATUS_JAMMED    2   // sonar was blocked by the target's smoke screen (power spent)

// Internal-only return codes of tryPowerUp()
#define PWR_OK                  0
#define PWR_JAMMED              40   // success, but sonar was jammed (power + turn spent)
#define PWR_FAIL_ID_MISMATCH    41
#define PWR_FAIL_BAD_POWER      42
#define PWR_FAIL_NOT_RUNNING    43
#define PWR_FAIL_WRONG_TURN     44
#define PWR_FAIL_DEAD           45
#define PWR_FAIL_ALREADY_USED   46
#define PWR_FAIL_BUDGET         47
#define PWR_FAIL_BAD_TARGET     48
#define PWR_FAIL_BAD_COORDS     49
#define PWR_FAIL_ALREADY_HIT    50
#define PWR_FAIL_BAD_SHAPE      51
#define PWR_FAIL_NOT_SETUP      52   // mines can only be placed before the game starts
#define PWR_FAIL_MINE_ON_SHIP   53
#define PWR_FAIL_NOTHING_TO_REPAIR 54
#define PWR_FAIL_NOT_REGISTERED 55

// =====================================================================
// SHIP PLACEMENT
// =====================================================================

typedef struct __attribute__((packed)) {
  uint8_t ship_len;     // 1, 3, or 5
  uint8_t start_x;      // 0-6
  uint8_t start_y;      // 0-6
  uint8_t orientation;  // ORIENT_HORIZONTAL / VERTICAL / DIAG_DOWN / DIAG_UP
} ShipPlacement;

// Builds outGrid from 3 ship placements. Returns true only if the
// layout is fully legal: exactly one ship of length 1, one of 3, one
// of 5, straight lines only (horizontal, vertical, or diagonal), fits
// in the 7x7 grid, no overlaps.
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

    int dx, dy;
    switch (orient) {
      case ORIENT_HORIZONTAL: dx = 1; dy = 0;  break;
      case ORIENT_VERTICAL:   dx = 0; dy = 1;  break;
      case ORIENT_DIAG_DOWN:  dx = 1; dy = 1;  break;
      case ORIENT_DIAG_UP:    dx = 1; dy = -1; break;
      default: return false;
    }
    if (sx >= GRID_SIZE || sy >= GRID_SIZE) return false;

    for (int i = 0; i < len; i++) {
      int x = sx + dx * i;
      int y = sy + dy * i;
      if (x < 0 || x >= GRID_SIZE || y < 0 || y >= GRID_SIZE) return false;
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

inline bool inGrid(int x, int y) {
  return x >= 0 && x < GRID_SIZE && y >= 0 && y < GRID_SIZE;
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
// POWER-UP STATE
// =====================================================================
// Everything the power-ups need to remember. All plain bytes so
// central_node.ino can save it to flash as part of its snapshot.
//
// "One round" for SHIELD and SMOKE means: from the moment the owner
// uses it until the owner's NEXT turn begins (i.e. every other team
// gets exactly one turn against it).

typedef struct __attribute__((packed)) {
  uint8_t used[MAX_TEAMS];                              // bit (power-1) set = already used
  uint8_t mineX[MAX_TEAMS], mineY[MAX_TEAMS];           // NO_CELL = none
  uint8_t shieldOn[MAX_TEAMS];
  uint8_t shieldX[MAX_TEAMS], shieldY[MAX_TEAMS];       // centre of the 3x3 block
  uint8_t smokeOn[MAX_TEAMS];
  uint8_t smokeMask[MAX_TEAMS][GRID_SIZE][GRID_SIZE];   // cells shot while smoked (hidden from the dashboard)
  uint8_t skipNext[MAX_TEAMS];                          // lose the next turn (stepped on a mine)
} PowerState;

inline void resetPowerState(PowerState &ps) {
  memset(&ps, 0, sizeof(ps));
  for (int i = 0; i < MAX_TEAMS; i++) { ps.mineX[i] = NO_CELL; ps.mineY[i] = NO_CELL; }
}

inline int powerCount(uint8_t usedMask) {
  int n = 0;
  for (int b = 0; b < 8; b++) if (usedMask & (1u << b)) n++;
  return n;
}

inline bool shieldCovers(const PowerState &ps, int team, int x, int y) {
  if (!ps.shieldOn[team]) return false;
  int dx = x - (int)ps.shieldX[team], dy = y - (int)ps.shieldY[team];
  return dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
}

inline void clearSmoke(PowerState &ps, int team) {
  ps.smokeOn[team] = 0;
  memset(ps.smokeMask[team], 0, sizeof(ps.smokeMask[team]));
}

// What the DASHBOARD may show for one cell / one team's HP. While a team
// is under smoke, cells shot at it stay hidden; once the smoke expires
// the mask is cleared and the truth appears.
inline uint8_t displayCell(const PowerState &ps, int team, int r, int c, uint8_t realValue) {
  if (ps.smokeMask[team][r][c]) return (realValue == CELL_HIT) ? CELL_SHIP : CELL_WATER;
  return realValue;
}

inline int displayRemaining(const PowerState &ps, int team, int remaining, const uint8_t grid[GRID_SIZE][GRID_SIZE]) {
  int shown = remaining;
  for (int r = 0; r < GRID_SIZE; r++)
    for (int c = 0; c < GRID_SIZE; c++)
      if (ps.smokeMask[team][r][c] && grid[r][c] == CELL_HIT) shown++;
  return shown;
}

// Power-up aware turn advance. Same as advanceTurn(), plus:
//  - when the turn reaches a team, that team's SHIELD and SMOKE expire
//  - a team that stepped on a mine (skipNext) is passed over once
//  - when the game ends, all shields/smoke drop so the final board is true
inline void advanceTurnPS(uint8_t &roundState, uint8_t &currentTurnIndex,
                          bool eliminated[MAX_TEAMS], bool registered[MAX_TEAMS], PowerState &ps) {
  int aliveCount = 0;
  for (int i = 0; i < MAX_TEAMS; i++) {
    if (registered[i] && !eliminated[i]) aliveCount++;
  }

  if (aliveCount <= 1) {
    roundState = STATE_GAMEOVER;
    for (int i = 0; i < MAX_TEAMS; i++) { clearSmoke(ps, i); ps.shieldOn[i] = 0; }
    return;
  }

  int idx = currentTurnIndex;
  for (int tries = 0; tries < MAX_TEAMS * 2; tries++) {   // x2: a skipped team may need a second lap
    idx = (idx + 1) % MAX_TEAMS;
    if (!registered[idx] || eliminated[idx]) continue;

    ps.shieldOn[idx] = 0;       // their protection ends the moment their turn comes round
    clearSmoke(ps, idx);

    if (ps.skipNext[idx]) { ps.skipNext[idx] = 0; continue; }

    currentTurnIndex = idx;
    return;
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
// FIRING ONE SHOT (shared by normal attacks, Salvo and Double Attack)
// =====================================================================
// The caller has ALREADY checked: coordinates are on the grid and the
// cell was not shot before. Returns the TRUE result (MISS / HIT / SUNK /
// BLOCKED / MINE) and puts what the attacker is allowed to be told in
// `reported` (differs only when the target is under SMOKE).
//
// ps may be nullptr (no power-ups) - then it's the classic behaviour.
inline uint8_t fireAtCell(
  int attackerIdx, int targetIdx, int x, int y,
  uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE],
  int remainingShips[MAX_TEAMS],
  bool eliminated[MAX_TEAMS],
  PowerState *ps,
  uint8_t &reported
) {
  uint8_t actual;
  uint8_t cell = grid[targetIdx][y][x];

  if (ps && shieldCovers(*ps, targetIdx, x, y)) {
    actual = RESULT_BLOCKED;                       // cell is NOT marked as shot
  } else if (cell == CELL_WATER) {
    grid[targetIdx][y][x] = CELL_MISS;
    actual = RESULT_MISS;
    if (ps && ps->mineX[targetIdx] == x && ps->mineY[targetIdx] == y) {
      actual = RESULT_MINE;                        // BOOM: mine is used up, attacker loses a turn
      ps->mineX[targetIdx] = NO_CELL;
      ps->mineY[targetIdx] = NO_CELL;
      ps->skipNext[attackerIdx] = 1;
    }
  } else {  // CELL_SHIP
    grid[targetIdx][y][x] = CELL_HIT;
    remainingShips[targetIdx]--;
    if (remainingShips[targetIdx] <= 0) {
      eliminated[targetIdx] = true;
      actual = RESULT_SUNK;
    } else {
      actual = RESULT_HIT;
    }
  }

  reported = actual;

  if (ps && ps->smokeOn[targetIdx]) {
    if (actual == RESULT_SUNK) {
      clearSmoke(*ps, targetIdx);                  // an elimination can't be hidden - reveal everything
    } else {
      if (actual != RESULT_BLOCKED) ps->smokeMask[targetIdx][y][x] = 1;
      reported = RESULT_UNKNOWN;
    }
  }
  return actual;
}

// =====================================================================
// ATTACKS
// =====================================================================

// Runs every attack-validation rule from the spec (all 11 checks) and,
// on a valid attack, updates the grid/remaining-ships/eliminated/turn
// state. Returns RESULT_MISS / HIT / SUNK / MINE / BLOCKED on success, or an
// ATTACK_FAIL_* code. Same division of responsibility as tryRegister().
//
// Optional (v3): pass &powerState to enable shields, smoke and mines, and
// &reported to learn what the attacker may be told (smoke hides it).
// A BLOCKED shot is a VALID action: it uses the attacker's turn.
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
  bool registered[MAX_TEAMS],
  PowerState *ps = nullptr,
  uint8_t *reportedOut = nullptr
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

  uint8_t reported = RESULT_MISS;
  uint8_t actual = fireAtCell(attackerIdx, targetIdx, x, y, grid, remainingShips, eliminated, ps, reported);
  if (reportedOut) *reportedOut = reported;

  if (ps) advanceTurnPS(roundState, currentTurnIndex, eliminated, registered, *ps);
  else    advanceTurn(roundState, currentTurnIndex, eliminated, registered);
  return actual;
}

// =====================================================================
// POWER-UPS
// =====================================================================

// What happened, for central_node.ino to log and to send back.
typedef struct {
  uint8_t power;
  uint8_t status;       // PWR_STATUS_* (goes on the wire)
  uint8_t count;        // SONAR: ship cells found in the 3x3 area
  uint8_t cells;        // SALVO = 3, DOUBLE = 2 (cells actually requested)
  uint8_t actual[3];    // TRUE result per fired cell (organizer log only)
  uint8_t reported[3];  // result per fired cell the attacker may see (RESULT_INVALID = not fired)
} PowerOutcome;

// n cells forming one straight, consecutive run in a single row or column.
inline bool cellsInLine(const int xs[], const int ys[], int n) {
  bool row = true, col = true;
  for (int i = 1; i < n; i++) {
    if (ys[i] != ys[0]) row = false;
    if (xs[i] != xs[0]) col = false;
  }
  if (row == col) return false;            // scattered, or all the same cell
  const int *v = row ? xs : ys;
  int mn = v[0], mx = v[0];
  for (int i = 1; i < n; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; }
  if (mx - mn != n - 1) return false;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (v[i] == v[j]) return false;
  return true;
}

// Validates and applies one power-up request. The caller (central_node.ino)
// resolves macTeam from the sender's MAC first, and does all I/O afterwards.
//
//   xs[]/ys[] meaning per power:
//     SONAR   target + (xs[0],ys[0]) = CENTRE of the 3x3 area (clipped at the edge)
//     SALVO   target + 3 cells that form a straight consecutive line
//     DOUBLE  target + 2 different cells (anywhere)
//     MINE    (xs[0],ys[0]) = one of YOUR OWN water cells; only before the game starts
//     REPAIR  (xs[0],ys[0]) = one of YOUR OWN hit cells
//     SHIELD  (xs[0],ys[0]) = CENTRE of the 3x3 block; must be 1..5 so all 9 cells exist
//     SMOKE   nothing
//
// Every power except MINE needs YOUR turn and USES it. A rejected request
// changes nothing and does not use the turn. Each power works once per game.
// Returns PWR_OK / PWR_JAMMED on success, otherwise a PWR_FAIL_* code.
inline uint8_t tryPowerUp(
  int macTeam, int claimedId, int power, int targetId,
  const int xs[3], const int ys[3],
  uint8_t &roundState, uint8_t &currentTurnIndex,
  uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE],
  int remainingShips[MAX_TEAMS],
  bool eliminated[MAX_TEAMS],
  bool registered[MAX_TEAMS],
  PowerState &ps,
  PowerOutcome &out
) {
  memset(&out, 0, sizeof(out));
  out.power = (uint8_t)power;
  out.status = PWR_STATUS_REJECTED;
  for (int i = 0; i < 3; i++) { out.actual[i] = RESULT_INVALID; out.reported[i] = RESULT_INVALID; }

  if (macTeam < 1 || macTeam > MAX_TEAMS) return PWR_FAIL_ID_MISMATCH;
  if (claimedId != macTeam) return PWR_FAIL_ID_MISMATCH;
  if (power < 1 || power > POWER_COUNT) return PWR_FAIL_BAD_POWER;

  const int me = macTeam - 1;
  if (!registered[me]) return PWR_FAIL_NOT_REGISTERED;
  const uint8_t bit = (uint8_t)(1u << (power - 1));

  // ---- MINE: placed BEFORE the game, costs no turn ----
  if (power == POWER_MINE) {
    if (roundState != STATE_SETUP && roundState != STATE_READY) return PWR_FAIL_NOT_SETUP;
    if (!inGrid(xs[0], ys[0])) return PWR_FAIL_BAD_COORDS;
    if (grid[me][ys[0]][xs[0]] != CELL_WATER) return PWR_FAIL_MINE_ON_SHIP;
    if (!(ps.used[me] & bit) && powerCount(ps.used[me]) >= POWERUP_BUDGET) return PWR_FAIL_BUDGET;
    ps.mineX[me] = (uint8_t)xs[0];            // placing again simply moves it
    ps.mineY[me] = (uint8_t)ys[0];
    ps.used[me] |= bit;
    out.status = PWR_STATUS_OK;
    return PWR_OK;
  }

  // ---- everything else: your turn, once per game ----
  if (roundState != STATE_RUNNING) return PWR_FAIL_NOT_RUNNING;
  if (me != currentTurnIndex) return PWR_FAIL_WRONG_TURN;
  if (eliminated[me]) return PWR_FAIL_DEAD;
  if (ps.used[me] & bit) return PWR_FAIL_ALREADY_USED;
  if (powerCount(ps.used[me]) >= POWERUP_BUDGET) return PWR_FAIL_BUDGET;

  int tIdx = -1;
  if (power == POWER_SONAR || power == POWER_SALVO || power == POWER_DOUBLE) {
    if (targetId < 1 || targetId > MAX_TEAMS) return PWR_FAIL_BAD_TARGET;
    tIdx = targetId - 1;
    if (tIdx == me || !registered[tIdx] || eliminated[tIdx]) return PWR_FAIL_BAD_TARGET;
  }

  uint8_t code = PWR_OK;

  switch (power) {
    case POWER_SONAR: {
      if (!inGrid(xs[0], ys[0])) return PWR_FAIL_BAD_COORDS;
      if (ps.smokeOn[tIdx]) {                 // smoke jams sonar; the power and the turn are spent
        out.status = PWR_STATUS_JAMMED;
        code = PWR_JAMMED;
        break;
      }
      int found = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int cx = xs[0] + dx, cy = ys[0] + dy;
          if (!inGrid(cx, cy)) continue;
          uint8_t v = grid[tIdx][cy][cx];
          if (v == CELL_SHIP || v == CELL_HIT) found++;   // a ship cell stays a ship cell once hit
        }
      out.count = (uint8_t)found;
      out.status = PWR_STATUS_OK;
      break;
    }

    case POWER_SALVO:
    case POWER_DOUBLE: {
      const int n = (power == POWER_SALVO) ? 3 : 2;
      for (int i = 0; i < n; i++)
        if (!inGrid(xs[i], ys[i])) return PWR_FAIL_BAD_COORDS;
      if (power == POWER_SALVO) {
        if (!cellsInLine(xs, ys, 3)) return PWR_FAIL_BAD_SHAPE;
      } else {
        if (xs[0] == xs[1] && ys[0] == ys[1]) return PWR_FAIL_BAD_SHAPE;
      }
      for (int i = 0; i < n; i++) {
        uint8_t v = grid[tIdx][ys[i]][xs[i]];
        if (v == CELL_MISS || v == CELL_HIT) return PWR_FAIL_ALREADY_HIT;   // whole action rejected
      }

      out.cells = (uint8_t)n;
      for (int i = 0; i < n; i++) {
        if (eliminated[tIdx]) break;          // an earlier cell sank them - the rest isn't fired
        out.actual[i] = fireAtCell(me, tIdx, xs[i], ys[i], grid, remainingShips, eliminated, &ps, out.reported[i]);
      }
      out.status = PWR_STATUS_OK;
      break;
    }

    case POWER_REPAIR: {
      if (!inGrid(xs[0], ys[0])) return PWR_FAIL_BAD_COORDS;
      if (grid[me][ys[0]][xs[0]] != CELL_HIT) return PWR_FAIL_NOTHING_TO_REPAIR;
      grid[me][ys[0]][xs[0]] = CELL_SHIP;
      remainingShips[me]++;
      ps.smokeMask[me][ys[0]][xs[0]] = 0;
      out.status = PWR_STATUS_OK;
      break;
    }

    case POWER_SHIELD: {
      if (xs[0] < 1 || xs[0] > GRID_SIZE - 2 || ys[0] < 1 || ys[0] > GRID_SIZE - 2) return PWR_FAIL_BAD_COORDS;
      ps.shieldOn[me] = 1;
      ps.shieldX[me] = (uint8_t)xs[0];
      ps.shieldY[me] = (uint8_t)ys[0];
      out.status = PWR_STATUS_OK;
      break;
    }

    case POWER_SMOKE: {
      clearSmoke(ps, me);
      ps.smokeOn[me] = 1;
      out.status = PWR_STATUS_OK;
      break;
    }

    default:
      return PWR_FAIL_BAD_POWER;
  }

  ps.used[me] |= bit;
  advanceTurnPS(roundState, currentTurnIndex, eliminated, registered, ps);
  return code;
}

#endif  // GAME_LOGIC_H