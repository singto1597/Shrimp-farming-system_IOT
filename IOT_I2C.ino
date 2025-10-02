#include <Wire.h>
#include <DS1302.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <time.h>

#define ONE_WIRE_BUS 14 // DS18B20
#define TDS_PIN 35 // TDS

#define FEEDER_SERVO_PIN 13 // Servo

#define LED_ON_BOARD 2

#define OXYGEN_PUMP_PIN 25
#define FILTER_PIN 26
#define HEATER_PIN 33 
#define COOLER_PIN 32 

#define LIGHT_PIN 27

#define RST_RTC 23
#define DAT_RTC 22
#define CLK_RTC 21

#define OLED_SDA 16
#define OLED_SCL 17

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

#define VREF 3.3   // ค่าแรงดันอ้างอิงของ ESP32
#define ADC_RESOLUTION 4095 // ความละเอียดของ ADC (12-bit)



float TDS_MAX = 150.0; 
float TEMP_MIN = 25.0;
float TEMP_MAX = 30.0;
const float HYSTERESIS = 0.3; 

const char* ssid = "กระจายบุญ";
const char* password = "25222524";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 25200; // GMT+7
const int daylightOffset_sec = 0;

const char* googleScriptURL = "https://script.google.com/macros/s/AKfycbxJml6kIWjmjraKd3SkN68tFN-4r4lITzbSk5Nua2th0zKBcdPc3aa3GVl73-hY0klf/exec";

// DS18B20
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

//OLED
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Servo for feeding
Servo feederServo;

bool foodGiven = false; 
bool oxygenOffMessage = false; 
bool oxygenOnMessage = false; 

// RTC Module
DS1302 rtc(RST_RTC, DAT_RTC, CLK_RTC);
Time t;

unsigned long previousMillis = 0;
const unsigned long interval = 60000; 

// ข้อมูล Telegram
String botToken = "7290873016:AAHMe_2TrILCar166zWO9s4jJsuo9JZ8tzg";  // ใส่โทเคนที่ได้จาก BotFather
String chatID = "7969041356";      // ใส่ Chat ID ของคุณ

float lastTemp = 0.00;
float lastTDS = 0.00;

void setup() {
  Serial.begin(115200);
  Wire.begin(OLED_SDA, OLED_SCL); 

  // Initialize OLED
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();

  // Output Pin Modes
  pinMode(OXYGEN_PUMP_PIN, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  pinMode(COOLER_PIN, OUTPUT);
  pinMode(FILTER_PIN, OUTPUT);

  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(LED_ON_BOARD, OUTPUT);

  digitalWrite(LIGHT_PIN, HIGH);

  digitalWrite(OXYGEN_PUMP_PIN, LOW);
  digitalWrite(HEATER_PIN, HIGH);
  digitalWrite(COOLER_PIN, HIGH);
  digitalWrite(FILTER_PIN, HIGH);

  // RTC Initialization
  rtc.halt(false);
  rtc.writeProtect(false);

  connectWiFi();

  setTime();
  // Sensors Initialization
  sensors.begin();

  // Servo Initialization
  feederServo.attach(FEEDER_SERVO_PIN);
  feederServo.write(180);
  analogReadResolution(12);
  
  display.clearDisplay();
  sendMessage("เริ่มต้นเปิดเครื่องแล้ว");
}

void loop() {
  // Get current time from RTC
  digitalWrite(LED_ON_BOARD, LOW);
  t = rtc.getTime();
  int currentHour = t.hour;
  int currentMinute = t.min;
  char currentTime[6]; 
  sprintf(currentTime, "%02d:%02d", currentHour, currentMinute);

  float temperature = readTemperature();
  float tdsValue = readTDS(temperature);
  char tempMessageChar[50];
  snprintf(tempMessageChar, sizeof(tempMessageChar), "อุณหภูมิ %.2f องศา", temperature);
  String tempMessage = String(tempMessageChar);

  char tdsMessageChar[50];
  snprintf(tdsMessageChar, sizeof(tdsMessageChar), "TDS: %.2f PPM", tdsValue);
  String tdsMessage = String(tdsMessageChar);
  // Define states
  bool oxygenPumpState = true;
  bool heaterState = false;
  bool coolerState = false;
  bool filterState = false;

  
  // Feeding Control
  if ((currentHour == 6 || currentHour == 18) && !foodGiven) {
    sendMessage("ให้อาหารเรียบร้อยแล้ว");
    feed();
    foodGiven = true;
  }
  if (currentHour != 18) {
    foodGiven = false;
  }

  // Oxygen Pump Control
  if (currentHour == 6 && currentMinute < 5) {
    if(!oxygenOffMessage){
      sendMessage("ปิดปั๊มก่อน พักปั๊ม 5 นาที");
      oxygenOffMessage = true;
      oxygenOnMessage = false;
    }
    digitalWrite(OXYGEN_PUMP_PIN, LOW);
    oxygenPumpState = false; // ปิดปั๊มออกซิเจน
  } else {
    if(!oxygenOnMessage && oxygenOffMessage){
      sendMessage("เปิดปั๊มแล้วนะ");
      oxygenOnMessage = true;
    }
    if(oxygenOffMessage){
      oxygenOffMessage = false;
    }
    
    digitalWrite(OXYGEN_PUMP_PIN, HIGH);
    oxygenPumpState = true; // เปิดปั๊มออกซิเจน
  }

  // Heater Control
  if (temperature <= TEMP_MIN ) {
    if (lastTemp + HYSTERESIS > temperature && lastTemp - HYSTERESIS > temperature){
      if(temperature != -99){
        sendMessage(tempMessage + " น้ำเย็นเกินไปแล้ว เปิดเครื่องทำความร้อนเลย");
      }
      else if (temperature == -99){
        sendMessage(tempMessage + " ไม่ได้เชื่อมต่อเซนเซอร์อุณหภูมินะ");
      }
    }
    else if (lastTemp + HYSTERESIS < temperature && lastTemp - HYSTERESIS < temperature){
      sendMessage(tempMessage + " น้ำเริ่มอุ่นขึ้นมาแล้ว");
    }

    if(temperature != -99){
      digitalWrite(HEATER_PIN, LOW);
      heaterState = true;
    }
    else if (temperature == -99){
      digitalWrite(HEATER_PIN, HIGH);
      digitalWrite(COOLER_PIN, HIGH);
      heaterState = false; 
      coolerState = false; 
    }
    
  } 
  else if (temperature >= TEMP_MAX){
    if (lastTemp + HYSTERESIS < temperature && lastTemp - HYSTERESIS < temperature){
      sendMessage(tempMessage + " น้ำร้อนเกินไปแล้ว ระบายความร้อนออกหน่อยดีกว่า");
    }
    else if (lastTemp + HYSTERESIS > temperature && lastTemp - HYSTERESIS > temperature){
      sendMessage(tempMessage + " น้ำเริ่มเย็นขึ้นมาแล้ว");
    }
    digitalWrite(COOLER_PIN, LOW);
    coolerState = true;
  }
  else{
    digitalWrite(HEATER_PIN, HIGH);
    digitalWrite(COOLER_PIN, HIGH);
    heaterState = false; 
    coolerState = false; 
  }
  lastTemp = temperature; // อัพเดตค่าอุณหภูมิก่อนหน้า

  if (tdsValue >= TDS_MAX) {
    if (lastTDS + 30 < tdsValue && lastTDS - 30 < tdsValue){
      sendMessage(tdsMessage + " น้ำขุ่นเกินละ เปิดเครื่องกรองนะ");
    }
    else if (lastTDS + 30 > tdsValue && lastTDS - 30 > tdsValue){
      sendMessage(tdsMessage + " น้ำเริ่มใสขึ้นมาแล้ว");
    }
    digitalWrite(FILTER_PIN, LOW);
    filterState = true;
  } 
  else{
    digitalWrite(FILTER_PIN, HIGH);
    filterState = false;
  }

  if (currentHour >= 6 && currentHour <= 18){
    digitalWrite(LIGHT_PIN, LOW);
  }
  else{
    digitalWrite(LIGHT_PIN, HIGH);
  }

  // Display data on OLED
  display.clearDisplay();
  display.setCursor(97, 0);
  display.printf(currentTime);
  display.setCursor(0, 0);
  display.printf("Temp: %.2f C\n", temperature);
  display.printf("TDS: %.2f ppm\n", tdsValue);

  display.printf("Oxygen: %s\n", oxygenPumpState ? "ON" : "OFF");
  display.printf("Heater: %s\n", heaterState ? "ON" : "OFF");
  display.printf("Cooler: %s\n", coolerState ? "ON" : "OFF");
  display.printf("Filter: %s\n", filterState ? "ON" : "OFF");

  display.display();
  //digitalWrite(LED_ON_BOARD, LOW);
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    digitalWrite(LED_ON_BOARD, HIGH);
    checkWifi();
    display.fillRect(0, 50, SCREEN_WIDTH, 64, BLACK);
    sendDataToGoogleSheets(currentTime, temperature, tdsValue, oxygenPumpState, heaterState, coolerState, filterState);
  }
  

  delay(1000);
}


float readTemperature() {
  sensors.requestTemperatures();
  float temperature = sensors.getTempCByIndex(0);
  if (temperature == DEVICE_DISCONNECTED_C) {
    Serial.println("ไม่สามารถอ่านอุณหภูมิได้");
    return -99.0; // ใช้ค่าเริ่มต้นเมื่ออ่านไม่ได้
  }
  return temperature;
}

float readTDS(int temperature) {
  int analogValue = analogRead(TDS_PIN); // อ่านค่าจาก TDS Sensor
  float voltage = analogValue * VREF / ADC_RESOLUTION; // แปลงค่า ADC เป็นแรงดันไฟฟ้า
  float tdsValue = (voltage / VREF) * 1000; // แปลงแรงดันไฟฟ้าเป็นค่า TDS (mg/L หรือ ppm)

  if (temperature < 0) {
    temperature = 25;
  }
  // การชดเชยอุณหภูมิ (Temperature Compensation)
  tdsValue = tdsValue / (1.0 + 0.02 * (temperature - 25.0));
  return tdsValue;
}

void setTime(){
  // Sync RTC with NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    rtc.setTime(
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec
    );
    Serial.println("RTC time updated from NTP!");
    display.setCursor(0, 50);
    display.println("Time Update!");
    display.display();
  } else {
    Serial.println("Failed to get time from NTP");
  }
}

void checkWifi(){
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    display.setCursor(0, 50);
    display.println("Connecting to Wi-Fi..");
    display.display();

  }
  else if (WiFi.status() == WL_CONNECTED){
    setTime();
  }
}

void connectWiFi() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Connecting to Wi-Fi...");
  display.display();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int attempts = 0;
  
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    WiFi.begin(ssid, password);
    digitalWrite(LED_ON_BOARD, HIGH);
    delay(250);
    digitalWrite(LED_ON_BOARD, LOW);
    delay(250);
    Serial.print(".");
    
    // แสดงความคืบหน้า
    display.fillRect(0, 10, SCREEN_WIDTH, 10, BLACK);
    display.setCursor(0, 10);
    display.printf("Attempt %d/20", attempts + 1);
    display.display();
    
    attempts++;
    
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected!");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Wi-Fi connected!");
    display.display();
  } else {
    Serial.println("\nFailed to connect.");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Failed to connect.");
    //display.println("Restarting...");
    display.display();
    delay(1000); // แสดงข้อความสักครู่ก่อนรีบูต
    //ESP.restart(); // รีบูตบอร์ด
  }
}

// ให้อาหาร
void feed() {
  feederServo.write(70); 
  delay(400);           
  feederServo.write(180);  
}

// ส่งข้อมูลไปยัง Google Sheets
void sendDataToGoogleSheets(String time, float temperature, float tds, bool oxygenPumpState, bool heaterState, bool coolerState, bool filterState) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    // Prepare JSON payload
    String jsonPayload = "{\"time\":\"" + time +
                         "\",\"temperature\":" + String(temperature) +
                         ",\"tds\":" + String(tds) +
                         ",\"oxygenPumpState\":" + (oxygenPumpState ? "true" : "false") +
                         ",\"heaterState\":" + (heaterState ? "true" : "false") +
                         ",\"coolerState\":" + (coolerState ? "true" : "false") +
                         ",\"filterState\":" + (filterState ? "true" : "false") + "}";

    http.begin(googleScriptURL);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(jsonPayload);

    if (httpResponseCode > 0) {
      Serial.println("Data sent successfully to Google Sheets.");
      Serial.println(http.getString());
    } else {
      Serial.print("Error sending data: ");
      Serial.println(httpResponseCode);
    }

    display.setCursor(0, 50);
    display.println("Sent to Sheet");
    display.display();

    http.end();
  } else {
    Serial.println("Wi-Fi not connected. Unable to send data.");
  }
}


void sendMessage(String message) {
  if(WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + botToken + "/sendMessage?chat_id=" + chatID + "&text=" + message;
    http.begin(url);
    int httpResponseCode = http.GET();
    
    if(httpResponseCode > 0) {
      Serial.println("ส่งข้อความสำเร็จ: " + String(httpResponseCode));
    } else {
      Serial.println("ไม่สามารถส่งข้อความได้, รหัสข้อผิดพลาด: " + String(httpResponseCode));
    }
    http.end();
  } else {
    Serial.println("ไม่ได้เชื่อมต่อ Wi-Fi");
  }
}