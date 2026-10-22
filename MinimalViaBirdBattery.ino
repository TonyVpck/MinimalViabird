#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"
#include "driver/touch_pad.h"
#include "esp_sleep.h"

// ====================== RÉGLAGES ======================
#define TOUCH_PIN          TOUCH_PAD_NUM6   // GPIO 14 = Touch Pad 6
#define TOUCH_GPIO         14
#define SEUIL_RATIO_PCT    75               // seuil = 75 % de la valeur de repos
#define SEUIL_DEFAUT       180              // utilisé si la calibration échoue
#define ENTRE_PHOTOS_S     7                // secondes entre 2 photos tant que l'oiseau est là
#define INACTIVITE_S       10               // secondes sans contact avant de dormir
#define FRAMES_ECHAUFFEMENT 3               // images jetées après l'init caméra

const int brocheLED = 12;                   // "LED de bug"
const int brocheFlash = 4;                  // LED flash (aussi DAT1 de la SD)

// Mémoire RTC : conservée pendant le deep sleep
RTC_DATA_ATTR int      photoNumber    = 0;
RTC_DATA_ATTR int      sessionFolder  = -1;   // -1 = pas encore créé
RTC_DATA_ATTR uint16_t seuilTactile   = 0;    // 0 = pas encore calibré

// Pin definition for CAMERA_MODEL_AI_THINKER
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

camera_config_t config;
bool touchEnabled = false;
bool cameraReady = false;
bool cameraChaude = false;
String currentFolderPath = "";
unsigned long lastTouchMs = 0;
unsigned long lastPhotoMs = 0;
unsigned long lastPrint = 0;

// ====================== TOUCH ======================
void enableTouch() {
  touch_pad_init();
  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
  touch_pad_config(TOUCH_PIN, 0);
  touchEnabled = true;
  delay(50); // laisse la mesure se stabiliser
}

void disableTouch() {
  if (touchEnabled) {
    touch_pad_deinit();
    pinMode(TOUCH_GPIO, INPUT);
    touchEnabled = false;
  }
}

uint16_t lireTouch() {
  uint16_t v = 0;
  touch_pad_read(TOUCH_PIN, &v);
  return v;
}

// Lecture filtrée : 3 mesures consécutives sous le seuil
bool estTouche() {
  for (int i = 0; i < 3; i++) {
    if (lireTouch() > seuilTactile) return false;
    delay(10);
  }
  return true;
}

// Valeur de repos moyenne (le touch doit être activé)
uint16_t mesurerRepos(bool &stable) {
  const int N = 20;
  uint32_t somme = 0;
  uint16_t mn = 65535, mx = 0;
  for (int i = 0; i < N; i++) {
    uint16_t v = lireTouch();
    somme += v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    delay(20);
  }
  uint16_t moy = somme / N;
  stable = (moy > 20) && ((mx - mn) <= moy / 5); // dispersion < 20 %
  return moy;
}

void calibrerSeuil() {
  bool stable;
  uint16_t repos = mesurerRepos(stable);
  if (stable) {
    seuilTactile = (uint32_t)repos * SEUIL_RATIO_PCT / 100;
    Serial.printf("Calibration : repos=%u -> seuil=%u\n", repos, seuilTactile);
  } else {
    seuilTactile = SEUIL_DEFAUT;
    Serial.printf("Calibration instable (repos=%u), seuil par défaut %u\n", repos, seuilTactile);
  }
}

// ====================== CAMÉRA ======================
void configInitCamera() {
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera initialization failed");
    ESP.restart();
  }
  cameraReady = true;
  cameraChaude = false;
}

// Jette les premières images (mauvaise exposition juste après l'init)
void echauffementCamera() {
  for (int i = 0; i < FRAMES_ECHAUFFEMENT; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }
  cameraChaude = true;
}

// ====================== SD ======================
bool initMicroSDCard() {
  if (!SD_MMC.begin("/sdcard", true)) { // mode 1-bit
    Serial.println("SD Card Mount Failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("No SD Card detected");
    SD_MMC.end();
    return false;
  }
  return true;
}

// Crée un dossier de session (une seule fois par mise sous tension)
void createSessionFolder() {
  Serial.println("Création du dossier de session...");
  disableTouch();
  delay(100);

  if (!initMicroSDCard()) {
    enableTouch();
    return;
  }

  int n = 0;
  while (n <= 9999) {
    String p = "/" + String(n);
    File t = SD_MMC.open(p.c_str());
    if (!t) break;
    t.close();
    n++;
  }
  if (n > 9999) n = 0;

  String chemin = "/" + String(n);
  if (SD_MMC.mkdir(chemin.c_str())) {
    sessionFolder = n;
    Serial.printf("Dossier de session créé : %s\n", chemin.c_str());
  } else {
    Serial.println("Erreur lors de la création du dossier");
  }

  SD_MMC.end();
  delay(200);
  enableTouch();
}

void takeSavePhoto() {
  Serial.println("=== Prise de photo ===");
  disableTouch();   // GPIO14 est aussi CLK de la SD
  delay(100);

  if (!cameraChaude) echauffementCamera();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    enableTouch();
    return;
  }

  if (!initMicroSDCard()) {
    esp_camera_fb_return(fb);
    enableTouch();
    return;
  }

  String path = currentFolderPath + "/photo" + String(photoNumber) + ".jpg";
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
  } else {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.printf("Photo saved: %s (%d bytes)\n", path.c_str(), fb->len);
    photoNumber++;
  }

  esp_camera_fb_return(fb);
  SD_MMC.end();     // libère GPIO14
  delay(200);

  for (int i = 0; i < 4; i++) {
    digitalWrite(brocheLED, HIGH);
    delay(75);
    digitalWrite(brocheLED, LOW);
    delay(75);
  }
  digitalWrite(brocheLED, LOW);

  enableTouch();
}

// ====================== SOMMEIL ======================
void goToSleep() {
  Serial.println("Inactivité -> préparation du deep sleep");

  // 1. Recalibrage du repos (compense la dérive température / humidité)
  if (!touchEnabled) enableTouch();
  bool stable;
  uint16_t repos = mesurerRepos(stable);
  if (stable && repos > seuilTactile) {
    seuilTactile = (uint32_t)repos * SEUIL_RATIO_PCT / 100;
    Serial.printf("Seuil mis à jour : repos=%u -> seuil=%u\n", repos, seuilTactile);
  }

  // 2. Caméra en veille (PWDN à HIGH, maintenu pendant le sommeil)
  if (cameraReady) esp_camera_deinit();
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  rtc_gpio_hold_en((gpio_num_t)PWDN_GPIO_NUM);

  // 3. LED éteintes et maintenues
  pinMode(brocheFlash, OUTPUT);
  digitalWrite(brocheFlash, LOW);
  rtc_gpio_hold_en((gpio_num_t)brocheFlash);
  digitalWrite(brocheLED, LOW);
  rtc_gpio_hold_en((gpio_num_t)brocheLED);

  // 4. Réveil par touch pad
  touch_pad_config(TOUCH_PIN, seuilTactile);
  touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
  touch_pad_fsm_start();
  esp_sleep_pd_config(ESP_SLEEP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_touchpad_wakeup();

  Serial.println("Deep sleep...");
  Serial.flush();
  esp_deep_sleep_start();
}

// ====================== SETUP / LOOP ======================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector
  Serial.begin(115200);
  delay(500);

  // Libère les broches maintenues pendant le sommeil
  rtc_gpio_hold_dis((gpio_num_t)PWDN_GPIO_NUM);
  rtc_gpio_hold_dis((gpio_num_t)brocheFlash);
  rtc_gpio_hold_dis((gpio_num_t)brocheLED);

  pinMode(brocheLED, OUTPUT);
  digitalWrite(brocheLED, LOW);
  pinMode(brocheFlash, OUTPUT);
  digitalWrite(brocheFlash, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool reveilTactile = (cause == ESP_SLEEP_WAKEUP_TOUCHPAD);
  Serial.printf("Démarrage (%s)\n", reveilTactile ? "réveil tactile" : "mise sous tension");

  configInitCamera();
  enableTouch();

  if (!reveilTactile || seuilTactile == 0) {
    // Première mise sous tension : calibration + nouveau dossier
    photoNumber = 0;
    sessionFolder = -1;
    calibrerSeuil();
    createSessionFolder();
  }

  if (sessionFolder >= 0) currentFolderPath = "/" + String(sessionFolder);
  else currentFolderPath = "";

  Serial.printf("Prêt - dossier : %s, seuil : %u\n", currentFolderPath.c_str(), seuilTactile);

  lastTouchMs = millis();
  lastPhotoMs = 0;

  // Réveillé par un toucher : première photo immédiate (gérée dans loop)
}

void loop() {
  if (touchEnabled) {
    if (millis() - lastPrint > 1000) {
      Serial.printf("Touch: %u (seuil %u)\n", lireTouch(), seuilTactile);
      lastPrint = millis();
    }

    if (estTouche()) {
      lastTouchMs = millis();
      if (lastPhotoMs == 0 || millis() - lastPhotoMs >= ENTRE_PHOTOS_S * 1000UL) {
        takeSavePhoto();
        lastPhotoMs = millis();
        lastTouchMs = millis();
      }
    }

    if (millis() - lastTouchMs > INACTIVITE_S * 1000UL) {
      goToSleep();
    }
  }
  delay(50);
}
