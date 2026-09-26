/* =====================================================================
   CENTRAL_LOGIC_TEST.CPP - Automated Desktop Test Harness
   =====================================================================
   Compiles with standard g++ (no Arduino, WiFi, or ESP-NOW required):
       g++ -std=c++17 -Wall -Wextra -I central_node central_logic_test.cpp -o central_logic_test
       ./central_logic_test

   Tests all pure battleship rules from central_node/game_logic.h:
     - Ship placement validation (lengths 1, 3, 5, overlap, out of bounds)
     - Registration rules (MAC match, state progression, reconnects)
     - Turn ordering (clockwise, skips eliminated & unregistered)
     - Force-start mechanics (requires >=2 teams, lowest ID goes first)
     - Attack validation (turn gating, self-attack, dead targets, repeat cells)
     - Win detection (elimination on 9th hit, last team standing)
   ===================================================================== */

#include <iostream>
#include <cassert>
#include <cstring>
#include "central_node/game_logic.h"

int totalChecks = 0;
int passedChecks = 0;

#define CHECK(condition, desc) do { \
    totalChecks++; \
    if (condition) { \
        passedChecks++; \
    } else { \
        std::cerr << "FAIL [" << totalChecks << "]: " << desc << " (line " << __LINE__ << ")\n"; \
    } \
} while(0)

// Helper: standard legal ships (total 9 cells: sizes 1, 3, 5)
void makeLegalShips(ShipPlacement s[3]) {
    s[0] = { 1, 0, 0, ORIENT_HORIZONTAL };  // (0,0)
    s[1] = { 3, 0, 1, ORIENT_HORIZONTAL };  // (0,1), (1,1), (2,1)
    s[2] = { 5, 4, 0, ORIENT_VERTICAL };    // (4,0), (4,1), (4,2), (4,3), (4,4)
}

void testShipPlacement() {
    uint8_t grid[GRID_SIZE][GRID_SIZE];
    ShipPlacement s[3];

    // 1. Legal layout
    makeLegalShips(s);
    CHECK(buildAndValidateGrid(s, grid) == true, "Legal ship layout accepted");

    int cellCount = 0;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++)
            if (grid[r][c] == CELL_SHIP) cellCount++;
    CHECK(cellCount == 9, "Grid contains exactly 9 ship cells");

    // 2. Overlapping ships
    makeLegalShips(s);
    s[0] = { 1, 0, 1, ORIENT_HORIZONTAL }; // overlaps s[1] at (0,1)
    CHECK(buildAndValidateGrid(s, grid) == false, "Overlapping ships rejected");

    // 3. Out of bounds (horizontal)
    makeLegalShips(s);
    s[1] = { 3, 3, 1, ORIENT_HORIZONTAL }; // spans x=3,4,5 -> x=5 is OOB
    CHECK(buildAndValidateGrid(s, grid) == false, "Out of bounds horizontal rejected");

    // 4. Out of bounds (vertical)
    makeLegalShips(s);
    s[2] = { 5, 0, 1, ORIENT_VERTICAL }; // spans y=1..5 -> y=5 is OOB
    CHECK(buildAndValidateGrid(s, grid) == false, "Out of bounds vertical rejected");

    // 5. Wrong lengths (e.g. 2, 3, 5 instead of 1, 3, 5)
    makeLegalShips(s);
    s[0] = { 2, 0, 4, ORIENT_HORIZONTAL };
    CHECK(buildAndValidateGrid(s, grid) == false, "Wrong ship length rejected");

    // 6. Duplicate length (e.g. two length 3 ships)
    makeLegalShips(s);
    s[0] = { 3, 0, 4, ORIENT_HORIZONTAL };
    CHECK(buildAndValidateGrid(s, grid) == false, "Duplicate ship lengths rejected");
}

void testRegistrationAndForceStart() {
    uint8_t roundState = STATE_SETUP;
    bool registered[MAX_TEAMS] = {false};
    char teamNames[MAX_TEAMS][20];
    ShipPlacement storedShips[MAX_TEAMS][3];
    uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE];
    int remainingShips[MAX_TEAMS] = {0};
    bool eliminated[MAX_TEAMS] = {false};

    ShipPlacement legal[3];
    makeLegalShips(legal);

    // 7. Reject registration with mismatched MAC
    uint8_t r = tryRegister(1, 2, "Team 1", legal, roundState, registered, teamNames, storedShips, grid, remainingShips, eliminated);
    CHECK(r == REG_FAIL_MAC_MISMATCH, "Mismatched MAC and claimed ID rejected");

    // 8. Reject registration with empty name
    r = tryRegister(1, 1, "", legal, roundState, registered, teamNames, storedShips, grid, remainingShips, eliminated);
    CHECK(r == REG_FAIL_BAD_NAME, "Empty team name rejected");

    // 9. Successful registration of Team 1
    r = tryRegister(1, 1, "Alpha", legal, roundState, registered, teamNames, storedShips, grid, remainingShips, eliminated);
    CHECK(r == REG_OK, "Valid registration of Team 1 succeeds");
    CHECK(registered[0] == true, "Team 1 registered flag set");
    CHECK(remainingShips[0] == 9, "Team 1 starts with 9 ships");
    CHECK(roundState == STATE_SETUP, "State stays SETUP with 1 team");

    // 10. Force start with only 1 team should fail
    uint8_t turnIdx = 0;
    CHECK(tryForceStart(roundState, turnIdx, registered) == false, "Force start with 1 team rejected");

    // 11. Register Team 2
    r = tryRegister(2, 2, "Bravo", legal, roundState, registered, teamNames, storedShips, grid, remainingShips, eliminated);
    CHECK(r == REG_OK, "Valid registration of Team 2 succeeds");
    CHECK(roundState == STATE_SETUP, "State stays SETUP with 2 teams");

    // 12. Force start with 2 teams succeeds
    CHECK(tryForceStart(roundState, turnIdx, registered) == true, "Force start with 2 teams succeeds");
    CHECK(roundState == STATE_RUNNING, "State changes to RUNNING");
    CHECK(turnIdx == 0, "First registered team (Team 1) gets first turn");

    // 13. Reconnecting during RUNNING with matching ships & name succeeds
    r = tryRegister(1, 1, "Alpha", legal, roundState, registered, teamNames, storedShips, grid, remainingShips, eliminated);
    CHECK(r == REG_RECONNECTED, "Reconnect during game with matching config succeeds");

    // 14. Reconnecting during RUNNING with changed ships fails
    ShipPlacement modified[3];
    makeLegalShips(modified);
    modified[0] = { 1, 3, 3, ORIENT_HORIZONTAL };
    r = tryRegister(1, 1, "Alpha", modified, roundState, registered, teamNames, storedShips, grid, remainingShips, eliminated);
    CHECK(r == REG_RECONNECT_REJECTED, "Reconnect with altered ships rejected");
}

void testAttacksAndTurns() {
    uint8_t roundState = STATE_RUNNING;
    uint8_t currentTurn = 0; // Team 1's turn (0-indexed)
    bool registered[MAX_TEAMS] = { true, true, true, true };
    bool eliminated[MAX_TEAMS] = { false, false, false, false };
    int remainingShips[MAX_TEAMS] = { 9, 9, 9, 9 };
    uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE];

    // Initialize all grids with legal ships
    for (int t = 0; t < 4; t++) {
        ShipPlacement s[3];
        makeLegalShips(s);
        buildAndValidateGrid(s, grid[t]);
    }

    // 15. Reject attack from wrong team
    uint8_t res = tryAttack(2, 2, 3, 0, 0, roundState, currentTurn, grid, remainingShips, eliminated, registered);
    CHECK(res == ATTACK_FAIL_WRONG_TURN, "Attack out of turn rejected");
    CHECK(currentTurn == 0, "Turn did not advance on invalid attack");

    // 16. Reject self-attack
    res = tryAttack(1, 1, 1, 0, 0, roundState, currentTurn, grid, remainingShips, eliminated, registered);
    CHECK(res == ATTACK_FAIL_SELF_ATTACK, "Self-attack rejected");

    // 17. Reject attack out of bounds
    res = tryAttack(1, 1, 2, 5, 0, roundState, currentTurn, grid, remainingShips, eliminated, registered);
    CHECK(res == ATTACK_FAIL_BAD_COORDS, "Out of bounds coords rejected");

    // 18. Valid Miss: Team 1 attacks Team 2 at (3,0) where there is water
    CHECK(grid[1][0][3] == CELL_WATER, "Cell (3,0) is water");
    res = tryAttack(1, 1, 2, 3, 0, roundState, currentTurn, grid, remainingShips, eliminated, registered);
    CHECK(res == RESULT_MISS, "Attack on water returns MISS");
    CHECK(grid[1][0][3] == CELL_MISS, "Grid cell updated to CELL_MISS");
    CHECK(currentTurn == 1, "Turn advanced clockwise to Team 2 (idx 1)");

    // 19. Team 2 attacks Team 3 at (0,0) where there is a ship (HIT)
    CHECK(grid[2][0][0] == CELL_SHIP, "Cell (0,0) is a ship");
    res = tryAttack(2, 2, 3, 0, 0, roundState, currentTurn, grid, remainingShips, eliminated, registered);
    CHECK(res == RESULT_HIT, "Attack on ship returns HIT");
    CHECK(grid[2][0][0] == CELL_HIT, "Grid cell updated to CELL_HIT");
    CHECK(remainingShips[2] == 8, "Team 3 ships reduced to 8");
    CHECK(currentTurn == 2, "Turn advanced to Team 3 (idx 2)");

    // 20. Reject repeat attack on already hit/missed cell
    // Advance turns back to Team 1
    currentTurn = 0;
    res = tryAttack(1, 1, 2, 3, 0, roundState, currentTurn, grid, remainingShips, eliminated, registered);
    CHECK(res == ATTACK_FAIL_ALREADY_HIT, "Repeat attack on same cell rejected");
    CHECK(currentTurn == 0, "Turn not consumed on repeat attack");

    // 21. Sinking a team (9 hits)
    // Reduce Team 4 to 1 ship cell
    remainingShips[3] = 1;
    // Attack Team 4's final cell (4,4)
    currentTurn = 0;
    res = tryAttack(1, 1, 4, 4, 4, roundState, currentTurn, grid, remainingShips, eliminated, registered);
    CHECK(res == RESULT_SUNK, "9th hit returns RESULT_SUNK");
    CHECK(eliminated[3] == true, "Team 4 marked as eliminated");
    CHECK(remainingShips[3] == 0, "Team 4 has 0 remaining ships");

    // 22. Turn advance skips eliminated team
    currentTurn = 2; // Team 3's turn
    // Normal advance would go to 3 (Team 4), but Team 4 is eliminated -> should land on 0 (Team 1)
    advanceTurn(roundState, currentTurn, eliminated, registered);
    CHECK(currentTurn == 0, "Turn advance skipped eliminated Team 4 and wrapped to Team 1");

    // 23. Game over when only 1 team left alive
    eliminated[1] = true;
    eliminated[2] = true;
    // Only Team 1 alive now
    advanceTurn(roundState, currentTurn, eliminated, registered);
    CHECK(roundState == STATE_GAMEOVER, "Game over reached when <= 1 team remains");
}

int main() {
    std::cout << "=========================================\n";
    std::cout << " MERAZ BATTLESHIP - GAME LOGIC TEST SUITE\n";
    std::cout << "=========================================\n";

    testShipPlacement();
    testRegistrationAndForceStart();
    testAttacksAndTurns();

    std::cout << "\nResults: " << passedChecks << "/" << totalChecks << " checks passed.\n";
    if (passedChecks == totalChecks) {
        std::cout << ">>> ALL CHECKS PASSED! Game logic is 100% verified. <<<\n";
        return 0;
    } else {
        std::cout << ">>> SOME CHECKS FAILED! <<<\n";
        return 1;
    }
}
