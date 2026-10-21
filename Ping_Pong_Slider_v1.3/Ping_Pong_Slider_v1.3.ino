#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// Journal serie FACULTATIF. En fonctionnement normal, aucun appel USB/GAME_LOG.
// Mettre a 1 uniquement pour diagnostiquer un probleme sur le moniteur serie.
#define ENABLE_SERIAL_DEBUG 0
#if ENABLE_SERIAL_DEBUG
  #define GAME_LOG Serial
#else
  struct SilentGameLogger {
    void begin(unsigned long) {}
    template<typename... T> void print(T...) {}
    template<typename... T> void println(T...) {}
    template<typename... T> void printf(T...) {}
  };
  SilentGameLogger silentGameLogger;
  #define GAME_LOG silentGameLogger
#endif

// Meme sketch sur les deux Seeed XIAO ESP32-S3.
// Une seule paire de cartes dans la zone radio (diffusion ESP-NOW).
// Arduino-ESP32 3.x : signature moderne de esp_now_register_recv_cb.

constexpr uint8_t SDA_PIN = 5;       // XIAO D4
constexpr uint8_t SCL_PIN = 6;       // XIAO D5
constexpr uint8_t RGB_PIN = 4;
constexpr uint8_t RGB_COUNT = 2;

// Matrice PCB : SW1 (haut) O_1/I_1, SW7 (bas) O_1/I_3,
// SW3 (A) O_3/I_1. Diodes sur les boutons du PCB.
constexpr uint8_t ROW_O1 = 44;       // D7
constexpr uint8_t ROW_O2 = 2;        // D1 (SW2 droit : O_2 / I_1)
constexpr uint8_t ROW_O3 = 43;       // D6
constexpr uint8_t COL_I1 = 7;        // D8
constexpr uint8_t COL_I3 = 8;        // D9
constexpr uint8_t COL_I2 = 9;        // D10 (SW4 gauche : O_1 / I_2)
constexpr uint32_t DEBOUNCE_MS = 40;
constexpr uint32_t TX_LED_MS = 250;
constexpr uint32_t ANNOUNCE_INTERVAL_MS = 250;
constexpr uint32_t START_REPEAT_MS = 100;
constexpr uint32_t PRE_COUNTDOWN_MS = 1300;
constexpr uint8_t WIFI_CHANNEL = 1;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_NeoPixel leds(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

const uint8_t BROADCAST_MAC[6] = {255, 255, 255, 255, 255, 255};
uint8_t localMac[6] = {};
uint8_t remoteMac[6] = {};
bool remoteKnown = false;

constexpr uint32_t PROTOCOL_MAGIC = 0x35534649; // Version 3 du protocole de jeu
// Role: annonce role + etat pret ; Start: delai avant le premier "3".
enum : uint8_t { PKT_ROLE = 1, PKT_START = 3, PKT_CLAIM = 4, PKT_BALL = 5, PKT_ACK = 6, PKT_LOSS = 7, PKT_BOUNCE = 8, PKT_QUIT = 9, PKT_CLAIM_ACK = 10, PKT_LOSS_ACK = 11 };
struct __attribute__((packed)) Packet {
  uint32_t magic;
  uint8_t type;
  uint8_t role;
  uint8_t ready;
  uint32_t remainingMs;
  char text[24];
  uint32_t sequence;
  float x;
  float vx;
  float speed;
  uint16_t roundId;
  uint16_t bounces;
  uint16_t score1;
  uint16_t score2;
};
struct RxEvent {
  uint8_t mac[6];
  Packet packet;
};
QueueHandle_t rxQueue = nullptr;

struct Button {
  bool raw = false;
  bool stable = false;
  uint32_t edgeAt = 0;
  bool pressedEvent = false;
};
Button upButton, downButton, aButton, bButton, leftButton, rightButton;

// Gameplay : balle unique transferee entre les deux XIAO.
constexpr int FIELD_WIDTH = 128;
constexpr int PADDLE_WIDTH = 20;
constexpr int PADDLE_HEIGHT = 2;
constexpr int PADDLE_Y = 56;
constexpr int BALL_RADIUS = 2;
constexpr float BALL_SPEED_Y = 35.0f; // pixels/seconde
constexpr float MAX_BALL_VX = 29.0f;
constexpr float SPEED_MULTIPLIER = 1.05f;
constexpr uint8_t TOTAL_ROUNDS = 5;
constexpr uint32_t WINNER_SCREEN_MS = 3000;
constexpr int TOP_BOUNDARY_Y = 12;
constexpr uint32_t CLAIM_WAIT_MS = 220;
constexpr uint32_t RETRY_MS = 90;
constexpr uint32_t FRAME_INTERVAL_MS = 33; // ~30 FPS
constexpr float PADDLE_SPEED_PX_S = 130.0f;
float paddleX = (FIELD_WIDTH - PADDLE_WIDTH) / 2.0f;
uint32_t lastFrameAt = 0;
uint16_t roundId = 1;
uint16_t reboundCount = 0;
uint16_t score1 = 0, score2 = 0;
uint32_t totalRebounds = 0;
uint32_t matchStartedAt = 0;
uint32_t matchElapsedMs = 0;
uint32_t victoryShownAt = 0;
uint8_t matchWinner = 0;
float ballSpeed = BALL_SPEED_Y;
uint32_t quitBroadcastUntil = 0, nextQuitAt = 0;
uint8_t quittingRole = 0;
uint32_t ignoreAutoRoleUntil = 0;
uint32_t gameStartedAt = 0;

enum Phase : uint8_t { MENU, ROLE_CONFIRM, WAITING, COUNTDOWN, PLAYING, FINISHED, QUIT_CONFIRM, ABANDONED, VICTORY, STATS };
enum BallMode : uint8_t { BALL_IDLE, BALL_ACTIVE, BALL_SENDING, BALL_REMOTE };
BallMode ballMode = BALL_IDLE;
float ballX = 64, ballY = PADDLE_Y - BALL_RADIUS;
float ballVx = 0, ballVy = -BALL_SPEED_Y;
uint32_t ballSequence = 0;
uint32_t pendingSequence = 0;
float pendingX = 64, pendingVx = 0, pendingSpeed = BALL_SPEED_Y;
uint32_t nextBallRetryAt = 0;
uint32_t claimAt = 0;
bool claimAcknowledged = false;
bool lossAcknowledged = false;
bool localClaim = false, remoteClaim = false;
uint32_t nextClaimAt = 0;
uint8_t lostPlayer = 0;
uint32_t lossBroadcastUntil = 0;
uint32_t nextLossAt = 0;
uint32_t lastRoundSequence = 0;
Phase phase = MENU;
uint8_t selectedPlayer = 1;
uint8_t player = 0;
bool localReady = false;
bool remoteReady = false;
uint8_t remoteRole = 0;
uint32_t nextAnnounceAt = 0;
uint32_t nextStartAt = 0;
uint32_t redLedOffAt = 0;
uint32_t countdownAt = 0; // millis() a l'affichage du premier "3".
int8_t lastCountdownScreen = -1;

void ledsOff() {
  leds.clear();
  leds.show();
  redLedOffAt = 0;
}
void redLedPulse() {
  for (int i = 0; i < RGB_COUNT; ++i)
    leds.setPixelColor(i, leds.Color(255, 0, 0));
  leds.show();
  redLedOffAt = millis() + TX_LED_MS;
}

void textMode() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
}
void showMenu() {
  textMode();
  display.setCursor(17, 2);
  display.println("PING PONG SLIDER");
  display.setCursor(9, 21);
  display.print(selectedPlayer == 1 ? "> " : "  ");
  display.println("JOUEUR 1");
  display.setCursor(9, 35);
  display.print(selectedPlayer == 2 ? "> " : "  ");
  display.println("JOUEUR 2");
  display.setCursor(0, 55);
  display.println("A: Selectionner"); // Police par defaut : sans accents.
  display.display();
}
void showRole() {
  textMode();
  // Ligne dans la bande coloree superieure de l'OLED bicolore.
  display.setCursor(33, 2);
  display.println("Vous etes");
  display.setTextSize(2);
  display.setCursor(12, 25);
  display.print("Joueur ");
  display.println(player);
  display.setTextSize(1);
  display.setCursor(0, 55);
  display.println("A: Suivant");
  display.display();
}
void showWaiting() {
  textMode();
  display.setCursor(0, 0);
  display.print("JOUEUR ");
  display.print(player);
  display.println(" | Pret");
  display.setCursor(0, 30);
  display.println("En attente de");
  display.setCursor(0, 43);
  display.print("Joueur ");
  display.print(3 - player);
  display.println("...");
  display.display();
}
void showCountdownNumber(int number) {
  textMode();
  display.setTextSize(5);
  display.setCursor(49, 18);
  display.print(number);
  display.display();
}
void showGameStarted() {
  textMode();
  display.setTextSize(2);
  display.setCursor(28, 13);
  display.println("Partie");
  display.setCursor(16, 36);
  display.println("lancee !");
  display.display();
}
// Barre de statistiques conservee pendant le jeu et a la fin de manche.
void drawTopBar() {
  display.setTextSize(1);
  display.setCursor(1, 0);
  display.print("J"); display.print(player);
  char scoreText[24];
  snprintf(scoreText, sizeof(scoreText), "| %u-%u |",
           player == 1 ? score1 : score2,
           player == 1 ? score2 : score1);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(scoreText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - (int)w) / 2, 0);
  display.print(scoreText);
  char countText[20];
  snprintf(countText, sizeof(countText), "%lu", (unsigned long)(totalRebounds + (phase == PLAYING ? reboundCount : 0)));
  display.getTextBounds(countText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(127 - (int)w, 0);
  display.print(countText);
  display.drawFastHLine(0, 12, 128, SSD1306_WHITE);
}

void showPlayScreen() {
  textMode();
  drawTopBar();
  display.drawFastVLine(0, 13, 51, SSD1306_WHITE);
  display.drawFastVLine(127, 13, 51, SSD1306_WHITE);
  int x = constrain((int)(paddleX + 0.5f), 1, FIELD_WIDTH - 1 - PADDLE_WIDTH);
  display.fillRect(x, PADDLE_Y, PADDLE_WIDTH, PADDLE_HEIGHT, SSD1306_WHITE);
  if (ballMode == BALL_ACTIVE)
    display.fillCircle((int)roundf(ballX), (int)roundf(ballY),
                       BALL_RADIUS, SSD1306_WHITE);
  display.display();
}

void updatePaddle() {
  uint32_t now = millis();
  if ((uint32_t)(now - lastFrameAt) < FRAME_INTERVAL_MS) return;

  float dt = min((uint32_t)(now - lastFrameAt), (uint32_t)80) / 1000.0f;
  lastFrameAt = now;

  if (leftButton.stable && !rightButton.stable)
    paddleX -= PADDLE_SPEED_PX_S * dt;
  else if (rightButton.stable && !leftButton.stable)
    paddleX += PADDLE_SPEED_PX_S * dt;

  // Le tout premier mouvement demande le service de la balle.
  if (ballMode == BALL_IDLE && !localClaim &&
      (leftButton.stable != rightButton.stable)) {
    localClaim = true;
    claimAt = now;
    nextClaimAt = 0;
    claimAcknowledged = false;
    GAME_LOG.println("Demande de service...");
  }

  paddleX = constrain(paddleX, 1.0f,
                      (float)(FIELD_WIDTH - 1 - PADDLE_WIDTH));
  showPlayScreen();
}

// Callback Wi-Fi : ne jamais piloter l'OLED ici.
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !info->src_addr || !rxQueue || len != (int)sizeof(Packet)) return;
  RxEvent event = {};
  memcpy(event.mac, info->src_addr, 6);
  memcpy(&event.packet, data, sizeof(Packet));
  if (event.packet.magic != PROTOCOL_MAGIC) return;
  event.packet.text[sizeof(event.packet.text) - 1] = '\0';
  xQueueSend(rxQueue, &event, 0);
}

void sendPacket(uint8_t type, uint8_t role, uint8_t ready = 0,
                uint32_t remainingMs = 0, const char* text = "",
                uint32_t sequence = 0, float x = 0, float vx = 0,
                float speed = BALL_SPEED_Y) {
  Packet p = {};
  p.magic = PROTOCOL_MAGIC;
  p.type = type;
  p.role = role;
  p.ready = ready;
  p.remainingMs = remainingMs;
  p.sequence = sequence;
  p.x = x;
  p.vx = vx;
  p.speed = speed;
  p.roundId = roundId;
  p.bounces = reboundCount;
  p.score1 = score1;
  p.score2 = score2;
  snprintf(p.text, sizeof(p.text), "%s", text);
  esp_err_t result = esp_now_send(BROADCAST_MAC,
                   reinterpret_cast<const uint8_t*>(&p), sizeof(p));
  if (result != ESP_OK) GAME_LOG.printf("Erreur ESP-NOW : %d\n", (int)result);
}
void announceRole() {
  if (!player) return;
  sendPacket(PKT_ROLE, player, localReady ? 1 : 0);
  nextAnnounceAt = millis() + ANNOUNCE_INTERVAL_MS;
}

void setRole(uint8_t newRole, const char* reason) {
  if (newRole != 1 && newRole != 2) return;
  if (player == newRole) return;
  player = newRole;
  if (phase == MENU) { score1 = score2 = reboundCount = 0; roundId = 1; lostPlayer = 0;
    totalRebounds = 0; matchStartedAt = matchElapsedMs = 0; matchWinner = 0; }
  localReady = false;
  remoteReady = false; // Reconfirmation necessaire si arbitrage de roles.
  phase = ROLE_CONFIRM;
  GAME_LOG.printf("Vous etes Joueur %u (%s)\n", player, reason);
  ledsOff();
  showRole();
  announceRole();
}
void setReady() {
  if (phase != ROLE_CONFIRM) return;
  localReady = true;
  phase = WAITING;
  showWaiting();
  GAME_LOG.printf("Joueur %u pret, attente du partenaire\n", player);
  announceRole();
}

bool amCoordinator() {
  return remoteKnown && memcmp(localMac, remoteMac, 6) < 0;
}
void initiateCountdown() {
  if (phase != WAITING || !localReady || !remoteReady || !amCoordinator()) return;
  // Le changement de manche est fait AVANT l'annonce START :
  // toutes les annonces portent le meme roundId.
  if (lostPlayer != 0) ++roundId;
  countdownAt = millis() + PRE_COUNTDOWN_MS;
  phase = COUNTDOWN;
  lastCountdownScreen = -1;
  nextStartAt = 0;
  GAME_LOG.println("Les deux joueurs sont prets : compte a rebours synchronise");
}
void broadcastCountdown() {
  if (phase != COUNTDOWN || !amCoordinator()) return;
  uint32_t now = millis();
  // Repetitions pour pallier la perte de paquets ESP-NOW.
  if ((int32_t)(now - nextStartAt) >= 0) {
    uint32_t remain = (int32_t)(countdownAt - now) > 0 ? countdownAt - now : 0;
    sendPacket(PKT_START, player, 1, remain);
    nextStartAt = now + START_REPEAT_MS;
  }
}


// =====================================================
// BALLE ET FIN DE MANCHE
// =====================================================

void showRoundResult() {
  textMode();
  drawTopBar();
  display.setTextSize(2);
  if (lostPlayer == player) {
    display.setCursor(28, 15); display.println("Balle");
    display.setCursor(16, 32); display.println("perdue !");
  } else {
    display.setCursor(28, 15); display.println("Manche");
    display.setCursor(16, 32); display.println("gagnee !");
  }
  display.setTextSize(1);
  display.setCursor(0, 49); display.println("A: Continuer");
  display.setCursor(0, 57); display.println("B: Menu principal");
  display.display();
}

void showVictory() {
  textMode();
  display.setTextSize(2);
  display.setCursor(10, 5); display.print("Joueur "); display.print(matchWinner);
  display.setCursor(4, 29); display.println("a gagne la");
  display.setCursor(20, 47); display.println("partie !");
  display.display();
}

void showMatchStats() {
  textMode();
  char line[32];
  snprintf(line, sizeof(line), "J1 | %u - %u | J2", score1, score2);
  int16_t bx, by; uint16_t tw, th;
  display.getTextBounds(line, 0, 0, &bx, &by, &tw, &th);
  display.setCursor(max(0, (128 - (int)tw) / 2), 4);
  display.println(line);
  display.setCursor(0, 20); display.print("Rebonds : ");
  display.println((unsigned long)totalRebounds);
  uint32_t seconds = matchElapsedMs / 1000UL;
  snprintf(line, sizeof(line), "Temps ecoule : %02lu:%02lu",
           (unsigned long)(seconds / 60UL), (unsigned long)(seconds % 60UL));
  display.setCursor(0, 34); display.println(line);
  display.setCursor(0, 55); display.println("A: Nouvelle partie");
  display.display();
}

void finishMatch() {
  matchWinner = score1 > score2 ? 1 : 2; // 5 manches => pas d'egalite.
  if (matchStartedAt && !matchElapsedMs)
    matchElapsedMs = millis() - matchStartedAt;
  phase = VICTORY;
  localReady = remoteReady = false;
  ballMode = BALL_REMOTE;
  victoryShownAt = millis();
  showVictory();
}

void showQuitConfirm() {
  textMode();
  display.setCursor(0, 8); display.println("Quittez la partie ?");
  display.setCursor(0, 39); display.println("A: OUI");
  display.setCursor(0, 53); display.println("B: NON");
  display.display();
}

void showAbandoned() {
  textMode();
  display.setCursor(0, 10); display.println("Partie abandonnee");
  display.setCursor(0, 23); display.print("par Joueur ");
  display.println(quittingRole);
  display.setCursor(0, 54); display.println("A: OK");
  display.display();
}

void goToMenu() {
  phase = MENU;
  ignoreAutoRoleUntil = millis() + 2500;
  player = 0;
  localReady = remoteReady = false;
  remoteKnown = false;
  remoteRole = 0;
  roundId = 1;
  score1 = score2 = reboundCount = 0;
  totalRebounds = 0; matchStartedAt = matchElapsedMs = 0; matchWinner = 0;
  ballMode = BALL_IDLE;
  localClaim = remoteClaim = false;
  claimAcknowledged = false;
  lossAcknowledged = false;
  lossBroadcastUntil = 0;
  quitBroadcastUntil = 0;
  ledsOff();
  showMenu();
}

void finishRound(uint8_t loser) {
  if (phase != PLAYING || (loser != 1 && loser != 2)) return;
  lostPlayer = loser;
  if (loser == 1) ++score2;
  else ++score1;
  totalRebounds += reboundCount;
  if (score1 + score2 >= TOTAL_ROUNDS && matchStartedAt)
    matchElapsedMs = millis() - matchStartedAt;
  phase = FINISHED;
  localReady = remoteReady = false;
  ballMode = BALL_REMOTE;
  lossBroadcastUntil = 1; // Etat actif jusqu'a un accuse de reception
  lossAcknowledged = (loser != player); // Le perdant doit informer le gagnant.
  nextLossAt = 0;
  if (score1 + score2 >= TOTAL_ROUNDS) finishMatch();
  else showRoundResult();
  GAME_LOG.printf("Fin manche %u, perdant J%u, score %u-%u\n",
                roundId, loser, score1, score2);
}

void prepareNextRound() {
  if (phase != FINISHED || score1 + score2 >= TOTAL_ROUNDS) return;
  localReady = true;
  phase = WAITING;
  // Ne pas stopper tout de suite les annonces de resultat :
  // le pair peut encore attendre PKT_LOSS.
  showWaiting();
  announceRole();
}

void spawnBall() {
  if (phase != PLAYING || ballMode != BALL_IDLE) return;
  ballMode = BALL_ACTIVE;
  ballX = paddleX + PADDLE_WIDTH / 2.0f;
  ballY = PADDLE_Y - BALL_RADIUS - 1;
  ballVx = 0;
  ballVy = -ballSpeed;
  localClaim = false;
  claimAcknowledged = false;
  GAME_LOG.println("Balle lancee depuis la raquette");
}

void sendBall() {
  sendPacket(PKT_BALL, player, 1, 0, "", pendingSequence,
             pendingX, pendingVx, pendingSpeed);
  nextBallRetryAt = millis() + RETRY_MS;
}

void updateBall(float dt) {
  if (ballMode != BALL_ACTIVE) return;
  const float oldY = ballY;
  ballX += ballVx * dt;
  ballY += ballVy * dt;

  if (ballX <= 1 + BALL_RADIUS) {
    ballX = 1 + BALL_RADIUS;
    ballVx = fabsf(ballVx);
  } else if (ballX >= 126 - BALL_RADIUS) {
    ballX = 126 - BALL_RADIUS;
    ballVx = -fabsf(ballVx);
  }

  // La balle sort par le bord haut et entre chez l'autre joueur.
  if (ballVy < 0 && ballY - BALL_RADIUS <= TOP_BOUNDARY_Y) {
    // Sequence globale continuee par les deux cartes, quel que soit le lanceur.
    pendingSequence = ++ballSequence;
    pendingX = ballX;
    pendingVx = ballVx;
    pendingSpeed = ballSpeed;
    ballMode = BALL_SENDING;
    sendBall();
    GAME_LOG.println("Transfert de balle -> autre XIAO");
    return;
  }

  // Collision uniquement lorsque la balle descend : on teste le
  // franchissement du dessus de la raquette, sans dependance au FPS.
  if (ballVy > 0 && oldY + BALL_RADIUS <= PADDLE_Y &&
      ballY + BALL_RADIUS >= PADDLE_Y) {
    float left = paddleX;
    float right = paddleX + PADDLE_WIDTH;
    if (ballX + BALL_RADIUS >= left && ballX - BALL_RADIUS <= right) {
      ballY = PADDLE_Y - BALL_RADIUS;
      ballSpeed *= SPEED_MULTIPLIER;
      ballVy = -ballSpeed;
      float offset = (ballX - (left + PADDLE_WIDTH / 2.0f)) /
                     (PADDLE_WIDTH / 2.0f);
      float maxVx = MAX_BALL_VX * (ballSpeed / BALL_SPEED_Y);
      ballVx = constrain(offset * maxVx, -maxVx, maxVx);
      ++reboundCount;
      sendPacket(PKT_BOUNCE, player);
      GAME_LOG.printf("Rebond total : %u\n", reboundCount);
      return;
    }
  }

  if (ballVy > 0 && ballY - BALL_RADIUS >= 63)
    finishRound(player);
}

void updateBallNetwork() {
  uint32_t now = millis();
  if (phase == PLAYING) {
    if (ballMode == BALL_IDLE && localClaim) {
      // Les demandes sont annoncees pendant une courte fenetre.
      if ((int32_t)(now - nextClaimAt) >= 0) {
        sendPacket(PKT_CLAIM, player);
        nextClaimAt = now + 45;
      }
      // Ne jamais creer une balle tant que l'autre XIAO n'a pas confirme
      // qu'il attend le service : evite deux balles locales independantes.
      if (claimAcknowledged &&
          (uint32_t)(now - claimAt) >= CLAIM_WAIT_MS) {
        if (!remoteClaim || amCoordinator()) spawnBall();
        else { localClaim = false; ballMode = BALL_REMOTE; }
      }
    }
    if (ballMode == BALL_SENDING &&
        (int32_t)(now - nextBallRetryAt) >= 0)
      sendBall();
  }
  if ((phase == FINISHED || phase == WAITING || phase == QUIT_CONFIRM ||
       phase == VICTORY || phase == STATS) &&
      lostPlayer == player && !lossAcknowledged &&
      lossBroadcastUntil != 0 &&
      (int32_t)(now - nextLossAt) >= 0) {
    // Le paquet transporte la duree et le total de rebonds pour la synchronisation.
    sendPacket(PKT_LOSS, player, 1,
               matchElapsedMs ? matchElapsedMs :
                   (matchStartedAt ? millis() - matchStartedAt : 0),
               "", totalRebounds, (float)lostPlayer);
    nextLossAt = now + 100;
  }
  if (quitBroadcastUntil && (int32_t)(quitBroadcastUntil - now) > 0 &&
      (int32_t)(now - nextQuitAt) >= 0) {
    sendPacket(PKT_QUIT, quittingRole);
    nextQuitAt = now + 75;
  }
}

void handlePacket(const RxEvent& e) {
  if (memcmp(e.mac, localMac, 6) == 0) return;
  const Packet& p = e.packet;
  if (p.role != 1 && p.role != 2) return;

  // Ce test prend en charge une seule paire de XIAO a la fois.
  if (!remoteKnown) {
    memcpy(remoteMac, e.mac, 6);
    remoteKnown = true;
  }
  if (memcmp(remoteMac, e.mac, 6) != 0) return;

  // Les messages issus d'une autre manche ne doivent rien modifier.
  // Un resultat de la manche precedente doit encore pouvoir etre accuse
  // pendant que le joueur gagnant entre dans l'ecran d'attente.
  if (p.type != PKT_QUIT && p.type != PKT_ROLE &&
      p.type != PKT_LOSS_ACK && p.roundId != roundId &&
      !(p.type == PKT_START && phase == WAITING &&
        lostPlayer != 0 && p.roundId == (uint16_t)(roundId + 1))) return;

  if (p.type == PKT_QUIT) {
    if (phase != MENU && phase != ABANDONED && player != p.role) {
      quittingRole = p.role;
      phase = ABANDONED;
      localReady = remoteReady = false;
      ballMode = BALL_REMOTE;
      showAbandoned();
    }
    return;
  }

  if (p.type == PKT_ROLE) {
    if (p.roundId != roundId &&
        !(phase == WAITING && lostPlayer != 0 &&
          p.roundId == (uint16_t)(roundId + 1))) return;
    remoteRole = p.role;
    if (player == 0 &&
        (int32_t)(millis() - ignoreAutoRoleUntil) >= 0) {
      setRole(3 - p.role, "selection du partenaire");
    } else if (player == p.role) {
      // Une selection simultanee est tranchee par la MAC inferieure.
      if (memcmp(remoteMac, localMac, 6) < 0) {
        setRole(3 - p.role, "arbitrage MAC");
      } else {
        announceRole();
      }
    }
    if (player != p.role) {
      remoteReady = p.ready != 0;
      if (phase == WAITING) initiateCountdown();
    }
  } else if (p.type == PKT_START) {
    // Seule la XIAO de MAC inferieure peut declarer le depart.
    if (!amCoordinator() && player != p.role && localReady &&
        (phase == WAITING || phase == COUNTDOWN)) {
      if (phase == WAITING) {
        // L'organisateur a commence la nouvelle manche.
        if (p.roundId == (uint16_t)(roundId + 1) && lostPlayer != 0)
          ++roundId;
        countdownAt = millis() + p.remainingMs;
        phase = COUNTDOWN;
        lastCountdownScreen = -1;
        GAME_LOG.println("Depart recu du coordinateur");
      }
      // Une fois synchronise, conserver la base locale pour eviter les sauts.
    }
  } else if (p.type == PKT_CLAIM) {
    if (phase == PLAYING && (ballMode == BALL_IDLE || ballMode == BALL_REMOTE)) {
      // Toujours accuser reception, meme si la premiere reponse est perdue.
      sendPacket(PKT_CLAIM_ACK, player);
      remoteClaim = true;
      if (!localClaim || !amCoordinator()) {
        localClaim = false;
        ballMode = BALL_REMOTE;
      }
    }
  } else if (p.type == PKT_CLAIM_ACK) {
    if (phase == PLAYING && localClaim && ballMode == BALL_IDLE)
      claimAcknowledged = true;
  } else if (p.type == PKT_BALL) {
    if ((phase == PLAYING || phase == FINISHED) && player != p.role) {
      // Acquitter meme les doublons; ils sont attendus pendant les retries.
      sendPacket(PKT_ACK, player, 1, 0, "", p.sequence);
      if (phase == PLAYING && ballMode != BALL_ACTIVE &&
          p.sequence > lastRoundSequence) {
        lastRoundSequence = p.sequence;
        ballSequence = p.sequence;
        ballX = constrain(p.x, (float)(1 + BALL_RADIUS),
                          (float)(126 - BALL_RADIUS));
        ballY = TOP_BOUNDARY_Y + BALL_RADIUS + 1;
        ballSpeed = max(BALL_SPEED_Y, p.speed);
        float maxVx = MAX_BALL_VX * (ballSpeed / BALL_SPEED_Y);
        ballVx = constrain(p.vx, -maxVx, maxVx);
        ballVy = ballSpeed;
        reboundCount = p.bounces;
        ballMode = BALL_ACTIVE;
        localClaim = false;
        GAME_LOG.println("Balle recue : descente vers la raquette");
      }
    }
  } else if (p.type == PKT_ACK) {
    if (phase == PLAYING && ballMode == BALL_SENDING &&
        p.sequence == pendingSequence) {
      lastRoundSequence = pendingSequence;
      ballMode = BALL_REMOTE;
      GAME_LOG.println("Transfert confirme");
    }
  } else if (p.type == PKT_LOSS_ACK) {
    if (p.role != player && lostPlayer == player)
      lossAcknowledged = true;
  } else if (p.type == PKT_BOUNCE) {
    if (phase == PLAYING && p.bounces > reboundCount)
      reboundCount = p.bounces;
  } else if (p.type == PKT_LOSS) {
    const uint8_t loser = (uint8_t)p.x;
    if (loser == 1 || loser == 2) {
      // Repondre meme si le message arrive plusieurs fois.
      sendPacket(PKT_LOSS_ACK, player);
      if (phase == PLAYING) {
        reboundCount = p.bounces;
        finishRound(loser);
        // Duree du perdant recue pour les statistiques partagees.
        if (score1 + score2 >= TOTAL_ROUNDS && p.remainingMs > matchElapsedMs)
          matchElapsedMs = p.remainingMs;
        if (score1 + score2 >= TOTAL_ROUNDS && p.sequence > totalRebounds)
          totalRebounds = p.sequence;
      }
    }
  }
}

void updateCountdown() {
  if (phase != COUNTDOWN) return;
  int32_t elapsed = (int32_t)(millis() - countdownAt);
  int8_t screen;
  if (elapsed < 0) return;            // Attente du debut commun.
  if (elapsed < 1000) screen = 3;
  else if (elapsed < 2000) screen = 2;
  else if (elapsed < 3000) screen = 1;
  else if (elapsed < 5000) screen = 0;
  else {
    phase = PLAYING;
    lostPlayer = 0;
    gameStartedAt = millis();
    if (!matchStartedAt) matchStartedAt = gameStartedAt;
    ballSpeed = BALL_SPEED_Y * powf(SPEED_MULTIPLIER, (float)(roundId - 1));
    paddleX = (FIELD_WIDTH - PADDLE_WIDTH) / 2.0f;
    ballMode = BALL_IDLE;
    localClaim = remoteClaim = false;
    claimAcknowledged = false;
    lastRoundSequence = 0;
    ballSequence = 0;
    reboundCount = 0;
    lastFrameAt = millis();
    showPlayScreen();
    GAME_LOG.println("Partie lancee : gauche SW4 / droite SW2");
    return;
  }
  if (screen != lastCountdownScreen) {
    lastCountdownScreen = screen;
    if (screen == 0) showGameStarted();
    else showCountdownNumber(screen);
  }
}


void scanMatrix() {
  // O_1 actif : SW1 haut (I_1), SW4 gauche (I_2), SW7 bas (I_3).
  pinMode(ROW_O2, INPUT);
  pinMode(ROW_O3, INPUT);
  pinMode(ROW_O1, OUTPUT);
  digitalWrite(ROW_O1, HIGH);
  delayMicroseconds(100);
  bool rawUp = digitalRead(COL_I1) == HIGH;
  bool rawLeft = digitalRead(COL_I2) == HIGH;
  bool rawDown = digitalRead(COL_I3) == HIGH;
  pinMode(ROW_O1, INPUT);

  // O_2 actif : SW2 droite (I_1).
  pinMode(ROW_O2, OUTPUT);
  digitalWrite(ROW_O2, HIGH);
  delayMicroseconds(100);
  bool rawRight = digitalRead(COL_I1) == HIGH;
  pinMode(ROW_O2, INPUT);

  // O_3 actif : SW3 A (I_1).
  pinMode(ROW_O3, OUTPUT);
  digitalWrite(ROW_O3, HIGH);
  delayMicroseconds(100);
  bool rawA = digitalRead(COL_I1) == HIGH;
  bool rawB = digitalRead(COL_I2) == HIGH; // SW6 = O_3 / I_2
  pinMode(ROW_O3, INPUT);

  uint32_t now = millis();
  auto debounce = [now](Button& b, bool raw) {
    b.pressedEvent = false;
    if (raw != b.raw) { b.raw = raw; b.edgeAt = now; }
    if (raw != b.stable && (uint32_t)(now - b.edgeAt) >= DEBOUNCE_MS) {
      b.stable = raw;
      if (b.stable) b.pressedEvent = true;
    }
  };
  debounce(upButton, rawUp);
  debounce(downButton, rawDown);
  debounce(aButton, rawA);
  debounce(bButton, rawB);
  debounce(leftButton, rawLeft);
  debounce(rightButton, rawRight);
}

void setup() {
  GAME_LOG.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  leds.begin();
  leds.setBrightness(20);
  ledsOff();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    GAME_LOG.println("OLED introuvable");
    while (true) delay(1000);
  }
  showMenu();

  pinMode(ROW_O1, INPUT);
  pinMode(ROW_O2, INPUT);
  pinMode(ROW_O3, INPUT);
  pinMode(COL_I1, INPUT_PULLDOWN);
  pinMode(COL_I2, INPUT_PULLDOWN);
  pinMode(COL_I3, INPUT_PULLDOWN);

  rxQueue = xQueueCreate(16, sizeof(RxEvent));
  if (!rxQueue) {
    GAME_LOG.println("Erreur allocation file ESP-NOW");
    while (true) delay(1000);
  }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    GAME_LOG.println("Erreur canal Wi-Fi");
    while (true) delay(1000);
  }
  esp_wifi_get_mac(WIFI_IF_STA, localMac);
  GAME_LOG.printf("MAC : %02X:%02X:%02X:%02X:%02X:%02X\n",
      localMac[0],localMac[1],localMac[2],localMac[3],localMac[4],localMac[5]);
  if (esp_now_init() != ESP_OK) {
    GAME_LOG.println("Erreur initialisation ESP-NOW");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onReceive);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    GAME_LOG.println("Erreur ajout broadcast ESP-NOW");
    while (true) delay(1000);
  }
  GAME_LOG.println("PING PONG SLIDER : Haut/Bas, A selectionne, A confirme pret");
}

void loop() {
  RxEvent event;
  while (xQueueReceive(rxQueue, &event, 0) == pdTRUE) handlePacket(event);

  scanMatrix();
  if (phase == MENU) {
    if (upButton.pressedEvent && selectedPlayer != 1) {
      selectedPlayer = 1;
      showMenu();
    }
    if (downButton.pressedEvent && selectedPlayer != 2) {
      selectedPlayer = 2;
      showMenu();
    }
    if (aButton.pressedEvent) setRole(selectedPlayer, "bouton A");
  } else if (phase == ROLE_CONFIRM) {
    if (aButton.pressedEvent) setReady();
  } else if (phase == FINISHED) {
    if (aButton.pressedEvent) prepareNextRound();
    if (bButton.pressedEvent) { phase = QUIT_CONFIRM; showQuitConfirm(); }
  } else if (phase == STATS) {
    if (aButton.pressedEvent) goToMenu();
  } else if (phase == QUIT_CONFIRM) {
    if (bButton.pressedEvent) { phase = FINISHED; showRoundResult(); }
    else if (aButton.pressedEvent) {
      quittingRole = player;
      sendPacket(PKT_QUIT, quittingRole);
      goToMenu();
      quitBroadcastUntil = millis() + 2200;
      nextQuitAt = 0;
    }
  } else if (phase == ABANDONED) {
    if (aButton.pressedEvent) goToMenu();
  } else if (phase == PLAYING) {
    uint32_t before = lastFrameAt;
    updatePaddle();
    if (lastFrameAt != before && ballMode == BALL_ACTIVE && phase == PLAYING) {
      float dt = min((uint32_t)(lastFrameAt - before), (uint32_t)80) / 1000.0f;
      updateBall(dt);
      if (phase == PLAYING) showPlayScreen();
    }
  }

  if (player != 0 && phase != STATS && phase != VICTORY && phase != ABANDONED &&
      (int32_t)(millis() - nextAnnounceAt) >= 0)
    announceRole();
  if (phase == WAITING) initiateCountdown();
  if (phase == COUNTDOWN) {
    broadcastCountdown();
    updateCountdown();
  }
  updateBallNetwork();
  if (phase == VICTORY && (uint32_t)(millis() - victoryShownAt) >= WINNER_SCREEN_MS) {
    phase = STATS;
    showMatchStats();
  }
  if (redLedOffAt && (int32_t)(millis() - redLedOffAt) >= 0)
    ledsOff();
  delay(5);
}
