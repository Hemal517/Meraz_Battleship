/* =====================================================================
   MERAZ BATTLESHIP - CENTRAL NODE
   =====================================================================
   This ESP32 is the ONLY authority for the game. It owns all four
   grids, all turn logic, all elimination logic, and all round state.
   The laptop (dashboard.py) is just a screen - it never decides
   anything, it only displays what this board tells it.

   TWO DESIGN DECISIONS THAT DIFFER FROM THE ORIGINAL SPEC WRITE-UP
   (both discussed with the organizer before writing this file):

   1) REGISTRATION PACKET FORMAT
      The original idea was to send the whole 5x5 ship grid over
      ESP-NOW. The problem: since ships are ALLOWED to touch each
      other (see spec), a raw grid of 9 filled cells can be ambiguous
      - there can be more than one way (or no valid way) to split a
      blob of touching cells back into "one size-1, one size-3, one
      size-5" ship. That makes validation unreliable.
      FIX: participants send 3 explicit ship placements instead
      (length, start X, start Y, orientation). Central builds the
      grid itself from that - no guessing required, and the config
      on the participant side is arguably easier to get right too
      (see participant_node.ino).

   2) PACKET TYPE DETECTION
      Instead of adding a "packet type" byte to every struct, we
      tell packets apart by their BYTE LENGTH, since:
        - Central only ever RECEIVES RegistrationPacket (33 bytes)
          or AttackPacket (4 bytes) from participants - never sends
          those, only receives them.
        - Participants only ever RECEIVE RegAckPacket (1 byte),
          FeedbackPacket (4 bytes) or TurnUpdatePacket (2 bytes) from
          Central - never send those, only receive them.
      Every packet a given board can receive has a different length,
      so length alone is enough. This keeps every struct exactly as
      simple as the spec asked for, with no extra header byte.

   Everything else follows the spec as written, including the two
   organizer-requested extras: broadcasting turn updates to all four
   boards after every move, and a SKIP_TURN serial command as a
   manual override if a board goes offline mid-turn.

   3) POWER-LOSS PROTECTION (added while waiting on hardware)
      The game state used to live only in RAM, so a Central power
      blip mid-round would silently wipe the whole match. Now every
      action that changes the state (registration, attack, start,
      skip, reset) also saves a snapshot to the ESP32's flash via the
      Preferences library, and setup() tries to load that snapshot
      before falling back to a fresh reset. Participants don't need
      to do anything different - they already retry registration
      until acknowledged, so once Central reboots and comes back with
      a matching stored ship layout, they just reconnect normally.
      See the "STATE PERSISTENCE" section below.

   Target: ESP32 Arduino core 2.x and 3.x. The ESP-NOW receive callback
   signature actually changed between them (core 3.x added the
   esp_now_recv_info_t wrapper; core 2.x just passes the sender's MAC
   directly), so OnDataRecv() below is compiled differently for each
   using ESP_IDF_VERSION_MAJOR - both versions are handled, you don't
   need to know or care which one is installed.
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
// See the setup instructions for how to read a board's MAC address.
// Order matters: index 0 = Team 1, index 1 = Team 2, etc.
uint8_t participantMacs[4][6] = {
  { 0xA4, 0xCF, 0x12, 0x00, 0x00, 0x01 },  // TEAM 1 - CHANGE ME
  { 0xA4, 0xCF, 0x12, 0x00, 0x00, 0x02 },  // TEAM 2 - CHANGE ME
  { 0xA4, 0xCF, 0x12, 0x00, 0x00, 0x03 },  // TEAM 3 - CHANGE ME
  { 0xA4, 0xCF, 0x12, 0x00, 0x00, 0x04 },  // TEAM 4 - CHANGE ME
};

// =====================================================================
// CONSTANTS SPECIFIC TO THIS FILE
// =====================================================================
// MAX_TEAMS, GRID_SIZE, the cell/result/reg/round-state codes, and the
// ShipPlacement struct all now live in game_logic.h (included above),
// since central_node.ino and the desktop test harness both need them.

#define MAX_PACKET_SIZE    64   // bigger than our largest packet (33 bytes)
#define PENDING_QUEUE_SIZE 4    // small buffer between the ESP-NOW callback and loop()
#define LOG_SIZE           12   // how many recent events we keep for the dashboard
#define LOG_LINE_LEN       80

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
  uint8_t result_code;   // RESULT_* code
} FeedbackPacket;        // 4 bytes

typedef struct __attribute__((packed)) {
  uint8_t current_turn;  // 1-4, or 0 if no team currently has the turn
  uint8_t round_state;   // STATE_* code
} TurnUpdatePacket;      // 2 bytes

// =====================================================================
// GAME STATE (this is the "database" - it all lives in RAM)
// =====================================================================

uint8_t grid[MAX_TEAMS][GRID_SIZE][GRID_SIZE];   // live grid per team
ShipPlacement storedShips[MAX_TEAMS][3];         // original layout, for reconnection checks
char teamNames[MAX_TEAMS][20];
bool registered[MAX_TEAMS];
bool eliminated[MAX_TEAMS];
int remainingShips[MAX_TEAMS];

uint8_t currentTurnIndex = 0;   // 0-3 internally (team_id = index + 1)
uint8_t roundState = STATE_SETUP;

// Recent-events log, shared with the dashboard
char eventLog[LOG_SIZE][LOG_LINE_LEN];
int logHead = 0;
int logCount = 0;

// Small queue so the ESP-NOW callback stays fast: it just copies bytes
// in here, and loop() does the real work.
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
// One struct holding everything needed to pick a round back up,
// saved to flash as a single blob. magic+version let us tell "a real
// save from this firmware" apart from empty/garbage flash - if either
// doesn't match, we treat it as no save at all and start fresh.

Preferences prefs;

#define PREFS_NAMESPACE "battleship"
#define SAVE_MAGIC       0xBA77CAFE
#define SAVE_VERSION     1  // bump this if SavedState's layout ever changes

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
} SavedState;

// =====================================================================
// FUNCTION DECLARATIONS
// =====================================================================

// ESP-NOW's receive callback signature differs between core versions -
// core 3.x (IDF5+) wraps the sender info in esp_now_recv_info_t, core
// 2.x (IDF4) just passes the MAC address directly. This handles both.
#if ESP_IDF_VERSION_MAJOR >= 5
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len);
#else
void OnDataRecv(const uint8_t *senderMac, const uint8_t *incomingData, int len);
#endif
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void processPacket(const uint8_t *mac, const uint8_t *data, int len);

int  getTeamIDFromMAC(const uint8_t *mac);

void handleRegistration(const uint8_t *mac, RegistrationPacket *pkt);
void handleAttack(const uint8_t *mac, AttackPacket *pkt);
void sendRegAck(const uint8_t *mac, uint8_t status);
void sendFeedback(const uint8_t *mac, uint8_t targetId, uint8_t x, uint8_t y, uint8_t result);
void broadcastTurnUpdate();
void resetGame();

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
  // Drain packets that arrived via ESP-NOW since the last loop().
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
  if (next == pendingHead) return;  // queue full (shouldn't happen at human pace) - drop

  memcpy(pendingQueue[pendingTail].mac, senderMac, 6);
  memcpy(pendingQueue[pendingTail].data, incomingData, len);
  pendingQueue[pendingTail].len = len;
  pendingTail = next;
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
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
// The actual rules (ship validation, MAC/team-id matching, reconnect
// matching) live in game_logic.h's tryRegister() now. This function's
// job is just: resolve the sender's MAC, call tryRegister(), and turn
// the result into the right RegAckPacket + log line + broadcast/save.

void handleRegistration(const uint8_t *mac, RegistrationPacket *pkt) {
  int macTeam = getTeamIDFromMAC(mac);
  if (macTeam == -1) {
    Serial.println("Registration from an unrecognized MAC - ignored.");
    return;  // we never added this MAC as a peer, so we can't reply anyway
  }

  uint8_t result = tryRegister(macTeam, pkt->team_id, pkt->team_name, pkt->ships,
                                roundState, registered, teamNames, storedShips, grid,
                                remainingShips, eliminated);

  switch (result) {
    case REG_OK:
      addLog("TEAM %d (%s) registered", macTeam, teamNames[macTeam - 1]);
      sendRegAck(mac, REG_OK);
      broadcastTurnUpdate();
      saveGameState();
      break;

    case REG_RECONNECTED:
      addLog("TEAM %d reconnected", macTeam);
      sendRegAck(mac, REG_RECONNECTED);
      broadcastTurnUpdate();  // let them know whose turn it is right now
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

void handleAttack(const uint8_t *mac, AttackPacket *pkt) {
  int macTeam = getTeamIDFromMAC(mac);
  if (macTeam == -1) {
    Serial.println("Attack from an unrecognized MAC - ignored.");
    return;
  }

  uint8_t previousRoundState = roundState;
  uint8_t result = tryAttack(macTeam, pkt->attacker_id, pkt->target_id, pkt->x, pkt->y,
                              roundState, currentTurnIndex, grid, remainingShips,
                              eliminated, registered);

  int targetId = pkt->target_id, x = pkt->x, y = pkt->y;

  switch (result) {
    case RESULT_MISS:
      addLog("TEAM %d attacked TEAM %d at (%d,%d) - MISS", macTeam, targetId, x, y);
      sendFeedback(mac, targetId, x, y, RESULT_MISS);
      break;
    case RESULT_HIT:
      addLog("TEAM %d attacked TEAM %d at (%d,%d) - HIT", macTeam, targetId, x, y);
      sendFeedback(mac, targetId, x, y, RESULT_HIT);
      break;
    case RESULT_SUNK:
      addLog("TEAM %d attacked TEAM %d at (%d,%d) - HIT, TEAM %d ELIMINATED",
             macTeam, targetId, x, y, targetId);
      sendFeedback(mac, targetId, x, y, RESULT_SUNK);
      break;

    case ATTACK_FAIL_ID_MISMATCH:
      addLog("TEAM %d attack rejected (claimed a different team)", macTeam);
      sendFeedback(mac, targetId, x, y, RESULT_INVALID);
      return;  // nothing changed - no need to broadcast/save
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

  // Only valid attacks (MISS/HIT/SUNK) reach here.
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
// TURN MANAGEMENT
// =====================================================================
// advanceTurn() itself now lives in game_logic.h (tryAttack() and
// cmdSkipTurn() both call it there). This just announces a winner on
// the Serial log / event log right after it happens, since the shared
// version has no logging of its own.

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
// STATE PERSISTENCE (see the note near the SavedState struct above)
// =====================================================================

void saveGameState() {
  SavedState s;
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

  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putBytes("state", &s, sizeof(s));
  prefs.end();
}

// Returns true if a valid saved state was found and restored into the
// live game-state globals. Returns false (and leaves globals alone)
// if there was nothing to restore, so the caller can fall back to
// resetGame().
bool loadGameState() {
  prefs.begin(PREFS_NAMESPACE, true);  // read-only
  size_t len = prefs.getBytesLength("state");

  if (len != sizeof(SavedState)) {
    prefs.end();
    return false;  // nothing saved yet, or it's the wrong size to trust
  }

  SavedState s;
  prefs.getBytes("state", &s, sizeof(s));
  prefs.end();

  if (s.magic != SAVE_MAGIC || s.version != SAVE_VERSION) {
    return false;  // not our data, or saved by an older firmware version
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
  currentTurnIndex = 0;
  roundState = STATE_SETUP;
  logCount = 0;
  logHead = 0;

  addLog("Game reset - waiting for teams to register");
  broadcastTurnUpdate();
  saveGameState();
}

void cmdStart() {
  if (roundState != STATE_READY) {
    Serial.println("Cannot start - not all 4 teams are registered yet.");
    return;
  }
  roundState = STATE_RUNNING;
  currentTurnIndex = 0;  // Team 1 always goes first (READY guarantees all 4 are in)
  addLog("GAME STARTED");
  broadcastTurnUpdate();
  saveGameState();
  Serial.println("Game started. Current turn: TEAM 1");
}

// Lets the organizer start with fewer than all 4 teams registered -
// e.g. one team never shows up. Needs at least 2 registered teams;
// the first registered team goes first.
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
  // Organizer safety valve: ESP-NOW has no built-in disconnect
  // detection, so if the team whose turn it is has gone offline,
  // the organizer can manually force the turn forward.
  if (roundState != STATE_RUNNING) {
    Serial.println("Cannot skip - game is not running.");
    return;
  }
  uint8_t previousRoundState = roundState;
  addLog("Organizer manually skipped TEAM %d's turn", currentTurnIndex + 1);
  advanceTurn(roundState, currentTurnIndex, eliminated, registered);
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

// Full state snapshot - sent in response to "GET_STATE" so the
// dashboard can rebuild the screen after a restart or refresh.
void sendStateJSON() {
  Serial.print("{\"type\":\"state\",\"round_state\":");
  Serial.print(roundState);
  Serial.print(",\"current_turn\":");
  Serial.print(roundState == STATE_RUNNING ? currentTurnIndex + 1 : 0);
  Serial.print(",\"teams\":[");

  for (int i = 0; i < MAX_TEAMS; i++) {
    if (i > 0) Serial.print(",");
    Serial.print("{\"id\":"); Serial.print(i + 1);
    Serial.print(",\"name\":\"");
    if (registered[i]) printEscapedJSON(teamNames[i]);
    Serial.print("\"");
    Serial.print(",\"registered\":"); Serial.print(registered[i] ? "true" : "false");
    Serial.print(",\"eliminated\":"); Serial.print(eliminated[i] ? "true" : "false");
    Serial.print(",\"remaining\":"); Serial.print(remainingShips[i]);
    Serial.print(",\"grid\":[");
    for (int r = 0; r < GRID_SIZE; r++) {
      Serial.print("[");
      for (int c = 0; c < GRID_SIZE; c++) {
        Serial.print(grid[i][r][c]);
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
// dashboard.py only ever sends: GET_STATE, START, RESET
// A human at the Serial Monitor can also use: STATUS, GRID, SKIP_TURN, HELP

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
    }
    Serial.println();
  }
  Serial.println("==============");
}

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
}
