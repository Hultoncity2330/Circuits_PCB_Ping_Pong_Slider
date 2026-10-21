#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>


// =====================================
// CONFIGURATION OLED
// =====================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

// Broches I2C de la XIAO ESP32-S3
#define SDA_PIN 5  // D4
#define SCL_PIN 6  // D5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);


// =====================================
// CONFIGURATION LED RGB
// =====================================
#define RGB_PIN 4
#define RGB_COUNT 2

Adafruit_NeoPixel leds(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);


// =====================================
// FONCTIONS LED RGB
// =====================================

void eteindreLEDs()
{
  leds.clear();
  leds.show();
}

void allumerLEDsRouge()
{
  leds.setPixelColor(0, leds.Color(255, 0, 0));
  leds.setPixelColor(1, leds.Color(255, 0, 0));
  leds.show();

  Serial.println("D4 et D5 : ROUGE");
}


// =====================================
// SETUP
// =====================================

void setup()
{
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialisation des LED
  leds.begin();
  leds.setBrightness(20);
  eteindreLEDs();

  // Initialisation OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
  {
    Serial.println("Erreur : OLED non detecté !");
    while (true)
    {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Premier message
  display.setTextSize(1);
  display.setCursor(20, 2);
  display.println("XIAO ESP32-S3");

  // Deuxième message
  display.setTextSize(2);
  display.setCursor(16, 30);
  display.println("OLED OK!");

  // Affichage
  display.display();
  Serial.println("Test OLED reussi !");

  // LED RGBG + Extinction
  delay(9000);
  allumerLEDsRouge();

  delay(1000);
  eteindreLEDs();

  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  Serial.println("RESET terminé");
}


void loop() {
  
}
