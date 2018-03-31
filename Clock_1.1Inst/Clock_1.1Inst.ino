
/*
ESP8266 Nixie Clock Barometer - Clock Control
Portions based on code developed by Craig A. Lindley
 
THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY OR FITNESS
FOR A PARTICULAR PURPOSE.  IT MAY INFRINGE THE INTELLECTUAL PROPERTY RIGHTS OF OTHERS
BECAUSE I COPY A LOT OF STUFF I FIND ON THE INTERWAVES. IT MAY KILL OR INJURE YOU OR RESULT IN THE LOSS
OF SOME OR ALL OF YOUR MONEY.  I WILL NOT INDEMNIFY OR DEFEND YOU IF ANY OF THIS HAPPENS.  THIS SOFTWARE MAY
HAVE BEEN PROVIDED SUBJECT TO A SEPARATE LICENSE AGREEMENT. IF THAT IS THE CASE YOUR RIGHTS AND MY 
OBLIGATIONS ARE FURTHER LIMITED (BUT IN NO EVENT EXPANDED BY THAT AGREEMENT.

ANY USE OF THIS SOFTWARE MEANS YOU HAVE AGREED TO ALL THAT.  SO THERE.

This is the clock portion of a three-part system using three separate ESP8266-based NodeMCUs
installed in a vaguely Arts and Crafts wooden clock case. 

*/

// ***************************************************************
// Libraries
// *************************************************************** 

#include <ESP8266WiFi.h>
#include <SPI.h>
#include <TimeLib.h>
#include <WiFiManager.h>          
#include <FS.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WebSocketsServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <SimpleTimer.h>
#include "NixieTubeShield.h"
#include "Adafruit_MQTT.h" // Adafruit IO.  Used as bridge between IFTTT GA and ESP8266
#include "Adafruit_MQTT_Client.h" // Same

// ***************************************************************
// Pin definitions
// ***************************************************************    

//MOSI  D7
//SCL  D5
//#define HV_PIN     D3            
//#define LE_PIN   D8
//#define DOTS_PIN   D9
#define MOTION_PIN   A0                                                                       
#define MR D0
#define MG D1 
#define MB D2

// ***************************************************************
//  NTP-related definitions and variable
// ***************************************************************

#define SYNC_INTERVAL_SECONDS 3600  // Sync with server every hour
#define NTP_SERVER_NAME "time.nist.gov" // NTP request server
#define NTP_SERVER_PORT  123 // NTP requests are to port 123
#define LOCALPORT       2390 // Local port to listen for UDP packets
#define NTP_PACKET_SIZE   48 // NTP time stamp is in the first 48 bytes of the message
#define RETRIES           20 // Times to try getting NTP time before failing
byte packetBuffer[NTP_PACKET_SIZE];

// ***************************************************************
// OTA  & MDNS constants
// ***************************************************************

const char* mdnsName = "MantelClock"; // Domain name for the mDNS responder
const char *OTAName = "MantelClock";           // A name and a password for the OTA service
const char *OTAPassword = "YOUR_OTA_PASSWORD";

// ***************************************************************
// Misc variables
// ***************************************************************

byte TZoneEEPROMAddress=0;
int timeZone;
unsigned long last_time;
unsigned long currentMillis;
bool motionDetected = false;
int cycleSpeed = 30; //for cycling LED colors
int r = 500;
int g = 500;
int b = 500;
bool HOUR_FORMAT_12=true;
bool SUPPRESS_LEADING_ZEROS=false;
bool dateCycle = false;
int previousMinute = 0;
int minutes = 0;
int previousSecond = 0;
int seconds = 0;
boolean dotToggle = true;
unsigned long motionMillis = 1800000; // turns tubes off after 30 minutes if no movement detected
unsigned long motionIntervalMillis; // stops multiple motion notifications
bool MQTTpause = false;
bool moodMode = false;
String stringOne;

// ***************************************************************
//  WiFi and Adafruit IO related definitions
// ***************************************************************

#define AP_NAME "MantelClock-Clock"
#define WIFI_CHK_TIME_SEC 300 // Checks WiFi connection. Reset after this time, if WiFi is not connected
#define WIFI_CHK_TIME_MS  (WIFI_CHK_TIME_SEC * 1000)
#define MQTT_SERV "io.adafruit.com"
#define MQTT_PORT 1883
#define MQTT_NAME "YOUR ADAFRUIT NAME"
#define MQTT_PASS "ADAFRUIT KEY"

// ***************************************************************
//  Instantiate objects
// ***************************************************************

WiFiUDP udp; //For NTP packets
ESP8266WebServer server ( 80 ); 
WebSocketsServer webSocket = WebSocketsServer(81);
WiFiManager wifiManager;
SimpleTimer timer;
File fsUploadFile;                            
NixieTubeShield SHIELD;

WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, MQTT_SERV, MQTT_PORT, MQTT_NAME, MQTT_PASS);
Adafruit_MQTT_Subscribe  mood = Adafruit_MQTT_Subscribe(&mqtt, MQTT_NAME "/f/mood");

// ***************************************************************
// Setup Functions
// ***************************************************************

void startOTA() { // Start the OTA service
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
    digitalWrite(MR, 0);    // turn off the LEDs
    digitalWrite(MG, 0);
    digitalWrite(MB, 0);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\r\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready\r\n");
}

void startSPIFFS() { // Start the SPIFFS and list all contents
  SPIFFS.begin();                             // Start the SPI Flash File System (SPIFFS)
  Serial.println("SPIFFS started. Contents:");
  {
    delay(1000);
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {                      // List the file system contents
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("\tFS File: %s, size: %s\r\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");
  }
}


void startWebSocket() { // Start a WebSocket server
  webSocket.begin();                          // start the websocket server
  webSocket.onEvent(webSocketEvent);          // if there's an incomming websocket message, go to function 'webSocketEvent'
  Serial.println("WebSocket server started.");
}

void startMDNS() { // Start the mDNS responder
  MDNS.begin(mdnsName);                        // start the multicast domain name server
  Serial.print("mDNS responder started: http://");
  Serial.print(mdnsName);
  Serial.println(".local");
}

void startServer() { // Start a HTTP server with a file read handler and an upload handler
  server.on("/edit.html",  HTTP_POST, []() {  // If a POST request is sent to the /edit.html address,
    server.send(200, "text/plain", ""); 
  }, handleFileUpload);                       // go to 'handleFileUpload'

  server.onNotFound(handleNotFound);          // if someone requests any other file or page, go to function 'handleNotFound'
                                              // and check if the file exists

  server.begin();                             // start the HTTP server
  Serial.println("HTTP server started.");
}

// ***************************************************************
//  Server page handlers
// ***************************************************************

void handleNotFound(){ // if the requested file or page doesn't exist, return a 404 not found error
  if(!handleFileRead(server.uri())){          // check if the file exists in the flash memory (SPIFFS), if so, send it
    server.send(404, "text/plain", "404: File Not Found");
  }
}

bool handleFileRead(String path) { // send the right file to the client (if it exists)
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "mantel_clock.html";  // If a folder is requested, send the index file
  mqtt.disconnect();// Disconnect MQTT service (for performance reasons)
  MQTTpause = true;
  Serial.println("MQTT Disconnected");
  String contentType = getContentType(path);             // Get the MIME type
  String pathWithGz = path + ".gz";
  
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
    if (SPIFFS.exists(pathWithGz))                         // If there's a compressed version available
      path += ".gz";                                         // Use the compressed verion
    File file = SPIFFS.open(path, "r");                    // Open the file
    size_t sent = server.streamFile(file, contentType);    // Send it to the client
    file.close();                                          // Close the file again
    Serial.println(String("\tSent file: ") + path);
    return true;
  }
  Serial.println(String("\tFile Not Found: ") + path);   // If the file doesn't exist, return false
  return false;
}


String formatBytes(size_t bytes) { // convert sizes in bytes to KB and MB
  if (bytes < 1024) {
    return String(bytes) + "B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + "KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  }
}

String getContentType(String filename) { // determine the filetype of a given filename, based on the extension
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

void setHue(int hue) { // Set the RGB LED to a given hue (color) (0° = Red, 120° = Green, 240° = Blue)
  hue %= 360;                   // hue is an angle between 0 and 359°
  float radH = hue*3.142/180;   // Convert degrees to radians
  float rf, gf, bf;
  
  if(hue>=0 && hue<120){        // Convert from HSI color space to RGB              
    rf = cos(radH*3/4);
    gf = sin(radH*3/4);
    bf = 0;
  } else if(hue>=120 && hue<240){
    radH -= 2.09439;
    gf = cos(radH*3/4);
    bf = sin(radH*3/4);
    rf = 0;
  } else if(hue>=240 && hue<360){
    radH -= 4.188787;
    bf = cos(radH*3/4);
    rf = sin(radH*3/4);
    gf = 0;
  }
  r = rf*rf*1023;
   g = gf*gf*1023;
   b = bf*bf*1023;
  
  analogWrite(MR,   r);    // Write the right color to the LED output pins
  analogWrite(MG, g);
  analogWrite(MB,  b);
}



void handleFileUpload(){ // upload a new file to the SPIFFS
  HTTPUpload& upload = server.upload();
  String path;
  if(upload.status == UPLOAD_FILE_START){
    path = upload.filename;
    if(!path.startsWith("/")) path = "/"+path;
    if(!path.endsWith(".gz")) {                          // The file server always prefers a compressed version of a file 
      String pathWithGz = path+".gz";                    // So if an uploaded file is not compressed, the existing compressed
      if(SPIFFS.exists(pathWithGz))                      // version of that file must be deleted (if it exists)
         SPIFFS.remove(pathWithGz);
    }
    Serial.print("handleFileUpload Name: "); Serial.println(path);
    fsUploadFile = SPIFFS.open(path, "w");            // Open the file for writing in SPIFFS (create if it doesn't exist)
    path = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile) {                                    // If the file was successfully created
      fsUploadFile.close();                               // Close the file again
      Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
      server.sendHeader("Location","/success.html");      // Redirect the client to the success page
      server.send(303);
    } else {
      server.send(500, "text/plain", "500: couldn't create file");
    }
  }
}

// ***************************************************************
// Clock Display Function (called every second)
// ***************************************************************

void updateDisplay(void) {

  minutes = minute();
  seconds = second();
  previousMinute = minutes;
  if ((minutes != 0) && (seconds%10==0) && (dateCycle == true)) { // 10 second event is the date display
    SHIELD.dotsEnable(true);
    int now_mon  = month();
    if (now_mon >= 10) {
      SHIELD.setNX1Digit(now_mon / 10);
    } else  {
      if (SUPPRESS_LEADING_ZEROS) {
        SHIELD.setNX1Digit(BLANK_DIGIT);
      } else  {
        SHIELD.setNX1Digit(0);
      }
    }
    // Display the NX2 digit
    SHIELD.setNX2Digit(now_mon % 10);
    // Get the current day 1..31
    int now_day  = day();
    // Display the NX3 digit
    if (now_day >= 10) {
      SHIELD.setNX3Digit(now_day / 10);
    } else  {
      if (SUPPRESS_LEADING_ZEROS) {
        SHIELD.setNX3Digit(BLANK_DIGIT);
      } else  {
        SHIELD.setNX3Digit(0);
      }
    }
    // Display the NX4 digit
    SHIELD.setNX4Digit(now_day % 10);
    // Get the current year
    int now_year = year() - 2000;
    // Display the NX5 digit
    if (now_year >= 10) {
      SHIELD.setNX5Digit(now_year / 10);
    } else  {
      if (SUPPRESS_LEADING_ZEROS) {
        SHIELD.setNX5Digit(BLANK_DIGIT);
      } else  {
        SHIELD.setNX5Digit(0);
      }
    }
    // Display the NX6 digit
    SHIELD.setNX6Digit(now_year % 10);
    // Display date on clock
    SHIELD.show();
    // Delay for date display
    delay(2500);

  } else {

    // Display the time
    int now_hour;
    // Make the dots blink off and on
    if (dotToggle) {
      dotToggle = false;
      SHIELD.dotsEnable(true);
    } else  {
      dotToggle = true;
      SHIELD.dotsEnable(false);
    }
    // Get the current hour
    if (HOUR_FORMAT_12) {
      // Using 12 hour format
      now_hour = hourFormat12();
    } else  {
      // Using 24 hour format
      now_hour = hour();  
    }
    // Display the NX1 digit
    if (now_hour >= 10) {
      SHIELD.setNX1Digit(now_hour / 10);
    } else  {
      if (SUPPRESS_LEADING_ZEROS) {
        SHIELD.setNX1Digit(BLANK_DIGIT);
      } else  {
        SHIELD.setNX1Digit(0);
      }
    }
    // Display the NX2 digit
    SHIELD.setNX2Digit(now_hour % 10);
    // Get the current minute
    int now_min  = minute();
    // Display the NX3 digit
    SHIELD.setNX3Digit(now_min / 10);
    // Display the NX4 digit
    SHIELD.setNX4Digit(now_min % 10);
    // Get the current second
    int now_sec  = second();
    // Display the NX5 digit
    SHIELD.setNX5Digit(now_sec / 10);
    // Display the NX6 digit
    SHIELD.setNX6Digit(now_sec % 10);
    // Display time on clock
    SHIELD.show();
  }
}

// ***************************************************************
// Motion Detect Function
// ***************************************************************

void motionDetect () {
  int stateMotion = analogRead(MOTION_PIN);
  Serial.println(stateMotion);
  if (stateMotion >500) {
    motionMillis = millis();
    SHIELD.hvEnable(true);
    Serial.println("Tubes On");
    Serial.println("Motion Detected");
    motionDetected = true;
    analogWrite(MR,   r);    // Write the right color to the LED output pins
    analogWrite(MG, g);
    analogWrite(MB,  b);
  }
  if ((stateMotion <500) && (millis() - motionMillis > 1800000) && (motionDetected == true))  {
    SHIELD.hvEnable(true);
    SHIELD.doAntiPoisoning();  //Before shutdown 
    SHIELD.hvEnable(false);
    motionDetected = false;
    analogWrite(MR,   0);    // Turn off LEDs
    analogWrite(MG, 0);
    analogWrite(MB,  0);  
  }
}
    
void powerCycle() { // Without this HV board would not start up again
  if (motionDetected == false)  { 
    SHIELD.hvEnable(true);
    Serial.println("HV On");
    delay(6);
    SHIELD.hvEnable(false);
  }
}

// ***************************************************************
// NTP
// ***************************************************************

time_t _getNTPTime() {

  // Set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);

  // Initialize values needed to form NTP request
  packetBuffer[0]  = 0xE3;  // LI, Version, Mode
  packetBuffer[2]  = 0x06;  // Polling Interval
  packetBuffer[3]  = 0xEC;  // Peer Clock Precision
  packetBuffer[12] = 0x31;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 0x31;
  packetBuffer[15] = 0x34;

  // All NTP fields initialized, now send a packet requesting a timestamp
  udp.beginPacket(NTP_SERVER_NAME, NTP_SERVER_PORT);
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();

  // Wait a second for the response
  delay(1000);

  // Listen for the response
  if (udp.parsePacket() == NTP_PACKET_SIZE) {
    udp.read(packetBuffer, NTP_PACKET_SIZE);  // Read packet into the buffer
    unsigned long secsSince1900;

    // Convert four bytes starting at location 40 to a long integer
    secsSince1900 =  (unsigned long) packetBuffer[40] << 24;
    secsSince1900 |= (unsigned long) packetBuffer[41] << 16;
    secsSince1900 |= (unsigned long) packetBuffer[42] << 8;
    secsSince1900 |= (unsigned long) packetBuffer[43];

    Serial.println("Got NTP time");

    return secsSince1900 - 2208988800UL +(timeZone*3600);
    Serial.println(hour());
  } else  {
    return 0;
  }
}

// Get NTP time with retries on access failure
time_t getNTPTime() {

  unsigned long result;

  for (int i = 0; i < RETRIES; i++) {
    result = _getNTPTime();
    if (result != 0) {
      return result;
    }
    Serial.println("Problem getting NTP time. Retrying");
    delay(300);
  }
  Serial.println("NTP Problem - Try reset");

  while (true) {};
}

// Initialize the NTP code
void initNTP() {

  // Login succeeded so set UDP local port
  udp.begin(LOCALPORT);

  // Set the time provider to NTP
  setSyncProvider(getNTPTime);

  // Set the interval between NTP calls
  setSyncInterval(SYNC_INTERVAL_SECONDS);
}

void reconnectMQTT()  {
  mqtt.disconnect();
  delay(100);
  MQTT_connect();
}

// ***************************************************************
// Program Setup
// ***************************************************************

void setup() {

  Serial.println("WiFi setup");
  WiFiManager wifiManager;
  wifiManager.autoConnect(AP_NAME);
  delay(100);
  pinMode(MOTION_PIN, INPUT); 
  pinMode(MR, OUTPUT); 
  pinMode(MG, OUTPUT); 
  pinMode(MB, OUTPUT); 
  // Configure serial interface - can'tbe used normally because D9 is used for neon dots
  //Serial.begin(112500);
  //delay(1000);
  Serial.println("\r\n");

  EEPROM.begin(512);
  EEPROM.get(TZoneEEPROMAddress, timeZone);
  Serial.print("Time Zone is: ");
  Serial.println(timeZone);
  SPI.begin();
  SPI.setFrequency(125000); // This seems to be necessary to configure manually for max stability
 
  timer.setInterval(4000, powerCycle);
  timer.setInterval(250000, reconnectMQTT);  // Pinging wasn't keeping connection open
  startOTA();       // Start the OTA service
  startSPIFFS();    // Start the SPIFFS and list all contents
  startWebSocket(); // Start a WebSocket server
  startMDNS();      // Start the MDNS service
  startServer();    // Start a HTTP server with a file read handler and an upload handler
  server.onNotFound(handleNotFound);    // if someone requests any other file or page, go to function 'handleNotFound'
                                        // and check if the file exists
  //server.begin();                             // start the HTTP server
  //Serial.println("HTTP server started.");
  initNTP();   // Initialize NTP code
  SHIELD.hvEnable(false);
  SHIELD.dotsEnable(true);  // Turn on the dots
  SHIELD.hvEnable(true);   // Turn on the high voltage for the clock
  SHIELD.doAntiPoisoning();
  SHIELD.dotsEnable(true);
  Serial.println("\nReady!\n");

  mqtt.subscribe(&mood); // subscribe to the Adafruit IO mood feed
  MQTT_connect(); 
}

// ***************************************************************
// Program Loop
// ***************************************************************

bool rainbow = false;
unsigned long nextConnectionCheckTime = 0;
unsigned long prevMillis = millis();
int hue = 0;

void loop() {
 timer.run();
  webSocket.loop();
  motionDetect();
  server.handleClient();
  ArduinoOTA.handle();
   
  if (MQTTpause = false) {MQTT_connect();} 
  // Check WiFi connectivity
  
  if (millis() > nextConnectionCheckTime) {
    //Serial.print("\n\nChecking WiFi... ");
    if (WiFi.status() != WL_CONNECTED) {
      //Serial.println("WiFi connection lost. Resetting...");
      ESP.reset();
    } else {
      //Serial.println("OK");
    }
    nextConnectionCheckTime = millis() + WIFI_CHK_TIME_MS;
  }
  
  if (rainbow) {                               // if the rainbow effect is turned on
    if  (millis() > prevMillis + cycleSpeed) {          
      if (++hue == 360) hue = 0;                        // Cycle through the color wheel (increment by one degree every 10 ms)
      setHue(hue);                            // Set the RGB LED to the right color
      prevMillis = millis();
    }
  }
  // Update the display only if time has changed
  if (timeStatus() != timeNotSet) {
    if (second() != previousSecond) {
      previousSecond = second();
      updateDisplay();
    }
  }

  Adafruit_MQTT_Subscribe * subscription;
  while ((subscription = mqtt.readSubscription(200)))  {
    if (subscription == &mood)  //If the subscription is updated...
    {
      Serial.print("Mood Barometer: ");
      Serial.println((char*) mood.lastread);
      String stringOne = ((char*) mood.lastread);
      Serial.println(stringOne);
      //delay(10);
      if (stringOne == " mad") {
        rainbow = false;
        moodMode = true;
        r = 1023;
        g = 0;
        b = 0;
        analogWrite(MR,  r);// write it to the LED output pins
        analogWrite(MG, g);
        analogWrite(MB,  b);
        Serial.println(stringOne);
        Serial.print("Mood Color = Red");
      } else if (stringOne == " sad") {
          rainbow = false;
          moodMode = true;
          r = 0;
          g = 0;
          b = 1023;
          analogWrite(MR,  r);// write it to the LED output pins
          analogWrite(MG, g);
          analogWrite(MB,  b); 
          Serial.println(stringOne);
          Serial.print("Mood Color = Blue");
      } else if (stringOne == " Moody") {
          rainbow = true;
          moodMode = true;
          cycleSpeed = 20;
          Serial.print("Mood Color = Cycling at 20ms");
      } else if (stringOne == " Happy") {
          rainbow = false;
          moodMode = true;
          r= 1023;
          g = 650;
          b = 50;
          analogWrite(MR,  r);// write it to the LED output pins
          analogWrite(MG, g);
          analogWrite(MB,  b);
          Serial.println(stringOne);
          Serial.print("Mood Color = Yellow"); 
      } else if (stringOne == " very happy") {
          rainbow = false;
          moodMode = true;
          r = 1023;
          g = 0;
          b = 750;
          analogWrite(MR,  r);// write it to the LED output pins
          analogWrite(MG, g);
          analogWrite(MB,  b);
          Serial.println(stringOne);
          Serial.print("Mood Color = Blue");
      } else if ((stringOne == " off") || (stringOne == "OFF")) {
          rainbow = false;
          moodMode = false;
          r = 500;
          g = 500;
          b = 500;
          analogWrite(MR, r);// write it to the LED output pins
          analogWrite(MG, g);
          analogWrite(MB,  b);
          Serial.println(stringOne);
          Serial.print("Mood Color = White");
      } 
    }
  }
  
  delay(10);  
}


// ***************************************************************
// End Program Loop
// ***************************************************************


void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) { // When a WebSocket message is received
  switch (type) {
    case WStype_DISCONNECTED:             // if the websocket is disconnected
      Serial.printf("[%u] Disconnected!\n", num);
      MQTTpause = false;
      MQTT_connect(); // Reconnect with MQTT service on session termination
    break;
    case WStype_CONNECTED: {              // if a new websocket connection is established
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
        //rainbow = false;                  // Turn rainbow off when a new connection is established
    }
    break;
    case WStype_TEXT:                     // if new text data is received
      Serial.printf("[%u] get Text: %s\n", num, payload);
      if ((payload[0] == '#') && (moodMode == false)) {            // we get RGB data
        uint32_t rgb = (uint32_t) strtol((const char *) &payload[1], NULL, 16);   // decode rgb data
        int r = ((rgb >> 20) & 0x3FF);                     // 10 bits per color, so R: bits 20-29
        int g = ((rgb >> 10) & 0x3FF);                     // G: bits 10-19
        int b =          rgb & 0x3FF;                      // B: bits  0-9
        //Serial.print("Blue = :");
        //Serial.println(b);
        analogWrite(MR,   r);// write it to the LED output pins
        //Serial.print("Red = :");
        //Serial.println(r);
        analogWrite(MG, g);
        analogWrite(MB,  b);
        //Serial.print("Blue = :");
        //Serial.println(b);
        
      } else if (payload[0] == 'A') {                      
        int s = atoi((const char *) &payload[1]);
       
        //Serial.println(s);
         cycleSpeed = s;

      }  else if (payload[0] == 'B') {                      
          rainbow = true;
          mqtt.disconnect();
          
      } else if (payload[0] == 'C') {                     
          rainbow = false;
          MQTT_connect();
         
      }  else if (payload[0] == 'D') {                       
        timeZone = atoi((const char *) &payload[1]);   
        EEPROM.begin(512);
        EEPROM.put(TZoneEEPROMAddress, timeZone);
        EEPROM.commit();
        Serial.print("Time Zone written to EEPROM. New TZ is ");
        Serial.println(timeZone);
        initNTP();
      }  else if (payload[0] == 'F') {                      
          SHIELD.hvEnable(true);
          SHIELD.dotsEnable(true);
          motionMillis = millis();
          motionDetected = true;
      } else if (payload[0] == 'G') {                      
         SHIELD.hvEnable(false);
         SHIELD.dotsEnable(false);
         motionDetected = false;
      } else if (payload[0] == 'H') {                      
          HOUR_FORMAT_12=false;
      } else if (payload[0] == 'I') {                      
          HOUR_FORMAT_12 = true;
      } else if (payload[0] == 'J') {                      
          SUPPRESS_LEADING_ZEROS=false;
      } else if (payload[0] == 'K') {                     
          SUPPRESS_LEADING_ZEROS = true;
      } else if (payload[0] == 'L') {                      
          dateCycle = true;
      } else if (payload[0] == 'M') {                      
          dateCycle = false;
      } 
    break;
  }
}

void MQTT_connect() {
  int8_t ret;
  // Stop if already connected
  if (mqtt.connected())
  {
    return;
  }
  Serial.print("Connecting to MQTT... ");
  uint8_t retries = 3;
  while ((ret = mqtt.connect()) != 0) {  // Give it three goes to connnect
    Serial.println(mqtt.connectErrorString(ret));
    Serial.println("Retrying MQTT connection in 5 seconds...");
    mqtt.disconnect();
    delay(5000);  // wait 5 seconds
    retries--;
    if (retries == 0) 
    {
     Serial.println("MQTT failed to connnect");
    }
  }
  Serial.println("MQTT Connected!");
}



