/* =====================================================================
   CENTRAL_LOGIC_TEST.CPP - desktop test harness for game_logic.h
   =====================================================================
   This is NOT Arduino code and doesn't get uploaded anywhere. It's a
   plain C++ program you compile and run on your own laptop to stress-
   test the battleship rules (ship validation, diagonal ships, registration, attacks,
   elimination, turn order, force-start) without any ESP32 hardware.

   Because it #includes the EXACT SAME game_logic.h that central_node.ino
   uses, a pass here is a real guarantee about the real rules - not a
   hand-copied approximation of them that could quietly drift out of
   sync.

   HOW TO RUN:
     Keep this file in the same folder as game_logic.h, then:

       g++ -std=c++11 -Wall -Wextra central_logic_test.cpp -o central_logic_test
       ./central_logic_test

     (On Windows with MinGW: same command; the output file will be
     central_logic_test.exe instead.)

   It prints [PASS]/[FAIL] for every check, a final summary, and exits
   with code 0 if everything passed or 1 if anything failed - so you
   can also drop it into a CI step or a pre-event checklist script.

   Adding a new test: write a new `void testSomething() { ... }`
   function using check() for each assertion, and call it from main()
   near the bottom. No frameworks, no magic - just plain functions.
   ===================================================================== */

#include "game_logic.h"
#include <cstdio>
#include <cstring>

// =====================================================================
// TINY TEST FRAMEWORK
// =====================================================================

int testsRun = 0;
int testsFailed = 0;

void check(bool condition, const char *description) {
  testsRun++;
  if (condition) {
    printf("  [PASS] %s\n", description);
  } else {
    testsFailed++;
    printf("  [FAIL] %s\n", description);
  }
}

void section(const char *title) {
  printf("\n=== %s ===\n", title);
}

// =====================================================================
// TEST HELPERS
// =====================================================================

// Bundles one team's worth of arrays the same way central_node.ino's
// globals do, so each test can set up a clean game world in one line.
struct TestState {
  uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE];
  ShipPlacement storedShips[MAX_TEAMS][3];
  char teamNames[MAX_TEAMS][20];
  bool registered[MAX_TEAMS];
  bool eliminated[MAX_TEAMS];
  int remainingShips[MAX_TEAMS];
  uint8_t currentTurnIndex;
  uint8_t roundState;
};

void resetState(TestState &s) {
  memset(&s, 0, sizeof(s));
  s.roundState = STATE_SETUP;
  s.currentTurnIndex = 0;
}

// The same layout used as the default example in participant_node.ino:
// size-1 at (2,4), size-3 horizontal at (0,0), size-5 vertical at (4,0).
void validShips(ShipPlacement out[3]) {
  out[0].ship_len = 1; out[0].start_x = 2; out[0].start_y = 4; out[0].orientation = ORIENT_HORIZONTAL;
  out[1].ship_len = 3; out[1].start_x = 0; out[1].start_y = 0; out[1].orientation = ORIENT_HORIZONTAL;
  out[2].ship_len = 5; out[2].start_x = 4; out[2].start_y = 0; out[2].orientation = ORIENT_VERTICAL;
}

// The 9 cells that layout occupies, in a fixed order - used to drive a
// team all the way to elimination without needing per-cell math in
// every test.
const int VALID_SHIP_CELLS[9][2] = {
  {2, 4}, {0, 0}, {1, 0}, {2, 0}, {4, 0}, {4, 1}, {4, 2}, {4, 3}, {4, 4}
};

// Registers all 4 teams with the same valid layout and puts the round
// into STATE_RUNNING with team 1's turn - the common starting point
// for the attack-related tests.
void setUpRunningGame(TestState &s) {
  resetState(s);
  ShipPlacement ships[3];
  validShips(ships);
  for (int t = 1; t <= 4; t++) {
    tryRegister(t, t, "TEAM X", ships, s.roundState, s.registered, s.teamNames,
                s.storedShips, s.grid, s.remainingShips, s.eliminated);
  }
  s.roundState = STATE_RUNNING;
  s.currentTurnIndex = 0;
}

// =====================================================================
// SHIP VALIDATION
// =====================================================================

void testShipValidation() {
  section("Ship validation");

  ShipPlacement ships[3];
  uint8_t grid[GRID_SIZE][GRID_SIZE];

  validShips(ships);
  check(buildAndValidateGrid(ships, grid), "a valid layout is accepted");

  ShipPlacement badSize[3] = {
    {2, 0, 0, ORIENT_HORIZONTAL}, {3, 0, 1, ORIENT_HORIZONTAL}, {5, 0, 2, ORIENT_HORIZONTAL}
  };
  check(!buildAndValidateGrid(badSize, grid), "a ship with an illegal length (2) is rejected");

  ShipPlacement dupSize[3] = {
    {3, 0, 0, ORIENT_HORIZONTAL}, {3, 1, 1, ORIENT_HORIZONTAL}, {5, 0, 2, ORIENT_HORIZONTAL}
  };
  check(!buildAndValidateGrid(dupSize, grid), "two ships of the same size (missing the size-1) is rejected");

  ShipPlacement overlap[3] = {
    {1, 0, 0, ORIENT_HORIZONTAL}, {3, 0, 0, ORIENT_HORIZONTAL}, {5, 0, 1, ORIENT_HORIZONTAL}
  };
  check(!buildAndValidateGrid(overlap, grid), "overlapping ships are rejected");

  ShipPlacement offEdge[3] = {
    {1, 4, 4, ORIENT_HORIZONTAL}, {3, 3, 0, ORIENT_HORIZONTAL}, {5, 1, 0, ORIENT_HORIZONTAL}
  };
  // the size-5 ship starting at x=1 going horizontal covers x=1..5 - x=5 is off the 0-4 grid
  check(!buildAndValidateGrid(offEdge, grid), "a ship running off the edge of the grid is rejected");

  ShipPlacement badOrientation[3] = {
    {1, 0, 0, 7 /* not one of 0-3 */}, {3, 0, 1, ORIENT_HORIZONTAL}, {5, 0, 2, ORIENT_HORIZONTAL}
  };
  check(!buildAndValidateGrid(badOrientation, grid), "an invalid orientation value is rejected");

  // Ships are allowed to touch (just not overlap) - this one has the
  // size-1 ship touching the size-5 ship's corner cell.
  ShipPlacement touching[3] = {
    {1, 4, 4, ORIENT_HORIZONTAL}, {3, 0, 0, ORIENT_HORIZONTAL}, {5, 3, 0, ORIENT_VERTICAL}
  };
  check(buildAndValidateGrid(touching, grid), "ships that touch (but don't overlap) are still accepted");
}

// =====================================================================
// REGISTRATION
// =====================================================================

void testRegistration() {
  section("Registration");

  TestState s;
  resetState(s);
  ShipPlacement ships[3];
  validShips(ships);

  uint8_t result = tryRegister(1, 1, "TEAM 1", ships, s.roundState, s.registered,
                                s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  check(result == REG_OK, "a fresh, valid registration is accepted");
  check(s.registered[0], "team is marked registered after REG_OK");
  check(s.remainingShips[0] == 9, "remaining ships is set to 9 after registration");
  check(s.roundState == STATE_SETUP, "round stays SETUP with only 1 of 4 teams registered");

  result = tryRegister(2, 3, "TEAM 2", ships, s.roundState, s.registered,
                        s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  check(result == REG_FAIL_MAC_MISMATCH, "claiming a different team than your MAC is rejected");

  char emptyName[20];
  memset(emptyName, 0, sizeof(emptyName));
  result = tryRegister(2, 2, emptyName, ships, s.roundState, s.registered,
                        s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  check(result == REG_FAIL_BAD_NAME, "an empty team name is rejected");

  ShipPlacement badShips[3] = {
    {2, 0, 0, ORIENT_HORIZONTAL}, {3, 0, 1, ORIENT_HORIZONTAL}, {5, 0, 2, ORIENT_HORIZONTAL}
  };
  result = tryRegister(2, 2, "TEAM 2", badShips, s.roundState, s.registered,
                        s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  check(result == REG_FAIL_BAD_SHIPS, "an invalid ship layout is rejected");

  tryRegister(2, 2, "TEAM 2", ships, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  tryRegister(3, 3, "TEAM 3", ships, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  result = tryRegister(4, 4, "TEAM 4", ships, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  check(result == REG_OK, "4th team registers fine");
  check(s.roundState == STATE_READY, "round becomes READY once all 4 teams are registered");

  s.roundState = STATE_RUNNING;  // simulate the organizer hitting START
  s.currentTurnIndex = 0;

  result = tryRegister(2, 2, "TEAM 2", ships, s.roundState, s.registered,
                        s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  check(result == REG_RECONNECTED, "re-registering with the exact same config mid-game is treated as a reconnect");

  result = tryRegister(2, 2, "DIFFERENT NAME", ships, s.roundState, s.registered,
                        s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  check(result == REG_RECONNECT_REJECTED, "reconnecting with a different team name is rejected");
  check(strcmp(s.teamNames[1], "TEAM 2") == 0, "a rejected reconnect does not overwrite the stored team name");

  TestState fresh;
  resetState(fresh);
  fresh.roundState = STATE_RUNNING;  // team 1 never registered before this round started
  result = tryRegister(1, 1, "LATECOMER", ships, fresh.roundState, fresh.registered,
                        fresh.teamNames, fresh.storedShips, fresh.grid, fresh.remainingShips, fresh.eliminated);
  check(result == REG_FAIL_GAME_STARTED, "a brand-new team trying to join after the game started is rejected");
}

// =====================================================================
// ATTACKS
// =====================================================================

void testAttacks() {
  section("Attacks");

  TestState s;
  setUpRunningGame(s);
  check(!s.registered[0] == false, "setup: team 1 is registered");  // sanity check on the helper itself

  s.roundState = STATE_SETUP;
  uint8_t r = tryAttack(1, 1, 2, 0, 0, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_NOT_RUNNING, "attacking while the game isn't running is rejected");
  s.roundState = STATE_RUNNING;

  r = tryAttack(2, 2, 3, 0, 0, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_WRONG_TURN, "attacking out of turn is rejected");
  check(s.currentTurnIndex == 0, "an invalid attack does not consume the turn");

  r = tryAttack(1, 1, 1, 0, 0, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_SELF_ATTACK, "attacking yourself is rejected");

  r = tryAttack(1, 2, 3, 0, 0, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_ID_MISMATCH, "claiming a different attacker id than your MAC is rejected");

  r = tryAttack(1, 1, 9, 0, 0, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_BAD_TARGET, "an out-of-range target team is rejected");

  r = tryAttack(1, 1, 2, 9, 9, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_BAD_COORDS, "out-of-range coordinates are rejected");

  // (1,1) is water in the default layout - valid MISS.
  r = tryAttack(1, 1, 2, 1, 1, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == RESULT_MISS, "attacking a water cell is a MISS");
  check(s.currentTurnIndex == 1, "a valid attack advances the turn to the next team");

  s.currentTurnIndex = 0;  // force it back to team 1 to test the duplicate-attack rule in isolation
  r = tryAttack(1, 1, 2, 1, 1, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_ALREADY_HIT, "attacking an already-attacked cell is rejected");

  // (2,4) is the size-1 ship in the default layout - valid HIT.
  r = tryAttack(1, 1, 2, 2, 4, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == RESULT_HIT, "attacking a ship cell is a HIT");
  check(s.remainingShips[1] == 8, "remaining ship count decreases after a hit");

  // an eliminated/unregistered target should be rejected too
  s.eliminated[2] = true;  // pretend team 3 is already eliminated
  s.currentTurnIndex = 0;
  r = tryAttack(1, 1, 3, 0, 0, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == ATTACK_FAIL_TARGET_INVALID, "attacking an eliminated team is rejected");
}

// =====================================================================
// ELIMINATION AND TURN SKIPPING
// =====================================================================

void testEliminationAndTurnSkipping() {
  section("Elimination and turn skipping");

  TestState s;
  setUpRunningGame(s);

  uint8_t lastResult = RESULT_MISS;
  for (int i = 0; i < 9; i++) {
    s.currentTurnIndex = 0;  // keep it team 1's turn for every hit in this focused test
    lastResult = tryAttack(1, 1, 2, VALID_SHIP_CELLS[i][0], VALID_SHIP_CELLS[i][1],
                            s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  }
  check(lastResult == RESULT_SUNK, "the 9th hit on a team returns SUNK, not HIT");
  check(s.eliminated[1], "team is marked eliminated after all 9 ship cells are hit");
  check(s.remainingShips[1] == 0, "remaining ships is 0 after elimination");

  s.currentTurnIndex = 0;  // team 1
  advanceTurn(s.roundState, s.currentTurnIndex, s.eliminated, s.registered);
  check(s.currentTurnIndex == 2, "turn order skips an eliminated team (1 -> 3, not 1 -> 2)");
}

// =====================================================================
// DIAGONAL SHIPS
// =====================================================================

int countShipCells(uint8_t grid[GRID_SIZE][GRID_SIZE]) {
  int n = 0;
  for (int r = 0; r < GRID_SIZE; r++)
    for (int c = 0; c < GRID_SIZE; c++)
      if (grid[r][c] == CELL_SHIP) n++;
  return n;
}

void testDiagonalShips() {
  section("Diagonal ships");

  uint8_t grid[GRID_SIZE][GRID_SIZE];

  // size-5 down-right corner to corner, size-3 along the bottom, size-1 top-right
  ShipPlacement diagDown[3] = {
    {1, 4, 0, ORIENT_HORIZONTAL}, {3, 0, 4, ORIENT_HORIZONTAL}, {5, 0, 0, ORIENT_DIAG_DOWN}
  };
  check(buildAndValidateGrid(diagDown, grid), "a down-right diagonal ship is accepted");
  check(countShipCells(grid) == 9, "diagonal layout occupies exactly 9 cells");
  check(grid[0][0] == CELL_SHIP && grid[2][2] == CELL_SHIP && grid[4][4] == CELL_SHIP,
        "down-right diagonal covers (0,0), (2,2) and (4,4)");

  // size-5 up-right (bottom-left to top-right)
  ShipPlacement diagUp[3] = {
    {1, 4, 4, ORIENT_HORIZONTAL}, {3, 0, 0, ORIENT_HORIZONTAL}, {5, 0, 4, ORIENT_DIAG_UP}
  };
  check(buildAndValidateGrid(diagUp, grid), "an up-right diagonal ship is accepted");
  check(countShipCells(grid) == 9, "up-right layout occupies exactly 9 cells");
  check(grid[4][0] == CELL_SHIP && grid[2][2] == CELL_SHIP && grid[0][4] == CELL_SHIP,
        "up-right diagonal covers (0,4), (2,2) and (4,0)");

  // short diagonals in the middle of the board
  ShipPlacement shortDiag[3] = {
    {1, 4, 4, ORIENT_HORIZONTAL}, {3, 0, 1, ORIENT_DIAG_DOWN}, {5, 0, 0, ORIENT_HORIZONTAL}
  };
  check(buildAndValidateGrid(shortDiag, grid), "a size-3 diagonal in the middle of the grid is accepted");

  ShipPlacement runsOffRight[3] = {
    {1, 4, 4, ORIENT_HORIZONTAL}, {3, 0, 4, ORIENT_HORIZONTAL}, {5, 1, 0, ORIENT_DIAG_DOWN}
  };
  check(!buildAndValidateGrid(runsOffRight, grid), "a diagonal running off the right/bottom edge is rejected");

  ShipPlacement runsOffTop[3] = {
    {1, 4, 4, ORIENT_HORIZONTAL}, {3, 0, 1, ORIENT_DIAG_UP}, {5, 0, 2, ORIENT_HORIZONTAL}
  };
  // size-3 up-right from (0,1) needs (2,-1) - above the top of the grid
  check(!buildAndValidateGrid(runsOffTop, grid), "an up-right diagonal running off the top edge is rejected");

  ShipPlacement crossing[3] = {
    {1, 4, 4, ORIENT_HORIZONTAL}, {3, 0, 2, ORIENT_DIAG_UP}, {5, 0, 0, ORIENT_DIAG_DOWN}
  };
  // both diagonals pass through (1,1)
  check(!buildAndValidateGrid(crossing, grid), "two diagonals crossing each other are rejected as overlapping");

  // Real hit/miss behaviour against a diagonal ship
  TestState s;
  resetState(s);
  for (int t = 1; t <= 4; t++) {
    tryRegister(t, t, "TEAM X", diagUp, s.roundState, s.registered, s.teamNames,
                s.storedShips, s.grid, s.remainingShips, s.eliminated);
  }
  check(s.roundState == STATE_READY, "diagonal layout registers for all 4 teams");
  s.roundState = STATE_RUNNING;
  s.currentTurnIndex = 0;

  uint8_t r = tryAttack(1, 1, 2, 2, 2, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == RESULT_HIT, "hitting the middle of a diagonal ship is a HIT");

  s.currentTurnIndex = 0;
  r = tryAttack(1, 1, 2, 3, 3, s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  check(r == RESULT_MISS, "a cell next to the diagonal (not on it) is a MISS");

  // sink the whole diagonal fleet
  const int diagCells[9][2] = {{4,4},{0,0},{1,0},{2,0},{0,4},{1,3},{2,2},{3,1},{4,0}};
  uint8_t last = RESULT_MISS;
  for (int i = 0; i < 9; i++) {
    if (diagCells[i][0] == 2 && diagCells[i][1] == 2) continue;  // already hit above
    s.currentTurnIndex = 0;
    last = tryAttack(1, 1, 2, diagCells[i][0], diagCells[i][1],
                     s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
  }
  check(last == RESULT_SUNK && s.eliminated[1], "a team with diagonal ships is eliminated after all 9 cells are hit");
}

// =====================================================================
// FORCE START
// =====================================================================

void testForceStart() {
  section("Force start");

  TestState s;
  resetState(s);
  ShipPlacement ships[3];
  validShips(ships);

  bool ok = tryForceStart(s.roundState, s.currentTurnIndex, s.registered);
  check(!ok, "force-starting with 0 registered teams fails");

  tryRegister(1, 1, "TEAM 1", ships, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  ok = tryForceStart(s.roundState, s.currentTurnIndex, s.registered);
  check(!ok, "force-starting with only 1 registered team fails");

  tryRegister(3, 3, "TEAM 3", ships, s.roundState, s.registered, s.teamNames, s.storedShips, s.grid, s.remainingShips, s.eliminated);
  ok = tryForceStart(s.roundState, s.currentTurnIndex, s.registered);
  check(ok, "force-starting with 2 registered teams (1 and 3) succeeds");
  check(s.roundState == STATE_RUNNING, "round becomes RUNNING after a force start");
  check(s.currentTurnIndex == 0, "the first registered team (team 1) goes first");

  ok = tryForceStart(s.roundState, s.currentTurnIndex, s.registered);
  check(!ok, "force-starting again while a round is already running fails");

  TestState s2;
  resetState(s2);
  tryRegister(2, 2, "TEAM 2", ships, s2.roundState, s2.registered, s2.teamNames, s2.storedShips, s2.grid, s2.remainingShips, s2.eliminated);
  tryRegister(4, 4, "TEAM 4", ships, s2.roundState, s2.registered, s2.teamNames, s2.storedShips, s2.grid, s2.remainingShips, s2.eliminated);
  ok = tryForceStart(s2.roundState, s2.currentTurnIndex, s2.registered);
  check(ok, "force-starting with teams 2 and 4 (not team 1) succeeds");
  check(s2.currentTurnIndex == 1, "team 2 (the first registered team) goes first when team 1 never registered");
}

// =====================================================================
// FULL GAME INTEGRATION
// =====================================================================

void testFullGameIntegration() {
  section("Full game integration (4 teams, play to a winner)");

  TestState s;
  setUpRunningGame(s);
  check(s.roundState == STATE_RUNNING, "integration: setup helper leaves the round RUNNING");

  // Team 1 eliminates teams 2, 3 and 4 in turn. currentTurnIndex is
  // forced back to team 1 before every attack so this test stays
  // focused on the elimination/game-over rules - realistic turn
  // rotation is already covered by testEliminationAndTurnSkipping().
  for (int target = 2; target <= 4; target++) {
    for (int i = 0; i < 9; i++) {
      s.currentTurnIndex = 0;
      tryAttack(1, 1, target, VALID_SHIP_CELLS[i][0], VALID_SHIP_CELLS[i][1],
                s.roundState, s.currentTurnIndex, s.grid, s.remainingShips, s.eliminated, s.registered);
    }
    char label[64];
    snprintf(label, sizeof(label), "integration: team %d is eliminated after 9 hits", target);
    check(s.eliminated[target - 1], label);
  }

  check(s.roundState == STATE_GAMEOVER, "integration: game ends once only one team remains");
  check(!s.eliminated[0], "integration: team 1 (the attacker) is left as the winner");
}

// =====================================================================
// MAIN
// =====================================================================

int main() {
  printf("MERAZ BATTLESHIP - CENTRAL LOGIC TEST HARNESS\n");
  printf("Testing game_logic.h directly - no ESP32 needed.\n");

  testShipValidation();
  testDiagonalShips();
  testRegistration();
  testAttacks();
  testEliminationAndTurnSkipping();
  testForceStart();
  testFullGameIntegration();

  printf("\n==============================\n");
  printf("%d/%d checks passed\n", testsRun - testsFailed, testsRun);

  if (testsFailed > 0) {
    printf("%d FAILED - see [FAIL] lines above\n", testsFailed);
    return 1;
  }

  printf("ALL PASSED\n");
  return 0;
}
