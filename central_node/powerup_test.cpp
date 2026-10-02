/* POWERUP_TEST.CPP - desktop tests for the loadout + power-up rules in game_logic.h (v4)
   Build:  g++ -std=c++11 -Wall -Wextra powerup_test.cpp -o powerup_test && ./powerup_test
   (Keep next to game_logic.h. central_logic_test.cpp is unchanged and still valid:
    tryRegister()'s new loadout parameters are optional.)

   Rules under test
     - A team picks 2 attack powers (Sonar/Salvo/Mine/Double) + 1 defence power
       (Smoke/Repair) before registering. Shield and Single Strike are compulsory.
     - Each chosen power: 2 uses per game. Shield: unlimited, never twice in a row.
     - Mines are placed DURING the game, on your own turn (it uses the turn),
       one active mine at a time, 2 placements per game. */

#include "game_logic.h"
#include <cstdio>

int testsRun = 0, testsFailed = 0;
void check(bool ok, const char *what) {
  testsRun++;
  if (!ok) testsFailed++;
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}
void section(const char *t) { printf("\n=== %s ===\n", t); }

struct TS {
  uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE];
  ShipPlacement storedShips[MAX_TEAMS][3];
  char teamNames[MAX_TEAMS][20];
  bool registered[MAX_TEAMS], eliminated[MAX_TEAMS];
  int remainingShips[MAX_TEAMS];
  uint8_t currentTurnIndex, roundState;
  PowerState ps;
};

// Loadout helper: two attack powers + one defence power (+ the shield bit).
static uint8_t LO(int a1, int a2, int d) {
  return (uint8_t)((1u << (a1 - 1)) | (1u << (a2 - 1)) | (1u << (d - 1)) | LOADOUT_SHIELD_BIT);
}

// Default loadouts: every power is somebody's, so the tests can pick the team they need.
//   team 1: Sonar + Salvo + Smoke      team 2: Mine + Double + Repair
//   team 3: Sonar + Mine + Repair      team 4: Salvo + Double + Smoke
static const uint8_t DEFAULT_LO[4] = {
  (uint8_t)((1u << (POWER_SONAR - 1)) | (1u << (POWER_SALVO - 1)) | (1u << (POWER_SMOKE - 1)) | LOADOUT_SHIELD_BIT),
  (uint8_t)((1u << (POWER_MINE - 1)) | (1u << (POWER_DOUBLE - 1)) | (1u << (POWER_REPAIR - 1)) | LOADOUT_SHIELD_BIT),
  (uint8_t)((1u << (POWER_SONAR - 1)) | (1u << (POWER_MINE - 1)) | (1u << (POWER_REPAIR - 1)) | LOADOUT_SHIELD_BIT),
  (uint8_t)((1u << (POWER_SALVO - 1)) | (1u << (POWER_DOUBLE - 1)) | (1u << (POWER_SMOKE - 1)) | LOADOUT_SHIELD_BIT) };

// Default layout: size-1 (2,4); size-3 (0,0)-(2,0); size-5 x=4, y 0..4
static void validShips(ShipPlacement o[3]) {
  o[0] = {1, 2, 4, ORIENT_HORIZONTAL};
  o[1] = {3, 0, 0, ORIENT_HORIZONTAL};
  o[2] = {5, 4, 0, ORIENT_VERTICAL};
}

// Everyone registers with its own loadout; lo[t-1] belongs to team t.
static void registerAll(TS &s, const uint8_t lo[4]) {
  memset(&s, 0, sizeof(s));
  resetPowerState(s.ps);
  ShipPlacement sh[3]; validShips(sh);
  for (int t = 1; t <= 4; t++)
    tryRegister(t, t, "TEAM X", sh, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid,
                s.remainingShips, s.eliminated, lo[t - 1], &s.ps);
}

static void running(TS &s) {
  registerAll(s, DEFAULT_LO);
  s.roundState = STATE_RUNNING; s.currentTurnIndex = 0;
}

static uint8_t power(TS &s, int team, int pw, int target, int x1, int y1, int x2 = 0, int y2 = 0, int x3 = 0, int y3 = 0, PowerOutcome *outp = nullptr) {
  int xs[3] = {x1, x2, x3}, ys[3] = {y1, y2, y3};
  PowerOutcome local; PowerOutcome &o = outp ? *outp : local;
  return tryPowerUp(team, team, pw, target, xs, ys, s.roundState, s.currentTurnIndex, s.grid,
                    s.remainingShips, s.eliminated, s.registered, s.ps, o);
}
static uint8_t shoot(TS &s, int a, int t, int x, int y, uint8_t *rep = nullptr) {
  return tryAttack(a, a, t, x, y, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips,
                   s.eliminated, s.registered, &s.ps, rep);
}
// Let a team spend its turn with a harmless shot. The cells come from a pool of
// 20 cells that are water in the default layout and that no test shoots on purpose,
// so a filler shot is never rejected as "already shot".
static int fillerCell = 0;
static void passTurn(TS &s, int team, int target) {
  static const int pool[20][2] = {
    {1,1},{1,2},{1,3},{1,4},{1,5},{1,6}, {3,1},{3,2},{3,3},{3,4},{3,5},{3,6},
    {5,2},{5,3},{5,4},{5,5}, {6,2},{6,3},{6,4},{6,5} };
  const int *c = pool[fillerCell++ % 20];
  s.currentTurnIndex = (uint8_t)(team - 1);
  shoot(s, team, target, c[0], c[1]);
}

// ---------------------------------------------------------------------
void testLoadoutValidation() {
  section("Loadout validation");
  check(validLoadout(LO(POWER_SONAR, POWER_SALVO, POWER_SMOKE)), "Sonar + Salvo + Smoke is legal");
  check(validLoadout(LO(POWER_MINE, POWER_DOUBLE, POWER_REPAIR)), "Mine + Double + Repair is legal");
  check(validLoadout((uint8_t)(LO(POWER_SONAR, POWER_MINE, POWER_SMOKE) & ~LOADOUT_SHIELD_BIT)), "the shield bit may be left clear");
  check(!validLoadout(0), "an empty loadout is illegal");
  check(!validLoadout(LOADOUT_SHIELD_BIT), "shield alone is illegal");
  check(!validLoadout(LO(POWER_SONAR, POWER_SONAR, POWER_SMOKE)), "only ONE attack power is illegal");
  check(!validLoadout((uint8_t)(LO(POWER_SONAR, POWER_SALVO, POWER_SMOKE) | (1u << (POWER_MINE - 1)))), "THREE attack powers is illegal");
  check(!validLoadout((uint8_t)((1u << (POWER_SONAR - 1)) | (1u << (POWER_SALVO - 1)))), "no defence power is illegal");
  check(!validLoadout((uint8_t)(LO(POWER_SONAR, POWER_SALVO, POWER_SMOKE) | (1u << (POWER_REPAIR - 1)))), "BOTH defence powers is illegal");
  check(!validLoadout((uint8_t)(LO(POWER_SONAR, POWER_SALVO, POWER_SMOKE) | 0x80)), "a stray bit (0x80) is illegal");
  check(!validLoadout((uint8_t)((1u << (POWER_REPAIR - 1)) | (1u << (POWER_SMOKE - 1)) | (1u << (POWER_SONAR - 1)))), "1 attack + 2 defence is illegal");
  check(LOADOUT_ATTACK_MASK == 0x47 && LOADOUT_DEFENCE_MASK == 0x18 && LOADOUT_SHIELD_BIT == 0x20, "masks are 0x47 / 0x18 / 0x20 (the PARTICIPANT_GUIDE values)");
  check(normalizeLoadout(0x51) == 0x71 && normalizeLoadout(0x71) == 0x71, "normalizing always sets the shield bit");
}

void testRegistration() {
  section("Registration with a loadout");
  TS s; memset(&s, 0, sizeof(s)); resetPowerState(s.ps);
  ShipPlacement sh[3]; validShips(sh);
  uint8_t lo = LO(POWER_SONAR, POWER_DOUBLE, POWER_SMOKE);
  uint8_t r = tryRegister(1, 1, "TEAM 1", sh, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated, lo, &s.ps);
  check(r == REG_OK && s.registered[0], "a legal loadout registers");
  check(s.ps.loadout[0] == lo, "the loadout is stored");
  check(powerUsesLeft(s.ps, 0, POWER_SONAR) == POWER_USES && powerUsesLeft(s.ps, 0, POWER_DOUBLE) == POWER_USES && powerUsesLeft(s.ps, 0, POWER_SMOKE) == POWER_USES, "each chosen power starts with 2 uses");
  check(powerUsesLeft(s.ps, 0, POWER_SALVO) == 0 && powerUsesLeft(s.ps, 0, POWER_MINE) == 0 && powerUsesLeft(s.ps, 0, POWER_REPAIR) == 0, "powers that were not chosen have 0 uses");
  check(powerUsesLeft(s.ps, 0, POWER_SHIELD) == 255, "shield is unlimited");

  r = tryRegister(2, 2, "TEAM 2", sh, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated, 0x03, &s.ps);
  check(r == REG_FAIL_BAD_LOADOUT && !s.registered[1], "an illegal loadout is rejected and nothing is stored");
  check(s.ps.loadout[1] == 0, "...including the loadout itself");
  r = tryRegister(2, 3, "TEAM 2", sh, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated, lo, &s.ps);
  check(r == REG_FAIL_MAC_MISMATCH, "a wrong team id is still reported as a MAC mismatch");

  // re-registration in SETUP replaces the loadout and refreshes the uses
  s.ps.uses[0][POWER_SONAR - 1] = 2;
  uint8_t lo2 = LO(POWER_MINE, POWER_SALVO, POWER_REPAIR);
  r = tryRegister(1, 1, "TEAM 1", sh, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated, lo2, &s.ps);
  check(r == REG_OK && s.ps.loadout[0] == lo2 && s.ps.uses[0][POWER_SONAR - 1] == 0, "re-registering before the game swaps the loadout and clears old use counts");

  // reconnection during the game
  TS g; running(g);
  uint8_t keep = g.ps.loadout[0];
  g.ps.uses[0][POWER_SONAR - 1] = 1;
  r = tryRegister(1, 1, "TEAM X", sh, g.roundState, g.registered, g.teamNames, g.storedShips, g.grid, g.remainingShips, g.eliminated, keep, &g.ps);
  check(r == REG_RECONNECTED && g.ps.uses[0][POWER_SONAR - 1] == 1, "same layout + same loadout reconnects and keeps the use counts");
  r = tryRegister(1, 1, "TEAM X", sh, g.roundState, g.registered, g.teamNames, g.storedShips, g.grid, g.remainingShips, g.eliminated, (uint8_t)(keep & ~LOADOUT_SHIELD_BIT), &g.ps);
  check(r == REG_RECONNECTED, "...even if the compulsory shield bit is sent clear");
  r = tryRegister(1, 1, "TEAM X", sh, g.roundState, g.registered, g.teamNames, g.storedShips, g.grid, g.remainingShips, g.eliminated, LO(POWER_MINE, POWER_DOUBLE, POWER_REPAIR), &g.ps);
  check(r == REG_RECONNECT_REJECTED && g.ps.loadout[0] == keep, "a reconnect with a DIFFERENT loadout is rejected, the stored one is untouched");
  r = tryRegister(1, 1, "TEAM X", sh, g.roundState, g.registered, g.teamNames, g.storedShips, g.grid, g.remainingShips, g.eliminated, 0x01, &g.ps);
  check(r == REG_FAIL_BAD_LOADOUT, "a reconnect with an illegal loadout is rejected as such");

  // the classic call (no loadout arguments) is unchanged
  TS c; memset(&c, 0, sizeof(c));
  r = tryRegister(1, 1, "TEAM 1", sh, c.roundState, c.registered, c.teamNames, c.storedShips, c.grid, c.remainingShips, c.eliminated);
  check(r == REG_OK, "tryRegister() without loadout arguments still works");

  // The exact bytes of the registration example in PARTICIPANT_GUIDE.md
  const uint8_t ex[34] = {
    0x01, 0x54,0x45,0x41,0x4D,0x20,0x31, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0x01,0x04,0x00,0x00,  0x03,0x00,0x04,0x00,  0x05,0x00,0x00,0x02,
    0x71 };
  char name[20]; memcpy(name, ex + 1, 20);
  ShipPlacement exShips[3]; memcpy(exShips, ex + 21, sizeof(exShips));
  TS e; memset(&e, 0, sizeof(e)); resetPowerState(e.ps);
  r = tryRegister(ex[0], ex[0], name, exShips, e.roundState, e.registered, e.teamNames, e.storedShips, e.grid, e.remainingShips, e.eliminated, ex[33], &e.ps);
  check(r == REG_OK && e.ps.loadout[0] == 0x71, "the guide's 34-byte example registers (loadout 0x71 = Sonar + Double + Smoke)");
}

void testUsesAndLoadout() {
  section("Uses and loadout limits");
  TS s; running(s); PowerOutcome o;
  check(power(s, 1, POWER_MINE, 0, 6, 6) == PWR_FAIL_NOT_IN_LOADOUT, "a power that wasn't chosen is rejected");
  check(s.currentTurnIndex == 0, "...and does not use the turn");
  check(power(s, 1, POWER_SONAR, 2, 1, 0, 0, 0, 0, 0, &o) == PWR_OK, "use 1 of Sonar");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 1);
  check(s.currentTurnIndex == 0, "back to team 1");
  check(power(s, 1, POWER_SONAR, 2, 4, 2) == PWR_OK, "use 2 of Sonar");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 1);
  check(power(s, 1, POWER_SONAR, 2, 4, 2) == PWR_FAIL_NO_USES_LEFT, "use 3 of Sonar is rejected");
  check(s.currentTurnIndex == 0 && powerUsesLeft(s.ps, 0, POWER_SONAR) == 0, "...it does not use the turn, 0 uses left");
  check(powerUsedMask(s.ps, 0) == (1u << (POWER_SONAR - 1)), "dashboard 'used' mask shows Sonar only");
  check(shoot(s, 1, 2, 6, 0) == RESULT_MISS, "Single Strike is always available");
}

void testSonar() {
  section("Sonar Ping");
  TS s; running(s); PowerOutcome o;
  check(power(s, 1, POWER_SONAR, 2, 1, 0, 0, 0, 0, 0, &o) == PWR_OK && o.count == 3, "area around (1,0) holds the 3 cells of the size-3 ship");
  check(s.currentTurnIndex == 1, "sonar uses the turn");
  check(s.grid[1][0][0] == CELL_SHIP, "sonar does not change the grid");

  TS t; running(t);
  check(power(t, 1, POWER_SONAR, 2, 6, 6, 0, 0, 0, 0, &o) == PWR_OK && o.count == 0, "corner centre is clipped to the grid, finds nothing");
  TS u; running(u);
  power(u, 1, POWER_SONAR, 2, 3, 2, 0, 0, 0, 0, &o);
  check(o.count == 3, "(3,2) area covers x=4,y=1..3 of the size-5 ship");
  TS v; running(v);
  v.ps.smokeOn[1] = 1;
  check(power(v, 1, POWER_SONAR, 2, 1, 0, 0, 0, 0, 0, &o) == PWR_JAMMED && o.status == PWR_STATUS_JAMMED && o.count == 0, "smoke jams sonar");
  check(v.currentTurnIndex == 1 && v.ps.uses[0][POWER_SONAR - 1] == 1, "a jammed sonar still spends a use and the turn");
  check(power(v, 2, POWER_SONAR, 2, 1, 0) == PWR_FAIL_NOT_IN_LOADOUT, "(team 2 never chose sonar)");
  TS w; running(w); w.currentTurnIndex = 2;
  check(power(w, 3, POWER_SONAR, 3, 1, 0) == PWR_FAIL_BAD_TARGET, "sonar on yourself is rejected");
}

void testSalvoDouble() {
  section("Salvo and Double Attack");
  TS s; running(s); PowerOutcome o;
  check(power(s, 1, POWER_SALVO, 2, 4, 0, 4, 1, 4, 2, &o) == PWR_OK, "salvo down column x=4 is accepted");
  check(o.actual[0] == RESULT_HIT && o.actual[1] == RESULT_HIT && o.actual[2] == RESULT_HIT && s.remainingShips[1] == 6, "3 hits, remaining drops 9 -> 6");
  check(s.currentTurnIndex == 1, "salvo uses one turn");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 1);
  check(power(s, 1, POWER_SALVO, 3, 0, 0, 1, 0, 2, 0) == PWR_OK, "second salvo is allowed");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 1);
  check(power(s, 1, POWER_SALVO, 3, 0, 5, 1, 5, 2, 5) == PWR_FAIL_NO_USES_LEFT, "a third salvo is rejected");

  TS t; running(t);
  check(power(t, 1, POWER_SALVO, 2, 0, 0, 1, 1, 2, 2) == PWR_FAIL_BAD_SHAPE, "diagonal salvo is rejected");
  check(power(t, 1, POWER_SALVO, 2, 0, 0, 1, 0, 3, 0) == PWR_FAIL_BAD_SHAPE, "gap in the line is rejected");
  check(power(t, 1, POWER_SALVO, 2, 5, 0, 6, 0, 7, 0) == PWR_FAIL_BAD_COORDS, "salvo off the grid is rejected");
  check(power(t, 1, POWER_SALVO, 2, 1, 1, 1, 1, 1, 1) == PWR_FAIL_BAD_SHAPE, "three identical cells are rejected");
  shoot(t, 1, 2, 3, 3); t.currentTurnIndex = 0;
  check(power(t, 1, POWER_SALVO, 2, 3, 2, 3, 3, 3, 4) == PWR_FAIL_ALREADY_HIT, "salvo over an already-shot cell rejects the WHOLE action");
  check(t.currentTurnIndex == 0 && t.ps.uses[0][POWER_SALVO - 1] == 0, "...and spends neither turn nor a use");

  TS d; running(d); d.currentTurnIndex = 1;
  check(power(d, 2, POWER_DOUBLE, 1, 0, 0, 6, 6, 0, 0, &o) == PWR_OK && o.actual[0] == RESULT_HIT && o.actual[1] == RESULT_MISS, "double attack: one hit, one miss, cells far apart");
  passTurn(d, 3, 4); passTurn(d, 4, 1); passTurn(d, 1, 3);
  check(power(d, 2, POWER_DOUBLE, 3, 2, 2, 5, 5) == PWR_OK, "second double attack is allowed");
  passTurn(d, 3, 4); passTurn(d, 4, 1); passTurn(d, 1, 3);
  check(power(d, 2, POWER_DOUBLE, 3, 0, 1, 6, 1) == PWR_FAIL_NO_USES_LEFT, "a third double attack is rejected");
  TS e; running(e); e.currentTurnIndex = 1;
  check(power(e, 2, POWER_DOUBLE, 1, 2, 2, 2, 2) == PWR_FAIL_BAD_SHAPE, "double attack on one cell twice is rejected");

  TS f; running(f); f.currentTurnIndex = 1;
  f.remainingShips[0] = 1;   // team 1 has one cell left
  power(f, 2, POWER_DOUBLE, 1, 2, 4, 6, 6, 0, 0, &o);
  check(o.actual[0] == RESULT_SUNK && o.reported[1] == RESULT_INVALID && f.grid[0][6][6] == CELL_WATER, "elimination on the first cell: the second is not fired");
}

void testMine() {
  section("Mine (placed during the game)");
  TS s; running(s); s.currentTurnIndex = 1;               // team 2 has the Mine
  check(power(s, 2, POWER_MINE, 0, 6, 6) == PWR_OK && s.ps.mineX[1] == 6 && s.ps.mineY[1] == 6, "a mine on water is accepted on your turn");
  check(s.currentTurnIndex == 2, "placing a mine USES the turn");
  check(s.ps.uses[1][POWER_MINE - 1] == 1, "placement 1 of 2 recorded");

  TS t; registerAll(t, DEFAULT_LO);
  check(t.roundState == STATE_READY, "setup: all registered, game not started");
  check(power(t, 2, POWER_MINE, 0, 6, 6) == PWR_FAIL_NOT_RUNNING, "mines can NOT be placed before the game starts");
  t.roundState = STATE_RUNNING; t.currentTurnIndex = 0;
  check(power(t, 2, POWER_MINE, 0, 6, 6) == PWR_FAIL_WRONG_TURN, "...nor out of turn");

  TS u; running(u); u.currentTurnIndex = 1;
  check(power(u, 2, POWER_MINE, 0, 2, 4) == PWR_FAIL_MINE_ON_SHIP, "a mine on a ship cell is rejected");
  check(power(u, 2, POWER_MINE, 0, 9, 9) == PWR_FAIL_BAD_COORDS, "a mine off the grid is rejected");
  shoot(u, 2, 1, 6, 6); u.currentTurnIndex = 1;           // team 1's grid is untouched; use team 2's own grid instead:
  u.grid[1][5][5] = CELL_MISS;                            // pretend someone already missed (5,5) on team 2
  check(power(u, 2, POWER_MINE, 0, 5, 5) == PWR_FAIL_ALREADY_HIT, "a mine on an already-shot cell is rejected");
  u.grid[1][1][4] = CELL_HIT;
  check(power(u, 2, POWER_MINE, 0, 4, 1) == PWR_FAIL_MINE_ON_SHIP, "a mine on a damaged ship cell is rejected");
  check(u.currentTurnIndex == 1 && u.ps.uses[1][POWER_MINE - 1] == 0, "rejected placements cost neither turn nor use");

  // one at a time
  TS v; running(v); v.currentTurnIndex = 1;
  power(v, 2, POWER_MINE, 0, 6, 6);
  passTurn(v, 3, 4); passTurn(v, 4, 1); passTurn(v, 1, 3);
  check(v.currentTurnIndex == 1, "team 2 to move again");
  check(power(v, 2, POWER_MINE, 0, 5, 6) == PWR_FAIL_MINE_ACTIVE, "a second mine while one is active is rejected");
  check(v.currentTurnIndex == 1 && v.ps.uses[1][POWER_MINE - 1] == 1 && v.ps.mineX[1] == 6, "...nothing changed, the first mine stays where it was");
  check(v.ps.mineX[1] == 6 && v.ps.mineY[1] == 6, "the mine survives whole rounds of turns");

  // trigger it
  uint8_t rep = 99;
  v.currentTurnIndex = 0;
  check(shoot(v, 1, 2, 6, 6, &rep) == RESULT_MINE && rep == RESULT_MINE, "shooting the mine cell reports MINE");
  check(v.grid[1][6][6] == CELL_MISS, "the mine cell is marked shot");
  check(v.ps.mineX[1] == NO_CELL && v.ps.skipNext[0] == 1, "the mine is used up and the attacker is flagged to skip");
  check(v.currentTurnIndex == 1, "turn moves on to team 2");
  passTurn(v, 2, 3); passTurn(v, 3, 4); passTurn(v, 4, 1);
  check(v.currentTurnIndex == 1, "team 1 is skipped once: turn goes 4 -> 2");
  check(v.ps.skipNext[0] == 0, "the skip is consumed");

  // second placement, then no third
  check(power(v, 2, POWER_MINE, 0, 5, 6) == PWR_OK && v.ps.uses[1][POWER_MINE - 1] == 2, "after the first mine is gone, placement 2 of 2 works");
  passTurn(v, 3, 4); passTurn(v, 4, 1); passTurn(v, 1, 3);
  v.currentTurnIndex = 2; v.ps.mineX[1] = NO_CELL; v.ps.mineY[1] = NO_CELL;   // pretend it was triggered
  v.currentTurnIndex = 1;
  check(power(v, 2, POWER_MINE, 0, 5, 3) == PWR_FAIL_NO_USES_LEFT, "a third placement is rejected (2 per game)");

  TS w; running(w);
  check(shoot(w, 1, 3, 6, 6) == RESULT_MISS, "no-mine control: a cell without a mine is a normal miss");
}

void testShield() {
  section("Shield (unlimited, never twice in a row)");
  TS s; running(s);
  check(power(s, 1, POWER_SHIELD, 0, 0, 0) == PWR_FAIL_BAD_COORDS, "shield centred on a corner (would be <9 cells) is rejected");
  check(power(s, 1, POWER_SHIELD, 0, 5, 3) == PWR_OK, "team 1 shields the block around (5,3): x 4..6, y 2..4");
  check(s.currentTurnIndex == 1, "shield uses the turn");
  uint8_t rep = 0;
  check(shoot(s, 2, 1, 4, 3, &rep) == RESULT_BLOCKED && rep == RESULT_BLOCKED, "shot inside the shield is BLOCKED");
  check(s.grid[0][3][4] == CELL_SHIP && s.remainingShips[0] == 9, "blocked shot changes nothing on the grid ((4,3) is still an untouched ship cell)");
  check(s.currentTurnIndex == 2, "a blocked shot still uses the attacker's turn");
  check(shoot(s, 3, 1, 0, 0) == RESULT_HIT, "shot outside the shield still hits");
  check(s.ps.shieldOn[0] == 1, "shield is still up before team 1's turn");
  passTurn(s, 4, 2);
  check(s.currentTurnIndex == 0 && s.ps.shieldOn[0] == 0, "shield expires when team 1's next turn begins");
  check(power(s, 1, POWER_SHIELD, 0, 5, 3) == PWR_FAIL_SHIELD_COOLDOWN, "shielding again on the very next turn is rejected");
  check(s.currentTurnIndex == 0 && s.ps.shieldOn[0] == 0, "...it does not use the turn and no shield appears");
  check(shoot(s, 1, 2, 6, 0) == RESULT_MISS, "an attack is still fine on that turn");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 2);
  check(power(s, 1, POWER_SHIELD, 0, 3, 3) == PWR_OK, "after a turn without a shield, shield is allowed again");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 2);
  check(shoot(s, 1, 2, 6, 1) == RESULT_MISS, "(attack in between)");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 2);
  check(power(s, 1, POWER_SHIELD, 0, 3, 3) == PWR_OK, "a third shield works: shield has no use limit");
  check(s.ps.uses[0][POWER_SHIELD - 1] == 3, "(three shields were used)");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 2);
  check(power(s, 1, POWER_SMOKE, 0, 0, 0) == PWR_OK, "another power-up between two shields also clears the cooldown");
  passTurn(s, 2, 3); passTurn(s, 3, 4); passTurn(s, 4, 2);
  check(power(s, 1, POWER_SHIELD, 0, 3, 3) == PWR_OK, "...so the next shield is allowed");
  TS q; running(q);
  q.ps.loadout[0] = (uint8_t)(LO(POWER_SONAR, POWER_SALVO, POWER_SMOKE) & ~LOADOUT_SHIELD_BIT);
  check(power(q, 1, POWER_SHIELD, 0, 3, 3) == PWR_OK, "shield works even if the stored loadout somehow lacks the shield bit (it is compulsory)");
}

void testSmoke() {
  section("Smoke Screen");
  TS s; running(s);
  check(power(s, 1, POWER_SMOKE, 0, 0, 0) == PWR_OK && s.ps.smokeOn[0], "team 1 deploys smoke");
  uint8_t rep = 0;
  check(shoot(s, 2, 1, 6, 0, &rep) == RESULT_MISS && rep == RESULT_UNKNOWN, "attacker is told UNKNOWN (truth is MISS: (6,0) is water)");
  check(shoot(s, 3, 1, 0, 0, &rep) == RESULT_HIT && rep == RESULT_UNKNOWN, "attacker is told UNKNOWN (truth is HIT)");
  check(s.grid[0][0][0] == CELL_HIT && s.remainingShips[0] == 8, "...but the hit is real");
  check(displayCell(s.ps, 0, 0, 0, s.grid[0][0][0]) == CELL_SHIP, "dashboard still shows an untouched ship cell");
  check(displayRemaining(s.ps, 0, s.remainingShips[0], s.grid[0]) == 9, "dashboard still shows 9/9");
  s.currentTurnIndex = 2;
  check(shoot(s, 3, 1, 0, 0) == ATTACK_FAIL_ALREADY_HIT, "the cell counts as shot");
  passTurn(s, 3, 2);
  passTurn(s, 4, 2);
  check(s.currentTurnIndex == 0 && s.ps.smokeOn[0] == 0, "smoke ends when team 1's turn begins");
  check(displayCell(s.ps, 0, 0, 0, s.grid[0][0][0]) == CELL_HIT && displayRemaining(s.ps, 0, s.remainingShips[0], s.grid[0]) == 8,
        "...and the dashboard now shows the truth");

  TS u; running(u);
  power(u, 1, POWER_SMOKE, 0, 0, 0);
  passTurn(u, 2, 3); passTurn(u, 3, 4); passTurn(u, 4, 2);
  check(u.currentTurnIndex == 0 && u.ps.smokeOn[0] == 0, "nobody attacked team 1, yet the smoke still ended at its next turn (even if unused)");
  shoot(u, 1, 2, 6, 0);
  passTurn(u, 2, 3); passTurn(u, 3, 4); passTurn(u, 4, 2);
  check(power(u, 1, POWER_SMOKE, 0, 0, 0) == PWR_OK, "second smoke screen is allowed");
  passTurn(u, 2, 3); passTurn(u, 3, 4); passTurn(u, 4, 2);
  shoot(u, 1, 2, 6, 1);
  passTurn(u, 2, 3); passTurn(u, 3, 4); passTurn(u, 4, 2);
  check(power(u, 1, POWER_SMOKE, 0, 0, 0) == PWR_FAIL_NO_USES_LEFT, "a third smoke screen is rejected");

  TS t; running(t);
  power(t, 1, POWER_SMOKE, 0, 0, 0);
  t.remainingShips[0] = 1;
  check(shoot(t, 2, 1, 0, 0, &rep) == RESULT_SUNK && rep == RESULT_SUNK, "an elimination is never hidden by smoke");
  check(t.ps.smokeOn[0] == 0, "...and it lifts the smoke");
}

void testRepair() {
  section("Repair");
  TS s; running(s); s.currentTurnIndex = 1;               // team 2 has Repair
  shoot(s, 2, 3, 4, 0); s.currentTurnIndex = 0;           // (keep team 2's own grid untouched; hit team 1 instead)
  shoot(s, 1, 2, 4, 0);                                   // hit team 2; now team 2's turn
  check(s.remainingShips[1] == 8 && s.currentTurnIndex == 1, "setup: team 2 was hit, it is their turn");
  check(power(s, 2, POWER_REPAIR, 0, 0, 5) == PWR_FAIL_NOTHING_TO_REPAIR, "repairing a cell that isn't a hit is rejected");
  check(s.currentTurnIndex == 1 && s.ps.uses[1][POWER_REPAIR - 1] == 0, "...without using the turn or a use");
  check(power(s, 2, POWER_REPAIR, 0, 4, 0) == PWR_OK, "repairing a hit cell works");
  check(s.grid[1][0][4] == CELL_SHIP && s.remainingShips[1] == 9, "cell is a ship again, HP back to 9");
  check(s.currentTurnIndex == 2, "repair uses the turn");
  s.currentTurnIndex = 0;
  check(shoot(s, 1, 2, 4, 0) == RESULT_HIT, "the repaired cell can be hit again");
  passTurn(s, 3, 4);
  s.currentTurnIndex = 1;
  check(power(s, 2, POWER_REPAIR, 0, 4, 0) == PWR_OK, "second repair works");
  s.currentTurnIndex = 0;
  shoot(s, 1, 2, 4, 1);
  s.currentTurnIndex = 1;
  check(power(s, 2, POWER_REPAIR, 0, 4, 1) == PWR_FAIL_NO_USES_LEFT, "a third repair is rejected");
  check(power(s, 1, POWER_REPAIR, 0, 4, 1) == PWR_FAIL_WRONG_TURN, "(out of turn is caught first)");
}

void testGuards() {
  section("Turn and permission guards");
  TS s; running(s);
  check(power(s, 2, POWER_SHIELD, 0, 3, 3) == PWR_FAIL_WRONG_TURN, "out-of-turn power-up is rejected");
  check(s.currentTurnIndex == 0, "...and does not use the turn");
  check(power(s, 1, 9, 0, 0, 0) == PWR_FAIL_BAD_POWER, "unknown power id is rejected");
  int xs[3] = {0, 0, 0}, ys[3] = {0, 0, 0}; PowerOutcome o;
  check(tryPowerUp(1, 2, POWER_SMOKE, 0, xs, ys, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips,
                   s.eliminated, s.registered, s.ps, o) == PWR_FAIL_ID_MISMATCH, "claiming another team's id is rejected");
  TS g; registerAll(g, DEFAULT_LO);
  check(power(g, 1, POWER_SMOKE, 0, 0, 0) == PWR_FAIL_NOT_RUNNING, "power-ups need a running game");
  check(power(g, 1, POWER_SHIELD, 0, 3, 3) == PWR_FAIL_NOT_RUNNING, "...the shield too");
  TS h; running(h); h.eliminated[0] = true;
  check(power(h, 1, POWER_SMOKE, 0, 0, 0) == PWR_FAIL_DEAD, "an eliminated team can't use power-ups");
  TS n; memset(&n, 0, sizeof(n)); resetPowerState(n.ps); n.roundState = STATE_RUNNING;
  check(power(n, 1, POWER_SMOKE, 0, 0, 0) == PWR_FAIL_NOT_REGISTERED, "an unregistered team can't use power-ups");

  TS c; running(c);
  uint8_t r = tryAttack(1, 1, 2, 4, 0, c.roundState, c.currentTurnIndex, c.grid, c.remainingShips, c.eliminated, c.registered);
  check(r == RESULT_HIT && c.currentTurnIndex == 1, "tryAttack() without power-up arguments is unchanged");
}

int main() {
  printf("MERAZ BATTLESHIP - LOADOUT + POWER-UP TESTS\n");
  testLoadoutValidation(); testRegistration(); testUsesAndLoadout();
  testSonar(); testSalvoDouble(); testMine(); testShield(); testSmoke(); testRepair(); testGuards();
  printf("\n==============================\n%d/%d checks passed\n", testsRun - testsFailed, testsRun);
  return testsFailed ? 1 : 0;
}
