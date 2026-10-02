/* =====================================================================
   MERAZ BATTLESHIP - CENTRAL NODE   (v3: power-ups)
   =====================================================================
   This ESP32 is the ONLY authority for the game. It owns all four
   grids, all turn logic, all elimination logic, and all round state.
   The laptop (dashboard.py) is just a screen - it never decides
   anything, it only displays what this board tells it.

   DESIGN DECISIONS (see the git history / README for the full story)
   1) Registration carries 3 explicit ship placements; Central builds
      the grid itself.
   2) Packet type is detected by BYTE LENGTH, so no type byte is needed:
        Central receives : Registration 33, Attack 4, PowerUp 9
        Boards   receive : RegAck 1, TurnUpdate 2, Feedback 4, PowerResult 7
      Every size a given board can receive is different. KEEP IT THAT WAY
      if you ever add a packet.
   3) Power-loss protection: every state change is saved to flash.
   4) Diagonal ships (rules live in game_logic.h).
   5) POWER-UPS (new): Sonar, Salvo, Mine, Repair, Smoke, Shield, Double.
      All rules are in game_logic.h (tryPowerUp / tryAttack). This file
      only does the radio, logging, flash and dashboard JSON.

   WHAT THE PUBLIC DASHBOARD MAY AND MAY NOT SEE
      - Sonar results go ONLY to the user. The public log says "used SONAR".
      - Mine positions are never printed in JSON or the public log.
        (The organizer sees them in the Serial Monitor: GRID / STATUS.)
      - Shield position is never published; the dashboard only learns
        THAT a shield is up ("shield": true).
      - While a team is under SMOKE, the JSON shows its board as if the
        shots hadn't happened (displayCell / displayRemaining). The real
        result appears when the smoke expires.
      Plain-text lines that start with "[organizer]" are NOT JSON, so
      dashboard.py ignores them; read them in the Arduino Serial Monitor
      if you need the truth during a smoke screen.

   WHERE THIS FILE FITS IN THE WHOLE SYSTEM
       team boards  <--ESP-NOW-->  THIS FILE  <--USB serial-->  dashboard.py
   ===================================================================== */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <Preferences.h>
#include <string.h>
#include <stdarg.h>
#include "game_logic.h"   // shared, pure game rules - keep this file next to this .ino

// =====================================================================
// CONFIGURATION - CHANGE THESE FOR YOUR EVENT
// =====================================================================

// All 5 boards (this one + 4 participants) must use the SAME channel.
#define ESPNOW_CHANNEL 1

// TODO: replace with the real MAC address of each participant ESP32.
// Order matters: index 0 = Team 1, index 1 = Team 2, etc.
uint8_t participantMacs[4][6] = {
  { 0x3C, 0x8A, 0x1F, 0x5D, 0x55, 0x5C },  // TEAM 1 - CHANGE ME
  { 0x6C, 0xC8, 0X40, 0x88, 0x00, 0x8C },  // TEAM 2 - CHANGE ME
  { 0xA4, 0xCF, 0x12, 0x00, 0x00, 0x03 },  // TEAM 3 - CHANGE ME
  { 0xA4, 0xCF, 0x12, 0x00, 0x00, 0x04 },  // TEAM 4 - CHANGE ME
};

// =====================================================================
// CONSTANTS SPECIFIC TO THIS FILE
// =====================================================================

#define MAX_PACKET_SIZE    64
#define PENDING_QUEUE_SIZE 4
#define LOG_SIZE           16   // how many recent events we keep for the dashboard
#define LOG_LINE_LEN       96

// =====================================================================
// PACKET STRUCTURES (must match participant_node.ino exactly)
// =====================================================================

typedef struct __attribute__((packed)) {
  uint8_t team_id;
  char team_name[20];
  ShipPlacement ships[3];
} RegistrationPacket;   // 33 bytes

typedef struct __attribute__((packed)) {
  uint8_t status;        // REG_* code
} RegAckPacket;          // 1 byte

typedef struct __attribute__((packed)) {
  uint8_t attacker_id;
  uint8_t target_id;
  uint8_t x;
  uint8_t y;
} AttackPacket;          // 4 bytes

typedef struct __attribute__((packed)) {
  uint8_t target_id;
  uint8_t x;
  uint8_t y;
  uint8_t result_code;   // RESULT_* code (now also MINE / BLOCKED / UNKNOWN)
} FeedbackPacket;        // 4 bytes

typedef struct __attribute__((packed)) {
  uint8_t current_turn;  // 1-4, or 0 if no team currently has the turn
  uint8_t round_state;   // STATE_* code
} TurnUpdatePacket;      // 2 bytes

// NEW: a team asks to use a power-up. Unused cells/target are sent as 0.
typedef struct __attribute__((packed)) {
  uint8_t power;         // POWER_* code
  uint8_t team_id;       // who is asking (must match the sender's MAC)
  uint8_t target_id;     // opponent for SONAR / SALVO / DOUBLE, else 0
  uint8_t x1, y1;        // meaning depends on the power - see game_logic.h tryPowerUp()
  uint8_t x2, y2;
  uint8_t x3, y3;
} PowerUpPacket;         // 9 bytes

// NEW: Central's answer to a PowerUpPacket (sent to the sender only).
typedef struct __attribute__((packed)) {
  uint8_t power;
  uint8_t status;        // PWR_STATUS_OK / REJECTED / JAMMED
  uint8_t target_id;
  uint8_t count;         // SONAR: ship cells in the area
  uint8_t result[3];     // SALVO / DOUBLE: result per cell (RESULT_*; RESULT_INVALID = not fired)
} PowerResultPacket;     // 7 bytes

static_assert(sizeof(RegistrationPacket) == 33, "RegistrationPacket must be 33 bytes");
static_assert(sizeof(AttackPacket) == 4, "AttackPacket must be 4 bytes");
static_assert(sizeof(PowerUpPacket) == 9, "PowerUpPacket must be 9 bytes");
static_assert(sizeof(PowerResultPacket) == 7, "PowerResultPacket must be 7 bytes");

// =====================================================================
// GAME STATE
// =====================================================================

uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE];   // live grid per team
ShipPlacement storedShips[MAX_TEAMS][3];         // original layout, for reconnection checks
char teamNames[MAX_TEAMS][20];
bool registered[MAX_TEAMS];
bool eliminated[MAX_TEAMS];
int remainingShips[MAX_TEAMS];
PowerState powerState;                           // NEW: mines, shields, smoke, used power-ups

uint8_t currentTurnIndex = 0;   // 0-3 internally (team_id = index + 1)
uint8_t roundState = STATE_SETUP;

char eventLog[LOG_SIZE][LOG_LINE_LEN];
int logHead = 0;
int logCount = 0;

typedef struct {
  uint8_t mac[6];
  uint8_t data[MAX_PACKET_SIZE];
  int len;
} PendingPacket;

PendingPacket pendingQueue[PENDING_QUEUE_SIZE];
volatile int pendingHead = 0;
volatile int pendingTail = 0;

String serialBuffer = "";

// =====================================================================
// STATE PERSISTENCE (survives a Central power loss / reboot)
// =====================================================================

Preferences prefs;

#define PREFS_NAMESPACE "battleship"
#define SAVE_MAGIC       0xBA77CAFE
#define SAVE_VERSION     3  // v3 added PowerState. An older save is ignored (fresh game).

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  version;
  uint8_t  roundState;
  uint8_t  currentTurnIndex;
  uint8_t  registered[MAX_TEAMS];
  uint8_t  eliminated[MAX_TEAMS];
  uint8_t  remainingShips[MAX_TEAMS];
  char     teamNames[MAX_TEAMS][20];
  uint8_t  grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE];
  ShipPlacement storedShips[MAX_TEAMS][3];
  PowerState power;
} SavedState;

// =====================================================================
// FUNCTION DECLARATIONS
// =====================================================================

#if ESP_IDF_VERSION_MAJOR >= 5
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len);
#else
void OnDataRecv(const uint8_t *senderMac, const uint8_t *incomingData, int len);
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status);
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
#endif
void processPacket(const uint8_t *mac, const uint8_t *data, int len);

int  getTeamIDFromMAC(const uint8_t *mac);

void handleRegistration(const uint8_t *mac, RegistrationPacket *pkt);
void handleAttack(const uint8_t *mac, AttackPacket *pkt);
void handlePowerUp(const uint8_t *mac, PowerUpPacket *pkt);
void sendRegAck(const uint8_t *mac, uint8_t status);
void sendFeedback(const uint8_t *mac, uint8_t targetId, uint8_t x, uint8_t y, uint8_t result);
void sendPowerResult(const uint8_t *mac, const PowerOutcome &out, uint8_t targetId);
void broadcastTurnUpdate();
void resetGame();
void logShot(int attacker, int target, int x, int y, uint8_t actual, uint8_t reported);
const char *powerName(int power);
const char *powerFailText(uint8_t code);

void saveGameState();
bool loadGameState();

void addLog(const char *fmt, ...);
void printEscapedJSON(const char *s);
void sendStateJSON();

void handleSerialInput();
void processSerialCommand(String cmd);
void cmdStart();
void cmdForceStart();
void cmdSkipTurn();
void logGameOverIfJustEnded(uint8_t previousRoundState);
void printStatus();
void printGrids();
void printHelp();
const char *roundStateName(uint8_t s);

// =====================================================================
// SETUP / LOOP
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("Central ESP32 MAC address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("(participants need this MAC in their centralMac[] config)");

  if (esp_now_init() != ESP_OK) {
    Serial.println("FATAL: esp_now_init() failed. Check the board and reset.");
    while (true) { delay(1000); }
  }

  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  for (int i = 0; i < MAX_TEAMS; i++) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, participantMacs[i], 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.print("WARNING: could not add ESP-NOW peer for TEAM ");
      Serial.println(i + 1);
    }
  }

  resetPowerState(powerState);   // loadGameState() overwrites this if a save exists

  if (loadGameState()) {
    Serial.println("Restored a saved game state from flash (Central must have rebooted mid-round).");
    addLog("Central rebooted - restored saved state (round state: %s)", roundStateName(roundState));
    broadcastTurnUpdate();
  } else {
    resetGame();
  }

  Serial.println("=== MERAZ BATTLESHIP - CENTRAL NODE READY ===");
  printHelp();
}

void loop() {
  while (pendingHead != pendingTail) {
    PendingPacket p = pendingQueue[pendingHead];
    pendingHead = (pendingHead + 1) % PENDING_QUEUE_SIZE;
    processPacket(p.mac, p.data, p.len);
  }

  handleSerialInput();
}

// =====================================================================
// ESP-NOW CALLBACKS (keep these SHORT - just copy and queue)
// =====================================================================

#if ESP_IDF_VERSION_MAJOR >= 5
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  const uint8_t *senderMac = recv_info->src_addr;
#else
void OnDataRecv(const uint8_t *senderMac, const uint8_t *incomingData, int len) {
#endif
  if (len <= 0 || len > MAX_PACKET_SIZE) return;

  int next = (pendingTail + 1) % PENDING_QUEUE_SIZE;
  if (next == pendingHead) return;  // queue full - drop

  memcpy(pendingQueue[pendingTail].mac, senderMac, 6);
  memcpy(pendingQueue[pendingTail].data, incomingData, len);
  pendingQueue[pendingTail].len = len;
  pendingTail = next;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("WARNING: an ESP-NOW send failed to reach a peer.");
  }
}

// =====================================================================
// PACKET ROUTING (by length - see note at top of file)
// =====================================================================

void processPacket(const uint8_t *mac, const uint8_t *data, int len) {
  if (len == sizeof(RegistrationPacket)) {
    RegistrationPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleRegistration(mac, &pkt);
  } else if (len == sizeof(AttackPacket)) {
    AttackPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleAttack(mac, &pkt);
  } else if (len == sizeof(PowerUpPacket)) {
    PowerUpPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handlePowerUp(mac, &pkt);
  } else {
    Serial.print("WARNING: ignored a packet of unexpected size (");
    Serial.print(len);
    Serial.println(" bytes)");
  }
}

// =====================================================================
// MAC / IDENTITY
// =====================================================================

int getTeamIDFromMAC(const uint8_t *mac) {
  for (int i = 0; i < MAX_TEAMS; i++) {
    if (memcmp(mac, participantMacs[i], 6) == 0) return i + 1;
  }
  return -1;  // unknown board
}

// =====================================================================
// REGISTRATION
// =====================================================================

void handleRegistration(const uint8_t *mac, RegistrationPacket *pkt) {
  int macTeam = getTeamIDFromMAC(mac);
  if (macTeam == -1) {
    Serial.println("Registration from an unrecognized MAC - ignored.");
    return;
  }

  uint8_t result = tryRegister(macTeam, pkt->team_id, pkt->team_name, pkt->ships,
                                roundState, registered, teamNames, storedShips, grid,
                                remainingShips, eliminated);

  switch (result) {
    case REG_OK:
      // A fresh (re)registration may have moved the ships, so any earlier
      // mine could now sit on a ship cell: the team places it again.
      powerState.mineX[macTeam - 1] = NO_CELL;
      powerState.mineY[macTeam - 1] = NO_CELL;
      powerState.used[macTeam - 1] &= (uint8_t)~(1u << (POWER_MINE - 1));
      addLog("TEAM %d (%s) registered", macTeam, teamNames[macTeam - 1]);
      sendRegAck(mac, REG_OK);
      broadcastTurnUpdate();
      saveGameState();
      break;

    case REG_RECONNECTED:
      addLog("TEAM %d reconnected", macTeam);
      sendRegAck(mac, REG_RECONNECTED);
      broadcastTurnUpdate();
      break;

    case REG_RECONNECT_REJECTED:
      addLog("TEAM %d reconnection rejected (config does not match)", macTeam);
      sendRegAck(mac, REG_RECONNECT_REJECTED);
      break;

    case REG_FAIL_MAC_MISMATCH:
      addLog("TEAM %d registration rejected (claimed a different team)", macTeam);
      sendRegAck(mac, REG_REJECTED);
      break;

    case REG_FAIL_BAD_NAME:
      addLog("TEAM %d registration rejected (bad team name)", macTeam);
      sendRegAck(mac, REG_REJECTED);
      break;

    case REG_FAIL_BAD_SHIPS:
      addLog("TEAM %d registration rejected (invalid ship layout)", macTeam);
      sendRegAck(mac, REG_REJECTED);
      break;

    case REG_FAIL_GAME_STARTED:
      addLog("TEAM %d tried to join after the game started - rejected", macTeam);
      sendRegAck(mac, REG_REJECTED);
      break;

    default:
      addLog("TEAM %d registration rejected", macTeam);
      sendRegAck(mac, REG_REJECTED);
      break;
  }
}

void sendRegAck(const uint8_t *mac, uint8_t status) {
  RegAckPacket ack;
  ack.status = status;
  esp_now_send(mac, (uint8_t *)&ack, sizeof(ack));
}

// =====================================================================
// ATTACKS
// =====================================================================

// One log line for one shot. What the PUBLIC log may say depends on
// smoke, shields and mines; the truth goes to the Serial Monitor only.
void logShot(int attacker, int target, int x, int y, uint8_t actual, uint8_t reported) {
  Serial.print("[organizer] TEAM "); Serial.print(attacker);
  Serial.print(" -> TEAM "); Serial.print(target);
  Serial.print(" at ("); Serial.print(x); Serial.print(","); Serial.print(y);
  Serial.print("): real result code "); Serial.println(actual);

  if (actual != reported) {   // smoke screen
    addLog("TEAM %d attacked TEAM %d - result hidden by SMOKE SCREEN", attacker, target);
    return;
  }
  switch (actual) {
    case RESULT_MISS:
      addLog("TEAM %d attacked TEAM %d at (%d,%d) - MISS", attacker, target, x, y);
      break;
    case RESULT_HIT:
      addLog("TEAM %d attacked TEAM %d at (%d,%d) - HIT", attacker, target, x, y);
      break;
    case RESULT_SUNK:
      addLog("TEAM %d attacked TEAM %d at (%d,%d) - HIT, TEAM %d ELIMINATED", attacker, target, x, y, target);
      break;
    case RESULT_MINE:
      addLog("TEAM %d attacked TEAM %d at (%d,%d) - MISS, it was a MINE! TEAM %d loses a turn",
             attacker, target, x, y, attacker);
      break;
    case RESULT_BLOCKED:   // no coordinates: they would reveal where the shield is
      addLog("TEAM %d attacked TEAM %d - BLOCKED by a SHIELD", attacker, target);
      break;
    default:
      break;
  }
}

void handleAttack(const uint8_t *mac, AttackPacket *pkt) {
  int macTeam = getTeamIDFromMAC(mac);
  if (macTeam == -1) {
    Serial.println("Attack from an unrecognized MAC - ignored.");
    return;
  }

  uint8_t previousRoundState = roundState;
  uint8_t reported = RESULT_INVALID;
  uint8_t result = tryAttack(macTeam, pkt->attacker_id, pkt->target_id, pkt->x, pkt->y,
                              roundState, currentTurnIndex, grid, remainingShips,
                              eliminated, registered, &powerState, &reported);

  int targetId = pkt->target_id, x = pkt->x, y = pkt->y;

  switch (result) {
    case RESULT_MISS:
    case RESULT_HIT:
    case RESULT_SUNK:
    case RESULT_MINE:
    case RESULT_BLOCKED:
      logShot(macTeam, targetId, x, y, result, reported);
      sendFeedback(mac, targetId, x, y, reported);   // reported, not result: smoke hides the truth
      break;

    case ATTACK_FAIL_ID_MISMATCH:
      addLog("TEAM %d attack rejected (claimed a different team)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_NOT_RUNNING:
      addLog("TEAM %d attack rejected (game is not running)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_WRONG_TURN:
      addLog("TEAM %d attack rejected (not their turn)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_ATTACKER_DEAD:
      addLog("TEAM %d attack rejected (they are eliminated)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_BAD_TARGET:
      addLog("TEAM %d attack rejected (bad target id %d)", macTeam, targetId);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_SELF_ATTACK:
      addLog("TEAM %d attack rejected (attacked themselves)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_TARGET_INVALID:
      addLog("TEAM %d attack rejected (target %d invalid/eliminated)", macTeam, targetId);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_BAD_COORDS:
      addLog("TEAM %d attack rejected (bad coordinates)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
    case ATTACK_FAIL_ALREADY_HIT:
      addLog("TEAM %d attack rejected (cell already attacked)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;

    default:
      addLog("TEAM %d attack rejected", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;
  }

  // Only valid attacks reach here.
  logGameOverIfJustEnded(previousRoundState);
  broadcastTurnUpdate();
  saveGameState();
}

void sendFeedback(const uint8_t *mac, uint8_t targetId, uint8_t x, uint8_t y, uint8_t result) {
  FeedbackPacket fb;
  fb.target_id = targetId;
  fb.x = x;
  fb.y = y;
  fb.result_code = result;
  esp_now_send(mac, (uint8_t *)&fb, sizeof(fb));
}

// =====================================================================
// POWER-UPS
// =====================================================================

const char *powerName(int power) {
  switch (power) {
    case POWER_SONAR:  return "SONAR PING";
    case POWER_SALVO:  return "SALVO";
    case POWER_MINE:   return "MINE";
    case POWER_REPAIR: return "REPAIR";
    case POWER_SMOKE:  return "SMOKE SCREEN";
    case POWER_SHIELD: return "SHIELD";
    case POWER_DOUBLE: return "DOUBLE ATTACK";
  }
  return "UNKNOWN POWER";
}

const char *powerFailText(uint8_t code) {
  switch (code) {
    case PWR_FAIL_ID_MISMATCH:       return "claimed a different team";
    case PWR_FAIL_BAD_POWER:         return "unknown power-up";
    case PWR_FAIL_NOT_RUNNING:       return "game is not running";
    case PWR_FAIL_WRONG_TURN:        return "not their turn";
    case PWR_FAIL_DEAD:              return "they are eliminated";
    case PWR_FAIL_ALREADY_USED:      return "already used";
    case PWR_FAIL_BUDGET:            return "no power-ups left in the budget";
    case PWR_FAIL_BAD_TARGET:        return "bad target";
    case PWR_FAIL_BAD_COORDS:        return "bad coordinates";
    case PWR_FAIL_ALREADY_HIT:       return "a cell was already attacked";
    case PWR_FAIL_BAD_SHAPE:         return "cells don't form a valid pattern";
    case PWR_FAIL_NOT_SETUP:         return "mines only before the game starts";
    case PWR_FAIL_MINE_ON_SHIP:      return "mine must be on a water cell";
    case PWR_FAIL_NOTHING_TO_REPAIR: return "that cell is not a hit";
    case PWR_FAIL_NOT_REGISTERED:    return "not registered";
  }
  return "rejected";
}

void handlePowerUp(const uint8_t *mac, PowerUpPacket *pkt) {
  int macTeam = getTeamIDFromMAC(mac);
  if (macTeam == -1) {
    Serial.println("Power-up from an unrecognized MAC - ignored.");
    return;
  }

  int xs[3] = { pkt->x1, pkt->x2, pkt->x3 };
  int ys[3] = { pkt->y1, pkt->y2, pkt->y3 };
  int target = pkt->target_id;
  int power = pkt->power;

  uint8_t previousRoundState = roundState;
  PowerOutcome out;
  uint8_t code = tryPowerUp(macTeam, pkt->team_id, power, target, xs, ys,
                            roundState, currentTurnIndex, grid, remainingShips,
                            eliminated, registered, powerState, out);

  if (code != PWR_OK && code != PWR_JAMMED) {
    addLog("TEAM %d %s rejected (%s)", macTeam, powerName(power), powerFailText(code));
    sendPowerResult(mac, out, pkt->target_id);   // out.status == PWR_STATUS_REJECTED
    return;                                      // nothing changed: no broadcast, no save
  }

  switch (power) {
    case POWER_MINE:
      // NOT in the public log - that would give the mine away.
      Serial.print("[organizer] TEAM "); Serial.print(macTeam);
      Serial.print(" armed a mine at ("); Serial.print(xs[0]); Serial.print(","); Serial.print(ys[0]); Serial.println(")");
      break;

    case POWER_SONAR:
      if (code == PWR_JAMMED) {
        addLog("TEAM %d's SONAR PING on TEAM %d was jammed by SMOKE", macTeam, target);
      } else {
        addLog("TEAM %d used SONAR PING on TEAM %d", macTeam, target);   // the count stays private
        Serial.print("[organizer] sonar result for TEAM "); Serial.print(macTeam);
        Serial.print(": "); Serial.print(out.count); Serial.println(" ship cell(s)");
      }
      break;

    case POWER_SALVO:
    case POWER_DOUBLE:
      addLog("TEAM %d launched %s at TEAM %d", macTeam, powerName(power), target);
      for (int i = 0; i < out.cells && i < 3; i++) {
        if (out.actual[i] == RESULT_INVALID) continue;   // not fired (target already sunk)
        logShot(macTeam, target, xs[i], ys[i], out.actual[i], out.reported[i]);
      }
      break;

    case POWER_REPAIR:
      addLog("TEAM %d REPAIRED a damaged cell", macTeam);
      break;

    case POWER_SHIELD:
      addLog("TEAM %d raised a SHIELD", macTeam);        // position stays secret
      Serial.print("[organizer] TEAM "); Serial.print(macTeam);
      Serial.print(" shield centred at ("); Serial.print(xs[0]); Serial.print(","); Serial.print(ys[0]); Serial.println(")");
      break;

    case POWER_SMOKE:
      addLog("TEAM %d deployed a SMOKE SCREEN", macTeam);
      break;
  }

  logGameOverIfJustEnded(previousRoundState);
  sendPowerResult(mac, out, pkt->target_id);
  if (power != POWER_MINE) broadcastTurnUpdate();   // a mine costs no turn
  saveGameState();
}

void sendPowerResult(const uint8_t *mac, const PowerOutcome &out, uint8_t targetId) {
  PowerResultPacket pr;
  pr.power = out.power;
  pr.status = out.status;
  pr.target_id = targetId;
  pr.count = out.count;
  for (int i = 0; i < 3; i++) pr.result[i] = out.reported[i];   // reported, never the true value
  esp_now_send(mac, (uint8_t *)&pr, sizeof(pr));
}

// =====================================================================
// TURN MANAGEMENT
// =====================================================================

void logGameOverIfJustEnded(uint8_t previousRoundState) {
  if (roundState != STATE_GAMEOVER || previousRoundState == STATE_GAMEOVER) return;

  int winner = -1;
  for (int i = 0; i < MAX_TEAMS; i++) {
    if (registered[i] && !eliminated[i]) { winner = i; break; }
  }
  if (winner != -1) addLog("GAME OVER - TEAM %d WINS", winner + 1);
  else addLog("GAME OVER - no teams remain");
}

void broadcastTurnUpdate() {
  TurnUpdatePacket tu;
  tu.current_turn = (roundState == STATE_RUNNING) ? (currentTurnIndex + 1) : 0;
  tu.round_state = roundState;
  for (int i = 0; i < MAX_TEAMS; i++) {
    esp_now_send(participantMacs[i], (uint8_t *)&tu, sizeof(tu));
  }
}

// =====================================================================
// STATE PERSISTENCE
// =====================================================================

void saveGameState() {
  SavedState s;
  memset(&s, 0, sizeof(s));
  s.magic = SAVE_MAGIC;
  s.version = SAVE_VERSION;
  s.roundState = roundState;
  s.currentTurnIndex = currentTurnIndex;

  for (int i = 0; i < MAX_TEAMS; i++) {
    s.registered[i] = registered[i] ? 1 : 0;
    s.eliminated[i] = eliminated[i] ? 1 : 0;
    s.remainingShips[i] = (uint8_t)remainingShips[i];
    memcpy(s.teamNames[i], teamNames[i], 20);
  }
  memcpy(s.grid, grid, sizeof(grid));
  memcpy(s.storedShips, storedShips, sizeof(storedShips));
  memcpy(&s.power, &powerState, sizeof(powerState));

  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putBytes("state", &s, sizeof(s));
  prefs.end();
}

bool loadGameState() {
  prefs.begin(PREFS_NAMESPACE, true);  // read-only
  size_t len = prefs.getBytesLength("state");

  if (len != sizeof(SavedState)) {
    prefs.end();
    return false;  // nothing saved yet, or saved by an older firmware (different size)
  }

  SavedState s;
  prefs.getBytes("state", &s, sizeof(s));
  prefs.end();

  if (s.magic != SAVE_MAGIC || s.version != SAVE_VERSION) {
    return false;
  }

  roundState = s.roundState;
  currentTurnIndex = s.currentTurnIndex;
  for (int i = 0; i < MAX_TEAMS; i++) {
    registered[i] = s.registered[i] != 0;
    eliminated[i] = s.eliminated[i] != 0;
    remainingShips[i] = s.remainingShips[i];
    memcpy(teamNames[i], s.teamNames[i], 20);
  }
  memcpy(grid, s.grid, sizeof(grid));
  memcpy(storedShips, s.storedShips, sizeof(storedShips));
  memcpy(&powerState, &s.power, sizeof(powerState));

  return true;
}

// =====================================================================
// ROUND / GAME CONTROL
// =====================================================================

void resetGame() {
  for (int i = 0; i < MAX_TEAMS; i++) {
    registered[i] = false;
    eliminated[i] = false;
    remainingShips[i] = 0;
    teamNames[i][0] = '\0';
    memset(grid[i], 0, sizeof(grid[i]));
    memset(storedShips[i], 0, sizeof(storedShips[i]));
  }
  resetPowerState(powerState);
  currentTurnIndex = 0;
  roundState = STATE_SETUP;
  logCount = 0;
  logHead = 0;

  addLog("Game reset - waiting for teams to register");   // dashboard.py keys on this exact text
  broadcastTurnUpdate();
  saveGameState();
}

void cmdStart() {
  if (roundState != STATE_READY) {
    Serial.println("Cannot start - not all 4 teams are registered yet.");
    return;
  }
  roundState = STATE_RUNNING;
  currentTurnIndex = 0;
  addLog("GAME STARTED");
  broadcastTurnUpdate();
  saveGameState();
  Serial.println("Game started. Current turn: TEAM 1");
}

void cmdForceStart() {
  int registeredCount = 0;
  for (int i = 0; i < MAX_TEAMS; i++) {
    if (registered[i]) registeredCount++;
  }

  if (!tryForceStart(roundState, currentTurnIndex, registered)) {
    if (roundState == STATE_RUNNING || roundState == STATE_GAMEOVER) {
      Serial.println("Cannot force-start - a game is already in progress.");
    } else {
      Serial.println("Cannot force-start - need at least 2 registered teams.");
    }
    return;
  }

  addLog("GAME FORCE-STARTED by organizer with %d team(s) registered", registeredCount);
  broadcastTurnUpdate();
  saveGameState();
  Serial.print("Game force-started. Current turn: TEAM ");
  Serial.println(currentTurnIndex + 1);
}

void cmdSkipTurn() {
  if (roundState != STATE_RUNNING) {
    Serial.println("Cannot skip - game is not running.");
    return;
  }
  uint8_t previousRoundState = roundState;
  addLog("Organizer manually skipped TEAM %d's turn", currentTurnIndex + 1);
  advanceTurnPS(roundState, currentTurnIndex, eliminated, registered, powerState);
  logGameOverIfJustEnded(previousRoundState);
  broadcastTurnUpdate();
  saveGameState();
  Serial.println("Turn skipped.");
}

// =====================================================================
// EVENT LOG / DASHBOARD JSON
// =====================================================================

void addLog(const char *fmt, ...) {
  char buf[LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, LOG_LINE_LEN, fmt, args);
  va_end(args);

  strncpy(eventLog[logHead], buf, LOG_LINE_LEN);
  eventLog[logHead][LOG_LINE_LEN - 1] = '\0';
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;

  // This is the line dashboard.py listens for.
  Serial.print("{\"type\":\"event\",\"text\":\"");
  printEscapedJSON(buf);
  Serial.println("\"}");
}

void printEscapedJSON(const char *s) {
  for (int i = 0; s[i] != '\0'; i++) {
    char c = s[i];
    if (c == '"' || c == '\\') Serial.print('\\');
    Serial.print(c);
  }
}

// Full state snapshot - sent in response to "GET_STATE".
// This is PUBLIC: it goes to the projector. So grids and HP go through
// displayCell()/displayRemaining() (smoke hides recent shots), mines are
// not included at all, the shield is only a boolean, and the mine's bit
// is removed from "used".
void sendStateJSON() {
  Serial.print("{\"type\":\"state\",\"round_state\":");
  Serial.print(roundState);
  Serial.print(",\"current_turn\":");
  Serial.print(roundState == STATE_RUNNING ? currentTurnIndex + 1 : 0);
  Serial.print(",\"teams\":[");

  const uint8_t publicUsedMask = (uint8_t)~(1u << (POWER_MINE - 1));

  for (int i = 0; i < MAX_TEAMS; i++) {
    if (i > 0) Serial.print(",");
    Serial.print("{\"id\":"); Serial.print(i + 1);
    Serial.print(",\"name\":\"");
    if (registered[i]) printEscapedJSON(teamNames[i]);
    Serial.print("\"");
    Serial.print(",\"registered\":"); Serial.print(registered[i] ? "true" : "false");
    Serial.print(",\"eliminated\":"); Serial.print(eliminated[i] ? "true" : "false");
    Serial.print(",\"remaining\":");
    Serial.print(displayRemaining(powerState, i, remainingShips[i], grid[i]));
    Serial.print(",\"shield\":"); Serial.print(powerState.shieldOn[i] ? "true" : "false");
    Serial.print(",\"smoke\":"); Serial.print(powerState.smokeOn[i] ? "true" : "false");
    Serial.print(",\"used\":"); Serial.print(powerState.used[i] & publicUsedMask);
    Serial.print(",\"grid\":[");
    for (int r = 0; r < GRID_SIZE; r++) {
      Serial.print("[");
      for (int c = 0; c < GRID_SIZE; c++) {
        Serial.print(displayCell(powerState, i, r, c, grid[i][r][c]));
        if (c < GRID_SIZE - 1) Serial.print(",");
      }
      Serial.print("]");
      if (r < GRID_SIZE - 1) Serial.print(",");
    }
    Serial.print("]}");
  }

  Serial.print("],\"log\":[");
  for (int i = 0; i < logCount; i++) {
    int idx = (logHead - logCount + i + LOG_SIZE) % LOG_SIZE;
    Serial.print("\"");
    printEscapedJSON(eventLog[idx]);
    Serial.print("\"");
    if (i < logCount - 1) Serial.print(",");
  }
  Serial.println("]}");
}

// =====================================================================
// SERIAL COMMANDS (organizer-facing)
// =====================================================================

void handleSerialInput() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      if (serialBuffer.length() > 100) serialBuffer = "";  // safety against garbage input
    }
  }
}

void processSerialCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "STATUS") printStatus();
  else if (cmd == "GRID") printGrids();
  else if (cmd == "START") cmdStart();
  else if (cmd == "FORCE_START") cmdForceStart();
  else if (cmd == "RESET") resetGame();
  else if (cmd == "SKIP_TURN") cmdSkipTurn();
  else if (cmd == "GET_STATE") sendStateJSON();
  else if (cmd == "HELP") printHelp();
  else {
    Serial.print("Unknown command: ");
    Serial.println(cmd);
    printHelp();
  }
}

const char *roundStateName(uint8_t s) {
  switch (s) {
    case STATE_SETUP: return "SETUP";
    case STATE_READY: return "READY";
    case STATE_RUNNING: return "RUNNING";
    case STATE_GAMEOVER: return "GAME OVER";
  }
  return "UNKNOWN";
}

void printStatus() {
  Serial.println();
  Serial.println("=== STATUS ===");
  Serial.print("GAME STATE: "); Serial.println(roundStateName(roundState));
  if (roundState == STATE_RUNNING) {
    Serial.print("CURRENT TURN: TEAM "); Serial.println(currentTurnIndex + 1);
  }
  for (int i = 0; i < MAX_TEAMS; i++) {
    Serial.print("TEAM "); Serial.print(i + 1); Serial.print(": ");
    Serial.print(registered[i] ? "REGISTERED" : "NOT REGISTERED");
    if (registered[i]) {
      Serial.print(" / ");
      Serial.print(eliminated[i] ? "ELIMINATED" : "ALIVE");
      Serial.print(" / ships remaining: ");
      Serial.print(remainingShips[i]);
      Serial.print(" / power-ups used: ");
      Serial.print(powerCount(powerState.used[i]));
      if (powerState.mineX[i] != NO_CELL) {
        Serial.print(" / mine at ("); Serial.print(powerState.mineX[i]);
        Serial.print(","); Serial.print(powerState.mineY[i]); Serial.print(")");
      }
      if (powerState.shieldOn[i]) Serial.print(" / SHIELD up");
      if (powerState.smokeOn[i]) Serial.print(" / SMOKE up");
      if (powerState.skipNext[i]) Serial.print(" / loses next turn");
    }
    Serial.println();
  }
  Serial.println("==============");
}

// Organizer view of the REAL grids. S = ship, M = miss, H = hit,
// m = a hidden mine, + = under the shield, . = water.
void printGrids() {
  for (int i = 0; i < MAX_TEAMS; i++) {
    Serial.print("TEAM "); Serial.print(i + 1);
    Serial.println(registered[i] ? (eliminated[i] ? " (ELIMINATED)" : " (ALIVE)") : " (not registered)");
    for (int r = 0; r < GRID_SIZE; r++) {
      for (int c = 0; c < GRID_SIZE; c++) {
        char ch = '.';
        switch (grid[i][r][c]) {
          case CELL_WATER: ch = '.'; break;
          case CELL_SHIP:  ch = 'S'; break;
          case CELL_MISS:  ch = 'M'; break;
          case CELL_HIT:   ch = 'H'; break;
        }
        if (ch == '.' && powerState.mineX[i] == c && powerState.mineY[i] == r) ch = 'm';
        else if (ch == '.' && shieldCovers(powerState, i, c, r)) ch = '+';
        Serial.print(ch);
        Serial.print(' ');
      }
      Serial.println();
    }
    Serial.println();
  }
}

void printHelp() {
  Serial.println("Commands: STATUS, GRID, START, FORCE_START, RESET, SKIP_TURN, GET_STATE, HELP");
  Serial.println("  FORCE_START: start with fewer than 4 teams registered (needs at least 2)");
  Serial.println("  GRID shows the REAL boards incl. hidden mines (m) and shields (+) - organizer eyes only");
}
