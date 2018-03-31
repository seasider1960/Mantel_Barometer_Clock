 
/* ESP8266 Nixie Clock Barometer - LED/BAROM/MOOD Control 

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY OR FITNESS
FOR A PARTICULAR PURPOSE.  IT MAY INFRINGE THE INTELLECTUAL PROPERTY RIGHTS OF OTHERS
BECAUSE I COPY A LOT OF STUFF I FIND ON THE INTERWAVES. IT MAY KILL OR INJURE YOU OR RESULT IN THE LOSS
OF SOME OR ALL OF YOUR MONEY.  I WILL NOT INDEMNIFY OR DEFEND YOU IF ANY OF THIS HAPPENS. THIS SOFTWARE MAY
HAVE BEEN PROVIDED SUBJECT TO A SEPARATE LICENSE AGREEMENT. IF THAT IS THE CASE YOUR RIGHTS AND MY 
OBLIGATIONS ARE FURTHER LIMITED BY THAT AGREEMENT.

ANY USE OF THIS SOFTWARE MEANS YOU HAVE AGREED TO ALL THAT.  SO THERE.

This is the barometer portion of a three-part system using three separate ESP8266-based NodeMCUs
installed in a vaguely Arts and Crafts wooden clock case. The barometer was bought on ebay for $15
and the (non-working) aneroid mechanism removed. The other NodeMCUs drive an IN-14  Nixie clock and
the original chiming mechanism of the clock.

The implementation uses a stepper motor to indicate barometric pressure, derived from a BMP180 sensor.
The stepper motor indexes by using an LDR in the barometer dial, which is covered by the tail of the
pointer in the setup routine. The barometer can be more finely calibrated from a web-based interface
at http://mantelbarometer.local/. As a frivolous twist, the same interface can be used to cause
the barometer to display prevailing mood, set from your phone or with the Google Assistant. The GA portion works
by using IFTTT which sends a command to an Adafruit IO feed which is acting as an MQTT broker.

The Arduino Stepper library didn't work for me using the J48 stepper motor but this code worked fine and
doesn't require any additional libraries.  https://www.instructables.com/id/BYJ48-Stepper-Motor/.  A quarter
turn (1024 steps) seemed to be the maximum the ESP8266 could do without a wdt reset.

*/

// ***************************************************************
// Libraries
// ***************************************************************    

#include <ESP8266WiFi.h>
#include <WiFiManager.h> // Configures WiFi network without hard coding         
#include <FS.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h> //Multicast DNS service
#include <WebSocketsServer.h> //For web interface
#include <ArduinoOTA.h>
#include <EEPROM.h> // Stores altitude of clock location. Pressure shown is sea-level 
                    // pressure corrected for this altitude (configurable on web interface).
#include <WiFiUdp.h>
#include <SFE_BMP180.h>
#include <SimpleTimer.h> // Not really necessary - just used for calling pressure reading/barometer
                         // setting every five minutes                        
#include "Adafruit_MQTT.h" // Adafruit IO.  Used as bridge between IFTTT GA and ESP8266
#include "Adafruit_MQTT_Client.h" // Same

// ***************************************************************
// Pin definitions
// ***************************************************************    

#define IN1 D0 //Stepper motor has four pins
#define IN2 D5 
#define IN3 D6
#define IN4 D7
#define BR  D4 // Pressure rising indicator (RH LED)
#define BS  D3 // Pressure steady indicator (Central LED)
#define BF  D8 // Pressure falling indicatore (LH LED)
#define calPin A0 // Reads LDR
// SDA D2 Default I2C for BMP 180
// SCL D1 Default I2C for BMP 180

// ***************************************************************
//  WiFi and Adafruit IO related definitions
// ***************************************************************

#define AP_NAME "MantelBarometer" // Wifi Manager configurable
#define WIFI_CHK_TIME_SEC 300  // Checks WiFi connection every figve minutes. Resets after this time if WiFi is not connected
#define WIFI_CHK_TIME_MS  (WIFI_CHK_TIME_SEC * 1000)
#define MQTT_SERV "io.adafruit.com"
#define MQTT_PORT 1883
#define MQTT_NAME "YOUR_ADAFRUIT IO NAME"
#define MQTT_PASS "YOUR ADAFRUIT KEY"

// ***************************************************************
// OTA  & MDNS constants
// ***************************************************************

const char* mdnsName = "MantelBarometer";  // Domain name for the mDNS responder
const char *OTAName = "MantelBarometer";      // A name and a password for the OTA service
const char *OTAPassword = "YOUR OTA PASSWORD";  // Ditto

// ***************************************************************
// Misc variables
// ***************************************************************

//byte altitudeEEPROMAddressH=0; // For altitude storage (high byte)
//byte altitudeEEPROMAddressL=1; // For altitude storage (low byte)
int altitudeEEPROMAddress = 0; // Using EEPROM.put()/EEPROM.get() so no need to write/read byte by byte
int Altitude; //= 201; // Ridgefield, CT altitude is 201 meters
int pressureNow; //Stores new pressure reading
int previousPressure = 2950; //  pressure in Hg inches * 100.  Stepper indexes to about this during calibration
float pressureHg; // holds current pressure in Hg inches
bool calibrate = false;
bool moodMode; // Mood mode
int m; //Mood index number (passed from HTML file via webSocet or GA command)
int vOne; // Baseline LDR reading
int vTwo; // LDR reading to trigger indexing
int vFail; // timeout number if calibration fails
int Steps = 0; // Used in stepper function
boolean Direction = true; // anticlockwise pointer movement
unsigned long last_time; // for stepper function
unsigned long currentMillis ;// ditto
int steps_left; // 4096 steps for full rotation 1024 for One quarter rotation. 
                     // Max that can be done before NodeMCU  watchdog reset
bool calibration = true;
int stepTime = 1000; // 1ms per step
bool MQTTpause = false;

// ***************************************************************
//  Instantiate objects
// ***************************************************************

ESP8266WebServer server ( 80 ); 
WebSocketsServer webSocket = WebSocketsServer(81);
WiFiManager wifiManager;
SFE_BMP180 pressure;
SimpleTimer timer;
WiFiUDP udp; //For NTP packets
File fsUploadFile;  //  Temporarily store SPIFFS file


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
    digitalWrite(BS, 0);
    digitalWrite(BR, 0);
    digitalWrite(BF, 0);
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
  if (path.endsWith("/")) path += "Barom3.html"; // If a folder is requested, send the index file
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
// Pressure read and barometer setup functions
// ***************************************************************
    
void pressureRead() {
  if (moodMode == false)  {
    char status;
    double T,P,p0,a;

    // Loop here getting pressure readings every 10 seconds.
  
    // If you want sea-level-compensated pressure, as used in weather reports,
    // you will need to know the altitude at which your measurements are taken.
    // We're using a constant called ALTITUDE in this sketch:
    
    Serial.println();
    Serial.print("provided altitude: ");
    Serial.print(Altitude);
    Serial.print(" meters, ");
    Serial.print(Altitude*3.28084);
    Serial.println(" feet");
    
    // If you want to measure altitude, and not pressure, you will instead need
    // to provide a known baseline pressure. This is shown at the end of the sketch.
  
    // You must first get a temperature measurement to perform a pressure reading.
    
    // Start a temperature measurement:
    // If request is successful, the number of ms to wait is returned.
    // If request is unsuccessful, 0 is returned.
  
    status = pressure.startTemperature();
    if (status != 0)
    {
      // Wait for the measurement to complete:
      delay(status);
  
      // Retrieve the completed temperature measurement:
      // Note that the measurement is stored in the variable T.
      // Function returns 1 if successful, 0 if failure.
  
      status = pressure.getTemperature(T);
      if (status != 0)
      {
        // Print out the measurement:
        Serial.print("temperature: ");
        Serial.print(T,2);
        Serial.print(" deg C, ");
        Serial.print((9.0/5.0)*T+32.0,2);
        Serial.println(" deg F");
        
        // Start a pressure measurement:
        // The parameter is the oversampling setting, from 0 to 3 (highest res, longest wait).
        // If request is successful, the number of ms to wait is returned.
        // If request is unsuccessful, 0 is returned.
  
        status = pressure.startPressure(3);
        if (status != 0)
        {
          // Wait for the measurement to complete:
          delay(status);
  
          // Retrieve the completed pressure measurement:
          // Note that the measurement is stored in the variable P.
          // Note also that the function requires the previous temperature measurement (T).
          // (If temperature is stable, you can do one temperature measurement for a number of pressure measurements.)
          // Function returns 1 if successful, 0 if failure.
  
          status = pressure.getPressure(P,T);
          if (status != 0)
          {
            // Print out the measurement:
            Serial.print("absolute pressure: ");
            Serial.print(P,2);
            Serial.print(" mb, ");
            Serial.print(P*0.0295333727,2);
            Serial.println(" inHg");
  
            // The pressure sensor returns abolute pressure, which varies with altitude.
            // To remove the effects of altitude, use the sealevel function and your current altitude.
            // This number is commonly used in weather reports.
            // Parameters: P = absolute pressure in mb, ALTITUDE = current altitude in m.
            // Result: p0 = sea-level compensated pressure in mb
  
            p0 = pressure.sealevel(P,Altitude); 
            Serial.print("relative (sea-level) pressure: ");
            Serial.print(p0,2);
            Serial.print(" mb, ");
            Serial.print(p0*0.0295333727,2);
            Serial.println(" inHg");
            pressureHg = p0*0.0295333727,2;
            pressureNow = pressureHg*100;
            Serial.println(pressureNow);
            delay(500);
            if (pressureNow < previousPressure) {
              Direction = true;
              analogWrite(BS, 0); //Using pwm because LEDs are very bright head-on
              analogWrite(BR,0); //"Warm white" LEDs used for period feel
              analogWrite(BF, 75);
              steps_left = (previousPressure - pressureNow)*10.19,0; //Plug number to match barometer scale
              Serial.println(steps_left);
              while (steps_left>0) { 
                currentMillis = micros();
                if (currentMillis-last_time>=stepTime)  {
                  stepper(1); 
                  last_time=micros();
                  steps_left--;
                }
              }
              previousPressure = pressureNow;
            }
            else if (pressureNow > previousPressure) {
              Direction = false;
              analogWrite(BS, 0);
              analogWrite(BR,75);
              analogWrite(BF, 0);
              steps_left = (pressureNow - previousPressure)*10.19,0;
              Serial.println(steps_left);
              while (steps_left>0) { 
                currentMillis = micros();
                if (currentMillis-last_time>=stepTime)  {
                  stepper(1); 
                  last_time=micros();
                  steps_left--;
                }
              }
              previousPressure = pressureNow;
            }
            else if (pressureNow = previousPressure) {
              analogWrite(BS, 75);
              analogWrite(BR,0);
              analogWrite(BF, 0);
            }
            else Serial.println("error retrieving pressure measurement\n");
          }
          else Serial.println("error starting pressure measurement\n");
        }
        else Serial.println("error retrieving temperature measurement\n");
      }
      else Serial.println("error starting temperature measurement\n");
    }
    else Serial.println("In Mood Mode");
  }
}

void resetBar() {  // For calibrating/indexing 
  Serial.println("calibrating");
  unsigned long timeOut = millis();
  Direction = true;
  steps_left = 1024;
  int vOneF = analogRead(calPin);
  delay(500);
  int vOneS = analogRead(calPin);
  delay(500);
  int vOneT = analogRead(calPin);
  vOne = ((vOneF+vOneS+vOneT)/3);
  Serial.print("vOne = ");
  Serial.println(vOne);
  if (vOne < 500) {
    while (steps_left > 0) {  
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime)  {
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
      vTwo = analogRead(calPin);
      Serial.println(vTwo);
      if ((vOne*130/100)-vTwo <1)  {
        steps_left = 0;
        calibration = true;
        delay(500);
      }
      else if (((vOne*130/100)-vTwo >=1) && (steps_left == 1) && (millis() - timeOut < 15000)) {
        steps_left = 1024;
        delay(500);
        Serial.println(Direction);
      }
      else if (((vOne*130/100)-vTwo >=1) && (steps_left == 1) && (millis() - timeOut >= 15000)) {
        steps_left = 0;
        Serial.println("Calibration Fail");
        calibration = false;
      }
    }
  }
  else if (vOne >= 500) {
    while (steps_left > 0) {  
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime)  {
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
      vTwo = analogRead(calPin);
      Serial.println(vTwo);
      if ((vOne*110/100)-vTwo <1)  {
        steps_left = 0;
        calibration = true;
        delay(500);
      }
      else if (((vOne*110/100)-vTwo >=1) && (steps_left == 1) && (millis() - timeOut < 15000)) {
        steps_left = 1024;
        delay(500);
        Serial.println(Direction);
      }
      else if (((vOne*110/100)-vTwo >=1) && (steps_left == 1) && (millis() - timeOut >= 15000)) {
        steps_left = 0;
        Serial.println("Calibration Fail");
        calibration = false;
      }
    }
  }
  if (calibration == true)  {
    Serial.println("Wait");
    Direction=!Direction;
    Serial.println(Direction);
    steps_left=1024;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime){
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
    delay(500);
    steps_left=630;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime){
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
  }
  else if (calibration == false) {
    Serial.println("Cal Fail");
    calFail();
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
  if (Direction==1){ Steps++;}
  if (Direction==0){ Steps--; }
  if (Steps>7){Steps=0;}
  if (Steps<0){Steps=7; }
}


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

void showTrend()  {
  if (pressureNow = previousPressure) {
     analogWrite(BS, 75);
     analogWrite(BR,0);
     analogWrite(BF, 0);
  } else if (pressureNow < previousPressure)  {
     digitalWrite(BS, LOW);
     digitalWrite(BR,LOW);
     analogWrite(BF, 75);
  } else if (pressureNow > previousPressure)  {
     digitalWrite(BS, LOW);
     analogWrite(BR,75);
     digitalWrite(BF, LOW);
  } 
}

void moodSet()  {
  analogWrite(BS, 75);
  analogWrite(BR,75);
  analogWrite(BF, 75);
  vOne = analogRead(calPin);
  Serial.print("vOne = ");
  Serial.println(vOne);
  if ((m==1)  && (moodMode == true)) {
    resetBar();
    Direction = true;
    Serial.println(Direction);
    Serial.println("Setting to Mad");
    delay(500);
    steps_left=1024;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime)  {
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
    delay(500);
    steps_left=500;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime){
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    } 
  }
  else if ((m==2)  && (moodMode == true)) {
    resetBar();
    Direction = true;
    Serial.println(Direction);
    delay(500);
    steps_left=900;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime){
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
    delay(500);
  }
  else if ((m==3) && (moodMode == true)) {
    resetBar();
  }
  else if ((m==4) && (moodMode == true)) {
    resetBar();
    Serial.println(Direction);
    Serial.println("Setting to Happy");
    delay(500);
    steps_left=825;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime)  {
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
  }
  else if ((m==5) && (moodMode == true)) {
    resetBar();
    Serial.println(Direction);
    delay(500);
    steps_left=1024;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime)  {
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    }
    delay(500);
    steps_left=500;
    while (steps_left > 0) {
      currentMillis = micros();
      if (currentMillis-last_time>=stepTime){
        stepper(1); 
        last_time=micros();
        steps_left--;
      }
    } 
  }
}

void calFail() {
  vFail = analogRead (calPin);
  Serial.println("Doing Cal Fail");
  while (vFail >850)  {
  vFail = analogRead (calPin);
  analogWrite(BS, 100);
  analogWrite(BR,100);
  analogWrite(BF, 100);
  delay(500);
  analogWrite(BS, 0);
  analogWrite(BR,0);
  analogWrite(BF, 0);
  delay(500);
  Serial.println("Still Doing Call Fail");
  }
  calibration = true;
  resetBar();
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
  Serial.begin(115200);
  delay(1000);
  Serial.println("\r\n");
  wifiManager.autoConnect("MantelBarometer"); 
  Serial.println("Connected");
  
  pinMode(IN1, OUTPUT);    // Set pin modes
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(calPin, INPUT);
  pinMode(BS, OUTPUT);
  pinMode(BR, OUTPUT);
  pinMode(BF, OUTPUT);
  delay(100);
  
  digitalWrite(IN1, LOW);   
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  digitalWrite(BS, LOW);
  digitalWrite(BR, LOW);
  digitalWrite(BF, LOW);
  
  EEPROM.begin(512);
  //byte high = EEPROM.read(altitudeEEPROMAddressH); //read the first half
  //byte low = EEPROM.read(altitudeEEPROMAddressL); //read the second half
  //Altitude = (high << 8) + low;
  EEPROM.get(altitudeEEPROMAddress, Altitude);
  Serial.print("Altitude is: ");
  Serial.println(Altitude);
  Serial.println("\nReady!\n");

  resetBar();
  if (pressure.begin()) {
    Serial.println("BMP180 init success");
  } 
  else
  {
    Serial.println("BMP180 init fail\n\n");
    while(1); // Pause forever.
  }
  
  timer.setInterval(3000000, pressureRead); // Read pressure every hour
  timer.setInterval(250000, reconnectMQTT);  // Pinging wasn't keeping connection open

  startOTA();                  // Start the OTA service
  startSPIFFS();               // Start the SPIFFS and list all contents
  startWebSocket();            // Start a WebSocket server
  startMDNS();                 // Start MDNS service
  startServer();               // Start a HTTP server with a file read handler and an upload handler
  server.onNotFound(handleNotFound); // if someone requests any other file or page, go to function 'handleNotFound'

  pressureRead();
  
  analogWrite(BS, 75); // Set LEDs to inidcate pressure steady
  analogWrite(BR,0);
  analogWrite(BF, 0);
  
  mqtt.subscribe(&mood); // subscribe to the Adafruit IO mood feed
  MQTT_connect();
}

// ***************************************************************
// Program Loop
// ***************************************************************

unsigned long nextConnectionCheckTime = 0; // for checking connection time
unsigned long prevMillis = millis();

void loop() {
  timer.run();
  webSocket.loop();
  server.handleClient();
  ArduinoOTA.handle(); 
  if (MQTTpause = false) {MQTT_connect();}
  if (millis() > nextConnectionCheckTime) {
    Serial.print("\n\nChecking WiFi... ");
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost. Resetting...");
      ESP.reset();
    } else {Serial.println("OK"); }    
    nextConnectionCheckTime = millis() + WIFI_CHK_TIME_MS;
  }
 
  //Read from our subscription queue until we run out, or
  //wait up to 5 seconds for subscription to update

  Adafruit_MQTT_Subscribe * subscription;
  while ((subscription = mqtt.readSubscription(100)))  {
    if (subscription == &mood)  //If the subscription is updated...
    {
      Serial.print("Mood Barometer: ");
      Serial.println((char*) mood.lastread);
      String stringOne = ((char*) mood.lastread);
      Serial.println(stringOne);
      //delay(10);
      if ((stringOne == " mad") || (stringOne == " Mad")){
        moodMode = true;
        m = 1;
        Serial.println(stringOne);
        Serial.print("Mood Index = ");
        Serial.println(m);
        analogWrite(BS, 75);
        analogWrite(BR,75);
        analogWrite(BF, 75);
        moodSet(); 
      } else if (stringOne == " sad") {
      moodMode = true;
      m = 2;
      Serial.println(stringOne);
      Serial.print("Mood Index = ");
      Serial.println(m);
      analogWrite(BS, 75);
      analogWrite(BR,75);
      analogWrite(BF, 75);
      moodSet();
      } else if ((stringOne == " Moody") || (stringOne == " moody")) {
        moodMode = true;
        m = 3;
        Serial.println(stringOne);
        Serial.print("Mood Index = ");
        Serial.println(m);
        analogWrite(BS, 75);
        analogWrite(BR,75);
        analogWrite(BF, 75);
        moodSet();
      } else if (stringOne == " Happy") {
        moodMode = true;
        m = 4;
        Serial.println(stringOne);
        Serial.print("Mood Index = ");
        Serial.println(m);
        analogWrite(BS, 75);
        analogWrite(BR,75);
        analogWrite(BF, 75);
        moodSet();
      } else if (stringOne == " very happy") {
        moodMode = true;
        m = 5;
        Serial.println(stringOne);
        Serial.println(m);
        analogWrite(BS, 75);
        analogWrite(BR,75);
        analogWrite(BF, 75);
        moodSet();
      } else if ((stringOne == " off") || (stringOne == "OFF")) {
        Serial.println(stringOne);
        moodMode = false;
        m = 0;
        Serial.println("Setting to Off");
        resetBar();
        if (calibration == true)  {
          previousPressure = 2950;
          pressureRead();
          analogWrite(BS, 75);
          analogWrite(BR,0);
          analogWrite(BF, 0);
        } else if (calibration == false)  {
            calFail();
        }
      } 
    }
  }
/*

  if (!mqtt.ping()) {
    Serial.println("Ping MQTT Server Fail -- disconnecting");
    mqtt.disconnect();
  } // ping the server to keep the mqtt connection alive
*/
  delay(10); 
}

// ***************************************************************
// End Program Loop
// ***************************************************************

// ***************************************************************
// Websocket events
// ***************************************************************

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) { // When a WebSocket message is received
  switch (type) {
    case WStype_DISCONNECTED:             // if the websocket is disconnected
      Serial.printf("[%u] Disconnected!\n", num);
      EEPROM.begin(512);
      //EEPROM.write(altitudeEEPROMAddressH, highByte(Altitude)); // Just used for testing
      //EEPROM.write(altitudeEEPROMAddressL, lowByte(Altitude));
      EEPROM.put(altitudeEEPROMAddress, Altitude); // Will only write if altitude has changed (uses EEPROM.update())
      EEPROM.commit();
      Serial.print("Altitide written to EEPROM. New altitude is ");
      Serial.println(Altitude);   
      MQTTpause = false;
      MQTT_connect(); // Reconnect with MQTT service on session termination
      break;
    case WStype_CONNECTED: {              // if a new websocket connection is established
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload); 
      }
      break;
    case WStype_TEXT:                     // if new text data is received
      Serial.printf("[%u] get Text: %s\n", num, payload);
    if (payload[0] == 'A') {                      
        Altitude = atoi((const char *) &payload[1]);  
    } else if (payload[0] == 'M') {                      
        m = atoi((const char *) &payload[1]); 
        Serial.println(m);
        moodSet();
    } else if (payload[0] == 'B') {                      // the browser sends a C to turn the headlights on
        calUP();
    } else if (payload[0] == 'C') {                      // the browser sends a D to turn the headlights off
        calDOWN();      
    }  else if (payload[0] == 'D') {                     // the browser sends a G to turn the hazards on
         moodMode = true;
         //resetBar();
          analogWrite(BS, 75);
          analogWrite(BR,75);
          analogWrite(BF, 75);
        
           
    } else if (payload[0] == 'E') {                      // the browser sends an H to turn the hazards off
        moodMode = false;
        resetBar();
        if (calibration == true)  {
          previousPressure = 2950;
          pressureRead();
          analogWrite(BS, 75);
          analogWrite(BR,0);
          analogWrite(BF, 0);
        } else if (calibration == false)  {
          calFail();
        }
             
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


