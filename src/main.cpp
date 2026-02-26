/**
 * PROJECT: ESP32 2.9" E-Paper Running Weather Station
 * DESCRIPTION: Battery-optimized (Deep Sleep) Weather and Running Gear Display.
 * AUTHOR: Christoph
 * DATE: 2026
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "time.h"
#include "secrets.h" // Enthält SSID, Passwort und API-Keys

// --- E-PAPER LIBRARIES ---
#include <GxEPD2_3C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// Pins für ESP32 DevKit V1
#define EPD_CS     5 
#define EPD_DC    17 
#define EPD_RST   16 
#define EPD_BUSY   4 
#define EPD_SCK   18 
#define EPD_MOSI  23 

// Deep Sleep Konfiguration
#define uS_TO_S_FACTOR 1000000ULL  /* Umrechnungsfaktor von Mikrosekunden zu Sekunden */
#define TIME_TO_SLEEP  900         /* Zeitintervall für Deep Sleep (900s = 15 Minuten) */

// ==========================================
// KONFIGURATION: SCHWELLENWERTE
// ==========================================
const float DL_LIMIT_SINGLET = 27.0;
const float DL_LIMIT_TANK    = 22.0;
const float DL_LIMIT_TSHIRT  = 12.0;
const float DL_LIMIT_LS      = 6.0;
const float DL_LIMIT_SHORT   = 9.0;

const float T_LIMIT_SINGLET  = 23.0;
const float T_LIMIT_TANK     = 16.0;
const float T_LIMIT_TSHIRT   = 9.0;
const float T_LIMIT_LS       = 3.0;
const float T_LIMIT_SHORT    = 6.0;

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> display(GxEPD2_290_C90c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

struct WeatherData {
  float temp = 0;
  float feelsLike = 0;
  int humidity = 0;
  float wind = 0;
  int weatherId = 800;
  String iconCode = "01d";
  String recDL = "";
  String recTempo = "";
} currentDisplayWeather;

void fetchWeather();
void updateDisplay();
String calculateRunningGear(float temp, float wind, int humidity, int weatherId, bool isTempo);

// --- GEOMETRISCHE ICON-ENGINE ---

void drawCloud(int x, int y, uint16_t color) {
    display.fillCircle(x, y, 9, color);
    display.fillCircle(x - 8, y + 4, 7, color);
    display.fillCircle(x + 8, y + 4, 7, color);
    display.fillRect(x - 8, y + 4, 16, 8, color);
}

void drawSun(int x, int y) {
    display.fillCircle(x, y, 9, GxEPD_RED);
    for (int i = 0; i < 360; i += 45) {
        int x1 = x + cos(i * DEG_TO_RAD) * 11;
        int y1 = y + sin(i * DEG_TO_RAD) * 11;
        int x2 = x + cos(i * DEG_TO_RAD) * 15;
        int y2 = y + sin(i * DEG_TO_RAD) * 15;
        display.drawLine(x1, y1, x2, y2, GxEPD_RED);
    }
}

void drawMoon(int x, int y) {
    display.fillCircle(x, y, 12, GxEPD_BLACK);
    display.fillCircle(x + 7, y - 4, 12, GxEPD_WHITE); 
}

void drawWeatherIcon(int x, int y, int id, String icon) {
    bool isNight = icon.endsWith("n");
    x += 20; y += 25; 

    if (id == 800) { // Clear
        if (isNight) drawMoon(x, y); else drawSun(x, y);
    } 
    else if (id >= 801 && id <= 802) { // Partly Cloudy
        if (isNight) drawMoon(x + 5, y - 5); else drawSun(x + 5, y - 5);
        display.fillCircle(x - 2, y + 5, 11, GxEPD_WHITE); 
        drawCloud(x - 2, y + 5, GxEPD_BLACK);
    } 
    else if (id >= 803 && id <= 804) { // Cloudy
        drawCloud(x, y, GxEPD_BLACK);
    } 
    else if (id >= 500 && id <= 531) { // Rain
        drawCloud(x, y - 4, GxEPD_BLACK);
        for (int i = 0; i < 3; i++) {
            display.drawLine(x - 6 + (i * 6), y + 8, x - 8 + (i * 6), y + 14, GxEPD_RED);
        }
    } else {
        display.drawCircle(x, y, 10, GxEPD_BLACK);
    }
}

// --- HILFSFUNKTIONEN ---

void printWrapped(int x, int y, String text, int maxChars) {
    if (text.length() <= maxChars) {
        display.setCursor(x, y);
        display.print(text);
    } else {
        int split = text.lastIndexOf(' ', maxChars);
        if (split == -1) split = maxChars;
        display.setCursor(x, y);
        display.print(text.substring(0, split));
        display.setCursor(x, y + 14); 
        display.print(text.substring(split + 1));
    }
}

void drawWifiIcon(int x, int y) {
    display.fillCircle(x, y, 1, GxEPD_BLACK);
    display.drawCircle(x, y, 3, GxEPD_BLACK);
    display.drawCircle(x, y, 6, GxEPD_BLACK);
    display.fillRect(x-8, y+1, 16, 8, GxEPD_WHITE);
}

void setup() {
  Serial.begin(115200);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 50, false);
  display.setRotation(1);

  // WiFi verbinden
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 20) { 
    delay(500); 
    counter++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    configTime(3600, 3600, "pool.ntp.org");
    fetchWeather();
    updateDisplay();
  }

  // Display schlafen legen (spart Strom)
  display.hibernate();

  // Deep Sleep konfigurieren
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  Serial.println("Entering Deep Sleep...");
  esp_deep_sleep_start();
}

void loop() {
  // Bleibt leer, da setup() den Deep Sleep startet
}

void updateDisplay() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    
    // --- LINKS: WETTER ---
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_RED);
    display.setCursor(10, 20);
    display.print(DISPLAY_LOCATION_NAME);

    drawWeatherIcon(95, 30, currentDisplayWeather.weatherId, currentDisplayWeather.iconCode);

    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(10, 55); 
    display.printf("%.1f C", currentDisplayWeather.temp);
    display.setCursor(10, 72); 
    display.printf("Gef: %.1f C", currentDisplayWeather.feelsLike);
    
    // Wind
    display.drawLine(10, 95, 20, 95, GxEPD_BLACK);
    display.drawLine(8, 98, 18, 98, GxEPD_BLACK);
    display.setCursor(25, 100);
    display.printf("%.0f km/h", currentDisplayWeather.wind);
    
    // Feuchte
    display.fillCircle(88, 97, 2, GxEPD_BLACK);
    display.fillTriangle(86, 97, 90, 97, 88, 92, GxEPD_BLACK);
    display.setCursor(95, 100); 
    display.printf("%d%%", currentDisplayWeather.humidity);

    display.drawLine(145, 10, 145, 105, GxEPD_BLACK);

    // --- RECHTER BEREICH: TRAINING ---
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_RED);
    display.setCursor(155, 20);
    display.print("Dauerlauf");
    display.setFont(NULL);
    display.setTextColor(GxEPD_BLACK);
    printWrapped(155, 28, currentDisplayWeather.recDL, 26);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_RED);
    display.setCursor(155, 75); 
    display.print("Tempolauf");
    display.setFont(NULL);
    display.setTextColor(GxEPD_BLACK);
    printWrapped(155, 83, currentDisplayWeather.recTempo, 26);

    // --- STATUS-BAR ---
    display.drawLine(0, 112, 296, 112, GxEPD_BLACK);
    drawWifiIcon(15, 124); 
    display.setFont(NULL);
    display.setCursor(28, 118);
    display.print("WiFi: ");
    display.print(DISPLAY_WIFI_NAME);
    display.setCursor(190, 118);
    display.printf("Akt.: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  } while (display.nextPage());
}

void fetchWeather() {
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + String(SECRET_LOCATION) + "&units=metric&lang=de&appid=" + String(SECRET_OPENWEATHER_API_KEY);
  http.begin(url);
  if (http.GET() == 200) {
    JsonDocument doc;
    deserializeJson(doc, http.getString());
    currentDisplayWeather.temp = doc["main"]["temp"];
    currentDisplayWeather.feelsLike = doc["main"]["feels_like"];
    currentDisplayWeather.humidity = doc["main"]["humidity"];
    currentDisplayWeather.wind = doc["wind"]["speed"].as<float>() * 3.6;
    currentDisplayWeather.weatherId = doc["weather"][0]["id"];
    currentDisplayWeather.iconCode = (const char*)doc["weather"][0]["icon"];
    
    currentDisplayWeather.recDL = calculateRunningGear(currentDisplayWeather.temp, currentDisplayWeather.wind, currentDisplayWeather.humidity, currentDisplayWeather.weatherId, false);
    currentDisplayWeather.recTempo = calculateRunningGear(currentDisplayWeather.temp, currentDisplayWeather.wind, currentDisplayWeather.humidity, currentDisplayWeather.weatherId, true);
  }
  http.end();
}

String calculateRunningGear(float temp, float wind, int humidity, int weatherId, bool isTempo) {
  String gearUpper = "";
  String gearLower = "";
  String gearAddon = "";
  String alert = "";

  if (isTempo) {
    if (temp >= T_LIMIT_SHORT) gearLower = "Kurz"; else gearLower = "Tights";
    if (temp > T_LIMIT_SINGLET) gearUpper = "Singlet";
    else if (temp > T_LIMIT_TANK) gearUpper = "Tanktop";
    else if (temp > T_LIMIT_TSHIRT) gearUpper = "Shirt";
    else if (temp > T_LIMIT_LS) gearUpper = "Longsleeve";
    else gearUpper = "LS + Layer";
  } else {
    if (temp >= DL_LIMIT_SHORT) gearLower = "Kurz"; else gearLower = "Tights";
    if (temp > DL_LIMIT_SINGLET) gearUpper = "Singlet";
    else if (temp > DL_LIMIT_TANK) gearUpper = "Tanktop";
    else if (temp > DL_LIMIT_TSHIRT) gearUpper = "Shirt";
    else if (temp > DL_LIMIT_LS) gearUpper = "Longsleeve";
    else gearUpper = "LS + Layer";
  }

  bool isRaining = (weatherId >= 200 && weatherId <= 531);
  if (isRaining && temp < 15.0) gearAddon += " + Regenjacke";
  else if (wind >= 20.0 && temp < 13.0) gearAddon += " + Windjacke";

  if (temp <= 5.0) gearAddon += " + Handschuhe/Muetze";
  else if ((weatherId == 800 && temp > 15.0) || isRaining) gearAddon += " + Cap";

  if (temp >= 25.0 || (temp >= 20.0 && humidity >= 80)) alert = " !H2O";

  return gearLower + ", " + gearUpper + gearAddon + alert;
}
