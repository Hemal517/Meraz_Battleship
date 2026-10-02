/* POWERUP_TEST.CPP - desktop tests for the power-up rules in game_logic.h
   Build:  g++ -std=c++11 -Wall -Wextra powerup_test.cpp -o powerup_test && ./powerup_test
   (Keep next to game_logic.h. central_logic_test.cpp is unchanged and still valid.) */

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

// Default layout: size-1 (2,4); size-3 (0,0)-(2,0); size-5 x=4, y 0..4
static void validShips(ShipPlacement o[3]) {
  o[0] = {1, 2, 4, ORIENT_HORIZONTAL};
  o[1] = {3, 0, 0, ORIENT_HORIZONTAL};
  o[2] = {5, 4, 0, ORIENT_VERTICAL};
}

static void registerAll(TS &s) {
  memset(&s, 0, sizeof(s));
  resetPowerState(s.ps);
  ShipPlacement sh[3]; validShips(sh);
  for (int t = 1; t <= 4; t++)
    tryRegister(t, t, "TEAM X", sh, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
}
static void running(TS &s) { registerAll(s); s.roundState = STATE_RUNNING; s.currentTurnIndex = 0; }

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

void testSonar() {
  section("Sonar Ping");
  TS s; running(s); PowerOutcome o;
  check(power(s, 1, POWER_SONAR, 2, 1, 0, 0, 0, 0, 0, &o) == PWR_OK && o.count == 3, "area around (1,0) holds the 3 cells of the size-3 ship");
  check(s.currentTurnIndex == 1, "sonar uses the turn");
  check(s.grid[1][0][0] == CELL_SHIP, "sonar does not change the grid");
  s.currentTurnIndex = 0;
  check(power(s, 1, POWER_SONAR, 3, 3, 2) == PWR_FAIL_ALREADY_USED, "a second sonar is rejected");
  check(s.currentTurnIndex == 0, "a rejected power-up does not use the turn");

  TS t; running(t);
  check(power(t, 1, POWER_SONAR, 2, 6, 6, 0, 0, 0, 0, &o) == PWR_OK && o.count == 0, "corner centre is clipped to the grid, finds nothing");
  TS u; running(u);
  power(u, 1, POWER_SONAR, 2, 3, 2, 0, 0, 0, 0, &o);
  check(o.count == 3, "(3,2) area covers x=4,y=1..3 of the size-5 ship");
  TS v; running(v);
  v.ps.smokeOn[1] = 1;
  check(power(v, 1, POWER_SONAR, 2, 1, 0, 0, 0, 0, 0, &o) == PWR_JAMMED && o.status == PWR_STATUS_JAMMED && o.count == 0, "smoke jams sonar");
  check(v.currentTurnIndex == 1 && (v.ps.used[0] & (1 << (POWER_SONAR - 1))), "a jammed sonar still spends the power and the turn");
  check(power(v, 2, POWER_SONAR, 2, 1, 0) == PWR_FAIL_BAD_TARGET, "sonar on yourself is rejected");
}

void testSalvoDouble() {
  section("Salvo and Double Attack");
  TS s; running(s); PowerOutcome o;
  check(power(s, 1, POWER_SALVO, 2, 4, 0, 4, 1, 4, 2, &o) == PWR_OK, "salvo down column x=4 is accepted");
  check(o.actual[0] == RESULT_HIT && o.actual[1] == RESULT_HIT && o.actual[2] == RESULT_HIT && s.remainingShips[1] == 6, "3 hits, remaining drops 9 -> 6");
  check(s.currentTurnIndex == 1, "salvo uses one turn");
  s.currentTurnIndex = 0;
  check(power(s, 1, POWER_SALVO, 3, 0, 0, 1, 0, 2, 0) == PWR_FAIL_ALREADY_USED, "salvo is once per game");

  TS t; running(t);
  check(power(t, 1, POWER_SALVO, 2, 0, 0, 1, 1, 2, 2) == PWR_FAIL_BAD_SHAPE, "diagonal salvo is rejected");
  check(power(t, 1, POWER_SALVO, 2, 0, 0, 1, 0, 3, 0) == PWR_FAIL_BAD_SHAPE, "gap in the line is rejected");
  check(power(t, 1, POWER_SALVO, 2, 5, 0, 6, 0, 7, 0) == PWR_FAIL_BAD_COORDS, "salvo off the grid is rejected");
  check(power(t, 1, POWER_SALVO, 2, 1, 1, 1, 1, 1, 1) == PWR_FAIL_BAD_SHAPE, "three identical cells are rejected");
  shoot(t, 1, 2, 3, 3); t.currentTurnIndex = 0;
  check(power(t, 1, POWER_SALVO, 2, 3, 2, 3, 3, 3, 4) == PWR_FAIL_ALREADY_HIT, "salvo over an already-shot cell rejects the WHOLE action");
  check(t.currentTurnIndex == 0 && !(t.ps.used[0] & (1 << (POWER_SALVO - 1))), "...and spends neither turn nor power");

  TS d; running(d);
  check(power(d, 1, POWER_DOUBLE, 2, 0, 0, 6, 6, 0, 0, &o) == PWR_OK && o.actual[0] == RESULT_HIT && o.actual[1] == RESULT_MISS, "double attack: one hit, one miss, cells far apart");
  d.currentTurnIndex = 0;
  check(power(d, 1, POWER_DOUBLE, 3, 2, 2, 2, 2) == PWR_FAIL_ALREADY_USED, "double attack once per game");
  TS e; running(e);
  check(power(e, 1, POWER_DOUBLE, 2, 2, 2, 2, 2) == PWR_FAIL_BAD_SHAPE, "double attack on one cell twice is rejected");

  TS f; running(f);
  f.remainingShips[1] = 1;   // team 2 has one cell left
  power(f, 1, POWER_DOUBLE, 2, 2, 4, 6, 6, 0, 0, &o);
  check(o.actual[0] == RESULT_SUNK && o.reported[1] == RESULT_INVALID && f.grid[1][6][6] == CELL_WATER, "elimination on the first cell: the second is not fired");
}

void testMine() {
  section("Mine");
  TS s; registerAll(s);
  check(s.roundState == STATE_READY, "setup: all registered");
  check(power(s, 2, POWER_MINE, 0, 2, 4) == PWR_FAIL_MINE_ON_SHIP, "a mine on a ship cell is rejected");
  check(power(s, 2, POWER_MINE, 0, 9, 9) == PWR_FAIL_BAD_COORDS, "a mine off the grid is rejected");
  check(power(s, 2, POWER_MINE, 0, 5, 5) == PWR_OK, "a mine on water is accepted before the game");
  check(power(s, 2, POWER_MINE, 0, 6, 6) == PWR_OK && s.ps.mineX[1] == 6, "placing again just moves it");
  s.roundState = STATE_RUNNING; s.currentTurnIndex = 0;
  check(power(s, 2, POWER_MINE, 0, 1, 1) == PWR_FAIL_NOT_SETUP, "mines cannot be placed once the game is running");

  uint8_t rep = 99;
  check(shoot(s, 1, 2, 6, 6, &rep) == RESULT_MINE && rep == RESULT_MINE, "shooting the mine cell reports MINE");
  check(s.grid[1][6][6] == CELL_MISS, "the mine cell is marked shot");
  check(s.ps.mineX[1] == NO_CELL && s.ps.skipNext[0] == 1, "the mine is used up and the attacker is flagged to skip");
  check(s.currentTurnIndex == 1, "turn moves on to team 2");
  shoot(s, 2, 3, 1, 1); shoot(s, 3, 4, 1, 1);
  check(s.currentTurnIndex == 3, "team 4 to move");
  shoot(s, 4, 1, 1, 1);
  check(s.currentTurnIndex == 1, "team 1 is skipped once: turn goes 4 -> 2");
  check(s.ps.skipNext[0] == 0, "the skip is consumed");

  TS t; registerAll(t);
  check(power(t, 1, POWER_MINE, 0, 6, 6) == PWR_OK, "no-mine control: team 1 mines (6,6)");
  t.roundState = STATE_RUNNING; t.currentTurnIndex = 1;
  check(shoot(t, 2, 3, 6, 6) == RESULT_MISS, "a cell without a mine is a normal miss");
}

void testShield() {
  section("Shield");
  TS s; running(s);
  check(power(s, 1, POWER_SHIELD, 0, 0, 0) == PWR_FAIL_BAD_COORDS, "shield centred on a corner (would be <9 cells) is rejected");
  s.currentTurnIndex = 0;
  shoot(s, 1, 3, 6, 6);                                  // team 1 moves; now team 2
  check(power(s, 2, POWER_SHIELD, 0, 5, 3) == PWR_OK, "team 2 shields the block around (5,3): x 4..6, y 2..4");
  uint8_t rep = 0;
  check(shoot(s, 3, 2, 4, 3, &rep) == RESULT_BLOCKED && rep == RESULT_BLOCKED, "shot inside the shield is BLOCKED");
  check(s.grid[1][3][4] == CELL_SHIP && s.remainingShips[1] == 9, "blocked shot changes nothing on the grid");
  check(s.currentTurnIndex == 3, "a blocked shot still uses the attacker's turn");
  check(shoot(s, 4, 2, 0, 0) == RESULT_HIT, "shot outside the shield still hits");
  check(s.ps.shieldOn[1] == 1, "shield is still up before team 2's turn");
  shoot(s, 1, 2, 0, 1);                                  // team 1, then team 2's turn arrives
  check(s.currentTurnIndex == 1 && s.ps.shieldOn[1] == 0, "shield expires when team 2's next turn begins");
  check(shoot(s, 2, 3, 0, 0) != ATTACK_FAIL_ALREADY_HIT, "(sanity) team 2 can act");
  check(shoot(s, 3, 2, 4, 3) == RESULT_HIT, "the formerly shielded cell can now be hit");
}

void testSmoke() {
  section("Smoke Screen");
  TS s; running(s);
  shoot(s, 1, 3, 6, 6);                                  // -> team 2
  check(power(s, 2, POWER_SMOKE, 0, 0, 0) == PWR_OK && s.ps.smokeOn[1], "team 2 deploys smoke");
  uint8_t rep = 0;
  check(shoot(s, 3, 2, 4, 0, &rep) == RESULT_HIT && rep == RESULT_UNKNOWN, "attacker is told UNKNOWN (truth is HIT)");
  check(s.grid[1][0][4] == CELL_HIT && s.remainingShips[1] == 8, "...but the hit is real");
  check(displayCell(s.ps, 1, 0, 4, s.grid[1][0][4]) == CELL_SHIP, "dashboard still shows an untouched ship cell");
  check(displayRemaining(s.ps, 1, s.remainingShips[1], s.grid[1]) == 9, "dashboard still shows 9/9");
  s.currentTurnIndex = 2;
  check(shoot(s, 3, 2, 4, 0) == ATTACK_FAIL_ALREADY_HIT, "the cell counts as shot");
  shoot(s, 3, 2, 6, 5, &rep);
  check(rep == RESULT_UNKNOWN && displayCell(s.ps, 1, 5, 6, s.grid[1][5][6]) == CELL_WATER, "a smoked miss is hidden too");
  shoot(s, 4, 1, 1, 1);                                  // team 4 -> team 1 -> team 2's turn
  shoot(s, 1, 3, 1, 1);
  check(s.currentTurnIndex == 1 && s.ps.smokeOn[1] == 0, "smoke ends when team 2's turn begins");
  check(displayCell(s.ps, 1, 0, 4, s.grid[1][0][4]) == CELL_HIT && displayRemaining(s.ps, 1, s.remainingShips[1], s.grid[1]) == 8,
        "...and the dashboard now shows the truth");

  TS t; running(t);
  shoot(t, 1, 3, 6, 6);
  power(t, 2, POWER_SMOKE, 0, 0, 0);
  t.remainingShips[1] = 1;
  check(shoot(t, 3, 2, 2, 4, &rep) == RESULT_SUNK && rep == RESULT_SUNK, "an elimination is never hidden by smoke");
  check(t.ps.smokeOn[1] == 0, "...and it lifts the smoke");
}

void testRepair() {
  section("Repair");
  TS s; running(s);
  shoot(s, 1, 2, 4, 0);                                  // hit team 2; now team 2's turn
  check(s.remainingShips[1] == 8, "setup: team 2 was hit");
  check(power(s, 2, POWER_REPAIR, 0, 0, 5) == PWR_FAIL_NOTHING_TO_REPAIR, "repairing a cell that isn't a hit is rejected");
  check(s.currentTurnIndex == 1, "...without using the turn");
  check(power(s, 2, POWER_REPAIR, 0, 4, 0) == PWR_OK, "repairing a hit cell works");
  check(s.grid[1][0][4] == CELL_SHIP && s.remainingShips[1] == 9, "cell is a ship again, HP back to 9");
  check(s.currentTurnIndex == 2, "repair uses the turn");
  check(shoot(s, 3, 2, 4, 0) == RESULT_HIT, "the repaired cell can be hit again");
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
  TS g; registerAll(g);
  check(power(g, 1, POWER_SMOKE, 0, 0, 0) == PWR_FAIL_NOT_RUNNING, "non-mine power-ups need a running game");
  check(powerCount(0x07) == 3 && powerCount(0) == 0, "powerCount works");
  check(POWERUP_BUDGET >= 1 && POWERUP_BUDGET <= POWER_COUNT, "budget constant is sane");

  // classic API still behaves without power-ups
  TS c; running(c);
  uint8_t r = tryAttack(1, 1, 2, 4, 0, c.roundState, c.currentTurnIndex, c.grid, c.remainingShips, c.eliminated, c.registered);
  check(r == RESULT_HIT && c.currentTurnIndex == 1, "tryAttack() without power-up arguments is unchanged");
}

int main() {
  printf("MERAZ BATTLESHIP - POWER-UP TESTS\n");
  testSonar(); testSalvoDouble(); testMine(); testShield(); testSmoke(); testRepair(); testGuards();
  printf("\n==============================\n%d/%d checks passed\n", testsRun - testsFailed, testsRun);
  return testsFailed ? 1 : 0;
}