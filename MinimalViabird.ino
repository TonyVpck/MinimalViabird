// Minimal ViaBird (avec ESP32-CAM) /////////////////////////////////////////////////////////////
//                                                                                             //
// Octobre 2023  - Tony Vanpoucke                                                              //
// Inspiration et versions précédentes : Emilien Jégou, Yannick Leduc, Pierre-André Souville.  //
/////////////////////////////////////////////////////////////////////////////////////////////////

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

const int capteurTactile = 14;  // la broche de déclanchement tactile branché sur le GPIO14.
int brocheLED = 12;             // "LED de bug" branché à la broche GPIO12.

const int seuilTactile = 20;    // Sensibilité du toucher de l'oiseau sur le perchoir
int Trigger = 7;                // tant qu'un oiseau est dessus il prend X photos par secondes

// --- Autres variables ---
static bool cartePresente = false;  // Détecte si la carte est bien insérée
int numero_fichier = 0;             // Numéro de la photo depuis l'allumage

void setup() {
  Serial.begin(115200);
  pinMode(brocheLED, OUTPUT);
  digitalWrite(brocheLED, LOW);

  // définition des broches de la caméra pour le modèle AI Thinker - ESP32-CAM
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;  //YUV422|GRAYSCALE|RGB565|JPEG
  config.frame_size = FRAMESIZE_VGA;     // QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
  config.jpeg_quality = 10;              // 0-63 ; plus bas = meilleure qualité
  config.fb_count = 2;                   // nombre de frame buffers

  // initialisation de la caméra
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Echec de l'initialisation de la camera, erreur 0x%x", err);
    digitalWrite(brocheLED, HIGH);
    return;
  }
  sensor_t *s = esp_camera_sensor_get();

  // initialisation de la carte micro SD
  // en mode 1 bit: plus lent, mais libère des broches
  if (SD_MMC.begin("/sdcard", true)) {
    uint8_t cardType = SD_MMC.cardType();
    if (cardType != CARD_NONE) {
      Serial.println("Carte SD Initialisee.");
      cartePresente = true;
    }
  } else {
    digitalWrite(brocheLED, HIGH);
  }
}

void loop() {
  int valeurCapa = touchRead(capteurTactile);  // get value using T1
  Serial.print("Data:");
  Serial.println(valeurCapa);

  if (valeurCapa <= seuilTactile) {
    enregistrer_photo();

    for (int i = 0; i < 4; i++) {  // Faire clignotter la led 4 fois quand la photo se prend
      delay(75);
      digitalWrite(brocheLED, HIGH);
      delay(75);
      digitalWrite(brocheLED, LOW);
    }

    delay(Trigger * 1000);  // Temps à attendre entre deux photos (en secondes)
    Serial.println("Prêt à reprendre une photo");
  }
}

// --- Capture de l'image ---
void enregistrer_photo(void) {
  char adresse[20] = "";   // nom du fichier à enregistrer
  camera_fb_t *fb = NULL;  // cache de la photo

  fb = esp_camera_fb_get();  // Prendre la photo
  Serial.println("Clic Clac !");
  if (!fb) {
    Serial.println("Echec de la prise de photo.");
    digitalWrite(brocheLED, HIGH);  // s'il y a un problème la LED de bug s'allume.
    return;
  }
  numero_fichier = numero_fichier + 1;

  // --- Enregistrement sur la SD ---
  sprintf(adresse, "/%d.jpg", numero_fichier);
  fs::FS &fs = SD_MMC;
  File file = fs.open(adresse, FILE_WRITE);

  if (!file) {
    Serial.println("Echec lors de la creation du fichier.");
    digitalWrite(brocheLED, HIGH);  // s'il y a un problème la LED de bug s'allume.
  } else {
    digitalWrite(brocheLED, LOW);  // si tout se passe bien la LED de bug s'éteint.
    file.write(fb->buf, fb->len);  // écriture de l'image
    Serial.printf("Fichier enregistre: %s\n", adresse);
  }
  file.close();  // ferme le fichier nouvellement créé.
  esp_camera_fb_return(fb);
}
