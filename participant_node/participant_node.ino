/* =====================================================================
   MERAZ BATTLESHIP - PARTICIPANT NODE
   =====================================================================
   This is the SAME file uploaded to all 4 team ESP32s. The only thing
   that changes between boards is the CONFIGURATION block just below -
   team ID, team name, ship placement, and the Central's MAC address.

   What this board does:
     1. Registers itself with the Central over ESP-NOW.
     2. Listens for turn updates so it knows when it's this team's turn.
     3. Lets the player pick a target team + coordinate and fires an
        AttackPacket at the Central.
     4. Shows the result (MISS / HIT / SUNK / INVALID) it gets back.

   This board NEVER decides whether an attack is valid - it just asks
   the Central and shows whatever answer comes back. All the real game
   logic lives in central_node.ino.

   The exact TFT model hasn't been picked yet, so the code is split
   into three clearly separate layers:
     - GAME LOGIC        (registration, turn tracking, attack requests)
     - ESP-NOW COMMS      (sending/receiving packets)
     - UI LAYER            (currently Serial-only "stub" functions -
                            see the "UI LAYER" section near the bottom)
   Once a TFT is chosen, only the UI LAYER functions need to be
   rewritten - nothing above them needs to change.

   For testing before any TFT is wired up, type these into the Serial
   Monitor (115200 baud):
     CONFIG          - show this board's configuration
     STATUS          - show registration/turn status
     HELP            - list commands
     2 3 4           - manually send an attack: target team 2, x=3, y=4
   ===================================================================== */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <string.h>
#include <stdio.h>
#include <SPI.h>
#include <TFT_eSPI.h>   // library: bollworm/Bodmer's TFT_eSPI - see the setup note below

// =====================================================================
// A couple of definitions the CONFIGURATION section below needs.
// You shouldn't need to touch these.
// =====================================================================

#define ORIENT_HORIZONTAL 0   // ship grows toward +x (rightward)
#define ORIENT_VERTICAL   1   // ship grows toward +y (downward)

// One ship: how long it is, where it starts, and which way it points.
typedef struct __attribute__((packed)) {
  uint8_t ship_len;     // 1, 3, or 5
  uint8_t start_x;      // column, 0-4
  uint8_t start_y;      // row, 0-4
  uint8_t orientation;  // ORIENT_HORIZONTAL or ORIENT_VERTICAL
} ShipPlacement;

// =====================================================================
// CONFIGURATION - CHANGE THESE FOR EACH TEAM'S BOARD
// =====================================================================
// This is the ONLY section that should differ between the 4 boards.

#define MY_TEAM_ID 1
const char *MY_TEAM_NAME = "TEAM 1";

// Your 3 ships. Grid is 5x5, x = column (0-4), y = row (0-4).
// Rules: exactly one ship of length 1, one of length 3, one of length
// 5; straight lines only; must fit inside the grid; ships must NOT
// overlap each other (touching is fine).
//
// Example below (already valid, feel free to leave as-is for testing):
//   Ship 1 (size 1): single cell at (2,4)
//   Ship 2 (size 3): horizontal, starts at (0,0) -> covers (0,0)(1,0)(2,0)
//   Ship 3 (size 5): vertical,   starts at (4,0) -> covers (4,0)..(4,4)
ShipPlacement myShips[3] = {
  { 1, 2, 4, ORIENT_HORIZONTAL },
  { 3, 0, 0, ORIENT_HORIZONTAL },
  { 5, 4, 0, ORIENT_VERTICAL   },
};

// TODO: replace with the Central ESP32's real MAC address.
// The Central prints its own MAC on boot - copy it from there.
uint8_t centralMac[6] = {
  0xA0, 0xC8, 0x40, 0x88, 0x00, 0x8C
};

// All 5 boards (Central + 4 participants) must use the SAME channel.
#define ESPNOW_CHANNEL 1

// =====================================================================
// TFT DISPLAY CONFIGURATION - 2.4" SPI touch TFT (ILI9341 + XPT2046)
// =====================================================================
// ONE-TIME LIBRARY SETUP (not per-sketch): TFT_eSPI needs its pin
// wiring set in the LIBRARY's own User_Setup.h, not here. Find
// TFT_eSPI/User_Setup.h in your Arduino libraries folder and paste
// this in (replacing whatever driver/pin lines are already there):
//
//   #define ILI9341_DRIVER
//   #define TFT_CS   5
//   #define TFT_DC   2
//   #define TFT_RST  4
//   #define TFT_MOSI 23
//   #define TFT_SCLK 18
//   #define TFT_MISO 19
//   #define TOUCH_CS 15
//   #define SPI_FREQUENCY        40000000
//   #define SPI_TOUCH_FREQUENCY  2500000
//
// This matches the wiring table already worked out for this project;
// rewire (and edit these lines to match) if you used different pins.
//
// PER-BOARD SETUP (do this once per physical board - touch panels are
// calibrated individually): run the TFT_eSPI example sketch at
// File > Examples > TFT_eSPI > Generic > Touch_calibrate on THIS
// board, then paste the 5 numbers it prints below.
uint16_t touchCalData[5] = { 300, 3600, 300, 3600, 7 };  // PLACEHOLDER - replace per board

TFT_eSPI tft = TFT_eSPI();
uint8_t opponentIds[3];  // the 3 team IDs that aren't MY_TEAM_ID, filled in by uiInit()

// =====================================================================
// EVERYTHING BELOW THIS LINE IS THE SAME FOR ALL 4 TEAMS
// =====================================================================

#define MAX_TEAMS  4
#define GRID_SIZE  5

// FeedbackPacket.result_code (must match central_node.ino)
#define RESULT_MISS     0
#define RESULT_HIT      1
#define RESULT_INVALID  2
#define RESULT_SUNK     3

// RegAckPacket.status (must match central_node.ino)
#define REG_OK                   0
#define REG_REJECTED             1
#define REG_RECONNECTED          2
#define REG_RECONNECT_REJECTED   3

// round_state (must match central_node.ino)
#define STATE_SETUP     0
#define STATE_READY     1
#define STATE_RUNNING   2
#define STATE_GAMEOVER  3

#define MAX_PACKET_SIZE     64
#define PENDING_QUEUE_SIZE  4
#define REG_RETRY_INTERVAL_MS 2000  // resend registration this often until acknowledged

// =====================================================================
// PACKET STRUCTURES (must match central_node.ino exactly, byte for byte)
// =====================================================================

typedef struct __attribute__((packed)) {
  uint8_t team_id;
  char team_name[20];
  ShipPlacement ships[3];
} RegistrationPacket;   // 33 bytes - we send this

typedef struct __attribute__((packed)) {
  uint8_t status;        // REG_* code
} RegAckPacket;          // 1 byte - Central sends this back

typedef struct __attribute__((packed)) {
  uint8_t attacker_id;
  uint8_t target_id;
  uint8_t x;
  uint8_t y;
} AttackPacket;          // 4 bytes - we send this

typedef struct __attribute__((packed)) {
  uint8_t target_id;
  uint8_t x;
  uint8_t y;
  uint8_t result_code;   // RESULT_* code
} FeedbackPacket;        // 4 bytes - Central sends this back

typedef struct __attribute__((packed)) {
  uint8_t current_turn;  // 1-4, or 0 if no team currently has the turn
  uint8_t round_state;   // STATE_* code
} TurnUpdatePacket;      // 2 bytes - Central broadcasts this to everyone

// =====================================================================
// GAME STATE (what THIS board currently believes about the match)
// =====================================================================

bool registrationConfirmed = false;   // true once Central sends REG_OK / REG_RECONNECTED
uint8_t currentTurnTeam = 0;          // 0 = unknown / no active turn
uint8_t roundState = STATE_SETUP;     // mirrors Central's round_state
unsigned long lastRegAttemptMillis = 0;

String serialBuffer = "";

// Small queue so the ESP-NOW callback stays short - it just copies
// bytes in here, and loop() does the actual work.
typedef struct {
  uint8_t data[MAX_PACKET_SIZE];
  int len;
} PendingPacket;

PendingPacket pendingQueue[PENDING_QUEUE_SIZE];
volatile int pendingHead = 0;
volatile int pendingTail = 0;

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
#if ESP_IDF_VERSION_MAJOR >= 5
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status);
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
#endif
void processPacket(const uint8_t *data, int len);

bool localValidateShips(ShipPlacement ships[3]);
void sendRegistration();
void sendAttack(uint8_t targetTeam, uint8_t x, uint8_t y);
void attemptManualAttack(uint8_t targetTeam, uint8_t x, uint8_t y);

void handleRegAck(RegAckPacket *pkt);
void handleFeedback(FeedbackPacket *pkt);
void handleTurnUpdate(TurnUpdatePacket *pkt);

void handleSerialInput();
void processSerialCommand(String line);
void printConfig();
void printStatus();
void printHelp();
const char *roundStateName(uint8_t s);

void uiInit();
void uiShowMessage(const char *msg);
void uiShowYourTurn();
void uiShowWaiting(uint8_t currentTeam);
void uiShowAttackResult(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result);
void uiTick();
void uiPollTouch();
void uiDrawYourTurn();
void uiDrawWaiting(uint8_t currentTeam);
void uiDrawMessage(const char *msg);
void uiDrawResultBanner(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result);
void drawTargetButton(int i, bool selected);
void drawGrid();
void drawAttackButton(bool enabled);
void computeOpponentIds();

// =====================================================================
// SETUP / LOOP
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.print("=== MERAZ BATTLESHIP - ");
  Serial.print(MY_TEAM_NAME);
  Serial.println(" ===");

  // Catch obviously-wrong ship configs locally before even trying to
  // register - saves a round trip and gives a clearer error message.
  if (!localValidateShips(myShips)) {
    Serial.println("WARNING: your ship configuration looks INVALID");
    Serial.println("(overlap, out of bounds, or wrong sizes). Fix myShips[] above.");
  } else {
    Serial.println("Local ship configuration check: OK");
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.print("My MAC address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("(Central needs this MAC in its participantMacs[] config)");

  if (esp_now_init() != ESP_OK) {
    Serial.println("FATAL: esp_now_init() failed. Check the board and reset.");
    while (true) { delay(1000); }
  }

  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, centralMac, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("WARNING: could not add Central as an ESP-NOW peer.");
  }

  uiInit();
  printHelp();

  sendRegistration();
  lastRegAttemptMillis = millis();
}

void loop() {
  // Drain packets that arrived via ESP-NOW since the last loop().
  while (pendingHead != pendingTail) {
    PendingPacket p = pendingQueue[pendingHead];
    pendingHead = (pendingHead + 1) % PENDING_QUEUE_SIZE;
    processPacket(p.data, p.len);
  }

  // Keep re-sending registration (non-blocking) until Central confirms it.
  // Handles the case where our very first registration packet gets lost.
  if (!registrationConfirmed && millis() - lastRegAttemptMillis > REG_RETRY_INTERVAL_MS) {
    sendRegistration();
    lastRegAttemptMillis = millis();
  }

  handleSerialInput();
  uiTick();
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
  // Only trust packets that actually came from the Central.
  if (memcmp(senderMac, centralMac, 6) != 0) return;
  if (len <= 0 || len > MAX_PACKET_SIZE) return;

  int next = (pendingTail + 1) % PENDING_QUEUE_SIZE;
  if (next == pendingHead) return;  // queue full (shouldn't happen) - drop

  memcpy(pendingQueue[pendingTail].data, incomingData, len);
  pendingQueue[pendingTail].len = len;
  pendingTail = next;
}

#if ESP_IDF_VERSION_MAJOR >= 5

void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("WARNING: an ESP-NOW send to Central failed.");
  }
}

#else

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("WARNING: an ESP-NOW send to Central failed.");
  }
}

#endif

// =====================================================================
// PACKET ROUTING - Central only ever sends us RegAck (1 byte),
// Feedback (4 bytes) or TurnUpdate (2 bytes), and those three sizes
// are all different, so length alone tells us what we received.
// (See the matching note in central_node.ino for the full reasoning.)
// =====================================================================

void processPacket(const uint8_t *data, int len) {
  if (len == sizeof(RegAckPacket)) {
    RegAckPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleRegAck(&pkt);
  } else if (len == sizeof(FeedbackPacket)) {
    FeedbackPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleFeedback(&pkt);
  } else if (len == sizeof(TurnUpdatePacket)) {
    TurnUpdatePacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handleTurnUpdate(&pkt);
  } else {
    Serial.print("WARNING: ignored a packet of unexpected size (");
    Serial.print(len);
    Serial.println(" bytes)");
  }
}

// =====================================================================
// LOCAL SHIP VALIDATION (a quick sanity check, mirrors the Central's
// real check so config mistakes show up immediately on this board
// instead of only after Central rejects the registration)
// =====================================================================

bool localValidateShips(ShipPlacement ships[3]) {
  uint8_t tempGrid[GRID_SIZE][GRID_SIZE];
  for (int r = 0; r < GRID_SIZE; r++)
    for (int c = 0; c < GRID_SIZE; c++)
      tempGrid[r][c] = 0;

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
      if (tempGrid[y][x] != 0) return false;
      tempGrid[y][x] = 1;
    }
  }

  return sawLen1 && sawLen3 && sawLen5;
}

// =====================================================================
// REGISTRATION
// =====================================================================

void sendRegistration() {
  RegistrationPacket pkt;
  pkt.team_id = MY_TEAM_ID;
  strncpy(pkt.team_name, MY_TEAM_NAME, 20);
  memcpy(pkt.ships, myShips, sizeof(myShips));

  esp_now_send(centralMac, (uint8_t *)&pkt, sizeof(pkt));
  Serial.println("Sent registration to Central...");
}

void handleRegAck(RegAckPacket *pkt) {
  switch (pkt->status) {
    case REG_OK:
      registrationConfirmed = true;
      Serial.println("Registration ACCEPTED by Central.");
      uiShowMessage("Registered!");
      break;
    case REG_RECONNECTED:
      registrationConfirmed = true;
      Serial.println("Reconnected - Central restored our existing game state.");
      uiShowMessage("Reconnected!");
      break;
    case REG_REJECTED:
      registrationConfirmed = false;
      Serial.println("Registration REJECTED. Check MY_TEAM_ID / myShips[] above.");
      uiShowMessage("Registration REJECTED");
      break;
    case REG_RECONNECT_REJECTED:
      registrationConfirmed = false;
      Serial.println("Reconnect REJECTED - our config doesn't match what Central has stored.");
      uiShowMessage("Reconnect REJECTED");
      break;
    default:
      Serial.println("Got an unknown registration status from Central.");
      break;
  }
}

// =====================================================================
// ATTACKING
// =====================================================================

void sendAttack(uint8_t targetTeam, uint8_t x, uint8_t y) {
  AttackPacket pkt;
  pkt.attacker_id = MY_TEAM_ID;
  pkt.target_id = targetTeam;
  pkt.x = x;
  pkt.y = y;
  esp_now_send(centralMac, (uint8_t *)&pkt, sizeof(pkt));
}

// Client-side sanity checks only, to avoid pointless sends and give
// fast feedback. Central re-checks everything anyway - it never
// trusts this board's judgement about what's a legal attack.
void attemptManualAttack(uint8_t targetTeam, uint8_t x, uint8_t y) {
  if (!registrationConfirmed) {
    Serial.println("Cannot attack - not registered with Central yet.");
    return;
  }
  if (roundState != STATE_RUNNING) {
    Serial.println("Cannot attack - the game is not running.");
    return;
  }
  if (currentTurnTeam != MY_TEAM_ID) {
    Serial.println("Cannot attack - it is not your turn.");
    return;
  }
  if (targetTeam < 1 || targetTeam > MAX_TEAMS || targetTeam == MY_TEAM_ID) {
    Serial.println("Invalid target team.");
    return;
  }
  if (x >= GRID_SIZE || y >= GRID_SIZE) {
    Serial.println("Invalid coordinates - must be 0-4.");
    return;
  }

  sendAttack(targetTeam, x, y);
  Serial.print("Attack sent -> TEAM "); Serial.print(targetTeam);
  Serial.print(" ("); Serial.print(x); Serial.print(","); Serial.print(y); Serial.println(")");
}

void handleFeedback(FeedbackPacket *pkt) {
  Serial.print("RESULT: TEAM "); Serial.print(pkt->target_id);
  Serial.print(" ("); Serial.print(pkt->x); Serial.print(","); Serial.print(pkt->y);
  Serial.print(") -> ");

  switch (pkt->result_code) {
    case RESULT_MISS:
      Serial.println("MISS");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_MISS);
      break;
    case RESULT_HIT:
      Serial.println("HIT");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_HIT);
      break;
    case RESULT_SUNK:
      Serial.print("HIT - TEAM "); Serial.print(pkt->target_id); Serial.println(" ELIMINATED!");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_SUNK);
      break;
    case RESULT_INVALID:
      Serial.println("INVALID (rejected by Central)");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_INVALID);
      break;
  }
}

// =====================================================================
// TURN TRACKING
// =====================================================================

void handleTurnUpdate(TurnUpdatePacket *pkt) {
  currentTurnTeam = pkt->current_turn;
  roundState = pkt->round_state;

  if (roundState == STATE_RUNNING) {
    if (currentTurnTeam == MY_TEAM_ID) {
      Serial.println(">>> YOUR TURN <<<");
      uiShowYourTurn();
    } else {
      Serial.print("Waiting - it is TEAM "); Serial.print(currentTurnTeam); Serial.println("'s turn.");
      uiShowWaiting(currentTurnTeam);
    }
  } else if (roundState == STATE_GAMEOVER) {
    Serial.println("GAME OVER.");
    uiShowMessage("GAME OVER");
  } else {
    // SETUP or READY
    Serial.println("Waiting for the organizer to start the game...");
    uiShowMessage("Waiting for start...");
  }
}

// =====================================================================
// SERIAL COMMANDS (testing mode - stand-in for the TFT until it's picked)
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

void processSerialCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  String upper = line;
  upper.toUpperCase();

  if (upper == "CONFIG") { printConfig(); return; }
  if (upper == "STATUS") { printStatus(); return; }
  if (upper == "HELP")   { printHelp();   return; }

  // Otherwise, try to parse it as a manual attack: "target x y", e.g. "2 3 4"
  int target, x, y;
  if (sscanf(line.c_str(), "%d %d %d", &target, &x, &y) == 3) {
    attemptManualAttack((uint8_t)target, (uint8_t)x, (uint8_t)y);
    return;
  }

  Serial.println("Unknown command. Try CONFIG, STATUS, HELP, or 'target x y' e.g. '2 3 4'");
}

void printConfig() {
  Serial.println("=== CONFIG ===");
  Serial.print("MY_TEAM_ID: "); Serial.println(MY_TEAM_ID);
  Serial.print("MY_TEAM_NAME: "); Serial.println(MY_TEAM_NAME);
  Serial.print("ESPNOW_CHANNEL: "); Serial.println(ESPNOW_CHANNEL);

  Serial.print("Central MAC: ");
  for (int i = 0; i < 6; i++) {
    if (centralMac[i] < 16) Serial.print("0");
    Serial.print(centralMac[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();

  Serial.print("My MAC: "); Serial.println(WiFi.macAddress());

  Serial.println("Ships:");
  for (int s = 0; s < 3; s++) {
    Serial.print("  ship "); Serial.print(s + 1);
    Serial.print(": len="); Serial.print(myShips[s].ship_len);
    Serial.print(" start=("); Serial.print(myShips[s].start_x);
    Serial.print(","); Serial.print(myShips[s].start_y); Serial.print(")");
    Serial.println(myShips[s].orientation == ORIENT_HORIZONTAL ? " HORIZONTAL" : " VERTICAL");
  }
  Serial.println("==============");
}

void printStatus() {
  Serial.println("=== STATUS ===");
  Serial.print("Registered with Central: "); Serial.println(registrationConfirmed ? "YES" : "NO (retrying...)");
  Serial.print("Round state: "); Serial.println(roundStateName(roundState));
  if (roundState == STATE_RUNNING) {
    Serial.print("Current turn: TEAM "); Serial.println(currentTurnTeam);
    Serial.println(currentTurnTeam == MY_TEAM_ID ? ">>> IT IS YOUR TURN <<<" : "(waiting for your turn)");
  }
  Serial.println("==============");
}

void printHelp() {
  Serial.println("Commands: CONFIG, STATUS, HELP, or 'target x y' to attack (e.g. 2 3 4)");
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

// =====================================================================
// UI LAYER - real implementation for a 2.4" SPI touch TFT
// (ILI9341 + XPT2046 via the TFT_eSPI library)
// =====================================================================
// This is the ONLY section that changed once the TFT was chosen -
// everything above (game logic, ESP-NOW) still only talks to the UI
// through uiInit()/uiShowMessage()/uiShowYourTurn()/uiShowWaiting()/
// uiShowAttackResult(), exactly as before.
//
// Screen is assumed 240x320 portrait. If your display is mounted
// upside down, change tft.setRotation(0) in uiInit() to 2.
//
// A result banner (MISS/HIT/SUNK/INVALID) stays on screen for
// UI_RESULT_DISPLAY_MS before the next screen (whatever it turns out
// to be - "your turn" or "waiting") is drawn. Since Central sends the
// attack result and the next turn update in quick succession, without
// this the result would flash by too fast to read.

#define UI_RESULT_DISPLAY_MS 1500

#define UI_TARGET_BTN_Y   40
#define UI_TARGET_BTN_H   36
#define UI_TARGET_BTN_W   70
#define UI_TARGET_GAP     10

#define UI_GRID_X         20
#define UI_GRID_Y         90
#define UI_CELL_SIZE      40

#define UI_ATTACK_BTN_X   70
#define UI_ATTACK_BTN_Y   292
#define UI_ATTACK_BTN_W   100
#define UI_ATTACK_BTN_H   26

unsigned long uiResultUntil = 0;   // while millis() < this, a result banner is showing
uint8_t uiPendingScreen = 0;       // 0 = none, 1 = your turn, 2 = waiting, 3 = message
uint8_t uiPendingWaitingTeam = 0;
char uiPendingMessage[40] = "";

uint8_t uiSelTarget = 0;   // 0 = no target chosen yet
int8_t uiSelX = -1, uiSelY = -1;  // -1 = no cell chosen yet

void computeOpponentIds() {
  int n = 0;
  for (uint8_t t = 1; t <= MAX_TEAMS; t++) {
    if (t != MY_TEAM_ID) opponentIds[n++] = t;
  }
}

void uiInit() {
  tft.init();
  tft.setRotation(0);  // portrait - change to 2 if mounted upside down
  tft.setTouch(touchCalData);
  tft.fillScreen(TFT_BLACK);
  computeOpponentIds();
}

// --- public interface (called from game logic / packet handlers) ---

void uiShowMessage(const char *msg) {
  Serial.print("[UI] "); Serial.println(msg);
  if (millis() < uiResultUntil) {
    uiPendingScreen = 3;
    strncpy(uiPendingMessage, msg, sizeof(uiPendingMessage) - 1);
    uiPendingMessage[sizeof(uiPendingMessage) - 1] = '\0';
    return;
  }
  uiDrawMessage(msg);
}

void uiShowYourTurn() {
  if (millis() < uiResultUntil) {
    uiPendingScreen = 1;
    return;
  }
  uiDrawYourTurn();
}

void uiShowWaiting(uint8_t currentTeam) {
  if (millis() < uiResultUntil) {
    uiPendingScreen = 2;
    uiPendingWaitingTeam = currentTeam;
    return;
  }
  uiDrawWaiting(currentTeam);
}

void uiShowAttackResult(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result) {
  uiDrawResultBanner(targetTeam, x, y, result);
  uiResultUntil = millis() + UI_RESULT_DISPLAY_MS;
}

// Called every loop() iteration: shows a deferred screen once the
// result banner's time is up, and polls touch input while "your turn"
// is genuinely on screen.
void uiTick() {
  if (uiPendingScreen != 0 && millis() >= uiResultUntil) {
    uint8_t screen = uiPendingScreen;
    uiPendingScreen = 0;
    if (screen == 1) uiDrawYourTurn();
    else if (screen == 2) uiDrawWaiting(uiPendingWaitingTeam);
    else if (screen == 3) uiDrawMessage(uiPendingMessage);
  }

  bool yourTurnOnScreen = (roundState == STATE_RUNNING && currentTurnTeam == MY_TEAM_ID
                            && uiPendingScreen == 0 && millis() >= uiResultUntil);
  if (yourTurnOnScreen) uiPollTouch();
}

// --- actual drawing ---

void uiDrawMessage(const char *msg) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 140);
  tft.print(msg);
}

void uiDrawWaiting(uint8_t currentTeam) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 130);
  tft.print("Waiting for");
  tft.setCursor(10, 160);
  char buf[20];
  snprintf(buf, sizeof(buf), "TEAM %d", currentTeam);
  tft.print(buf);
}

void uiDrawResultBanner(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result) {
  uint16_t bg;
  const char *label;
  switch (result) {
    case RESULT_MISS:  bg = TFT_BLUE;   label = "MISS";       break;
    case RESULT_HIT:   bg = TFT_ORANGE; label = "HIT";        break;
    case RESULT_SUNK:  bg = TFT_RED;    label = "ELIMINATED!"; break;
    default:            bg = TFT_DARKGREY; label = "INVALID";  break;
  }

  tft.fillScreen(bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextSize(3);
  int16_t tw = tft.textWidth(label);
  tft.setCursor((240 - tw) / 2, 120);
  tft.print(label);

  tft.setTextSize(2);
  char buf[32];
  snprintf(buf, sizeof(buf), "TEAM %d (%d,%d)", targetTeam, x, y);
  tw = tft.textWidth(buf);
  tft.setCursor((240 - tw) / 2, 160);
  tft.print(buf);
}

void uiDrawYourTurn() {
  uiSelTarget = 0;
  uiSelX = -1;
  uiSelY = -1;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("YOUR TURN");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, UI_TARGET_BTN_Y - 14);
  tft.print("Select target:");

  for (int i = 0; i < 3; i++) drawTargetButton(i, false);
  drawGrid();
  drawAttackButton(false);
}

void drawTargetButton(int i, bool selected) {
  int x = 5 + i * (UI_TARGET_BTN_W + UI_TARGET_GAP);
  int y = UI_TARGET_BTN_Y;
  uint16_t fillColor = selected ? TFT_YELLOW : TFT_NAVY;
  uint16_t textColor = selected ? TFT_BLACK : TFT_WHITE;

  tft.fillRoundRect(x, y, UI_TARGET_BTN_W, UI_TARGET_BTN_H, 6, fillColor);
  tft.drawRoundRect(x, y, UI_TARGET_BTN_W, UI_TARGET_BTN_H, 6, TFT_WHITE);

  char label[10];
  snprintf(label, sizeof(label), "T%d", opponentIds[i]);
  tft.setTextColor(textColor, fillColor);
  tft.setTextSize(2);
  int16_t tw = tft.textWidth(label);
  tft.setCursor(x + (UI_TARGET_BTN_W - tw) / 2, y + 10);
  tft.print(label);
}

void drawGrid() {
  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      int x = UI_GRID_X + c * UI_CELL_SIZE;
      int y = UI_GRID_Y + r * UI_CELL_SIZE;
      bool selected = (uiSelX == c && uiSelY == r);
      tft.fillRect(x + 1, y + 1, UI_CELL_SIZE - 2, UI_CELL_SIZE - 2, selected ? TFT_YELLOW : TFT_DARKGREY);
      tft.drawRect(x, y, UI_CELL_SIZE, UI_CELL_SIZE, TFT_WHITE);
    }
  }
}

void drawAttackButton(bool enabled) {
  uint16_t fillColor = enabled ? TFT_RED : TFT_DARKGREY;
  tft.fillRoundRect(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H, 6, fillColor);
  tft.drawRoundRect(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fillColor);
  tft.setTextSize(2);
  const char *label = "ATTACK";
  int16_t tw = tft.textWidth(label);
  tft.setCursor(UI_ATTACK_BTN_X + (UI_ATTACK_BTN_W - tw) / 2, UI_ATTACK_BTN_Y + 6);
  tft.print(label);
}

// --- touch handling ---

void uiPollTouch() {
  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return;

  static unsigned long lastTouchMillis = 0;
  if (millis() - lastTouchMillis < 250) return;  // simple debounce
  lastTouchMillis = millis();

  // target buttons
  for (int i = 0; i < 3; i++) {
    int x = 5 + i * (UI_TARGET_BTN_W + UI_TARGET_GAP);
    int y = UI_TARGET_BTN_Y;
    if (tx >= x && tx < x + UI_TARGET_BTN_W && ty >= y && ty < y + UI_TARGET_BTN_H) {
      uiSelTarget = opponentIds[i];
      for (int j = 0; j < 3; j++) drawTargetButton(j, opponentIds[j] == uiSelTarget);
      drawAttackButton(uiSelTarget != 0 && uiSelX >= 0);
      return;
    }
  }

  // grid cells
  if (tx >= UI_GRID_X && tx < UI_GRID_X + GRID_SIZE * UI_CELL_SIZE
      && ty >= UI_GRID_Y && ty < UI_GRID_Y + GRID_SIZE * UI_CELL_SIZE) {
    uiSelX = (tx - UI_GRID_X) / UI_CELL_SIZE;
    uiSelY = (ty - UI_GRID_Y) / UI_CELL_SIZE;
    drawGrid();
    drawAttackButton(uiSelTarget != 0 && uiSelX >= 0);
    return;
  }

  // ATTACK button
  if (tx >= UI_ATTACK_BTN_X && tx < UI_ATTACK_BTN_X + UI_ATTACK_BTN_W
      && ty >= UI_ATTACK_BTN_Y && ty < UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H) {
    if (uiSelTarget != 0 && uiSelX >= 0 && uiSelY >= 0) {
      // attemptManualAttack() already does the same client-side checks
      // and Serial logging as the Serial testing mode - reused as-is.
      attemptManualAttack(uiSelTarget, (uint8_t)uiSelX, (uint8_t)uiSelY);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextSize(1);
      tft.setCursor(10, UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H + 8);
      tft.print("Sent - waiting for result...");
    }
  }
}
