/*
  FSWebServer - Example WebServer with SPIFFS backend for esp8266
  Copyright (c) 2015 Hristo Gochkov. All rights reserved.
  This file is part of the ESP8266WebServer library for Arduino environment.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  upload the contents of the data folder with MkSPIFFS Tool ("ESP8266 Sketch Data Upload" in Tools menu in Arduino IDE)
  or you can upload the contents of a folder if you CD in that folder and run the following command:
  for file in `ls -A1`; do curl -F "file=@$PWD/$file" esp8266fs.local/edit; done

  access the sample web page at http://esp8266fs.local
  edit the page by going to http://esp8266fs.local/edit
*/
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <WebSocketsServer.h>
#include <Hash.h>
#include "OneButton.h"
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <Wire.h>
#include <Rtc_Pcf8563.h>
#include <FS.h>
#include <time.h>

#define REC_LED     1
#define BAT_LED    0x2000
#define FULL_LED   0x2001
#define MODE_KEY    4
#define START_KEY   16

#define CS_0  0
#define CS_1  2
#define CS_2  13
#define CS_3  15


#define CS_HIGH  (uint16_t) (1<<CS_0 | 1<<CS_1 | 1<<CS_2 |1<<CS_3)
const uint16_t  TC[] = {0xA001, 0xA005, 0xA000, 0xA004, 0x8005, 0x8000, 0x8004, 0x2005, 0x8001, 0x2004};


#define DBG_OUTPUT_PORT Serial
#define USE_SERIAL Serial

#define LOGGER_STRUCT_SIZE 26

struct logger_struct {
  uint32_t timestamp;
  uint16_t mSec;
  int16_t channel[10];
};


struct systemConfig {
  uint32_t samplingInterval = 1000;
  uint8_t logDataON = 0;
  uint8_t diskFull = 0;
};


uint8_t wsConnection = 0;

ADC_MODE(ADC_VCC);

Rtc_Pcf8563 rtc;

// Setup a new OneButton on MODE_KEY
//OneButton selectKey(MODE_KEY, true);
OneButton startKey(START_KEY, true);

const char* updatehost = "pchunter.synology.me";
const char* ssid = "xx";
const char* password = "xx";
const char* host = "tc10";
const uint32_t interval = 1000;     // Sampling interval
const uint8_t LOG_BUFF_SIZE = 1;
const size_t DATA_FILE_SIZE = 1500000;
const size_t DATA_FILE_WARNING_SIZE = 1400000;


logger_struct logData;
systemConfig sysConfig;
uint32_t startTime;
unsigned long previousMillis = 0;
unsigned long sysCheckPreviousMillis = 0;

bool recordStop = false;
bool probeError = false;

uint8_t buffCount = 0;
uint32_t REC_LED_Blink_Speed = 0x300;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WebSocketsServer webSocket = WebSocketsServer(81);

//holds the current upload
File fsUploadFile;


void setup(void) {

  max31855Setup();


  //set a time to start with.
  rtc.setSquareWave(SQW_DISABLE);


  pinMode(MODE_KEY, INPUT);
  pinMode(START_KEY, INPUT);

  if ( !digitalRead(START_KEY)) {
    delay(500);
    for (uint8_t i = 0 ; i < 20 ; i++) {
      rtc.setSquareWave(SQW_DISABLE);
      delay(50);
      rtc.setSquareWave(SQW_1024HZ);
      delay(50);
    }
    if ( digitalRead(START_KEY)) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP("LoggerDirect", "12345678");
    } else {
      WiFiManager wifiManager;
      wifiManager.startConfigPortal("LoggerSetup");
      delay(2000);
      ESP.reset();
    }
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin();
  }

  //  //WIFI INIT
  DBG_OUTPUT_PORT.printf("Connecting to %s\n", ssid);
  //  if (String(WiFi.SSID()) != String(ssid)) {
  //    WiFi.begin(ssid, password);
  //  }

  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.print("\n");
  DBG_OUTPUT_PORT.setDebugOutput(true);
  SPIFFS.begin();
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      DBG_OUTPUT_PORT.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    DBG_OUTPUT_PORT.printf("\n");
  }


  // load system config value
  File sys = SPIFFS.open("/config.sys", "r");
  if (sys) {
    sys.readBytes((char*) &sysConfig, sizeof(sysConfig));
    sys.close();
  }


  uint8_t count = 3;
  while (!count) {
    if (WiFi.status() == WL_CONNECTED)
      break;
    delay(500);
    DBG_OUTPUT_PORT.print(".");
    count--;
  }

  //Wait for MODE_KEY release before read the RTC due to they use the same pin.
  while (!digitalRead(MODE_KEY)) {}
  rtc.setSquareWave(SQW_DISABLE);
  startTime = pcf8563_time_to_unix() - millis() / 1000;


  MDNS.begin(host);


  //SERVER INIT
  //list directory
  server.on("/list", HTTP_GET, handleFileList);
  //load editor
  server.on("/edit", HTTP_GET, []() {
    if (!handleFileRead("/edit.htm")) server.send(404,  F( "text/plain"), "FileNotFound");
  });
  //create file  server.on("/edit", HTTP_PUT, handleFileCreate);
  //delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, []() {
    server.send(200,  F( "text/plain"), "");
  }, handleFileUpload);

  //reboot
  server.on("/reboot", HTTP_GET, []() {
    server.send(200,  F( "text/plain"), "Logger rebooting...");
    rtc.setSquareWave(SQW_DISABLE);
    delay(2000);
    ESP.reset();
  });

  // adjust RTC
  server.on("/rtc", HTTP_GET, adjustRTC);

  // set Config
  server.on("/setconfig", HTTP_GET, setConfig);

  // get Config
  server.on("/sysconfig", HTTP_GET, getConfig);

  // clean all data
  server.on("/clean", HTTP_GET, []() {
    recordStop = true;
    deleteData();
    server.send(200,  F( "text/plain"), "OK");
    recordStop = false;
  });

  //http update
  server.on("/httpupdate", HTTP_GET, httpUpdate);

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([]() {
    if (!handleFileRead(server.uri()))
      server.send(404,  F( "text/plain"), "FileNotFound");
  });

  // Get system data
  server.on("/sysinfo", HTTP_GET, sysinfo );




  httpUpdater.setup(&server);
  server.begin();
  DBG_OUTPUT_PORT.println("HTTP server started");

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);


  ArduinoOTA.setHostname(host);
  ArduinoOTA.begin();

  MDNS.addService("http", "tcp", 80);


  //define pin modes
  pinMode(CS_0, OUTPUT);
  pinMode(CS_1, OUTPUT);
  pinMode(CS_2, OUTPUT);
  pinMode(CS_3, OUTPUT);

  digitalWrite(CS_0, HIGH);
  digitalWrite(CS_1, HIGH);
  digitalWrite(CS_2, HIGH);
  digitalWrite(CS_3, HIGH);

  startKey.attachLongPressStart(startLongPress);


  delay(1000); //delay 3s for Sensor stability

  digitalWrite(REC_LED, HIGH);
  pinMode(REC_LED, OUTPUT);

}

void loop(void) {

  //  GPOS = 0xA001;
  server.handleClient();
  webSocket.loop();
  ArduinoOTA.handle();

  startKey.tick();

  //  GPOC = 0xA001;


  logger_struct LogDataBuff[LOG_BUFF_SIZE];

  unsigned long currentMillis = millis();

  // Recording REC_LED Blinking.
  digitalWrite(REC_LED,  !((currentMillis & REC_LED_Blink_Speed) && sysConfig.logDataON || sysConfig.diskFull));

  GPOS = CS_HIGH;
  if (currentMillis & 0x200)
    GPOC = BAT_LED;
  else
    GPOC = FULL_LED;

  if (currentMillis - previousMillis >= sysConfig.samplingInterval || previousMillis == 0) {
    previousMillis = currentMillis;

    uint32_t m = millis();
    logData.timestamp = startTime + m / 1000;
    logData.mSec = m % 1000;

    GPOS = CS_HIGH;

    uint32_t tc_raw[10];
    max3188_readAllRaw(tc_raw);

    for (byte i = 0; i < 10; i++) {
      //check error bit
      if (tc_raw[i] & 0x100) {
        logData.channel[i] = 5000; //big value for javascript error check.
      } else {

        // skip ten sampling if probe error
        if (logData.channel[i] > (int16_t)(5000 - 2000 / sysConfig.samplingInterval) )
          logData.channel[i] --;
        else {
          // check netvigative value
          if (((byte*)&tc_raw[i])[0] & 0x80)
            logData.channel[i] = (((((byte*)&tc_raw[i])[0] | 0xffC0) << 6) ) | ((byte*)&tc_raw[i])[1] >> 2;
          else
            logData.channel[i] = ((byte*)&tc_raw[i])[0] << 6 | ((byte*)&tc_raw[i])[1] >> 2;
        }
      }
    }


    if (wsConnection != 0)
      webSocket.broadcastBIN((uint8_t *)&logData, LOGGER_STRUCT_SIZE);
    else {
      // check wifi status turn on led
      if (WiFi.status() == WL_CONNECTED)
        rtc.setSquareWave(SQW_1024HZ);
      else
        rtc.setSquareWave(SQW_DISABLE);
    }

    if (sysConfig.logDataON && !recordStop ) {
      LogDataBuff[buffCount] = logData;
      buffCount++;
      if (buffCount >= LOG_BUFF_SIZE) {
        buffCount = 0;
        File binfile = SPIFFS.open("/data.bin", "a+");
        if (binfile) {
          binfile.write((uint8_t *)&LogDataBuff, LOGGER_STRUCT_SIZE * LOG_BUFF_SIZE);
          // binfile.close();
        }
      }

    }

    //check system status
    if ((millis() - sysCheckPreviousMillis  > 60000 ) )
      checkSystemStatus();

  }

  yield();


}





void checkSystemStatus( ) {

  sysCheckPreviousMillis = millis();
  webSocket.broadcastTXT("checking");

  File f = SPIFFS.open("/data.bin", "r");
  String  s = String(f.size());

  webSocket.broadcastTXT(s);
  if (f.size() > DATA_FILE_SIZE   ) {
    webSocket.broadcastTXT("diskFull");
    REC_LED_Blink_Speed = 0x1; //alway on
    sysConfig.logDataON = 0  ;
    sysConfig.diskFull = 1;
    webSocket.broadcastTXT("rec-off");
    saveSysConfig();
  }
  else if (f.size() > DATA_FILE_WARNING_SIZE) {
    REC_LED_Blink_Speed = 0x40; //fast blanking
    webSocket.broadcastTXT("spaceWarning");
  } else {
    REC_LED_Blink_Speed = 0x300; //slow blanking
    webSocket.broadcastTXT("spaceOK");
  }
  f.close();
}

void startLongPress() {
  if (digitalRead(MODE_KEY)) {

    if (sysConfig.logDataON) {
      sysConfig.logDataON = 0  ;
      webSocket.broadcastTXT("rec-off");
    } else {
      sysConfig.logDataON = 1 ;
      webSocket.broadcastTXT("rec-on");
    }

    buffCount = 0;
    saveSysConfig();
    checkSystemStatus();
  } else {
    deleteData();
  }
}


void selectLongPress() {
  if (digitalRead(START_KEY))
    return;
  deleteData();

}


void deleteData() {
  recordStop = true;

  //remove the data file
  SPIFFS.remove("/data.bin");
  //create new data file
  File file = SPIFFS.open("/data.bin", "w");
  if (file) {
    file.close();
  }
  // blinking LED
  for (uint8_t i = 0 ; i < 20 ; i++) {
    digitalWrite(REC_LED,  i & 0x1 );
    if (i & 0x1)
      rtc.setSquareWave(SQW_DISABLE);
    else
      rtc.setSquareWave(SQW_1HZ);
    delay(50);
  }
  sysConfig.diskFull = 0;
  REC_LED_Blink_Speed = 0x300;

  checkSystemStatus();
  // resume LED status
  if (wsConnection != 0)
    rtc.setSquareWave(SQW_1HZ);
  delay(100);
  recordStop = false;
}


void saveSysConfig() {

  File sys = SPIFFS.open("/config.sys", "w");
  if (sys) {
    sys.write((uint8_t*) &sysConfig, sizeof(sysConfig));
    sys.close();
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) {

  switch (type) {
    case WStype_DISCONNECTED:
      USE_SERIAL.printf("[%u] Disconnected!\n", num);

      wsConnection--;
      if (wsConnection == 0)
        rtc.setSquareWave(SQW_1024HZ);     //no websocket connection off the LED
      break;

    case WStype_CONNECTED:
      {
        rtc.setSquareWave(SQW_1HZ);
        wsConnection++;
        // IPAddress ip = webSocket.remoteIP(num);
        // USE_SERIAL.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

        // send message to client
        // webSocket.sendTXT(num, "Connected");

        //        File f = SPIFFS.open("/data.bin", "r");
        //
        //        if (f) {
        //          // f.seek( -(sizeof(logData) / sizeof(uint8_t)) , SeekEnd);
        //
        //          size_t pkgSize = sizeof(logData) * 100;
        //          uint16_t cycle = f.size() / pkgSize;
        //          byte b[pkgSize];
        //
        //          for (uint16_t j = 0 ; j < cycle; j++) {
        //            for (uint16_t i = 0 ; i < pkgSize; i++) {
        //              b[i] = f.read();
        //            }
        //            webSocket.sendBIN(num, b, pkgSize);
        //            yield();
        //          }
        //          f.close();
        //        }

      }
      break;
    case WStype_TEXT:
      USE_SERIAL.printf("[%u] get Text: %s\n", num, payload);

      // send message to client
      // webSocket.sendTXT(num, "message here");

      // send data to all connected clients
      // webSocket.broadcastTXT("message here");

      break;


    case WStype_BIN:
      USE_SERIAL.printf("[%u] get binary lenght: %u\n", num, lenght);
      hexdump(payload, lenght);

      // send message to client
      // webSocket.sendBIN(num, payload, lenght);
      break;
  }

}

