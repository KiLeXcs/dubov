#include <Arduino.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <Wire.h>

// === АППАРАТНЫЕ ПИНЫ HELTEC V4 ===
#define Vext 36
#define LED_PIN 35
#define BOOT_BUTTON 0
#define LORA_SCK 9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_NSS 8
#define LORA_RST 12
#define LORA_DIO1 14
#define LORA_BUSY 13
#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21

// === НАСТРОЙКИ MESH СЕТИ ===
#define MY_NODE_ID 2
#define ADMIN_ID 1
#define CLIMBER_ID 3
#define MAX_TTL 3
#define SLEEP_TIMEOUT 120000
#define PING_INTERVAL 30000 // 5 минут = 300000 мс

// === СТРУКТУРА ПАКЕТА ===
struct MeshPacket {
  uint8_t src;
  uint8_t dst;
  uint8_t ttl;
  uint32_t msgId;
  char payload[64];
};

// === КЭШ ДЛЯ ЗАЩИТЫ ОТ ПЕТЕЛЬ ===
#define CACHE_SIZE 30
uint32_t msgCache[CACHE_SIZE] = {0};
uint8_t cacheIndex = 0;

// === ИНИЦИАЛИЗАЦИЯ ===
SPIClass spi(HSPI);
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, spi);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);

volatile bool receivedFlag = false;
unsigned long lastActivity = 0;
unsigned long lastPacketTime = 0;
unsigned long lastPingTime = 0;
uint8_t pingTarget = 0; // 0 = Admin, 1 = Climber
bool pingInProgress = false;
unsigned long pingStartTime = 0;
#define PING_TIMEOUT 10000 // Ждем ответа 10 секунд

// === СЧЕТЧИКИ ===
uint32_t rxCount = 0;
uint32_t txCount = 0;
uint32_t repeatCount = 0;
uint32_t pingCount = 0;
uint32_t pongCount = 0;
int lastRSSI = 0;
int bestRSSI = -200;
bool lastPingSuccess = false;
uint8_t pingAnimFrame = 0; // Для анимации
unsigned long lastAnimFrame = 0;

// === ПРЕРЫВАНИЕ ПРИЕМА ===
void IRAM_ATTR setFlag() {
  receivedFlag = true;
}

// === ПРОВЕРКА ДУБЛИКАТОВ ===
bool isDuplicate(uint32_t msgId) {
  for (int i = 0; i < CACHE_SIZE; i++) {
    if (msgCache[i] == msgId) return true;
  }
  return false;
}

void addToCache(uint32_t msgId) {
  msgCache[cacheIndex] = msgId;
  cacheIndex = (cacheIndex + 1) % CACHE_SIZE;
}

// === АНИМАЦИЯ: БЕГУЩИЕ ТОЧКИ ===
String getAnimatedDots() {
  unsigned long now = millis();
  if (now - lastAnimFrame > 400) { // Меняем кадр каждые 400мс
    pingAnimFrame = (pingAnimFrame + 1) % 4;
    lastAnimFrame = now;
  }
  
  switch(pingAnimFrame) {
    case 0: return "";
    case 1: return ".";
    case 2: return "..";
    case 3: return "...";
  }
  return "";
}

// === ЭКРАН: ГЛАВНЫЙ ===
void showMainScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  u8g2.setCursor(0, 12);
  u8g2.print("RECEIVER #");
  u8g2.print(MY_NODE_ID);
  
  u8g2.setCursor(0, 26);
  u8g2.print("RX:");
  u8g2.print(rxCount);
  u8g2.print(" TX:");
  u8g2.print(txCount);
  
  u8g2.setCursor(0, 40);
  u8g2.print("RSSI:");
  u8g2.print(lastRSSI);
  u8g2.print(" dBm");
  
  u8g2.setCursor(0, 54);
  uint32_t idleSec = (millis() - lastPacketTime) / 1000;
  u8g2.print(idleSec);
  u8g2.print("s idle");
  
  u8g2.sendBuffer();
}

// === ЭКРАН: ПРИЕМ ПАКЕТА ===
void showRxScreen(uint8_t from, uint8_t ttl, int rssi, const char* payload) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  u8g2.setCursor(0, 12);
  u8g2.print("RX FROM:");
  u8g2.print(from);
  
  u8g2.setCursor(0, 26);
  u8g2.print("TTL:");
  u8g2.print(ttl);
  u8g2.print(" RSSI:");
  u8g2.print(rssi);
  
  if (rssi > bestRSSI) bestRSSI = rssi;
  
  u8g2.setCursor(0, 40);
  u8g2.print("Best:");
  u8g2.print(bestRSSI);
  u8g2.print(" dBm");
  
  u8g2.setCursor(0, 54);
  char shortPayload[17];
  strncpy(shortPayload, payload, 16);
  shortPayload[16] = '\0';
  u8g2.print(shortPayload);
  
  u8g2.sendBuffer();
  
  rxCount++;
  lastPacketTime = millis();
  lastRSSI = rssi;
}

// === ЭКРАН: РЕТРАНСЛЯЦИЯ ===
void showRepeatScreen(uint8_t src, uint8_t dst, uint8_t oldTTL, uint8_t newTTL) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  u8g2.setCursor(0, 12);
  u8g2.print(">>REPEATING<<");
  
  u8g2.setCursor(0, 26);
  u8g2.print(src);
  u8g2.print(" -> ");
  if (dst == 0) {
    u8g2.print("ALL");
  } else {
    u8g2.print(dst);
  }
  
  u8g2.setCursor(0, 40);
  u8g2.print("TTL:");
  u8g2.print(oldTTL);
  u8g2.print("->");
  u8g2.print(newTTL);
  
  u8g2.setCursor(0, 54);
  u8g2.print("REP #");
  u8g2.print(repeatCount);
  
  u8g2.sendBuffer();
  
  repeatCount++;
  txCount++;
  lastPacketTime = millis();
}

// === ЭКРАН: ПИНГ С АНИМАЦИЕЙ ===
void showPingScreen(uint8_t target, bool success, bool showAnimation = false) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  u8g2.setCursor(0, 12);
  u8g2.print("PING -> ");
  if (target == ADMIN_ID) {
    u8g2.print("ADMIN");
  } else if (target == CLIMBER_ID) {
    u8g2.print("CLIMBER");
  } else {
    u8g2.print("UNKNOWN");
  }
  
  u8g2.setCursor(0, 30);
  if (showAnimation) {
    // Анимация "отправки" с бегущими точками
    String dots = getAnimatedDots();
    u8g2.print("Sending");
    u8g2.print(dots.c_str());
  } else if (success) {
    u8g2.print("PONG OK!");
  } else {
    u8g2.print("NO RESPONSE");
  }
  
  u8g2.setCursor(0, 48);
  u8g2.print("Ping #:");
  u8g2.print(pingCount);
  
  u8g2.sendBuffer();
  
  lastPacketTime = millis();
}

// === ЭКРАН: СОН ===
void showSleepScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  u8g2.setCursor(0, 20);
  u8g2.print("DEEP SLEEP");
  
  u8g2.setCursor(0, 40);
  u8g2.print("Wake on");
  
  u8g2.setCursor(0, 54);
  u8g2.print("packet");
  
  u8g2.sendBuffer();
}

// === ЭКРАН: ОШИБКА ===
void showErrorScreen(const char* error) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  u8g2.setCursor(0, 12);
  u8g2.print("ERROR!");
  
  u8g2.setCursor(0, 30);
  u8g2.print(error);
  
  u8g2.setCursor(0, 54);
  u8g2.print("Check Serial");
  
  u8g2.sendBuffer();
}

// === ОТПРАВКА ПИНГА ===
void sendPing(uint8_t targetId) {
  MeshPacket pkt;
  pkt.src = MY_NODE_ID;
  pkt.dst = targetId;
  pkt.ttl = MAX_TTL;
  pkt.msgId = millis();
  snprintf(pkt.payload, sizeof(pkt.payload), "PING");
  
  Serial.printf("[PING] Sending to node %d\n", targetId);
  
  int state = radio.transmit((uint8_t*)&pkt, sizeof(pkt));
  
  if (state == RADIOLIB_ERR_NONE) {
    pingCount++;
    txCount++;
    Serial.println("[PING] Sent successfully");
    pingInProgress = true;
    pingStartTime = millis();
  } else {
    Serial.printf("[PING] Failed, code %d\n", state);
  }
  
  radio.startReceive();
}

// === ОТПРАВКА PONG ===
void sendPong(uint8_t targetId) {
  MeshPacket pkt;
  pkt.src = MY_NODE_ID;
  pkt.dst = targetId;
  pkt.ttl = MAX_TTL;
  pkt.msgId = millis();
  snprintf(pkt.payload, sizeof(pkt.payload), "PONG");
  
  Serial.printf("[PONG] Sending to node %d\n", targetId);
  
  int state = radio.transmit((uint8_t*)&pkt, sizeof(pkt));
  
  if (state == RADIOLIB_ERR_NONE) {
    pongCount++;
    txCount++;
    Serial.println("[PONG] Sent successfully");
  } else {
    Serial.printf("[PONG] Failed, code %d\n", state);
  }
  
  radio.startReceive();
}

void setup() {
  // Включаем питание периферии
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(500);

  // Кнопка BOOT
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  // Инициализация Serial
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== RECEIVER LoRa START ===");
  Serial.printf("Node ID: %d\n", MY_NODE_ID);
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Инициализация OLED
  u8g2.begin();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.clearBuffer();
  u8g2.setCursor(0, 20);
  u8g2.print("RECEIVER");
  u8g2.setCursor(0, 40);
  u8g2.print("ID: ");
  u8g2.print(MY_NODE_ID);
  u8g2.setCursor(0, 55);
  u8g2.print("Starting...");
  u8g2.sendBuffer();
  
  delay(1000);
  
  // Инициализация Radio
  spi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  
  Serial.print("Init SX1262... ");
  int state = radio.begin(868.0, 125.0, 7, 5, 0x18, 10, 8, 1.6, false);
  
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Failed, code %d\n", state);
    showErrorScreen("Radio init");
    while (true);
  }
  
  radio.setOutputPower(22);
  radio.setDio1Action(setFlag);
  radio.startReceive();
  
  lastPacketTime = millis();
  lastActivity = millis();
  lastPingTime = millis();
  
  showMainScreen();
  Serial.println("Receiver ready. Mesh network active.");
}

void loop() {
  // === 1. ОБРАБОТКА ВХОДЯЩИХ ПАКЕТОВ ===
  if (receivedFlag) {
    MeshPacket pkt;
    int state = radio.readData((uint8_t*)&pkt, sizeof(pkt));
    
    if (state == RADIOLIB_ERR_NONE) {
      int rssi = radio.getRSSI();
      lastActivity = millis();
      
      Serial.printf("[RX] From:%d To:%d TTL:%d MsgID:%lu RSSI:%d Data:%s\n",
                    pkt.src, pkt.dst, pkt.ttl, pkt.msgId, rssi, pkt.payload);
      
      // Проверка на PING
      if (strcmp(pkt.payload, "PING") == 0) {
        Serial.println(">>> PING received - sending PONG");
        showRxScreen(pkt.src, pkt.ttl, rssi, pkt.payload);
        
        if (!isDuplicate(pkt.msgId)) {
          addToCache(pkt.msgId);
          sendPong(pkt.src);
          showPingScreen(pkt.src, true);
          delay(2000);
        }
        
        receivedFlag = false;
        radio.startReceive();
        return;
      }
      
      // Проверка на PONG
      if (strcmp(pkt.payload, "PONG") == 0) {
        Serial.println(">>> PONG received");
        showPingScreen(pkt.src, true);
        lastPingSuccess = true;
        pingInProgress = false;
        delay(2000);
        
        receivedFlag = false;
        radio.startReceive();
        return;
      }
      
      // Обычный пакет
      showRxScreen(pkt.src, pkt.ttl, rssi, pkt.payload);
      
      if (isDuplicate(pkt.msgId)) {
        Serial.println(">>> DUPLICATE - ignored");
        delay(2000);
        receivedFlag = false;
        radio.startReceive();
        return;
      }
      addToCache(pkt.msgId);
      
      // === ЛОГИКА РЕТРАНСЛЯТОРА ===
      
      // Admin → Climber: пересылаем
      if (pkt.src == ADMIN_ID && pkt.dst == CLIMBER_ID) {
        Serial.println(">>> Forward: ADMIN -> CLIMBER");
        showRepeatScreen(pkt.src, pkt.dst, pkt.ttl, pkt.ttl - 1);
        digitalWrite(LED_PIN, HIGH);
        
        pkt.ttl--;
        delay(random(20, 100));
        radio.transmit((uint8_t*)&pkt, sizeof(pkt));
        
        digitalWrite(LED_PIN, LOW);
        delay(1500);
      }
      
      // Climber → Admin: пересылаем
      else if (pkt.src == CLIMBER_ID && pkt.dst == ADMIN_ID) {
        Serial.println(">>> Forward: CLIMBER -> ADMIN");
        showRepeatScreen(pkt.src, pkt.dst, pkt.ttl, pkt.ttl - 1);
        digitalWrite(LED_PIN, HIGH);
        
        pkt.ttl--;
        delay(random(20, 100));
        radio.transmit((uint8_t*)&pkt, sizeof(pkt));
        
        digitalWrite(LED_PIN, LOW);
        delay(1500);
      }
      
      // Broadcast (0 = всем): пересылаем если TTL > 0
      else if (pkt.dst == 0 && pkt.ttl > 0 && pkt.src != MY_NODE_ID) {
        Serial.println(">>> Repeat BROADCAST");
        showRepeatScreen(pkt.src, pkt.dst, pkt.ttl, pkt.ttl - 1);
        
        pkt.ttl--;
        delay(random(20, 100));
        radio.transmit((uint8_t*)&pkt, sizeof(pkt));
        delay(1500);
      }
      
      // Остальные пакеты игнорируем
      else {
        Serial.println(">>> IGNORED (not for me)");
        delay(2000);
      }
    } else {
      Serial.printf("RX error: %d\n", state);
    }
    
    receivedFlag = false;
    radio.startReceive();
  }
  
  // === 2. АВТОМАТИЧЕСКИЙ ПИНГ ===
  if (!pingInProgress && millis() - lastPingTime > PING_INTERVAL) {
    lastPingTime = millis();
    
    // Чередуем пинг между Admin и Climber
    if (pingTarget == 0) {
      sendPing(ADMIN_ID);
      pingTarget = 1;
    } else {
      sendPing(CLIMBER_ID);
      pingTarget = 0;
    }
    
    // Показываем анимацию отправки
    unsigned long animStart = millis();
    while (millis() - animStart < 3000) { // 3 секунды анимации
      showPingScreen(pingTarget == 0 ? CLIMBER_ID : ADMIN_ID, false, true);
      delay(100);
    }
  }
  
  // === 3. ТАЙМАУТ ПИНГА (если не получили ответ) ===
  if (pingInProgress && millis() - pingStartTime > PING_TIMEOUT) {
    Serial.println(">>> Ping timeout - no response");
    showPingScreen(pingTarget == 0 ? CLIMBER_ID : ADMIN_ID, false);
    pingInProgress = false;
    lastPingSuccess = false;
    delay(2000);
  }
  
  // === 4. РУЧНОЙ ПИНГ ПО КНОПКЕ BOOT ===
  if (digitalRead(BOOT_BUTTON) == LOW) {
    delay(50); // Debounce
    if (digitalRead(BOOT_BUTTON) == LOW) {
      Serial.println(">>> Manual ping triggered");
      sendPing(ADMIN_ID);
      
      // Анимация отправки
      unsigned long animStart = millis();
      while (millis() - animStart < 3000) {
        showPingScreen(ADMIN_ID, false, true);
        delay(100);
      }
      
      // Ждем отпускания кнопки
      while (digitalRead(BOOT_BUTTON) == LOW) {
        delay(10);
      }
    }
  }
  
  // === 5. ВОЗВРАТ К ГЛАВНОМУ ЭКРАНУ ===
  if (millis() - lastPacketTime > 3000 && !pingInProgress) {
    static unsigned long lastScreenUpdate = 0;
    if (millis() - lastScreenUpdate > 1000) {
      showMainScreen();
      lastScreenUpdate = millis();
    }
  }
  
  // === 6. ГЛУБОКИЙ СОН (для Receiver) ===
  if (millis() - lastActivity > SLEEP_TIMEOUT && !pingInProgress) {
    Serial.println(">>> GOING TO SLEEP (Receiver)");
    showSleepScreen();
    delay(1000);
    
    radio.sleep();
    
    esp_sleep_enable_ext1_wakeup(1ULL << LORA_DIO1, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();
  }
  
  delay(10);
}