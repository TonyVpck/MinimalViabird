#include "esp_camera.h"
#include "FS.h" // SD Card ESP32
#include "SD_MMC.h" // SD Card ESP32
#include "soc/soc.h" // Disable brownout problems
#include "soc/rtc_cntl_reg.h" // Disable brownout problems
#include "driver/rtc_io.h"
#include "driver/touch_pad.h" // Pour désactiver complètement le touch

#define TOUCH_PIN TOUCH_PAD_NUM6 // GPIO 14 = Touch Pad 6
#define TOUCH_GPIO 14
int brocheLED = 12; // "LED de bug" branché à la broche GPIO12.
const int seuilTactile = 180; // Sensibilité du toucher de l'oiseau sur le perchoir
int Trigger = 7; // tant qu'un oiseau est dessus il prend X photos par seconde
bool touchEnabled = false; // État du touch sensor

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
int photoNumber = 0;
int sessionFolder = 0; // Numéro du dossier de session
String currentFolderPath = ""; // Chemin du dossier actuel

// Activer le touch sensor
void enableTouch() {
  // Initialiser le système touch
  touch_pad_init();
  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
  touch_pad_config(TOUCH_PIN, 0);
  touchEnabled = true;
  Serial.println("Touch activé");
}

// Désactiver COMPLÈTEMENT le touch sensor
void disableTouch() {
  if (touchEnabled) {
    // Désinitialiser complètement le touch
    touch_pad_deinit();
    
    // Mettre le GPIO en mode input normal
    pinMode(TOUCH_GPIO, INPUT);
    
    touchEnabled = false;
    Serial.println("Touch COMPLÈTEMENT désactivé");
  }
}

// Initialize the camera
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
}

// Initialize SD card (appelé uniquement pendant la prise de photo)
bool initMicroSDCard() {
  Serial.println("Initializing SD Card...");
  if (!SD_MMC.begin("/sdcard", true)) { // Mode 1-bit pour éviter conflits
    Serial.println("SD Card Mount Failed");
    return false;
  }
  
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD Card detected");
    SD_MMC.end();
    return false;
  }
  
  Serial.println("SD Card initialized successfully");
  return true;
}

// Créer un nouveau dossier de session au démarrage
void createSessionFolder() {
  Serial.println("Création du dossier de session...");
  
  disableTouch();
  delay(100);
  
  if (!initMicroSDCard()) {
    Serial.println("Impossible de créer le dossier de session");
    enableTouch();
    return;
  }
  
  // Chercher le prochain numéro de dossier disponible
  sessionFolder = 0;
  while (true) {
    String folderPath = "/" + String(sessionFolder);
    
    // Vérifier si le dossier existe
    File testDir = SD_MMC.open(folderPath.c_str());
    if (!testDir) {
      // Le dossier n'existe pas, on peut l'utiliser
      break;
    }
    testDir.close();
    sessionFolder++;
    
    // Sécurité : limiter à 9999 dossiers
    if (sessionFolder > 9999) {
      Serial.println("Trop de dossiers ! Réinitialisation à 0");
      sessionFolder = 0;
      break;
    }
  }
  
  // Créer le nouveau dossier
  currentFolderPath = "/" + String(sessionFolder);
  if (SD_MMC.mkdir(currentFolderPath.c_str())) {
    Serial.printf("Dossier de session créé : %s\n", currentFolderPath.c_str());
  } else {
    Serial.println("Erreur lors de la création du dossier");
    currentFolderPath = ""; // Fallback sur la racine
  }
  
  SD_MMC.end();
  delay(200);
  enableTouch();
}

// Capture and save photo
void takeSavePhoto() {
  // 1. Désactiver COMPLÈTEMENT le touch avant d'utiliser la SD
  Serial.println("=== Début prise de photo ===");
  disableTouch();
  delay(100); // Délai important pour stabiliser
  
  // 2. Capturer la photo
  Serial.println("Capture de l'image...");
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    enableTouch(); // Réactiver le touch même en cas d'erreur
    return;
  }
  Serial.println("Image capturée");
  
  // 3. Initialiser la SD
  if (!initMicroSDCard()) {
    esp_camera_fb_return(fb);
    enableTouch();
    return;
  }
  
  // 4. Sauvegarder la photo
  String path = currentFolderPath + "/photo" + String(photoNumber) + ".jpg";
  Serial.printf("Saving photo to: %s\n", path.c_str());
  
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
  } else {
    file.write(fb->buf, fb->len);
    Serial.printf("Photo saved: %s (%d bytes)\n", path.c_str(), fb->len);
    file.close();
    photoNumber++;
  }
  
  // 5. Libérer les ressources
  esp_camera_fb_return(fb);
  Serial.println("Fermeture de la SD...");
  SD_MMC.end(); // Fermer la SD pour libérer GPIO 14
  
  // 6. Délai de stabilisation avant de réactiver le touch
  delay(200); // Délai plus long pour être sûr
  
  // 7. Faire clignoter la LED
  for (int i = 0; i < 4; i++) {
    digitalWrite(brocheLED, HIGH);
    delay(75);
    digitalWrite(brocheLED, LOW);
    delay(75);
  }
  
  // 8. Réactiver le touch
  enableTouch();
  Serial.println("=== Photo terminée, touch réactivé ===\n");
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector
  Serial.begin(115200);
  delay(2000);
  
  pinMode(brocheLED, OUTPUT);
  digitalWrite(brocheLED, LOW);
  
  Serial.println("Initialisation de la caméra...");
  configInitCamera();
  
  // Créer le dossier de session
  Serial.println("Création du dossier de session...");
  createSessionFolder();
  
  // Activer le touch après l'initialisation
  Serial.println("Activation du touch sensor...");
  enableTouch();
  
  Serial.println("\n=================================");
  Serial.printf("Système prêt - Dossier session: %s\n", currentFolderPath.c_str());
  Serial.println("En attente de toucher...");
  Serial.println("=================================\n");
}

void loop() {
  if (touchEnabled) {
    uint16_t touchvalue;
    touch_pad_read(TOUCH_PIN, &touchvalue);
    
    // Debug moins verbeux
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 500) {
      Serial.print("Touch value: ");
      Serial.println(touchvalue);
      lastPrint = millis();
    }
    
    if (touchvalue <= seuilTactile) {
      Serial.println("\n>>> Touch détecté ! Prise de photo...");
      takeSavePhoto();
      
      Serial.println("Attente avant prochaine photo...");
      delay(Trigger * 1000); // Temps à attendre entre deux photos (en secondes)
      Serial.println("Prêt à reprendre une photo\n");
    }
  }
  
  delay(50); // Petit délai pour éviter de saturer le Serial
}
