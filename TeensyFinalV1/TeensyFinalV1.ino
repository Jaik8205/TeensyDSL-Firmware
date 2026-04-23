#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <SD.h>
#include <SPI.h>

// ---------- OLED ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- DHT ----------
#define DHTPIN 6
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ---------- Pins ----------
#define IR_PIN 7
#define LDR_PIN A0
#define PWM_PIN 23

// ---------- States ----------
enum STATE {
  WAIT_ESP,
  WAIT_WIFI_REQ,
  WAIT_WIFI_OK,
  RUNNING
};

STATE systemState = WAIT_ESP;

// ---------- Variables ----------
File logFile;

float tempDHT = 0;
float humDHT = 0;
float smoothed = 0;

unsigned long lastSensor = 0;
unsigned long recordID = 0;

// ---------- Alert Blink ----------
bool blinkState = true;
unsigned long lastBlink = 0;

// ---------- Crop Configuration ----------
String cropName = "";

float cropTempMin = 0;
float cropTempMax = 0;

float cropHumMin = 0;
float cropHumMax = 0;

float cropLightMin = 0;
float cropLightMax = 0;

// ---------- Alert ----------
bool alertActive = false;
String alertMessage = "";
float alertValue = 0;
float alertLimit = 0;

// ---------- PWM Smoothing ----------
float currentPWM = 0;

// ---------- OLED Status ----------
void oledMessage(String a, String b)
{
  display.clearDisplay();
  display.setTextSize(1);

  display.setCursor(10,15);
  display.println(a);

  display.setCursor(10,35);
  display.println(b);

  display.display();
}

// ---------- Warning Icon ----------
void drawWarningIcon(int x, int y)
{
  display.drawTriangle(x, y+20, x+20, y+20, x+10, y, SSD1306_WHITE);
  display.drawLine(x+10, y+5, x+10, y+13, SSD1306_WHITE);
  display.drawPixel(x+10, y+16, SSD1306_WHITE);
}

// ---------- OLED Dashboard ----------
void updateDisplay(int ir,float lightPercent)
{
  display.clearDisplay();

  // crop name centered
  int16_t x1,y1;
  uint16_t w,h;

  display.setTextSize(1);
  display.getTextBounds(cropName,0,0,&x1,&y1,&w,&h);
  display.setCursor((128-w)/2,0);
  display.print(cropName);

  // grid
  display.drawLine(64,10,64,64,SSD1306_WHITE);
  display.drawLine(0,36,128,36,SSD1306_WHITE);

  String tempSym  = getSymbol(tempDHT,cropTempMin,cropTempMax);
  String humSym   = getSymbol(humDHT,cropHumMin,cropHumMax);
  String lightSym = getSymbol(lightPercent,cropLightMin,cropLightMax);

  // ----- TEMP -----
  display.setTextSize(1);
  display.setCursor(5,12);
  display.print("TEMP ");
  display.print(tempSym);

  display.setTextSize(2);
  String t = String(tempDHT,1) + "C";
  display.getTextBounds(t,0,0,&x1,&y1,&w,&h);
  display.setCursor((64-w)/2,20);
  display.print(t);

  // ----- HUM -----
  display.setTextSize(1);
  display.setCursor(69,12);
  display.print("HUM ");
  display.print(humSym);

  display.setTextSize(2);
  String hu = String(humDHT,0) + "%";
  display.getTextBounds(hu,0,0,&x1,&y1,&w,&h);
  display.setCursor(64 + (64-w)/2,20);
  display.print(hu);

  // ----- TANK -----
  display.setTextSize(1);
  display.setCursor(5,38);
  display.print("TANK");

  display.setTextSize(2);
  String tank = ir ? "FULL" : "EMPTY";
  display.getTextBounds(tank,0,0,&x1,&y1,&w,&h);
  display.setCursor((64-w)/2,48);
  display.print(tank);

  // ----- LIGHT -----
  display.setTextSize(1);
  display.setCursor(69,38);
  display.print("LIGHT ");
  display.print(lightSym);

  display.setTextSize(2);
  String li = String(lightPercent,0) + "%";
  display.getTextBounds(li,0,0,&x1,&y1,&w,&h);
  display.setCursor(64 + (64-w)/2,48);
  display.print(li);

  display.display();
}

// ---------- Alert Screen ----------
void showAlert()
{
  if(millis() - lastBlink > 500)
  {
    blinkState = !blinkState;
    lastBlink = millis();
  }

  display.clearDisplay();

  int16_t x1,y1;
  uint16_t w,h;

  // ----- draw centered warning icon -----
  if(blinkState)
    drawWarningIcon(54,2);   // triangle already centered

  // ----- center "WARNING" -----
  display.setTextSize(1);
  display.getTextBounds("WARNING",0,0,&x1,&y1,&w,&h);
  display.setCursor((128-w)/2,28);
  display.print("WARNING");

  // ----- center alert message -----
  display.getTextBounds(alertMessage,0,0,&x1,&y1,&w,&h);
  display.setCursor((128-w)/2,40);
  display.print(alertMessage);

  // ----- center values -----
  String valueLine = "Now:" + String(alertValue) + " Lim:" + String(alertLimit);

  display.getTextBounds(valueLine,0,0,&x1,&y1,&w,&h);
  display.setCursor((128-w)/2,52);
  display.print(valueLine);

  display.display();
}

// ---------- Send WiFi ----------
void sendWifi()
{
  File file = SD.open("wifi.txt");
  if(!file) return;

  String ssid="";
  String pass="";

  while(file.available())
  {
    String line=file.readStringUntil('\n');
    line.trim();

    if(line.startsWith("SSID="))
      ssid=line.substring(5);

    if(line.startsWith("PASS="))
      pass=line.substring(5);
  }

  file.close();

  String msg="WIFI,"+ssid+","+pass;

  Serial.println("Teensy: "+msg);
  Serial1.println(msg);

  Serial.println("Teensy: WIFI_END");
  Serial1.println("WIFI_END");

  oledMessage("WiFi Sent",ssid);
}

// ---------- Load Crop Configuration ----------
void loadCropConfig()
{
  File file = SD.open("currentcrop.txt");

  if(!file)
  {
    Serial.println("Crop config missing");
    return;
  }

  while(file.available())
  {
    String line = file.readStringUntil('\n');
    line.trim();

    if(line.startsWith("CROP="))
      cropName = line.substring(5);

    if(line.startsWith("TEMP_MIN="))
      cropTempMin = line.substring(9).toFloat();

    if(line.startsWith("TEMP_MAX="))
      cropTempMax = line.substring(9).toFloat();

    if(line.startsWith("HUM_MIN="))
      cropHumMin = line.substring(8).toFloat();

    if(line.startsWith("HUM_MAX="))
      cropHumMax = line.substring(8).toFloat();

    if(line.startsWith("LIGHT_MIN="))
      cropLightMin = line.substring(10).toFloat();

    if(line.startsWith("LIGHT_MAX="))
      cropLightMax = line.substring(10).toFloat();
  }

  file.close();

  Serial.println("Crop Config Loaded:");
  Serial.println("Crop: " + cropName);
}

// ---------- Log ----------
void logData(int ir,float light)
{
  recordID++;

  logFile.print(recordID);
  logFile.print(",");
  logFile.print(tempDHT);
  logFile.print(",");
  logFile.print(humDHT);
  logFile.print(",");
  logFile.print(ir);
  logFile.print(",");
  logFile.println(light);

  logFile.flush();
}

// ---------- Real-time Lighting ----------
void updateLighting()
{
  int ldrRaw = analogRead(LDR_PIN);
  ldrRaw = ldrRaw >> 2;

  float lightPercent = (ldrRaw / 1023.0) * 100.0;

  float deficit = cropLightMin - lightPercent;

  float margin = 3.0;   // extra 2–4%

  int targetPWM = 0;

  if(deficit > 0)
  {
    float required = deficit + margin;

    if(required > 100)
      required = 100;

    targetPWM = map(required, 0, 100, 0, 255);
  }
  else
  {
    targetPWM = 0;
  }

  // ----- Smooth transition -----
  currentPWM = currentPWM * 0.80 + targetPWM * 0.20;

  analogWrite(PWM_PIN, (int)currentPWM);
}

// ---------- Status Helper ----------
String getStatus(float value, float minVal, float maxVal)
{
  if(value < minVal)
    return "blw opt";

  if(value > maxVal)
    return "abv opt";

  return "opt opt";
}

// ---------- Status Symbol ----------
String getSymbol(float value, float minVal, float maxVal)
{
  if(value < minVal)
    return "v";

  if(value > maxVal)
    return "^";

  return "o";
}

// ---------- Sensors ----------
void readSensors()
{
  int ldrRaw=analogRead(LDR_PIN);
  ldrRaw = ldrRaw >> 2;

  float lightPercent=(ldrRaw / 1023.0) * 100.0;

  float t=dht.readTemperature();
  float h=dht.readHumidity();

  if(!isnan(t)) tempDHT=t;
  if(!isnan(h)) humDHT=h;

  int ir=digitalRead(IR_PIN);

  // ----- Determine Status -----
  String tempStatus = getStatus(tempDHT, cropTempMin, cropTempMax);
  String humStatus  = getStatus(humDHT, cropHumMin, cropHumMax);
  String lightStatus = getStatus(lightPercent, cropLightMin, cropLightMax);

  alertActive = false;

  if(tempDHT < cropTempMin)
  {
    alertActive = true;
    alertMessage = "TEMP LOW";
    alertValue = tempDHT;
    alertLimit = cropTempMin;
  }

  else if(tempDHT > cropTempMax)
  {
    alertActive = true;
    alertMessage = "TEMP HIGH";
    alertValue = tempDHT;
    alertLimit = cropTempMax;
  }

  else if(humDHT < cropHumMin)
  {
    alertActive = true;
    alertMessage = "HUM LOW";
    alertValue = humDHT;
    alertLimit = cropHumMin;
  }

  else if(humDHT > cropHumMax)
  {
    alertActive = true;
    alertMessage = "HUM HIGH";
    alertValue = humDHT;
    alertLimit = cropHumMax;
  }

  else if(lightPercent < cropLightMin)
  {
    alertActive = true;
    alertMessage = "LIGHT LOW";
    alertValue = lightPercent;
    alertLimit = cropLightMin;
  }

  else if(lightPercent > cropLightMax)
  {
    alertActive = true;
    alertMessage = "LIGHT HIGH";
    alertValue = lightPercent;
    alertLimit = cropLightMax;
  }

  // ----- Serial Output -----
  Serial.print("Sensor Packet -> DATA,");
  Serial.print(tempDHT);
  Serial.print(" ");
  Serial.print(tempStatus);
  Serial.print(",");

  Serial.print(humDHT);
  Serial.print(" ");
  Serial.print(humStatus);
  Serial.print(",");

  Serial.print(ir);
  Serial.print(",");

  Serial.print(lightPercent);
  Serial.print(" ");
  Serial.println(lightStatus);

  // ----- Send to ESP32 -----
  Serial1.print("DATA,");
  Serial1.print(tempDHT);
  Serial1.print(" ");
  Serial1.print(tempStatus);
  Serial1.print(",");

  Serial1.print(humDHT);
  Serial1.print(" ");
  Serial1.print(humStatus);
  Serial1.print(",");

  Serial1.print(ir);
  Serial1.print(",");

  Serial1.print(lightPercent);
  Serial1.print(" ");
  Serial1.println(lightStatus);

  if(alertActive)
    showAlert();
  else
    updateDisplay(ir,lightPercent);

  logData(ir,lightPercent);
}


// ---------- Setup ----------
void setup()
{
  Serial.begin(115200);
  Serial1.begin(115200);

  Wire.begin();

  display.begin(SSD1306_SWITCHCAPVCC,0x3C);
  display.setTextColor(SSD1306_WHITE);

  dht.begin();

  pinMode(IR_PIN,INPUT);
  pinMode(PWM_PIN,OUTPUT);

  analogReadResolution(12);

  oledMessage("BOOTING","");

  if(!SD.begin(BUILTIN_SDCARD))
  {
    Serial.println("SD FAIL");
    while(1);
  }

  loadCropConfig();

  if(!SD.exists("datalog.csv"))
  {
    File f=SD.open("datalog.csv",FILE_WRITE);
    f.println("id,temp,humidity,ir,light");
    f.close();
  }

  logFile=SD.open("datalog.csv",FILE_WRITE);

  Serial.println("Teensy ready");
  oledMessage("Waiting ESP32","HELLO");
}


// ---------- Loop ----------
void loop()
{
  if(Serial1.available())
  {
    String cmd=Serial1.readStringUntil('\n');
    cmd.trim();

    if(cmd.length()==0) return;

    Serial.println("ESP32: "+cmd);

    if(systemState==WAIT_ESP && cmd=="HELLO")
    {
      Serial.println("Teensy: HI");
      Serial1.println("HI");

      oledMessage("ESP32 Connected","HI sent");

      systemState=WAIT_WIFI_REQ;
    }

    else if(systemState==WAIT_WIFI_REQ && cmd=="GET_WIFI")
    {
      sendWifi();
      systemState=WAIT_WIFI_OK;
    }

    else if(systemState==WAIT_WIFI_OK && cmd=="WIFI_OK")
    {
      Serial.println("ESP32 WiFi OK");
      oledMessage("WiFi Connected","Starting sensors");

      systemState=RUNNING;
    }
  }

  if(systemState==RUNNING)
  {
    updateLighting();

    if(millis()-lastSensor>=1000)
    {
      readSensors();
      lastSensor=millis();
    }
  }
}