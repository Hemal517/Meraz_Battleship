/* =====================================================================
   MERAZ BATTLESHIP - PARTICIPANT NODE (the organizers' reference version)
   =====================================================================
   WHAT THIS FILE IS
     The finished firmware for one team's ESP32 + touch display. The
     SAME file goes on all 4 team boards; only the CONFIGURATION block
     just below changes (team ID, team name, ships, Central's MAC).
     (Participants who are building their own firmware use
     participant_starter.ino + PARTICIPANT_GUIDE.md instead.)

   WHERE IT FITS IN THE WHOLE SYSTEM
       this board  <--ESP-NOW radio-->  central_node.ino  <--USB-->  dashboard.py
     Central (central_node.ino) is the only referee. This board NEVER
     decides whether a shot is valid - it asks Central and shows the
     answer. All real game rules live in game_logic.h on the Central side.

   FILES THIS SKETCH NEEDS
     meraz_loadout.h the power ids, the loadout byte and its validity check
                     (keep it in the same folder as this sketch)
     VirtualTFT.h    only when USE_VIRTUAL_DISPLAY is switched on (below)
     NOTE: this file keeps its OWN copy of the constants and packet
     structs (it does not include game_logic.h). They must stay
     byte-for-byte identical to the ones in central_node.ino and
     game_logic.h, or Central will ignore this board's packets.

   WHAT THE BOARD DOES
     1. LOADOUT: the team picks 2 attack powers (Sonar, Salvo, Mine, Double
        Attack) and 1 defence power (Smoke Screen, Repair) and locks them in.
        Single Strike and Shield are always theirs. NOTHING is sent to
        Central before the loadout is locked.
     2. Registers with Central (retrying until accepted), loadout byte included.
     3. Listens for turn updates so it knows when it is this team's turn.
     4. On your turn: Single Strike (target team, tap a cell, ATTACK) or
        POWERS -> pick a power -> set it up -> press its button.
        Every power takes the turn. Shield: never on two turns in a row.
     5. Shows the result (MISS / HIT / ELIMINATED / MINE / BLOCKED / NO REPORT /
        INVALID, or the power's result) from Central.

   HOW THE FILE IS ORGANISED (top to bottom)
     CONFIGURATION            <- the only part you edit per board
     TFT DISPLAY CONFIGURATION
     PACKET STRUCTURES + CONSTANTS (must match Central)
     ESP-NOW communication
     SERIAL COMMANDS          <- typed test commands (see below)
     LOADOUT AND POWER-UPS    <- what the powers are, local checks, sending
     UI LAYER                 <- everything that draws or reads touches

   NO DISPLAY YET?
     Uncomment  #define USE_VIRTUAL_DISPLAY  (near the includes). The
     same UI then draws on a virtual screen in your browser through
     virtual_display.py, and mouse clicks act as touches. Details are
     in VirtualTFT.h. Comment it out again for the real TFT.

   SERIAL COMMANDS (type into the Serial Monitor at 115200 baud;
   with the virtual display, use the box on the virtual display page):
     CONFIG          - show this board's configuration
     STATUS          - show registration / turn status
     HELP            - list commands
     2 3 4           - send a Single Strike: target team 2, x=3, y=4
     LOADOUT 1 7 5   - pick powers by id (Sonar 1, Salvo 2, Mine 3, Repair 4,
                       Smoke 5, Double 7: two attacks + one defence) and lock in
                       (the touch screen does the same; this is for testing)
     POWER SONAR 2 3 3     - use a power. Forms:  POWER SONAR <team> <x> <y>
                             POWER SALVO <team> x1 y1 x2 y2 x3 y3
                             POWER DOUBLE <team> x1 y1 x2 y2
                             POWER MINE x y | REPAIR x y | SHIELD x y | SMOKE
     (virtual display only - walk through the screens WITHOUT Central:)
     DEMO LOADOUT    - show the "choose your powers" screen
     DEMO TURN       - show the "your turn" screen
     DEMO MENU       - show the powers menu
     DEMO WAIT       - show the "waiting for another team" screen
     DEMO HIT / MISS / SUNK / INVALID / MINE / BLOCKED / UNKNOWN - result banners
     DEMO SONAR / SALVO - power result banners
     DEMO MSG        - show a message screen
   ===================================================================== */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <string.h>
#include <stdio.h>
#include "meraz_loadout.h"   // power ids, loadout byte, loadoutIsValid() - must sit next to this sketch

// ---------------------------------------------------------------------
// NO DISPLAY YET? Uncomment the next line to run the exact same UI on a
// virtual screen in your browser (see VirtualTFT.h and
// virtual_display.py). Comment it out again for the real TFT.
// ---------------------------------------------------------------------
 #define USE_VIRTUAL_DISPLAY

#ifdef USE_VIRTUAL_DISPLAY
  #include "VirtualTFT.h"   // must be in the same folder as this sketch
#else
  #include <SPI.h>
  #include <TFT_eSPI.h>     // library: bollworm/Bodmer's TFT_eSPI - see the setup note below
#endif

// =====================================================================
// A couple of definitions the CONFIGURATION section below needs.
// You shouldn't need to touch these.
// =====================================================================

// start_x/start_y is always the ship's FIRST cell; it grows from there:
#define ORIENT_HORIZONTAL 0   // rightward    (dx=+1, dy= 0)
#define ORIENT_VERTICAL   1   // downward     (dx= 0, dy=+1)
#define ORIENT_DIAG_DOWN  2   // down-right   (dx=+1, dy=+1)  e.g. (0,0) -> (4,4)
#define ORIENT_DIAG_UP    3   // up-right     (dx=+1, dy=-1)  e.g. (0,4) -> (4,0)

// One ship: how long it is, where it starts, and which way it points.
typedef struct __attribute__((packed)) {
  uint8_t ship_len;     // 1, 3, or 5
  uint8_t start_x;      // column, 0-6
  uint8_t start_y;      // row, 0-6
  uint8_t orientation;  // ORIENT_HORIZONTAL / VERTICAL / DIAG_DOWN / DIAG_UP
} ShipPlacement;

// =====================================================================
// CONFIGURATION - CHANGE THESE FOR EACH TEAM'S BOARD
// =====================================================================
// This is the ONLY section that should differ between the 4 boards.

#define MY_TEAM_ID 2
const char *MY_TEAM_NAME = "TRI2";

// Your 3 ships. Grid is 7x7, x = column (0-6), y = row (0-6).
// Rules: exactly one ship of length 1, one of length 3, one of length
// 5; straight lines only (horizontal, vertical or diagonal); must fit
// inside the grid; ships must NOT overlap each other (touching is fine).
// Diagonal ships: ORIENT_DIAG_DOWN starts at the top-left end and goes
// down-right; ORIENT_DIAG_UP starts at the bottom-left end and goes
// up-right. A size-5 diagonal needs a 5x5 block of free cells.
//
// Example below (already valid, feel free to leave as-is for testing):
//   Ship 1 (size 1): single cell at (2,4)
//   Ship 2 (size 3): horizontal, starts at (0,0) -> covers (0,0)(1,0)(2,0)
//   Ship 3 (size 5): vertical,   starts at (4,0) -> covers (4,0)..(4,4)
// Diagonal example: { 3, 0, 2, ORIENT_DIAG_DOWN } -> (0,2)(1,3)(2,4)
ShipPlacement myShips[3] = {
  { 1, 2, 4, ORIENT_HORIZONTAL },
  { 3, 0, 0, ORIENT_HORIZONTAL },
  { 5, 4, 0, ORIENT_VERTICAL   },
};

// TODO: replace with the Central ESP32's real MAC address.
// The Central prints its own MAC on boot - copy it from there.
uint8_t centralMac[6] = {
  0xA0, 0xB7, 0x65, 0x0E, 0xF7, 0xAC
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
#if !defined(USE_VIRTUAL_DISPLAY) && !defined(TOUCH_CS)
  #warning "TOUCH_CS is not defined in TFT_eSPI's User_Setup.h - touch will be disabled. Add: #define TOUCH_CS 15"
#endif
uint16_t touchCalData[5] = { 300, 3600, 300, 3600, 7 };  // PLACEHOLDER - replace per board

#ifdef USE_VIRTUAL_DISPLAY
VirtualTFT tft;
#else
TFT_eSPI tft = TFT_eSPI();
#endif
uint8_t opponentIds[3];  // the 3 team IDs that aren't MY_TEAM_ID, filled in by uiInit()

// =====================================================================
// EVERYTHING BELOW THIS LINE IS THE SAME FOR ALL 4 TEAMS
// =====================================================================

#define MAX_TEAMS  4
#define GRID_SIZE  7

// FeedbackPacket.result_code (must match central_node.ino)
#define RESULT_MISS     0
#define RESULT_HIT      1
#define RESULT_INVALID  2
#define RESULT_SUNK     3
#define RESULT_MINE     4   // the cell held a mine: counts as a miss, you lose your next turn
#define RESULT_BLOCKED  5   // the cell is under a shield: nothing happened, turn used
#define RESULT_UNKNOWN  6   // the target is behind a smoke screen: you are not told

// PowerResultPacket.status (must match central_node.ino)
#define PWR_STATUS_OK        0
#define PWR_STATUS_REJECTED  1
#define PWR_STATUS_JAMMED    2   // sonar blocked by smoke: the use and the turn are still spent

#define POWER_USES  2            // uses of each optional power per match

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
  uint8_t loadout;       // the locked-in loadout byte (see meraz_loadout.h)
} RegistrationPacket;   // 34 bytes - we send this

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

typedef struct __attribute__((packed)) {
  uint8_t power;         // POWER_* id
  uint8_t team_id;       // our team id
  uint8_t target_id;     // the opponent for Sonar / Salvo / Double, else 0
  uint8_t x1, y1;        // meaning depends on the power (see PARTICIPANT_GUIDE.md)
  uint8_t x2, y2;
  uint8_t x3, y3;
} PowerUpPacket;         // 9 bytes - we send this

typedef struct __attribute__((packed)) {
  uint8_t power;
  uint8_t status;        // PWR_STATUS_*
  uint8_t target_id;
  uint8_t count;         // Sonar: ship cells found in the 3x3 area
  uint8_t result[3];     // Salvo / Double: result per cell (RESULT_*, 2 = not fired)
} PowerResultPacket;     // 7 bytes - Central sends this back

static_assert(sizeof(RegistrationPacket) == 34, "RegistrationPacket must be 34 bytes");
static_assert(sizeof(AttackPacket) == 4, "AttackPacket must be 4 bytes");
static_assert(sizeof(PowerUpPacket) == 9, "PowerUpPacket must be 9 bytes");
static_assert(sizeof(PowerResultPacket) == 7, "PowerResultPacket must be 7 bytes");
static_assert(sizeof(RegAckPacket) == 1 && sizeof(FeedbackPacket) == 4 && sizeof(TurnUpdatePacket) == 2,
              "reply packets must be 1, 4 and 2 bytes (Central tells packets apart by length)");

// =====================================================================
// GAME STATE (what THIS board currently believes about the match)
// =====================================================================

bool registrationConfirmed = false;   // true once Central sends REG_OK / REG_RECONNECTED
uint8_t currentTurnTeam = 0;          // 0 = unknown / no active turn
uint8_t roundState = STATE_SETUP;     // mirrors Central's round_state
unsigned long lastRegAttemptMillis = 0;

// ---- loadout + power-ups (what this team chose, and local bookkeeping) ----
// Central is the referee for ALL of this. These copies only drive the screens,
// so a wrong guess here can never cheat: Central simply refuses the move.
uint8_t myLoadout = 0;               // the locked-in loadout byte (shield bit set)
bool loadoutLocked = false;          // nothing goes to Central before this is true
bool demoNoRegister = false;         // DEMO commands pretend a loadout without registering
uint8_t usesLeft[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // uses left, indexed by power id (the shield is unlimited)
bool lastMoveWasShield = false;      // hint for the menu only: Central enforces "no Shield twice in a row"
bool ownShip[GRID_SIZE][GRID_SIZE];  // where my ships are (built from myShips[])
int8_t myMineX = -1, myMineY = -1;   // my last mine (Central never tells us when it goes off)
uint8_t pendingPower = 0, pendingTarget = 0, pendingCells = 0;   // the power-up now on its way to Central
uint8_t pendingX[3] = {0, 0, 0}, pendingY[3] = {0, 0, 0};
unsigned long uiSendLockUntil = 0;   // briefly ignore touches after sending a move
unsigned long lastLoadoutPromptMillis = 0;
bool uiMyTurnShown = false;          // my ACTION/MENU screen is up (so a repeated turn update doesn't wipe it)
uint8_t ldAttack = 0;                // LOADOUT screen: bit (power id) set for each attack picked
uint8_t ldDefence = 0;               // ... the defence power picked (0 = none)

// How each power looks on screen and how many cells it needs.
typedef struct {
  uint8_t id;
  const char *name;      // short, fits the buttons (size-2 text)
  const char *hint;      // one line, size-1 text
  bool needTarget;       // pick an opponent first
  bool ownGrid;          // the cells are on MY board, not an opponent's
  uint8_t cells;         // how many cells to pick (0 = none)
  const char *action;    // label of the big red button
} PowerSpec;

static const PowerSpec POWER_SPECS[7] = {
  { POWER_SONAR,  "SONAR",  "scan a 3x3 area",   true,  false, 1, "PING"       },
  { POWER_SALVO,  "SALVO",  "3 in a line", true,  false, 3, "FIRE x3"     },
  { POWER_MINE,   "MINE",   "hidden trap",       false, true,  1, "PLACE MINE"  },
  { POWER_REPAIR, "REPAIR", "fix a hit cell",    false, true,  1, "REPAIR"      },
  { POWER_SMOKE,  "SMOKE",  "hide attacks", false, false, 0, "DEPLOY"    },
  { POWER_SHIELD, "SHIELD", "block a 3x3 area",  false, true,  1, "RAISE"       },
  { POWER_DOUBLE, "DOUBLE", "2 shots at once",   true,  false, 2, "FIRE x2"     },
};

int specIndex(uint8_t id) {
  for (int i = 0; i < 7; i++) if (POWER_SPECS[i].id == id) return i;
  return -1;
}

// n cells in one straight, consecutive run in a single row or column (same rule as Central).
bool cellsFormLine(const uint8_t xs[], const uint8_t ys[], int n) {
  bool row = true, col = true;
  for (int i = 1; i < n; i++) {
    if (ys[i] != ys[0]) row = false;
    if (xs[i] != xs[0]) col = false;
  }
  if (row == col) return false;            // scattered, or all the same cell
  const uint8_t *v = row ? xs : ys;
  int mn = v[0], mx = v[0];
  for (int i = 1; i < n; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; }
  if (mx - mn != n - 1) return false;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (v[i] == v[j]) return false;
  return true;
}

// Marks the cells of myShips[] on ownShip[][] (used to show my board and to keep mines off my ships).
void computeOwnShips() {
  for (int r = 0; r < GRID_SIZE; r++)
    for (int c = 0; c < GRID_SIZE; c++) ownShip[r][c] = false;
  for (int s = 0; s < 3; s++) {
    int dx = 1, dy = 0;
    switch (myShips[s].orientation) {
      case ORIENT_VERTICAL:  dx = 0; dy = 1;  break;
      case ORIENT_DIAG_DOWN: dx = 1; dy = 1;  break;
      case ORIENT_DIAG_UP:   dx = 1; dy = -1; break;
      default: break;
    }
    for (int i = 0; i < myShips[s].ship_len; i++) {
      int x = myShips[s].start_x + dx * i, y = myShips[s].start_y + dy * i;
      if (x >= 0 && x < GRID_SIZE && y >= 0 && y < GRID_SIZE) ownShip[y][x] = true;
    }
  }
}

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
// The send callback changed too, but LATER than the receive one: it only
// switched to wifi_tx_info_t in IDF 5.5 (Arduino-ESP32 core 3.3.x).
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status);
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
#endif
void processPacket(const uint8_t *data, int len);

bool localValidateShips(ShipPlacement ships[3]);
void sendRegistration();
void sendAttack(uint8_t targetTeam, uint8_t x, uint8_t y);
bool attemptManualAttack(uint8_t targetTeam, uint8_t x, uint8_t y);

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
void uiShowPowerResult(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]);
void uiTick();
void uiRedraw();
void uiPollTouch();
void uiPollLoadoutTouch();
void uiDrawLoadout();
void uiDrawYourTurn();
void uiDrawActionScreen();
void uiDrawPowerMenu();
void uiDrawWaiting(uint8_t currentTeam);
void uiDrawMessage(const char *msg);
void uiDrawResultBanner(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result);
void uiDrawPowerBanner(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]);
void drawTargetButton(int i, bool selected);
void drawGrid();
void drawAltButton(const char *label);
void drawAttackButton(bool enabled, const char *label);
void computeOpponentIds();

void lockLoadout(uint8_t loadout);
bool attemptPower(uint8_t power, uint8_t target, const uint8_t xs[3], const uint8_t ys[3]);
void handlePowerResult(PowerResultPacket *pkt);
void handleLoadoutCommand(const char *line);
void handlePowerCommand(const char *line);

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
  computeOwnShips();
  printHelp();

  // Nothing is sent to Central until the team has locked in its loadout.
  uiDrawLoadout();
  Serial.println("[LOADOUT] Choose your powers on the screen (or type  LOADOUT 1 7 5  - see HELP).");
  lastLoadoutPromptMillis = millis();
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
  if (loadoutLocked && !demoNoRegister && !registrationConfirmed && millis() - lastRegAttemptMillis > REG_RETRY_INTERVAL_MS) {
    sendRegistration();
    lastRegAttemptMillis = millis();
  }

  // Remind (on Serial) while the loadout is still open - the screen mirror keys off this line.
  if (!loadoutLocked && millis() - lastLoadoutPromptMillis > 3000) {
    Serial.println("[LOADOUT] waiting for the team to choose powers (screen, or: LOADOUT 1 7 5)");
    lastLoadoutPromptMillis = millis();
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

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("WARNING: an ESP-NOW send to Central failed.");
  }
}

// =====================================================================
// PACKET ROUTING - Central only ever sends us RegAck (1 byte),
// TurnUpdate (2 bytes), Feedback (4 bytes) or PowerResult (7 bytes), and
// those four sizes are all different, so length alone tells us what we received.
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
  } else if (len == sizeof(PowerResultPacket)) {
    PowerResultPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    handlePowerResult(&pkt);
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
  pkt.loadout = myLoadout;

  esp_now_send(centralMac, (uint8_t *)&pkt, sizeof(pkt));
  Serial.print("Sent registration to Central (loadout 0x"); Serial.print(myLoadout, HEX); Serial.println(")...");
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
      Serial.println("Registration REJECTED. Check MY_TEAM_ID / myShips[] above (or the game already started).");
      uiShowMessage("Registration REJECTED");
      break;
    case REG_RECONNECT_REJECTED:
      registrationConfirmed = false;
      Serial.println("Reconnect REJECTED - our ships or LOADOUT don't match what Central has stored.");
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
bool attemptManualAttack(uint8_t targetTeam, uint8_t x, uint8_t y) {
  if (!registrationConfirmed) {
    Serial.println("Cannot attack - not registered with Central yet.");
    return false;
  }
  if (roundState != STATE_RUNNING) {
    Serial.println("Cannot attack - the game is not running.");
    return false;
  }
  if (currentTurnTeam != MY_TEAM_ID) {
    Serial.println("Cannot attack - it is not your turn.");
    return false;
  }
  if (targetTeam < 1 || targetTeam > MAX_TEAMS || targetTeam == MY_TEAM_ID) {
    Serial.println("Invalid target team.");
    return false;
  }
  if (x >= GRID_SIZE || y >= GRID_SIZE) {
    Serial.println("Invalid coordinates - must be 0-6.");
    return false;
  }

  sendAttack(targetTeam, x, y);
  uiSendLockUntil = millis() + 2000;
  Serial.print("Attack sent -> TEAM "); Serial.print(targetTeam);
  Serial.print(" ("); Serial.print(x); Serial.print(","); Serial.print(y); Serial.println(")");
  return true;
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
    case RESULT_MINE:
      Serial.println("MINE - it was a mine! You lose your next turn.");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_MINE);
      break;
    case RESULT_BLOCKED:
      Serial.println("BLOCKED by a shield (turn used, nothing marked).");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_BLOCKED);
      break;
    case RESULT_UNKNOWN:
      Serial.println("UNKNOWN - the target is behind a smoke screen (the cell counts as shot).");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_UNKNOWN);
      break;
    case RESULT_INVALID:
      Serial.println("INVALID (rejected by Central)");
      uiShowAttackResult(pkt->target_id, pkt->x, pkt->y, RESULT_INVALID);
      break;
    default:
      Serial.println("(unknown result code)");
      return;
  }
  if (pkt->result_code != RESULT_INVALID) lastMoveWasShield = false;   // a shot is a turn without a shield
}

// =====================================================================
// LOADOUT AND POWER-UPS
// =====================================================================
// The loadout is picked on the touch screen BEFORE anything is sent to
// Central (see uiDrawLoadout()). Locking it in starts the registration, with
// the loadout byte as the last byte of the 34-byte registration packet.
// Every power-up uses the turn. Central checks everything again, and it is the
// only one that counts uses - the copies here only decide what the screens show.

void lockLoadout(uint8_t loadout) {
  myLoadout = loadout;
  loadoutLocked = true;
  for (int p = 1; p <= 7; p++) usesLeft[p] = (p != POWER_SHIELD && (loadout & LOADOUT_BIT(p))) ? POWER_USES : 0;
  Serial.print("Loadout locked: "); printLoadout(loadout);
  Serial.print("   (byte 0x"); Serial.print(loadout, HEX); Serial.println(")");
  uiShowMessage("Connecting...");
  if (!demoNoRegister) {
    sendRegistration();
    lastRegAttemptMillis = millis();
  }
}

// "SONAR", "sonar" or "1" -> 1, ... ; 0 if not a power.
uint8_t parsePowerToken(const char *tok) {
  if (tok[0] >= '1' && tok[0] <= '7' && tok[1] == '\0') return (uint8_t)(tok[0] - '0');
  static const char *names[7] = { "SONAR", "SALVO", "MINE", "REPAIR", "SMOKE", "SHIELD", "DOUBLE" };
  for (int i = 0; i < 7; i++) {
    const char *a = names[i], *b = tok;
    while (*a && *b && (*b | 0x20) == (*a | 0x20)) { a++; b++; }
    if (!*a && !*b) return (uint8_t)(i + 1);
  }
  return 0;
}

static bool sendPower(uint8_t power, uint8_t target, const uint8_t xs[3], const uint8_t ys[3]) {
  PowerUpPacket pkt;
  pkt.power = power;
  pkt.team_id = MY_TEAM_ID;
  pkt.target_id = target;
  pkt.x1 = xs[0]; pkt.y1 = ys[0];
  pkt.x2 = xs[1]; pkt.y2 = ys[1];
  pkt.x3 = xs[2]; pkt.y3 = ys[2];
  return esp_now_send(centralMac, (uint8_t *)&pkt, sizeof(pkt)) == ESP_OK;
}

// Local sanity checks, then send. Returns true if the request was sent.
bool attemptPower(uint8_t power, uint8_t target, const uint8_t xs[3], const uint8_t ys[3]) {
  const int si = specIndex(power);
  if (si < 0) { Serial.println("Unknown power."); return false; }
  const PowerSpec &sp = POWER_SPECS[si];

  if (!registrationConfirmed) { Serial.println("Cannot use a power - not registered with Central yet."); return false; }
  if (roundState != STATE_RUNNING) { Serial.println("Cannot use a power - the game is not running."); return false; }
  if (currentTurnTeam != MY_TEAM_ID) { Serial.println("Cannot use a power - it is not your turn."); return false; }
  if (power != POWER_SHIELD && !(myLoadout & LOADOUT_BIT(power))) {
    Serial.print(sp.name); Serial.println(" is not in your loadout."); return false;
  }
  if (power != POWER_SHIELD && usesLeft[power] == 0) {
    Serial.print("No uses of "); Serial.print(sp.name); Serial.println(" left."); return false;
  }
  if (sp.needTarget && (target < 1 || target > MAX_TEAMS || target == MY_TEAM_ID)) {
    Serial.println("Invalid target team."); return false;
  }
  for (int i = 0; i < sp.cells; i++) {
    if (xs[i] >= GRID_SIZE || ys[i] >= GRID_SIZE) { Serial.println("Invalid coordinates - must be 0-6."); return false; }
  }
  if (power == POWER_SALVO && !cellsFormLine(xs, ys, 3)) {
    Serial.println("A Salvo needs 3 consecutive cells in one row or one column."); return false;
  }
  if (power == POWER_DOUBLE && xs[0] == xs[1] && ys[0] == ys[1]) {
    Serial.println("A Double Attack needs 2 different cells."); return false;
  }
  if (power == POWER_MINE && ownShip[ys[0]][xs[0]]) {
    Serial.println("A mine can't go on one of your ships."); return false;
  }
  if (power == POWER_SHIELD && (xs[0] < 1 || xs[0] > GRID_SIZE - 2 || ys[0] < 1 || ys[0] > GRID_SIZE - 2)) {
    Serial.println("Shield centre must be 1-5 in both x and y."); return false;
  }

  uint8_t sx[3] = {0, 0, 0}, sy[3] = {0, 0, 0};
  for (int i = 0; i < sp.cells; i++) { sx[i] = xs[i]; sy[i] = ys[i]; }
  if (!sendPower(power, sp.needTarget ? target : 0, sx, sy)) { Serial.println("Could not send the power-up."); return false; }

  pendingPower = power; pendingTarget = target; pendingCells = sp.cells;
  for (int i = 0; i < 3; i++) { pendingX[i] = sx[i]; pendingY[i] = sy[i]; }
  uiSendLockUntil = millis() + 2000;

  Serial.print("Power sent -> "); Serial.print(sp.name);
  if (sp.needTarget) { Serial.print(" TEAM "); Serial.print(target); }
  for (int i = 0; i < sp.cells; i++) { Serial.print(" ("); Serial.print(sx[i]); Serial.print(","); Serial.print(sy[i]); Serial.print(")"); }
  Serial.println();
  return true;
}

void handlePowerResult(PowerResultPacket *pkt) {
  // One easy-to-parse line (the screen mirror reads it), then the screen.
  Serial.print("POWER_RESULT: power="); Serial.print(pkt->power);
  Serial.print(" status="); Serial.print(pkt->status);
  Serial.print(" target="); Serial.print(pkt->target_id);
  Serial.print(" count="); Serial.print(pkt->count);
  Serial.print(" r="); Serial.print(pkt->result[0]); Serial.print(",");
  Serial.print(pkt->result[1]); Serial.print(","); Serial.println(pkt->result[2]);

  const int si = specIndex(pkt->power);
  if (pkt->status == PWR_STATUS_REJECTED) {
    Serial.print("REJECTED by Central: "); Serial.println(si >= 0 ? POWER_SPECS[si].name : "power");
    Serial.println("(your turn and your uses are NOT spent - fix the move and try again)");
  } else {
    if (pkt->power != POWER_SHIELD && pkt->power >= 1 && pkt->power <= 7 && usesLeft[pkt->power] > 0) usesLeft[pkt->power]--;
    lastMoveWasShield = (pkt->power == POWER_SHIELD);
    if (pkt->power == POWER_MINE) { myMineX = (int8_t)pendingX[0]; myMineY = (int8_t)pendingY[0]; }
    if (pkt->power == POWER_SONAR) {
      if (pkt->status == PWR_STATUS_JAMMED) Serial.println("SONAR JAMMED by a smoke screen (use and turn spent).");
      else { Serial.print("SONAR: "); Serial.print(pkt->count); Serial.println(" ship cell(s) in the area."); }
    }
  }
  uiShowPowerResult(pkt->power, pkt->status, pkt->target_id, pkt->count, pkt->result);
}

// ---- typed commands (testing, and the screen mirror) ----

// Splits `line` into words (spaces / commas) into toks[]; returns how many.
static int splitWords(const char *line, char toks[][12], int maxToks) {
  int n = 0;
  const char *p = line;
  while (*p && n < maxToks) {
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    if (!*p) break;
    int k = 0;
    while (*p && *p != ' ' && *p != ',' && *p != '\t') { if (k < 11) toks[n][k++] = *p; p++; }
    toks[n][k] = '\0';
    n++;
  }
  return n;
}

// LOADOUT 1 7 5   (power ids or names: two attacks + one defence)
void handleLoadoutCommand(const char *line) {
  if (loadoutLocked) {
    Serial.print("Loadout already locked: "); printLoadout(myLoadout); Serial.println();
    return;
  }
  char toks[8][12];
  int n = splitWords(line, toks, 8);
  uint8_t loadout = LOADOUT_SHIELD_BIT;
  for (int i = 1; i < n; i++) {                        // toks[0] is the word LOADOUT
    uint8_t id = parsePowerToken(toks[i]);
    if (id == 0) { Serial.print("Unknown power: "); Serial.println(toks[i]); return; }
    if (id != POWER_SHIELD) loadout |= LOADOUT_BIT(id);
  }
  if (!loadoutIsValid(loadout)) {
    Serial.println("A loadout is exactly 2 attack powers (Sonar 1, Salvo 2, Mine 3, Double 7) and 1 defence power (Repair 4, Smoke 5).");
    Serial.println("Example:  LOADOUT 1 7 5");
    return;
  }
  lockLoadout(loadout);
}

// POWER SONAR 2 3 3   POWER SALVO 2 1 1 2 1 3 1   POWER DOUBLE 2 0 3 6 6   POWER MINE 5 6   POWER SMOKE
void handlePowerCommand(const char *line) {
  char toks[10][12];
  int n = splitWords(line, toks, 10);
  uint8_t power = (n >= 2) ? parsePowerToken(toks[1]) : 0;
  const int si = specIndex(power);
  if (si < 0) {
    Serial.println("Usage: POWER <SONAR|SALVO|MINE|REPAIR|SMOKE|SHIELD|DOUBLE> [team] [x y] ...  e.g.  POWER SONAR 2 3 3");
    return;
  }
  const PowerSpec &sp = POWER_SPECS[si];
  int vals[7], nv = 0;
  for (int i = 2; i < n && nv < 7; i++) vals[nv++] = atoi(toks[i]);
  const int need = (sp.needTarget ? 1 : 0) + 2 * sp.cells;
  if (nv != need) {
    Serial.print(sp.name); Serial.print(" needs "); Serial.print(need); Serial.println(" number(s): [team] then x y for each cell.");
    return;
  }
  uint8_t target = 0, xs[3] = {0, 0, 0}, ys[3] = {0, 0, 0};
  int k = 0;
  if (sp.needTarget) target = (uint8_t)vals[k++];
  for (int i = 0; i < sp.cells; i++) { xs[i] = (uint8_t)vals[k++]; ys[i] = (uint8_t)vals[k++]; }
  attemptPower(power, target, xs, ys);
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
// SERIAL COMMANDS (typed test commands; they work alongside the touch UI)
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

#ifdef USE_VIRTUAL_DISPLAY
  // Messages from virtual_display.py (not typed by a person)
  if (line.startsWith("@TOUCH ")) {
    int tx, ty;
    if (sscanf(line.c_str() + 7, "%d %d", &tx, &ty) == 2 && tx >= 0 && ty >= 0) {
      tft.feedTouch((uint16_t)tx, (uint16_t)ty);
    }
    return;
  }
  if (line == "@REDRAW") { uiRedraw(); return; }

  // DEMO commands: show UI screens without needing Central. These only
  // exist in virtual-display builds, so the real firmware never has them.
  String demo = line;
  demo.toUpperCase();
  if (demo.startsWith("DEMO")) {
    if (demo == "DEMO LOADOUT") {
      loadoutLocked = false; demoNoRegister = true; ldAttack = 0; ldDefence = 0;
      uiDrawLoadout();
      return;
    }
    // every other demo needs a locked loadout; pretend one (Sonar + Double + Smoke) without registering
    if (!loadoutLocked) {
      demoNoRegister = true;
      myLoadout = (uint8_t)(LOADOUT_SHIELD_BIT | LOADOUT_BIT(POWER_SONAR) | LOADOUT_BIT(POWER_DOUBLE) | LOADOUT_BIT(POWER_SMOKE));
      loadoutLocked = true;
      for (int p = 1; p <= 7; p++) usesLeft[p] = (p != POWER_SHIELD && (myLoadout & LOADOUT_BIT(p))) ? POWER_USES : 0;
    }
    if (demo == "DEMO MENU") {
      roundState = STATE_RUNNING;
      currentTurnTeam = MY_TEAM_ID;
      uiMyTurnShown = true;
      uiDrawPowerMenu();
    } else if (demo == "DEMO SONAR") {
      pendingX[0] = 3; pendingY[0] = 4;
      uint8_t r[3] = {2, 2, 2};
      uiShowPowerResult(POWER_SONAR, PWR_STATUS_OK, opponentIds[0], 2, r);
    } else if (demo == "DEMO SALVO") {
      pendingCells = 3; pendingX[0] = 1; pendingX[1] = 2; pendingX[2] = 3; pendingY[0] = pendingY[1] = pendingY[2] = 4;
      uint8_t r[3] = {RESULT_MISS, RESULT_HIT, RESULT_UNKNOWN};
      uiShowPowerResult(POWER_SALVO, PWR_STATUS_OK, opponentIds[0], 0, r);
    } else if (demo == "DEMO MINE")    { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_MINE); }
    else if (demo == "DEMO BLOCKED")   { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_BLOCKED); }
    else if (demo == "DEMO UNKNOWN")   { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_UNKNOWN); }
    else if (demo == "DEMO TURN") {
      roundState = STATE_RUNNING;
      currentTurnTeam = MY_TEAM_ID;          // makes the touch handling active too
      uiShowYourTurn();
    } else if (demo == "DEMO WAIT") {
      roundState = STATE_RUNNING;
      currentTurnTeam = (MY_TEAM_ID % MAX_TEAMS) + 1;   // some other team
      uiShowWaiting(currentTurnTeam);
    } else if (demo == "DEMO HIT")     { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_HIT); }
    else if (demo == "DEMO MISS")      { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_MISS); }
    else if (demo == "DEMO SUNK")      { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_SUNK); }
    else if (demo == "DEMO INVALID")   { uiShowAttackResult(opponentIds[0], 2, 3, RESULT_INVALID); }
    else if (demo == "DEMO MSG")       { uiShowMessage("Demo message"); }
    else Serial.println("DEMO options: LOADOUT, TURN, MENU, WAIT, HIT, MISS, SUNK, INVALID, MINE, BLOCKED, UNKNOWN, SONAR, SALVO, MSG");
    return;
  }
#endif

  String upper = line;
  upper.toUpperCase();

  if (upper == "CONFIG") { printConfig(); return; }
  if (upper == "STATUS") { printStatus(); return; }
  if (upper == "HELP")   { printHelp();   return; }
  if (upper.startsWith("LOADOUT")) { handleLoadoutCommand(line.c_str()); return; }
  if (upper.startsWith("POWER"))   { handlePowerCommand(line.c_str());   return; }

  // Otherwise, try to parse it as a manual attack: "target x y", e.g. "2 3 4"
  int target, x, y;
  if (sscanf(line.c_str(), "%d %d %d", &target, &x, &y) == 3) {
    attemptManualAttack((uint8_t)target, (uint8_t)x, (uint8_t)y);
    return;
  }

  Serial.println("Unknown command. Try CONFIG, STATUS, HELP, LOADOUT, POWER, or 'target x y' e.g. '2 3 4'");
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
    switch (myShips[s].orientation) {
      case ORIENT_HORIZONTAL: Serial.println(" HORIZONTAL"); break;
      case ORIENT_VERTICAL:   Serial.println(" VERTICAL"); break;
      case ORIENT_DIAG_DOWN:  Serial.println(" DIAGONAL (down-right)"); break;
      case ORIENT_DIAG_UP:    Serial.println(" DIAGONAL (up-right)"); break;
      default:                Serial.println(" INVALID ORIENTATION"); break;
    }
  }
  Serial.print("Loadout: ");
  if (loadoutLocked) { printLoadout(myLoadout); Serial.print("  (byte 0x"); Serial.print(myLoadout, HEX); Serial.println(")"); }
  else Serial.println("not chosen yet");
  Serial.println("==============");
}

void printStatus() {
  Serial.println("=== STATUS ===");
  Serial.print("Registered with Central: "); Serial.println(registrationConfirmed ? "YES" : "NO (retrying...)");
  Serial.print("Round state: "); Serial.println(roundStateName(roundState));
  if (loadoutLocked) {
    Serial.print("Loadout: "); printLoadout(myLoadout); Serial.println();
    Serial.print("Uses left:");
    for (int p = 1; p <= 7; p++) if (p != POWER_SHIELD && (myLoadout & LOADOUT_BIT(p))) {
      Serial.print(" "); Serial.print(POWER_SPECS[specIndex(p)].name); Serial.print(" x"); Serial.print(usesLeft[p]);
    }
    Serial.println("  (Shield unlimited)");
  } else {
    Serial.println("Loadout: NOT CHOSEN YET - nothing is sent to Central until it is locked in.");
  }
  if (roundState == STATE_RUNNING) {
    Serial.print("Current turn: TEAM "); Serial.println(currentTurnTeam);
    Serial.println(currentTurnTeam == MY_TEAM_ID ? ">>> IT IS YOUR TURN <<<" : "(waiting for your turn)");
  }
  Serial.println("==============");
}

void printHelp() {
  Serial.println("Commands: CONFIG, STATUS, HELP, 'target x y' to Single-Strike (e.g. 2 3 4)");
  Serial.println("  LOADOUT 1 7 5   pick powers by id (Sonar 1, Salvo 2, Mine 3, Repair 4, Smoke 5, Double 7) and lock in");
  Serial.println("  POWER SONAR 2 3 3 | SALVO t x1 y1 x2 y2 x3 y3 | DOUBLE t x1 y1 x2 y2 | MINE x y | REPAIR x y | SHIELD x y | SMOKE");
#ifdef USE_VIRTUAL_DISPLAY
  Serial.println("Virtual display only: DEMO LOADOUT | TURN | MENU | WAIT | HIT | MISS | SUNK | INVALID | MINE | BLOCKED | UNKNOWN | SONAR | SALVO | MSG");
#endif
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
// Screens, in the order a team meets them:
//   LOADOUT   before registering: tap 2 attack powers + 1 defence power, LOCK IN
//   MESSAGE / WAITING   while connecting, waiting for the start, or for another team
//   ACTION    on your turn: the Single Strike (target + cell + ATTACK), or - after
//             POWERS -> pick one - the same screen set up for that power
//   POWER MENU   the powers you can use right now, with the uses left
//   BANNER    the result of a shot or a power, shown for a moment
//
// Only game logic talks to this layer, and only through uiInit() / uiDrawLoadout() /
// uiShowMessage() / uiShowYourTurn() / uiShowWaiting() / uiShowAttackResult() /
// uiShowPowerResult(). Screen is assumed 240x320 portrait. If your display is
// mounted upside down, change tft.setRotation(0) in uiInit() to 2.
//
// A result banner stays on screen for a moment before the next screen is drawn.
// Central sends the result and the next turn update in quick succession, so
// without this the result would flash by too fast to read.

#define UI_RESULT_DISPLAY_MS 1500
#define UI_POWER_RESULT_MS   3500   // sonar / salvo / double results need longer to read

// --- ACTION screen layout (strike and every power that picks cells) ---
#define UI_TARGET_BTN_Y   36
#define UI_TARGET_BTN_H   30
#define UI_TARGET_BTN_W   70
#define UI_TARGET_GAP     10

#define UI_GRID_X         22
#define UI_GRID_Y         74
#define UI_CELL_SIZE      28

// bottom row:  [ POWERS / BACK ]  [ ATTACK / the power's action ]
#define UI_ALT_BTN_X      10
#define UI_ALT_BTN_W      84
#define UI_ATTACK_BTN_X   104
#define UI_ATTACK_BTN_Y   280
#define UI_ATTACK_BTN_W   126
#define UI_ATTACK_BTN_H   28

// --- POWER MENU layout ---
#define UI_MENU_X         10
#define UI_MENU_W         220
#define UI_MENU_Y0        36
#define UI_MENU_H         50
#define UI_MENU_GAP       6

// --- LOADOUT screen layout ---
#define LD_BTN_W          108
#define LD_BTN_H          40
#define LD_COL1_X         8
#define LD_COL2_X         124
#define LD_ATK_Y1         56
#define LD_ATK_Y2         100
#define LD_DEF_Y          158
#define LD_OK_X           40
#define LD_OK_Y           264
#define LD_OK_W           160
#define LD_OK_H           40

// The 7x7 grid must fit between the target buttons and the bottom buttons on the 240x320 screen.
static_assert(UI_GRID_X + GRID_SIZE * UI_CELL_SIZE <= 240, "grid is too wide for the screen");
static_assert(UI_GRID_Y + GRID_SIZE * UI_CELL_SIZE < UI_ATTACK_BTN_Y, "grid overlaps the bottom buttons");
static_assert(UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H <= 320, "bottom buttons are off the screen");
static_assert(UI_ALT_BTN_X + UI_ALT_BTN_W < UI_ATTACK_BTN_X, "bottom buttons overlap");
static_assert(UI_ATTACK_BTN_X + UI_ATTACK_BTN_W <= 240, "action button is off the screen");
static_assert(UI_MENU_Y0 + 3 * (UI_MENU_H + UI_MENU_GAP) + UI_MENU_H < UI_ATTACK_BTN_Y, "power menu overlaps the BACK button");
static_assert(LD_COL2_X + LD_BTN_W <= 240, "loadout buttons are too wide");
static_assert(LD_OK_Y + LD_OK_H <= 320, "LOCK IN button is off the screen");

// ids for uiLastScreen (what is on screen, so uiRedraw() can repaint it)
#define SCR_NONE     0
#define SCR_ACTION   1
#define SCR_WAITING  2
#define SCR_MESSAGE  3
#define SCR_LOADOUT  4
#define SCR_MENU     5

#define UIM_STRIKE   0     // the ACTION screen is set up for the Single Strike
#define UIM_POWER    1     // ... or for the power in uiPower

unsigned long uiResultUntil = 0;   // while millis() < this, a result banner is showing
bool uiBannerShowing = false;      // a banner was drawn and the screen after it has not been drawn yet
uint8_t uiPendingScreen = 0;       // 0 = none, else SCR_ACTION / SCR_WAITING / SCR_MESSAGE
uint8_t uiPendingWaitingTeam = 0;
char uiPendingMessage[40] = "";

uint8_t uiLastScreen = SCR_NONE;
uint8_t uiLastWaitingTeam = 0;
char uiLastMessage[40] = "";

uint8_t uiMode = UIM_STRIKE;
uint8_t uiPower = 0;               // the power being set up (UIM_POWER)
uint8_t uiSelTarget = 0;           // 0 = no target chosen yet
uint8_t uiSelCount = 0;            // cells chosen so far
int8_t uiSelX[3] = {0, 0, 0}, uiSelY[3] = {0, 0, 0};

bool ldArmed = false;              // LOCK IN was tapped once; tap again to confirm
unsigned long ldArmedUntil = 0;

static const uint8_t LD_ATTACK_IDS[4] = { POWER_SONAR, POWER_SALVO, POWER_MINE, POWER_DOUBLE };
static const uint8_t LD_DEFENCE_IDS[2] = { POWER_SMOKE, POWER_REPAIR };

// --- small helpers ---

static bool inBox(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

static void uiCenterText(const char *s, int y, uint8_t size, uint16_t fg, uint16_t bg) {
  tft.setTextSize(size);
  tft.setTextColor(fg, bg);
  int16_t tw = tft.textWidth(s);
  tft.setCursor((240 - tw) / 2, y);
  tft.print(s);
}

// One touch per tap (the panel reports a held finger over and over).
static bool uiReadTouch(uint16_t *tx, uint16_t *ty) {
#if defined(USE_VIRTUAL_DISPLAY) || defined(TOUCH_CS)
  if (!tft.getTouch(tx, ty)) return false;
#else
  return false;  // touch not compiled into TFT_eSPI - see the TOUCH_CS note above
#endif
  static unsigned long lastTouchMillis = 0;
  if (millis() - lastTouchMillis < 250) return false;  // simple debounce
  lastTouchMillis = millis();
  return true;
}

void computeOpponentIds() {
  int n = 0;
  for (uint8_t t = 1; t <= MAX_TEAMS; t++) {
    if (t != MY_TEAM_ID) opponentIds[n++] = t;
  }
}

void uiInit() {
  tft.init();
  tft.setRotation(0);  // portrait - change to 2 if mounted upside down
#ifndef USE_VIRTUAL_DISPLAY
  #ifdef TOUCH_CS
  tft.setTouch(touchCalData);
  #else
  Serial.println("WARNING: TOUCH_CS is not defined in TFT_eSPI's User_Setup.h - touch is DISABLED.");
  #endif
#else
  tft.setTouch(touchCalData);
#endif
  tft.fillScreen(TFT_BLACK);
  computeOpponentIds();
}

// --- public interface (called from game logic / packet handlers) ---

void uiShowMessage(const char *msg) {
  Serial.print("[UI] "); Serial.println(msg);
  if (millis() < uiResultUntil) {
    uiPendingScreen = SCR_MESSAGE;
    strncpy(uiPendingMessage, msg, sizeof(uiPendingMessage) - 1);
    uiPendingMessage[sizeof(uiPendingMessage) - 1] = '\0';
    return;
  }
  uiDrawMessage(msg);
}

void uiShowYourTurn() {
  if (millis() < uiResultUntil) {
    uiPendingScreen = SCR_ACTION;
    return;
  }
  if (uiMyTurnShown) return;   // a repeated turn update must not wipe a half-made selection
  uiDrawYourTurn();
}

void uiShowWaiting(uint8_t currentTeam) {
  if (millis() < uiResultUntil) {
    uiPendingScreen = SCR_WAITING;
    uiPendingWaitingTeam = currentTeam;
    return;
  }
  uiDrawWaiting(currentTeam);
}

void uiShowAttackResult(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result) {
  uiDrawResultBanner(targetTeam, x, y, result);
  uiResultUntil = millis() + UI_RESULT_DISPLAY_MS;
  uiBannerShowing = true;
  uiSendLockUntil = 0;
}

void uiShowPowerResult(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]) {
  uiDrawPowerBanner(power, status, target, count, res);
  bool longRead = (status != PWR_STATUS_REJECTED) &&
                  (power == POWER_SONAR || power == POWER_SALVO || power == POWER_DOUBLE);
  uiResultUntil = millis() + (longRead ? UI_POWER_RESULT_MS : UI_RESULT_DISPLAY_MS + 300);
  uiBannerShowing = true;
  uiSendLockUntil = 0;
}

// Called every loop() iteration: finishes a result banner, and reads touches.
void uiTick() {
  if (!loadoutLocked) {            // still choosing powers
    uiPollLoadoutTouch();
    return;
  }

  if (uiBannerShowing && millis() >= uiResultUntil) {
    uiBannerShowing = false;
    uint8_t screen = uiPendingScreen;
    uiPendingScreen = 0;
    if (screen == SCR_ACTION) uiDrawYourTurn();
    else if (screen == SCR_WAITING) uiDrawWaiting(uiPendingWaitingTeam);
    else if (screen == SCR_MESSAGE) uiDrawMessage(uiPendingMessage);
    else if (roundState == STATE_RUNNING && currentTurnTeam == MY_TEAM_ID) uiDrawYourTurn();   // rejected move: still my turn
    else if (roundState == STATE_RUNNING) uiDrawWaiting(currentTurnTeam);
  }

  bool interactive = (roundState == STATE_RUNNING && currentTurnTeam == MY_TEAM_ID
                      && !uiBannerShowing && uiPendingScreen == 0
                      && (uiLastScreen == SCR_ACTION || uiLastScreen == SCR_MENU));
  if (interactive) uiPollTouch();
}

// Repaint whatever full screen was last showing (no-op if none yet).
void uiRedraw() {
  if (uiLastScreen == SCR_LOADOUT) uiDrawLoadout();
  else if (uiLastScreen == SCR_ACTION) uiDrawActionScreen();
  else if (uiLastScreen == SCR_MENU) uiDrawPowerMenu();
  else if (uiLastScreen == SCR_WAITING) uiDrawWaiting(uiLastWaitingTeam);
  else if (uiLastScreen == SCR_MESSAGE) {
    char msg[40];
    strncpy(msg, uiLastMessage, sizeof(msg));
    msg[sizeof(msg) - 1] = '\0';
    uiDrawMessage(msg);
  }
}

// --- simple full screens ---

void uiDrawMessage(const char *msg) {
  uiLastScreen = SCR_MESSAGE;
  uiMyTurnShown = false;
  strncpy(uiLastMessage, msg, sizeof(uiLastMessage) - 1);
  uiLastMessage[sizeof(uiLastMessage) - 1] = '\0';
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 140);
  tft.print(msg);
}

void uiDrawWaiting(uint8_t currentTeam) {
  uiLastScreen = SCR_WAITING;
  uiMyTurnShown = false;
  uiLastWaitingTeam = currentTeam;
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

// --- result banners ---

void uiDrawResultBanner(uint8_t targetTeam, uint8_t x, uint8_t y, uint8_t result) {
  uiMyTurnShown = false;
  uint16_t bg;
  const char *label;
  const char *extra = "";
  switch (result) {
    case RESULT_MISS:    bg = TFT_BLUE;     label = "MISS";        break;
    case RESULT_HIT:     bg = TFT_ORANGE;   label = "HIT";         break;
    case RESULT_SUNK:    bg = TFT_RED;      label = "ELIMINATED!"; break;
    case RESULT_MINE:    bg = TFT_MAROON;   label = "MINE!";       extra = "YOU LOSE NEXT TURN";  break;
    case RESULT_BLOCKED: bg = TFT_PURPLE;   label = "BLOCKED";     extra = "SHIELD ABSORBED IT";  break;
    case RESULT_UNKNOWN: bg = TFT_DARKGREY; label = "NO REPORT";   extra = "SMOKE SCREEN";        break;
    default:             bg = TFT_DARKGREY; label = "INVALID";     break;
  }

  tft.fillScreen(bg);
  uiCenterText(label, 110, 3, TFT_WHITE, bg);

  char buf[32];
  snprintf(buf, sizeof(buf), "TEAM %d (%d,%d)", targetTeam, x, y);
  uiCenterText(buf, 150, 2, TFT_WHITE, bg);
  if (extra[0]) uiCenterText(extra, 182, 2, TFT_WHITE, bg);
}

static const char *uiResultName(uint8_t r) {
  switch (r) {
    case RESULT_MISS:    return "MISS";
    case RESULT_HIT:     return "HIT";
    case RESULT_SUNK:    return "SUNK";
    case RESULT_MINE:    return "MINE";
    case RESULT_BLOCKED: return "BLOCKED";
    case RESULT_UNKNOWN: return "UNKNOWN";
  }
  return "NOT FIRED";
}

static const char *uiRejectHint(uint8_t power) {
  switch (power) {
    case POWER_SONAR:
    case POWER_SALVO:
    case POWER_DOUBLE: return "Cell already shot? Target out?";
    case POWER_MINE:   return "Mine still active? Cell shot?";
    case POWER_REPAIR: return "That cell is not damaged";
    case POWER_SHIELD: return "No Shield two turns in a row";
    case POWER_SMOKE:  return "Smoke can't be used now";
  }
  return "Central refused it";
}

// The result of a power-up. `res` holds Salvo / Double results in the order they were sent.
void uiDrawPowerBanner(uint8_t power, uint8_t status, uint8_t target, uint8_t count, const uint8_t res[3]) {
  uiMyTurnShown = false;
  const int si = specIndex(power);
  const char *name = (si >= 0) ? POWER_SPECS[si].name : "POWER";
  char buf[32];
  uint16_t bg = TFT_DARKGREEN;

  if (status == PWR_STATUS_REJECTED) {
    bg = TFT_DARKGREY;
    tft.fillScreen(bg);
    uiCenterText("REJECTED", 70, 3, TFT_WHITE, bg);
    uiCenterText(name, 110, 2, TFT_WHITE, bg);
    uiCenterText("Turn NOT used.", 150, 2, TFT_WHITE, bg);
    uiCenterText(uiRejectHint(power), 186, 1, TFT_WHITE, bg);
    return;
  }

  switch (power) {
    case POWER_SONAR:
      if (status == PWR_STATUS_JAMMED) {
        bg = TFT_DARKGREY;
        tft.fillScreen(bg);
        uiCenterText("SONAR", 70, 3, TFT_WHITE, bg);
        uiCenterText("JAMMED", 120, 3, TFT_WHITE, bg);
        uiCenterText("Smoke blocked the ping", 170, 1, TFT_WHITE, bg);
      } else {
        bg = TFT_DARKCYAN;
        tft.fillScreen(bg);
        uiCenterText("SONAR", 70, 3, TFT_WHITE, bg);
        snprintf(buf, sizeof(buf), "TEAM %d around (%d,%d)", target, pendingX[0], pendingY[0]);
        uiCenterText(buf, 114, 1, TFT_WHITE, bg);
        snprintf(buf, sizeof(buf), count == 1 ? "%d SHIP CELL" : "%d SHIP CELLS", count);
        uiCenterText(buf, 140, 3, TFT_YELLOW, bg);
      }
      break;

    case POWER_SALVO:
    case POWER_DOUBLE: {
      bg = TFT_BLUE;
      for (int i = 0; i < pendingCells && i < 3; i++) {   // the most serious result sets the colour
        if (res[i] == RESULT_SUNK) bg = TFT_RED;
        else if (res[i] == RESULT_HIT && bg != TFT_RED) bg = TFT_ORANGE;
        else if (res[i] == RESULT_MINE && bg != TFT_RED && bg != TFT_ORANGE) bg = TFT_MAROON;
        else if ((res[i] == RESULT_UNKNOWN || res[i] == RESULT_BLOCKED) && bg == TFT_BLUE) bg = TFT_DARKGREY;
      }
      tft.fillScreen(bg);
      uiCenterText(name, 40, 3, TFT_WHITE, bg);
      snprintf(buf, sizeof(buf), "at TEAM %d", target);
      uiCenterText(buf, 80, 1, TFT_WHITE, bg);
      for (int i = 0; i < pendingCells && i < 3; i++) {
        snprintf(buf, sizeof(buf), "(%d,%d) %s", pendingX[i], pendingY[i], uiResultName(res[i]));
        uiCenterText(buf, 110 + i * 30, 2, TFT_WHITE, bg);
      }
      break;
    }

    case POWER_MINE:
      tft.fillScreen(bg);
      uiCenterText("MINE SET", 80, 3, TFT_WHITE, bg);
      snprintf(buf, sizeof(buf), "at (%d,%d)", pendingX[0], pendingY[0]);
      uiCenterText(buf, 130, 2, TFT_YELLOW, bg);
      uiCenterText("Only you know where it is.", 170, 1, TFT_WHITE, bg);
      break;

    case POWER_REPAIR:
      tft.fillScreen(bg);
      uiCenterText("REPAIRED", 80, 3, TFT_WHITE, bg);
      snprintf(buf, sizeof(buf), "(%d,%d) is intact", pendingX[0], pendingY[0]);
      uiCenterText(buf, 130, 2, TFT_YELLOW, bg);
      break;

    case POWER_SMOKE:
      tft.fillScreen(bg);
      uiCenterText("SMOKE UP", 80, 3, TFT_WHITE, bg);
      uiCenterText("Attacks on you are hidden", 130, 1, TFT_WHITE, bg);
      uiCenterText("until your next turn.", 144, 1, TFT_WHITE, bg);
      break;

    case POWER_SHIELD:
      tft.fillScreen(bg);
      uiCenterText("SHIELD UP", 80, 3, TFT_WHITE, bg);
      snprintf(buf, sizeof(buf), "3x3 around (%d,%d)", pendingX[0], pendingY[0]);
      uiCenterText(buf, 130, 2, TFT_YELLOW, bg);
      uiCenterText("until your next turn.", 170, 1, TFT_WHITE, bg);
      break;

    default:
      tft.fillScreen(bg);
      uiCenterText("DONE", 120, 3, TFT_WHITE, bg);
      break;
  }
}

// ---------------------------------------------------------------------
// ACTION screen: the Single Strike, or a power being set up
// ---------------------------------------------------------------------

static int uiActionCells() {
  if (uiMode == UIM_STRIKE) return 1;
  int si = specIndex(uiPower);
  return (si >= 0) ? POWER_SPECS[si].cells : 0;
}

static bool uiActionNeedsTarget() {
  if (uiMode == UIM_STRIKE) return true;
  int si = specIndex(uiPower);
  return si >= 0 && POWER_SPECS[si].needTarget;
}

static bool uiActionOwnGrid() {
  if (uiMode == UIM_STRIKE) return false;
  int si = specIndex(uiPower);
  return si >= 0 && POWER_SPECS[si].ownGrid;
}

static const char *uiActionLabel() {
  if (uiMode == UIM_STRIKE) return "ATTACK";
  int si = specIndex(uiPower);
  return (si >= 0) ? POWER_SPECS[si].action : "GO";
}

static bool uiActionReady() {
  if (uiActionNeedsTarget() && uiSelTarget == 0) return false;
  if (uiSelCount != uiActionCells()) return false;
  if (uiMode == UIM_POWER && uiPower == POWER_SALVO) {
    uint8_t xs[3], ys[3];
    for (int i = 0; i < 3; i++) { xs[i] = (uint8_t)uiSelX[i]; ys[i] = (uint8_t)uiSelY[i]; }
    if (!cellsFormLine(xs, ys, 3)) return false;
  }
  return true;
}

// May this cell be picked in the current mode?
static bool uiCellAllowed(int c, int r) {
  if (uiMode != UIM_POWER) return true;
  switch (uiPower) {
    case POWER_MINE:   return !ownShip[r][c];      // an empty cell of mine
    case POWER_REPAIR: return ownShip[r][c];       // a hit cell can only be a ship cell
    case POWER_SHIELD: return c >= 1 && c <= GRID_SIZE - 2 && r >= 1 && r <= GRID_SIZE - 2;   // centre of a full 3x3
  }
  return true;
}

static void uiToggleCell(int c, int r) {
  const int maxCells = uiActionCells();
  if (maxCells == 1) {                  // single pick: tapping another cell replaces it
    uiSelX[0] = c; uiSelY[0] = r; uiSelCount = 1;
    return;
  }
  for (int i = 0; i < uiSelCount; i++) {
    if (uiSelX[i] == c && uiSelY[i] == r) {          // tapping a picked cell un-picks it
      for (int j = i; j < uiSelCount - 1; j++) { uiSelX[j] = uiSelX[j + 1]; uiSelY[j] = uiSelY[j + 1]; }
      uiSelCount--;
      return;
    }
  }
  if (uiSelCount == maxCells) {                      // full: drop the oldest pick
    for (int j = 0; j < uiSelCount - 1; j++) { uiSelX[j] = uiSelX[j + 1]; uiSelY[j] = uiSelY[j + 1]; }
    uiSelCount--;
  }
  uiSelX[uiSelCount] = c; uiSelY[uiSelCount] = r; uiSelCount++;
}

void uiDrawYourTurn() {
  uiMode = UIM_STRIKE;
  uiPower = 0;
  uiSelTarget = 0;
  uiSelCount = 0;
  uiMyTurnShown = true;
  uiDrawActionScreen();
}

static void uiStartPower(uint8_t power) {
  uiMode = UIM_POWER;
  uiPower = power;
  uiSelTarget = 0;
  uiSelCount = 0;
  uiDrawActionScreen();
}

void uiDrawActionScreen() {
  uiLastScreen = SCR_ACTION;
  const bool strike = (uiMode == UIM_STRIKE);
  const int si = strike ? -1 : specIndex(uiPower);
  const bool needTarget = uiActionNeedsTarget();
  const bool noGrid = (uiActionCells() == 0);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(strike ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print(strike ? "YOUR TURN" : (si >= 0 ? POWER_SPECS[si].name : "POWER"));

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  if (needTarget) {
    tft.setCursor(10, UI_TARGET_BTN_Y - 10);
    tft.print("Select target:");
    for (int i = 0; i < 3; i++) drawTargetButton(i, opponentIds[i] == uiSelTarget);
  } else {
    const char *l1 = "", *l2 = "";
    switch (uiPower) {
      case POWER_MINE:   l1 = "Tap an EMPTY cell of YOUR board.";   l2 = "Hidden until someone shoots it."; break;
      case POWER_REPAIR: l1 = "Tap one of YOUR damaged cells."; l2 = "(teal = your ships)"; break;
      case POWER_SHIELD: l1 = "Tap the CENTRE of a 3x3 block on";   l2 = "YOUR board (inner cells only)."; break;
      case POWER_SMOKE:  l1 = "No cells to pick."; break;
    }
    tft.setCursor(10, 30); tft.print(l1);
    tft.setCursor(10, 42); tft.print(l2);
  }

  if (noGrid) {                                    // smoke screen: just an explanation
    tft.setTextSize(2);
    tft.setCursor(10, 96);  tft.print("SMOKE SCREEN");
    tft.setTextSize(1);
    tft.setCursor(10, 130); tft.print("Until your next turn, every attack");
    tft.setCursor(10, 144); tft.print("on you is hidden from the attacker");
    tft.setCursor(10, 158); tft.print("AND from the projector.");
    tft.setCursor(10, 180); tft.print("Sonar pings at you are jammed.");
    tft.setCursor(10, 194); tft.print("Using it takes your turn.");
  } else {
    drawGrid();
  }

  drawAltButton(strike ? "POWERS" : "BACK");
  drawAttackButton(uiActionReady(), uiActionLabel());
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
  tft.setCursor(x + (UI_TARGET_BTN_W - tw) / 2, y + (UI_TARGET_BTN_H - 16) / 2);
  tft.print(label);
}

static uint16_t uiCellColor(int c, int r) {
  bool sel = false;
  if (uiMode == UIM_POWER && (uiPower == POWER_SHIELD || uiPower == POWER_SONAR) && uiSelCount == 1) {
    int dx = c - uiSelX[0], dy = r - uiSelY[0];
    sel = (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1);      // show the whole 3x3 area
  } else {
    for (int i = 0; i < uiSelCount; i++) if (uiSelX[i] == c && uiSelY[i] == r) sel = true;
  }
  if (sel) return TFT_YELLOW;
  if (uiActionOwnGrid()) {
    if (c == myMineX && r == myMineY) return TFT_MAROON;
    if (ownShip[r][c]) return TFT_DARKCYAN;
  }
  return TFT_DARKGREY;
}

void drawGrid() {
  const bool own = uiActionOwnGrid();
  for (int r = 0; r < GRID_SIZE; r++) {
    for (int c = 0; c < GRID_SIZE; c++) {
      int x = UI_GRID_X + c * UI_CELL_SIZE;
      int y = UI_GRID_Y + r * UI_CELL_SIZE;
      uint16_t color = uiCellColor(c, r);
      tft.fillRect(x + 1, y + 1, UI_CELL_SIZE - 2, UI_CELL_SIZE - 2, color);
      tft.drawRect(x, y, UI_CELL_SIZE, UI_CELL_SIZE, TFT_WHITE);
      if (own && color == TFT_MAROON) {                      // my last mine (it may already be gone)
        tft.setTextColor(TFT_WHITE, TFT_MAROON);
        tft.setTextSize(2);
        tft.setCursor(x + 8, y + 6);
        tft.print("M");
      }
    }
  }
}

void drawAltButton(const char *label) {
  tft.fillRoundRect(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, 6, TFT_NAVY);
  tft.drawRoundRect(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(2);
  int16_t tw = tft.textWidth(label);
  tft.setCursor(UI_ALT_BTN_X + (UI_ALT_BTN_W - tw) / 2, UI_ATTACK_BTN_Y + 6);
  tft.print(label);
}

void drawAttackButton(bool enabled, const char *label) {
  uint16_t fillColor = enabled ? TFT_RED : TFT_DARKGREY;
  tft.fillRoundRect(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H, 6, fillColor);
  tft.drawRoundRect(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fillColor);
  tft.setTextSize(2);
  int16_t tw = tft.textWidth(label);
  tft.setCursor(UI_ATTACK_BTN_X + (UI_ATTACK_BTN_W - tw) / 2, UI_ATTACK_BTN_Y + 6);
  tft.print(label);
}

// A short note next to the heading ("Sent - waiting...", "Not allowed").
static void uiNote(const char *text) {
  tft.fillRect(125, 8, 110, 12, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(125, 12);
  tft.print(text);
}

static void uiRefreshAction() {
  drawGrid();
  drawAttackButton(uiActionReady(), uiActionLabel());
}

// ---------------------------------------------------------------------
// POWER MENU
// ---------------------------------------------------------------------

// The powers this team can pick from: its 3 chosen ones, then the Shield. Returns how many.
static int uiMenuItems(uint8_t items[4]) {
  static const uint8_t order[6] = { POWER_SONAR, POWER_SALVO, POWER_MINE, POWER_DOUBLE, POWER_SMOKE, POWER_REPAIR };
  int n = 0;
  for (int i = 0; i < 6; i++) if ((myLoadout & LOADOUT_BIT(order[i])) && n < 3) items[n++] = order[i];
  items[n++] = POWER_SHIELD;
  return n;
}

static bool uiPowerUsable(uint8_t id) {
  return id == POWER_SHIELD || usesLeft[id] > 0;     // the shield is unlimited
}

void uiDrawPowerMenu() {
  uiLastScreen = SCR_MENU;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("POWERS");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 26);
  tft.print("Using a power takes your turn.");

  uint8_t items[4];
  const int n = uiMenuItems(items);
  for (int i = 0; i < n; i++) {
    const int si = specIndex(items[i]);
    const bool usable = uiPowerUsable(items[i]);
    const int y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP);
    const uint16_t fill = usable ? TFT_NAVY : TFT_DARKGREY;
    tft.fillRoundRect(UI_MENU_X, y, UI_MENU_W, UI_MENU_H, 8, fill);
    tft.drawRoundRect(UI_MENU_X, y, UI_MENU_W, UI_MENU_H, 8, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, fill);
    tft.setTextSize(2);
    tft.setCursor(UI_MENU_X + 10, y + 8);
    tft.print(POWER_SPECS[si].name);
    tft.setTextSize(1);
    tft.setCursor(UI_MENU_X + 10, y + 32);
    tft.print(POWER_SPECS[si].hint);

    char buf[12];
    if (items[i] == POWER_SHIELD) {
      tft.setCursor(UI_MENU_X + 150, y + 20);
      tft.print(lastMoveWasShield ? "COOLDOWN" : "ALWAYS");
    } else {
      tft.setTextSize(3);
      snprintf(buf, sizeof(buf), "%d", usesLeft[items[i]]);
      tft.setCursor(UI_MENU_X + 192, y + 8);
      tft.print(buf);
      tft.setTextSize(1);
      tft.setCursor(UI_MENU_X + 184, y + 36);
      tft.print("left");
    }
  }
  drawAltButton("BACK");
}

static void uiTouchMenu(int tx, int ty) {
  if (inBox(tx, ty, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H)) { uiDrawYourTurn(); return; }
  uint8_t items[4];
  const int n = uiMenuItems(items);
  for (int i = 0; i < n; i++) {
    const int y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP);
    if (inBox(tx, ty, UI_MENU_X, y, UI_MENU_W, UI_MENU_H)) {
      if (uiPowerUsable(items[i])) uiStartPower(items[i]);
      return;
    }
  }
}

// ---------------------------------------------------------------------
// touch handling on my turn
// ---------------------------------------------------------------------

static void uiSendSelection() {
  bool sent;
  if (uiMode == UIM_STRIKE) {
    sent = attemptManualAttack(uiSelTarget, (uint8_t)uiSelX[0], (uint8_t)uiSelY[0]);
  } else {
    uint8_t xs[3] = {0, 0, 0}, ys[3] = {0, 0, 0};
    for (int i = 0; i < uiSelCount && i < 3; i++) { xs[i] = (uint8_t)uiSelX[i]; ys[i] = (uint8_t)uiSelY[i]; }
    sent = attemptPower(uiPower, uiActionNeedsTarget() ? uiSelTarget : 0, xs, ys);
  }
  uiNote(sent ? "Sent - waiting..." : "Not allowed");
}

void uiPollTouch() {
  uint16_t tx, ty;
  if (!uiReadTouch(&tx, &ty)) return;
  if (millis() < uiSendLockUntil) return;          // a move is already on its way to Central

  if (uiLastScreen == SCR_MENU) { uiTouchMenu(tx, ty); return; }

  // target buttons
  if (uiActionNeedsTarget()) {
    for (int i = 0; i < 3; i++) {
      int x = 5 + i * (UI_TARGET_BTN_W + UI_TARGET_GAP);
      if (inBox(tx, ty, x, UI_TARGET_BTN_Y, UI_TARGET_BTN_W, UI_TARGET_BTN_H)) {
        uiSelTarget = opponentIds[i];
        for (int j = 0; j < 3; j++) drawTargetButton(j, opponentIds[j] == uiSelTarget);
        drawAttackButton(uiActionReady(), uiActionLabel());
        return;
      }
    }
  }

  // grid cells
  if (uiActionCells() > 0 && inBox(tx, ty, UI_GRID_X, UI_GRID_Y, GRID_SIZE * UI_CELL_SIZE, GRID_SIZE * UI_CELL_SIZE)) {
    int c = (tx - UI_GRID_X) / UI_CELL_SIZE;
    int r = (ty - UI_GRID_Y) / UI_CELL_SIZE;
    if (uiCellAllowed(c, r)) {
      uiToggleCell(c, r);
      uiRefreshAction();
    }
    return;
  }

  // POWERS / BACK
  if (inBox(tx, ty, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H)) {
    uiDrawPowerMenu();
    return;
  }

  // ATTACK / the power's action
  if (inBox(tx, ty, UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H)) {
    if (uiActionReady()) uiSendSelection();
  }
}

// ---------------------------------------------------------------------
// LOADOUT screen (before registering)
// ---------------------------------------------------------------------

static bool ldValid() {
  return loadoutBitCount(ldAttack) == 2 && ldDefence != 0;
}

static void drawLoadoutButton(int x, int y, uint8_t id, bool selected) {
  const int si = specIndex(id);
  const uint16_t fill = selected ? TFT_YELLOW : TFT_NAVY;
  const uint16_t text = selected ? TFT_BLACK : TFT_WHITE;
  tft.fillRoundRect(x, y, LD_BTN_W, LD_BTN_H, 6, fill);
  tft.drawRoundRect(x, y, LD_BTN_W, LD_BTN_H, 6, TFT_WHITE);
  tft.setTextColor(text, fill);
  tft.setTextSize(2);
  tft.setCursor(x + 8, y + 6);
  tft.print(POWER_SPECS[si].name);
  tft.setTextSize(1);
  tft.setCursor(x + 8, y + 26);
  tft.print(POWER_SPECS[si].hint);
}

static void drawLoadoutConfirm() {
  uint16_t fill;
  const char *label;
  if (!ldValid())    { fill = TFT_DARKGREY;  label = "PICK 2 + 1"; }
  else if (ldArmed)  { fill = TFT_RED;       label = "TAP AGAIN"; }
  else               { fill = TFT_DARKGREEN; label = "LOCK IN"; }
  tft.fillRoundRect(LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H, 8, fill);
  tft.drawRoundRect(LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H, 8, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fill);
  tft.setTextSize(2);
  int16_t tw = tft.textWidth(label);
  tft.setCursor(LD_OK_X + (LD_OK_W - tw) / 2, LD_OK_Y + 12);
  tft.print(label);
}

static void drawLoadoutStatus() {
  char buf[40];
  snprintf(buf, sizeof(buf), "Picked: %d/2 attack, %d/1 defence   ", loadoutBitCount(ldAttack), ldDefence ? 1 : 0);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 214);
  tft.print(buf);
}

void uiDrawLoadout() {
  uiLastScreen = SCR_LOADOUT;
  uiMyTurnShown = false;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 8);
  tft.print("CHOOSE POWERS");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 30);
  tft.print("Always yours: STRIKE + SHIELD");
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 44);
  tft.print("ATTACK - pick 2 (2 uses each)");
  for (int i = 0; i < 4; i++) {
    int x = (i % 2) ? LD_COL2_X : LD_COL1_X;
    int y = (i / 2) ? LD_ATK_Y2 : LD_ATK_Y1;
    drawLoadoutButton(x, y, LD_ATTACK_IDS[i], (ldAttack & (1u << LD_ATTACK_IDS[i])) != 0);
  }
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 146);
  tft.print("DEFENCE - pick 1 (2 uses each)");
  for (int j = 0; j < 2; j++) {
    drawLoadoutButton(j ? LD_COL2_X : LD_COL1_X, LD_DEF_Y, LD_DEFENCE_IDS[j], ldDefence == LD_DEFENCE_IDS[j]);
  }
  drawLoadoutStatus();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 232);
  tft.print("Locked in = no changes this match.");
  drawLoadoutConfirm();
}

void uiPollLoadoutTouch() {
  if (ldArmed && millis() > ldArmedUntil) { ldArmed = false; drawLoadoutConfirm(); }

  uint16_t tx, ty;
  if (!uiReadTouch(&tx, &ty)) return;

  for (int i = 0; i < 4; i++) {
    int x = (i % 2) ? LD_COL2_X : LD_COL1_X;
    int y = (i / 2) ? LD_ATK_Y2 : LD_ATK_Y1;
    if (!inBox(tx, ty, x, y, LD_BTN_W, LD_BTN_H)) continue;
    const uint8_t bit = (uint8_t)(1u << LD_ATTACK_IDS[i]);
    if (ldAttack & bit) ldAttack &= (uint8_t)~bit;                       // un-pick
    else if (loadoutBitCount(ldAttack) < 2) ldAttack |= bit;             // pick (max 2)
    ldArmed = false;
    uiDrawLoadout();
    return;
  }
  for (int j = 0; j < 2; j++) {
    int x = j ? LD_COL2_X : LD_COL1_X;
    if (!inBox(tx, ty, x, LD_DEF_Y, LD_BTN_W, LD_BTN_H)) continue;
    ldDefence = (ldDefence == LD_DEFENCE_IDS[j]) ? 0 : LD_DEFENCE_IDS[j];   // pick one, tap again to clear
    ldArmed = false;
    uiDrawLoadout();
    return;
  }
  if (inBox(tx, ty, LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H) && ldValid()) {
    if (!ldArmed) {
      ldArmed = true;
      ldArmedUntil = millis() + 3000;
      drawLoadoutConfirm();
    } else {
      uint8_t loadout = LOADOUT_SHIELD_BIT;
      for (int i = 0; i < 4; i++) if (ldAttack & (1u << LD_ATTACK_IDS[i])) loadout |= LOADOUT_BIT(LD_ATTACK_IDS[i]);
      loadout |= LOADOUT_BIT(ldDefence);
      ldArmed = false;
      lockLoadout(loadout);
    }
  }
}
