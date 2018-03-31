
/*
ESP8266 Nixie Clock Barometer - Chime Control

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY OR FITNESS
FOR A PARTICULAR PURPOSE.  IT MAY INFRINGE THE INTELLECTUAL PROPERTY RIGHTS OF OTHERS
BECAUSE I COPY A LOT OF STUFF I FIND ON THE INTERWAVES. IT MAY KILL OR INJURE YOU OR RESULT IN THE LOSS
OF SOME OR ALL OF YOUR MONEY.  I WILL NOT INDEMNIFY OR DEFEND YOU IF ANY OF THIS HAPPENS.
THIS SOFTWARE MAY HAVE BEEN PROVIDED SUBJECT TO A SEPARATE LICENSE AGREEMENT. IF THAT IS THE CASE YOUR RIGHTS AND MY 
OBLIGATIONS ARE FURTHER LIMITED BY THAT AGREEMENT.

ANY USE OF THIS SOFTWARE MEANS YOU HAVE AGREED TO ALL THAT.  SO THERE.

This is the chime portion of a three-part system using three separate ESP8266-based NodeMCUs
installed in a vaguely Arts and Crafts wooden clock case. The other NodeMCUs drive an IN-14  Nixie clock and
the pointer of a barometer which gets pressure data from a BMP180 sensor. 
The chime works by using a stepper motor to trigger the original clock striker mechanism, 
or at least a good chunk of it. Time is obtained from the internet so is more or less synced with the 
clock portion of the project, which does the same. The chime can be turned off and otherwise configured 
using a web-based  interface at http://mantelclockchimer.local/.

The Arduino Stepper library didn't work for me using the J48 stepper motor but this code worked fine and
doesn't require any additional libraries.  https://www.instructables.com/id/BYJ48-Stepper-Motor/.  A quarter
turn (1024 steps) seemed to be the maximum the ESP8266 could do without a wdt reset.
  
*/

// ***************************************************************
// Libraries
// ***************************************************************    

#include <ESP8266WiFi.h>
#include <TimeLib.h>
#include <WiFiManager.h>
//#include <DNSServer.h>          
#include <FS.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WebSocketsServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>

// ***************************************************************
// Pin definitions
// ***************************************************************    

#define IN1 D0
#define IN2 D1 
#define IN3 D2
#define IN4 D7

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

const char* mdnsName = "MantelClockChimer"; // Domain name for the mDNS responder
const char *OTAName = "Chimer";           // A name and a password for the OTA service
const char *OTAPassword = "YOUR_OTA_PASSWORD";

// ***************************************************************
// Misc variables
// ***************************************************************

byte TZoneEEPROMAddress=0;
int timeZone;
bool HOUR_FORMAT_12=true;
bool chimeDone = false;
bool chime = true; // chime on/off
int Steps = 0; // for step switching sequence in stepper function
boolean Direction = true;// // anticlockwise pointer movement
unsigned long last_time;
unsigned long currentMillis;
int steps_left=1024; // One quarter rotation of stepper
                     // Seemd to be the max that can be done before NodeMCU  watchdog reset
bool half = true; // chime the half hour
bool quarter = true; // chime the quarter hour
int stepTime = 1000; // 1ms per step

// ***************************************************************
//  WiFi definitions
// ***************************************************************

#define AP_NAME "MantelChimer" // Wifi Manager configurable
#define WIFI_CHK_TIME_SEC 300  // Checks WiFi connection. Resets after this time, if WiFi is not connected
#define WIFI_CHK_TIME_MS  (WIFI_CHK_TIME_SEC * 1000)

// ***************************************************************
//  Instantiate objects
// ***************************************************************

WiFiUDP udp; //For NTP packets
ESP8266WebServer server ( 80 ); 
WebSocketsServer webSocket = WebSocketsServer(81);
WiFiManager wifiManager;
File fsUploadFile;  // a File variable to temporarily store the received file

// ***************************************************************
// Setup Functions
// ***************************************************************

void startOTA() { // Start the OTA service
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
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
  if (path.endsWith("/")) path += "Chime.html";          // If a folder is requested, send the index file
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

void handleFileUpload() { // upload a new file to the SPIFFS
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
// Chimer functions
// ***************************************************************

void calUP()  {
  Serial.println("Calibrating");
  Direction = 0; // false/clockwise
  Serial.println(Direction);
  steps_left=10;
  while (steps_left > 0) {
    currentMillis = micros();
    if(currentMillis-last_time>=stepTime){
      stepper(1); 
      last_time=micros();
      steps_left--;
    }
  }
  Direction = 1; // Striker must always be left to move ACW
}


void calDOWN()  {
  Serial.println("Calibrating");
  Direction = 1; //true/antoclockwise
  Serial.println(Direction);
  steps_left=10;
  while (steps_left > 0) {
    currentMillis = micros();
    if (currentMillis-last_time>=stepTime)  {
      stepper(1); 
      last_time=micros();
      steps_left--;
    }
  }
}

void chimeHour() {  
  if ((minute() == 0) && (second() == 0) && (chime == true) && (chimeDone == false)) { 
    Serial.print("Chiming");
    for (int i = 0; i <= hourFormat12(); i++) { 
    while(steps_left>0){
      currentMillis = micros();
      if(currentMillis-last_time>=stepTime){
      stepper(1); 
      last_time=micros();
      steps_left--;
    }
  }
   
   Serial.println("Wait...!");
   delay(500);
   steps_left=1024;  
      Serial.print("hour chiming:  ");
      Serial.println(hourFormat12());
      chimeDone = true;
    }
  }
  else if ((minute() == 2) && (chimeDone == true)) {
    chimeDone = false;
    Serial.println("chimeDone reset");
  }
}

void chimeHourConfirm() {

  for (int i = 1; i <= hourFormat12(); i++) {
    while(steps_left>0){
      currentMillis = micros();
      if(currentMillis-last_time>=stepTime){
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
    Serial.println("Wait...!");
    delay(500);
    steps_left=1024;  
    Serial.print("hour chiming:  ");
    Serial.println(hourFormat12());  
  }
}

void chimeHalf() {  
  if ((minute() == 30) && second() == 0 && (chime == true) && (half == true) && (chimeDone == false)) {
    Serial.print("Chiming Half");
    steps_left=1024;
    while(steps_left>0){
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime)  {
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
    chimeDone = true;
  }
  else if ((minute() == 31) && (chimeDone == true)) {
    chimeDone = false;
    Serial.println("chimeDone reset");
  }
}

void chimeQuarter() {  
  if (((minute() == 15) || (minute() == 45)) && second() == 0 && (chime == true) && (quarter == true) && (chimeDone == false)) {
    Serial.print("Chiming Quarter");
    steps_left=1024;
    while(steps_left!=0){
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime){
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
    chimeDone = true;
  }
  else if (((minute() == 16) || (minute() == 46)) && (chimeDone == true)) {
    chimeDone = false;
    Serial.println("chimeDone reset");
  }
}

void chimeTest() {  
  Serial.print("Chiming Test");
  steps_left=1024;
  while(steps_left!=0){
    currentMillis = micros();
    if (currentMillis-last_time>=stepTime){
      stepper(1); 
      last_time=micros();
      steps_left--;
    }
  }
  delay(500);
  steps_left=1024;
  while(steps_left!=0){
    currentMillis = micros();
    if (currentMillis-last_time>=stepTime){
      stepper(1); 
      last_time=micros();
      steps_left--; 
    }
  }
}

void stepper(int xw)  {
  for (int x=0;x<xw;x++)  {
  switch(Steps) {
   case 0:
     digitalWrite(IN1, LOW); 
     digitalWrite(IN2, LOW);
     digitalWrite(IN3, LOW);
     digitalWrite(IN4, HIGH);
   break; 
   case 1:
     digitalWrite(IN1, LOW); 
     digitalWrite(IN2, LOW);
     digitalWrite(IN3, HIGH);
     digitalWrite(IN4, HIGH);
   break; 
   case 2:
     digitalWrite(IN1, LOW); 
     digitalWrite(IN2, LOW);
     digitalWrite(IN3, HIGH);
     digitalWrite(IN4, LOW);
   break; 
   case 3:
     digitalWrite(IN1, LOW); 
     digitalWrite(IN2, HIGH);
     digitalWrite(IN3, HIGH);
     digitalWrite(IN4, LOW);
   break; 
   case 4:
     digitalWrite(IN1, LOW); 
     digitalWrite(IN2, HIGH);
     digitalWrite(IN3, LOW);
     digitalWrite(IN4, LOW);
   break; 
   case 5:
     digitalWrite(IN1, HIGH); 
     digitalWrite(IN2, HIGH);
     digitalWrite(IN3, LOW);
     digitalWrite(IN4, LOW);
   break; 
     case 6:
     digitalWrite(IN1, HIGH); 
     digitalWrite(IN2, LOW);
     digitalWrite(IN3, LOW);
     digitalWrite(IN4, LOW);
   break; 
   case 7:
     digitalWrite(IN1, HIGH); 
     digitalWrite(IN2, LOW);
     digitalWrite(IN3, LOW);
     digitalWrite(IN4, HIGH);
   break; 
   default:
     digitalWrite(IN1, LOW); 
     digitalWrite(IN2, LOW);
     digitalWrite(IN3, LOW);
     digitalWrite(IN4, LOW);
   break; 
    }
   SetDirection();
  }
} 


void SetDirection() {
  if(Direction==1){ Steps++;}
  if(Direction==0){ Steps--; }
  if(Steps>7){Steps=0;}
  if(Steps<0){Steps=7; }
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


// ***************************************************************
// Program Setup
// ***************************************************************

void setup() {

  Serial.println("WiFi setup");
  WiFiManager wifiManager;
  wifiManager.autoConnect(AP_NAME);
  delay(100);
 
  pinMode(IN1, OUTPUT); 
  pinMode(IN2, OUTPUT); 
  pinMode(IN3, OUTPUT); 
  pinMode(IN4, OUTPUT); 

  Serial.begin(115200);
  delay(1000);
  Serial.println("\r\n");

  EEPROM.begin(512);
  timeZone = EEPROM.read(TZoneEEPROMAddress); // Just testing; EEPROM.get() would be better
  if (timeZone > 12)  {timeZone = timeZone -256;}
  Serial.print("Time Zone is: ");
  Serial.println(timeZone);

  startOTA();                  // Start the OTA service
  startSPIFFS();               // Start the SPIFFS and list all contents
  startWebSocket();            // Start a WebSocket server 
  startMDNS();                 // Start MDNS service
  startServer();               // Start a HTTP server with a file read handler and an upload handler

  server.onNotFound(handleNotFound);          // if someone requests any other file or page, go to function 'handleNotFound'
                                              // and check if the file exists

  // Reset saved settings for testing purposes
  // Should be commented out for normal operation
  // wifiManager.resetSettings();

  // Fetches ssid and pass from eeprom and tries to connect
  // If it does not connect it starts an access point with the specified name
  // and goes into a blocking loop awaiting configuration

  // Initialize NTP code
  initNTP();

  Serial.println("\nReady!\n");
}

// ***************************************************************
// Program Loop
// ***************************************************************


unsigned long nextConnectionCheckTime = 0;
unsigned long prevMillis = millis();
int hue = 0;

void loop() {

  webSocket.loop();
  server.handleClient();
  ArduinoOTA.handle(); 
  chimeHour();
  chimeHalf();
  chimeQuarter();
  // Check WiFi connectivity
  if (millis() > nextConnectionCheckTime) {
    Serial.print("\n\nChecking WiFi... ");
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost. Resetting...");
      ESP.reset();
    } else {
      Serial.println("OK");
    }
    nextConnectionCheckTime = millis() + WIFI_CHK_TIME_MS;
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
      break;
    case WStype_CONNECTED: {              // if a new websocket connection is established
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
     
    }
    break;
    case WStype_TEXT:                     // if new text data is received
      Serial.printf("[%u] get Text: %s\n", num, payload);
    if (payload[0] == 'D') {                      // the browser sends an R when the rainbow effect is enabled
      timeZone = atoi((const char *) &payload[1]);   
      EEPROM.begin(512);
      EEPROM.put(TZoneEEPROMAddress, timeZone);
      EEPROM.commit();
      Serial.print("Time Zone written to EEPROM. New TZ is ");
      Serial.println(timeZone);
      initNTP();
      chimeHourConfirm();
      } else if (payload[0] == 'E') {                      // the browser sends a C to turn the headlights on
        calUP();
      } else if (payload[0] == 'F') {                      // the browser sends a D to turn the headlights off
        calDOWN();  
      } else if (payload[0] == 'A') {                      // the browser sends an R when the rainbow effect is enabled
          chime=true;
          chimeHourConfirm();
      } else if (payload[0] == 'B') {                      // the browser sends an N when the rainbow effect is disabled
          chime = false;
          half = false;
          quarter = false;
      } else if (payload[0] == 'X') {                      // the browser sends an R when the rainbow effect is enabled
          half = false;
          quarter = false;
          chimeTest();
      } else if (payload[0] == 'Y') {                      // the browser sends an N when the rainbow effect is disabled
          quarter = false;
          half = true;
          chime = true;
          chimeTest();
      } else if (payload[0] == 'Z') {                      // the browser sends an N when the rainbow effect is disabled
          quarter = true;
          half = true;
          chime = true;
          chimeTest();
      }
      break;
    }
}



