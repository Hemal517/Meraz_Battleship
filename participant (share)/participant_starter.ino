/* =====================================================================
   MERAZ BATTLESHIP - PARTICIPANT SKELETON

   Your job: make this ESP32 talk to the Central node over ESP-NOW and
   play the game. The radio setup is YOURS to figure out - that's the
   point of the challenge.

   Read PARTICIPANT_GUIDE.md first. It tells you everything Central
   expects: the exact message formats, the rules, and the game flow.
   This file only gives you the message formats as C structs (so you
   don't fight with byte packing) plus a TODO list.

   Useful places to look:
     - Arduino IDE: File > Examples > ESP32 > ESPNow
     - Espressif's ESP-NOW documentation
   Gotcha: the ESP-NOW callback signatures changed between ESP32
   Arduino core 2.x and 3.x. Check which core you have installed.
   ===================================================================== */

#include <WiFi.h>
#include <esp_now.h>

// =====================================================================
// CONFIGURATION - the organizer will tell you these values
// =====================================================================
#define MY_TEAM_ID 1                       // 1..4
const char *MY_TEAM_NAME = "TEAM 1";       // max 19 characters
uint8_t centralMac[6] = {0, 0, 0, 0, 0, 0};  // Central's MAC address
#define ESPNOW_CHANNEL 1

// =====================================================================
// MESSAGE FORMATS (given - these must match Central byte for byte)
// =====================================================================
#define GRID_SIZE 5

#define ORIENT_HORIZONTAL 0   // grows rightward    (dx=+1, dy= 0)
#define ORIENT_VERTICAL   1   // grows downward     (dx= 0, dy=+1)
#define ORIENT_DIAG_DOWN  2   // grows down-right   (dx=+1, dy=+1)
#define ORIENT_DIAG_UP    3   // grows up-right     (dx=+1, dy=-1)

typedef struct __attribute__((packed)) {
  uint8_t ship_len;      // 1, 3 or 5
  uint8_t start_x;       // the ship's FIRST cell
  uint8_t start_y;
  uint8_t orientation;   // ORIENT_*
} ShipPlacement;         // 4 bytes

typedef struct __attribute__((packed)) {
  uint8_t team_id;
  char team_name[20];    // null-padded
  ShipPlacement ships[3];
} RegistrationPacket;    // 33 bytes   -> you send this to Central

typedef struct __attribute__((packed)) {
  uint8_t attacker_id;   // your team id
  uint8_t target_id;     // the team you are shooting at
  uint8_t x;
  uint8_t y;
} AttackPacket;          // 4 bytes    -> you send this to Central

typedef struct __attribute__((packed)) {
  uint8_t status;        // REG_*
} RegAckPacket;          // 1 byte     <- Central replies to your registration
#define REG_OK                 0
#define REG_REJECTED           1
#define REG_RECONNECTED        2
#define REG_RECONNECT_REJECTED 3

typedef struct __attribute__((packed)) {
  uint8_t target_id;
  uint8_t x;
  uint8_t y;
  uint8_t result_code;   // RESULT_*
} FeedbackPacket;        // 4 bytes    <- Central replies to your attack
#define RESULT_MISS    0
#define RESULT_HIT     1
#define RESULT_INVALID 2
#define RESULT_SUNK    3

typedef struct __attribute__((packed)) {
  uint8_t current_turn;  // team id whose turn it is (0 = nobody)
  uint8_t round_state;   // STATE_*
} TurnUpdatePacket;      // 2 bytes    <- Central tells everyone after every move
#define STATE_SETUP    0
#define STATE_READY    1
#define STATE_RUNNING  2
#define STATE_GAMEOVER 3

// If these fail to compile, the struct layout has drifted from what
// Central expects. Don't delete them - fix the struct instead.
static_assert(sizeof(RegistrationPacket) == 33, "RegistrationPacket must be 33 bytes");
static_assert(sizeof(AttackPacket) == 4, "AttackPacket must be 4 bytes");
static_assert(sizeof(RegAckPacket) == 1, "RegAckPacket must be 1 byte");
static_assert(sizeof(FeedbackPacket) == 4, "FeedbackPacket must be 4 bytes");
static_assert(sizeof(TurnUpdatePacket) == 2, "TurnUpdatePacket must be 2 bytes");

// =====================================================================
// YOUR CODE
// =====================================================================

ShipPlacement myShips[3];   // TODO: decide where your fleet goes (see the guide's ship rules)

void setup() {
  Serial.begin(115200);

  // TODO 1: Put the radio in the mode ESP-NOW needs, then print this
  //         board's MAC address. Give that MAC to the organizer -
  //         Central only accepts registrations from known boards.
  //
  // TODO 2: Start ESP-NOW. If it fails, say so on Serial.
  //
  // TODO 3: Tell ESP-NOW about Central (it must be a known peer before
  //         you can send to it). Use centralMac and ESPNOW_CHANNEL.
  //
  // TODO 4: Register a callback for incoming messages (Central's
  //         replies and turn updates) and one for send status.
  //         Incoming messages are told apart by their LENGTH - see the guide.
  //
  // TODO 5: Fill in myShips[], build a RegistrationPacket, send it to
  //         Central, and read the RegAckPacket that comes back.
}

void loop() {
  // TODO 6: If Central hasn't accepted you yet, retry registration every
  //         couple of seconds. Don't use a long blocking delay().
  //
  // TODO 7: Play the game: when a TurnUpdatePacket says it's YOUR turn,
  //         send an AttackPacket. Read the FeedbackPacket to see what happened.
}
