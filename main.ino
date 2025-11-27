/*
 * ESP32-S2 Mini + DFPlayer Mini + RFID(EM4100) - v6.3 "S2-DFP-RFID (No AutoPlay / Low Latency)"
 *
 * 변경 요약 (v6.3)
 *  - RFID 태그 인식 후 재생까지 텀 최소화:
 *    · startPlay()/stopPlay() 에서 대부분 delay() 제거
 *    · 볼륨은 setup 시 1회만 설정 (매 재생마다 volume 명령 제거)
 *    · RFID UART read timeout/딜레이 축소 → 큐에 태그 올리는 시간 단축
 *    · loop() 의 delay(10) → delay(1)
 *
 * 정상 동작 구성:
 *  - DFPlayer @ UART1  (RX=GPIO17 ← DFP TX,  TX=GPIO18 → DFP RX)
 *  - BUSY  @ GPIO16    (LOW = Playing)
 *  - STOP  @ GPIO10    (Active LOW)
 *  - LED   @ GPIO15
 *  - RFID  @ UART0(IDF 드라이버)  RX=GPIO5 (TX 미사용)
 */

#include <Arduino.h>
#include <DFRobotDFPlayerMini.h>

extern "C" {
  #include "driver/uart.h"
  #include "driver/gpio.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/queue.h"
}

/* ========= 스위치 ========= */
#define ENABLE_SERIAL_LOG        0   // 0: 로그 최소화, 1: 개발 로그
#define USE_BUSY_PIN             1   // BUSY 감시 사용

/* ========= 핀/보드 ========= */
// DFPlayer (UART1)
#define DFP_RX_PIN               17  // ESP32-S2 RX1  ← DFPlayer TX
#define DFP_TX_PIN               18  // ESP32-S2 TX1  → DFPlayer RX
#define DFP_BAUD                 9600
// BUSY
#define DFP_BUSY_PIN             16
#define DFP_BUSY_ACTIVE_LOW      1   // BUSY: LOW=재생중, HIGH=정지

// RFID (UART0 / ESP-IDF driver)
#define RFID_UART_NUM            UART_NUM_0
#define RFID_RX_PIN              (gpio_num_t)5
#define RFID_TX_PIN              (gpio_num_t)-1  // 미사용
#define RFID_BAUD                9600
#define RFID_RX_BUF_SIZE         2048
#define RFID_READ_TMO_MS         5      // 기존 20 → 5ms로 축소 (더 빠른 반응)
#define RFID_QUEUE_LEN           16
#define RFID_DEDUP_MS            100

// 버튼/LED
#define STOP_BTN_PIN             10
#define BTN_ACTIVE_LOW           1
#define BTN_DEBOUNCE_MS          50
#define LED_PIN                  15

/* ========= 재생 설정 ========= */
#define DFP_VOLUME_DEFAULT       25        // 0~30
#define MAX_TRACK_NUMBER         17         // 카드 매핑 시 최대 트랙 번호
#define DFP_BOOT_DELAY_MS        1000      // DFPlayer 부팅 대기

/* ========= 전역 ========= */
HardwareSerial DFPSerial(1);   // UART1
DFRobotDFPlayerMini dfp;

typedef struct { char id[11]; } CardMsg;
static QueueHandle_t g_cardQueue;

static bool g_isPlaying         = false;
static uint16_t g_currentTrack  = 0;
static unsigned long g_lastPlayStartMs = 0;

static volatile unsigned long g_lastStopBtnMs = 0;
static volatile bool g_stopRequested = false;
static char lastCardSeen[11] = "";
static unsigned long lastCardSeenMs = 0;

static bool g_dfpOk             = false;  // DFPlayer 초기화 성공 여부

/* ========= 유틸 ========== */
inline void LED_ON()  { digitalWrite(LED_PIN, HIGH); }
inline void LED_OFF() { digitalWrite(LED_PIN, LOW);  }

static void ledBlink(uint8_t t, uint16_t onMs=150, uint16_t offMs=150){
  for(uint8_t i=0;i<t;i++){ LED_ON(); delay(onMs); LED_OFF(); delay(offMs); }
}

#if USE_BUSY_PIN
inline bool busyIsPlaying(){
  int v = digitalRead(DFP_BUSY_PIN);
  return DFP_BUSY_ACTIVE_LOW ? (v==LOW) : (v==HIGH);
}
#endif

inline void dfpFlushRx(){
  while(DFPSerial.available()>0){ (void)DFPSerial.read(); }
}

inline void logln(const char* s){
#if ENABLE_SERIAL_LOG
  Serial.println(s);
#endif
}
inline void logf(const char* s, int v){
#if ENABLE_SERIAL_LOG
  Serial.print(s); Serial.println(v);
#endif
}

/* ========= RFID 파서 ========= */
inline bool isDigitChar(char c){ return (c>='0' && c<='9'); }

// 0x02 ... 10 digits ... 0x03 패턴
bool parse10FromSTXETX(const uint8_t* b, size_t l, char out[11]){
  for(size_t i=0;i<l;i++) if(b[i]==0x02){
    for(size_t j=i+1;j<l;j++) if(b[j]==0x03){
      size_t span=j>i+1 ? j-i-1 : 0;
      if(span>=10){
        for(size_t k=i+1;k+9<=j-1;k++){
          bool ok=true; for(int n=0;n<10;n++){ if(!isDigitChar((char)b[k+n])){ok=false;break;} }
          if(ok){ for(int n=0;n<10;n++) out[n]=(char)b[k+n]; out[10]='\0'; return true; }
        }
      }
      i=j; break;
    }
  }
  return false;
}

// 10자리 + CR/LF 경계
bool parse10CRLF(const uint8_t* b, size_t l, char out[11]){
  for(size_t i=0;i+9<l;i++){
    if(i+10<l){
      uint8_t t=b[i+10];
      if(!(t=='\r'||t=='\n'||t==0x03)) continue;
    }
    bool left=(i==0)||!isDigitChar((char)b[i-1]); if(!left) continue;
    bool ok=true; for(int n=0;n<10;n++){ if(!isDigitChar((char)b[i+n])){ok=false;break;} }
    if(!ok) continue;
    bool right=(i+10>=l)||!isDigitChar((char)b[i+10]); if(!right) continue;
    for(int n=0;n<10;n++) out[n]=(char)b[i+n];
    out[10]='\0';
    return true;
  }
  return false;
}

// 느슨한 10자리
bool parse10Loose(const uint8_t* b, size_t l, char out[11]){
  for(size_t i=0;i+9<l;i++){
    bool left=(i==0)||!isDigitChar((char)b[i-1]); if(!left) continue;
    bool ok=true; for(int n=0;n<10;n++){ if(!isDigitChar((char)b[i+n])){ok=false;break;} }
    if(!ok) continue;
    bool right=(i+10>=l)||!isDigitChar((char)b[i+10]); if(!right) continue;
    for(int n=0;n<10;n++) out[n]=(char)b[i+n];
    out[10]='\0';
    return true;
  }
  return false;
}

bool extractCardIdRobust(const uint8_t* buf, size_t len, char idOut[11]){
  if(parse10FromSTXETX(buf,len,idOut)) return true;
  if(parse10CRLF(buf,len,idOut))       return true;
  if(parse10Loose(buf,len,idOut))      return true;
  return false;
}

/* ========= RFID 태스크 ========= */
void rfidTask(void*){
  uint8_t buf[512];
  for(;;){
    int n = uart_read_bytes(
      RFID_UART_NUM,
      buf,
      sizeof(buf),
      RFID_READ_TMO_MS/portTICK_PERIOD_MS  // 최대 5ms 블로킹
    );

    if(n > 0){
      char id[11];
      if(extractCardIdRobust(buf, (size_t)n, id)){
        unsigned long now = millis();
        bool same = (strncmp(id, lastCardSeen, 10)==0);
        if(!same || (now - lastCardSeenMs) >= RFID_DEDUP_MS){
          CardMsg msg{}; memcpy(msg.id, id, 11);
          xQueueSend(g_cardQueue, &msg, 0);
          memcpy(lastCardSeen, id, 11);
          lastCardSeenMs = now;
#if ENABLE_SERIAL_LOG
          Serial.print("[RFID] "); Serial.println(id);
          Serial.print(" at "); Serial.println(now);
#endif
        }
      }
    }

    // 너무 오래 양보하면 반응성이 떨어지므로 1ms만 양보
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/* ========= DFPlayer 제어 ========= */
// 볼륨은 setup에서 1회만 설정
void applyInitialVolume(uint8_t v){
  dfp.volume(v);
#if ENABLE_SERIAL_LOG
  Serial.print("Volume init: "); Serial.println(v);
#endif
}

// 재생 정지 (불필요한 delay 제거)
void stopPlay(){
  if(!g_isPlaying || !g_dfpOk) return;
  dfp.stop();            // 명령만 보내고 기다리지 않음
  g_isPlaying    = false;
  g_currentTrack = 0;
  LED_OFF();
  logln("■ Stopped");
}

// 재생 시작 (최소한의 명령만 전송)
void startPlay(uint16_t track){
  if(!g_dfpOk) return;
  if(track == 0 || track > MAX_TRACK_NUMBER) return;

  unsigned long now = millis();

  // 같은 트랙이 300ms 이내에 반복 요청되면 무시 (원하면 제거 가능)
  if(g_isPlaying && g_currentTrack == track && (now - g_lastPlayStartMs) < 300){
    return;
  }

  // 다른 트랙으로 교체 시 이전 트랙 강제중지
  if(g_isPlaying && g_currentTrack != track){
    dfp.stop();  // 여기서는 약간만 기다려도 괜찮지만 지연 최소화를 위해 delay 생략
  }

#if ENABLE_SERIAL_LOG
  Serial.print("▶ Play: "); Serial.println(track);
  Serial.print(" at "); Serial.println(millis());
#endif

  dfp.playMp3Folder(track);   // /mp3/000X.mp3
  g_isPlaying       = true;
  g_currentTrack    = track;
  g_lastPlayStartMs = now;
  LED_ON();
}

// DFPlayer 상태 업데이트(BUSY/이벤트) - 비블로킹
void updatePlaybackState(){
  static unsigned long lastCheck=0;
  unsigned long now = millis();
  if(now - lastCheck < 50) return;   // 50ms 주기로 상태 확인
  lastCheck = now;

  // DFPlayer 이벤트 처리 (최대 몇 개만 읽어서 시간 제한)
  uint8_t handled = 0;
  while(dfp.available() && handled < 4){
    uint8_t type = dfp.readType();
    int value = dfp.read();
    handled++;

    if(type == DFPlayerPlayFinished){
      logln("♪ Finished (event)");
      stopPlay();
    }
#if ENABLE_SERIAL_LOG
    else if(type == DFPlayerError){
      Serial.print("⚠ DFP Error: "); Serial.println(value);
    }
#endif
  }

#if USE_BUSY_PIN
  if(g_isPlaying){
    bool busy = busyIsPlaying();
    // 재생 시작 직후 BUSY 변동 때문에 1초 정도는 기다렸다가 해석
    if(!busy && (now - g_lastPlayStartMs > 1000)){
      logln("♪ Finished (BUSY inactive)");
      stopPlay();
    }
  }
#endif
}

/* ========= 정지 버튼 ISR ========= */
void IRAM_ATTR stopBtnISR(){
  unsigned long now = millis();
  if(now - g_lastStopBtnMs < BTN_DEBOUNCE_MS) return;
  g_lastStopBtnMs = now;
  int lvl = digitalRead(STOP_BTN_PIN);
  if(BTN_ACTIVE_LOW ? (lvl==LOW) : (lvl==HIGH)){
    g_stopRequested = true;
  }
}

/* ========= 카드→트랙 매핑 ========= */
// 예시: 카드 뒤 2자리 사용 (..10→1, ..11→5, ..12→2, 그 외 3)
// uint16_t mapCardToTrack(const char id[11]){
//   int tail = ( (id[8]-'0')*10 + (id[9]-'0') );
//   switch(tail){
//     case 10: return 1;
//     case 11: return 5;
//     case 12: return 2;
//     default: return 3;
//   }
// }
// 카드 뒤 2자리(10~26)를 트랙 1~17로 매핑
uint16_t mapCardToTrack(const char id[11]){
  int tail = ( (id[8] - '0') * 10 + (id[9] - '0') );

#if ENABLE_SERIAL_LOG
  Serial.print("[MAP] tail="); Serial.println(tail);
#endif

  // 10 → 1, 11 → 2, ..., 26 → 17
  if (tail >= 10 && tail <= 26) {
    uint16_t track = (uint16_t)(tail - 9);  // track = tail - 9
    if (track >= 1 && track <= MAX_TRACK_NUMBER) {
      return track;
    }
  }

  // 그 외 번호는 0 반환 → startPlay()에서 무시됨
  return 0;
}

/* ========= SETUP ========= */
void setup(){
#if ENABLE_SERIAL_LOG
  Serial.begin(115200);
  delay(1500);  // USB CDC 안정화
  Serial.println("\n[S2-DFP-RFID] v6.3 Boot (No AutoPlay / Low Latency)");
#endif

  pinMode(LED_PIN, OUTPUT);
  LED_OFF();
  ledBlink(2,120,120);

  // STOP 버튼
  pinMode(STOP_BTN_PIN, BTN_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
  attachInterrupt(digitalPinToInterrupt(STOP_BTN_PIN), stopBtnISR, CHANGE);

#if USE_BUSY_PIN
  pinMode(DFP_BUSY_PIN, DFP_BUSY_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
#endif

  // DFPlayer 초기화
  DFPSerial.begin(DFP_BAUD, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
  delay(DFP_BOOT_DELAY_MS);
  dfpFlushRx();

  g_dfpOk = false;
  for(int attempt=1; attempt<=3 && !g_dfpOk; ++attempt){
#if ENABLE_SERIAL_LOG
    Serial.print("[DFP] begin attempt "); Serial.println(attempt);
#endif
    if(dfp.begin(DFPSerial, false, true)){
      // delay(300);
      // dfp.setTimeOut(800);
      // delay(100);
      dfpFlushRx();
      int files = dfp.readFileCounts(DFPLAYER_DEVICE_SD);
      delay(200);
      if(files > 0){
#if ENABLE_SERIAL_LOG
        Serial.print("[DFP] files="); Serial.println(files);
#endif
        applyInitialVolume(DFP_VOLUME_DEFAULT);
        g_dfpOk = true;
        break;
      }
    }
    delay(200);
  }

  // DFPlayer 결과 표시
  if(g_dfpOk) {
    LED_ON();  delay(250);
    LED_OFF(); delay(250);
  } else {
    LED_OFF(); delay(500);
  }

  // RFID 초기화(UART0 / IDF)
  bool rfidOk = true;
  uart_config_t cfg{
    .baud_rate = (int)RFID_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity    = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION_MAJOR >= 5
    .source_clk= UART_SCLK_DEFAULT
#else
    .source_clk= UART_SCLK_APB
#endif
  };

  if(ESP_OK != uart_param_config(RFID_UART_NUM, &cfg))                         rfidOk=false;
  if(ESP_OK != uart_set_pin(RFID_UART_NUM, RFID_TX_PIN, RFID_RX_PIN,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE))           rfidOk=false;
  if(ESP_OK != uart_driver_install(RFID_UART_NUM, RFID_RX_BUF_SIZE, 0, 0,
                                   NULL, 0))                                   rfidOk=false;
  if(ESP_OK != uart_set_rx_timeout(RFID_UART_NUM, 2))                          rfidOk=false;
  if(ESP_OK != uart_set_rx_full_threshold(RFID_UART_NUM, 1))                   rfidOk=false;

#if ENABLE_SERIAL_LOG
  Serial.print("[RFID] init: "); Serial.println(rfidOk ? "OK" : "FAIL");
#endif

  g_cardQueue = xQueueCreate(RFID_QUEUE_LEN, sizeof(CardMsg));

  // RFID 태스크 (우선순위 2로 약간 높게)
  xTaskCreate(rfidTask, "rfidTask", 4096, nullptr, 2, nullptr);

#if ENABLE_SERIAL_LOG
  Serial.println("[SYS] Ready (No AutoPlay / Low Latency)");
#endif
}

/* ========= LOOP ========= */
void loop(){
  // STOP 버튼 처리
  if(g_stopRequested){
    stopPlay();
    g_stopRequested=false;
  }

  // RFID → 재생 (큐 비울 때까지 처리)
  CardMsg msg;
  while(xQueueReceive(g_cardQueue, &msg, 0)==pdTRUE){
    uint16_t track = mapCardToTrack(msg.id);
#if ENABLE_SERIAL_LOG
    Serial.print("[RFID] play track "); Serial.println(track);
#endif
    startPlay(track);
  }

  // DFPlayer 상태 업데이트(BUSY/이벤트)
  if(g_dfpOk){
    updatePlaybackState();
  }

  // loop는 1ms만 양보 → RFID→재생 경로 딜레이 최소화
  delay(1);
}
