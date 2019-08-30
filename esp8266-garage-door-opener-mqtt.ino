#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// Copy config.tmpl.h to config.h and replace with your own values
#include "config.h"

char outputState = 0;
char lastState = 0;

/************ Global State (you don't need to change this!) ******************/

// Create an ESP8266 WiFiClient class to connect to the MQTT server.
WiFiClientSecure client;

// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details.
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

/****************************** Feeds ***************************************/

Adafruit_MQTT_Publish openCloseSetFeed = Adafruit_MQTT_Publish(&mqtt, OPEN_CLOSE_SET_FEED);
Adafruit_MQTT_Subscribe openCloseFeed = Adafruit_MQTT_Subscribe(&mqtt, OPEN_CLOSE_FEED);

/*************************** Sketch Code ************************************/

// Bug workaround for Arduino 1.6.6, it seems to need a function declaration
// for some reason (only affects ESP8266, likely an arduino-builder bug).
void MQTT_connect();

void setup() {
  Serial.begin(115200);
  delay(10);

  Serial.println(F("Controle de lampe - Google Home"));

  pinMode(LED_BUILTIN, OUTPUT);     // Initialize the LED_BUILTIN pin as an output
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(D1, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(D1), handleInterrupt, FALLING);
  // Connect to WiFi access point.
  Serial.println(); Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WLAN_SSID);

  WiFi.begin(WLAN_SSID, WLAN_PASS);
  //WiFi.scanNetworksAsync(prinScanResult);

  int wifiStatus;
  while ((wifiStatus = WiFi.status()) != WL_CONNECTED) {
    delay(500);
    Serial.print(wifiStatus);
    Serial.print(" ");
  }

  Serial.println();

  Serial.println("WiFi connected");
  Serial.println("IP address: "); Serial.println(WiFi.localIP());
  
  // Setup MQTT subscription for onoff feed.
  mqtt.subscribe(&openCloseFeed);
}

uint32_t x=0;

void loop() {
  // Ensure the connection to the MQTT server is alive (this will make the first
  // connection and automatically reconnect when disconnected).  See the MQTT_connect
  // function definition further below.
  MQTT_connect();
  
  // this is our 'wait for incoming subscription packets' busy subloop
  // try to spend your time here

  Adafruit_MQTT_Subscribe *subscription;
  while ((subscription = mqtt.readSubscription(3000))) {
    if (subscription == &openCloseFeed) {
      Serial.print(F("Got: "));
      Serial.println((char *)openCloseFeed.lastread);
      if (strcmp((char *)openCloseFeed.lastread, "1") == 0) {
        digitalWrite(LED_BUILTIN, LOW); 
        outputState = 1;
      }
      if (strcmp((char *)openCloseFeed.lastread, "0") == 0) {
        digitalWrite(LED_BUILTIN, HIGH);
        outputState = 0;
      }
    }

  }
  
  // Now we can publish stuff!
  if (outputState != lastState) {
    lastState = outputState;
    Serial.print(F("\nSending state val "));
    Serial.print(outputState, BIN);
    Serial.print("...");
    if (! openCloseSetFeed.publish(outputState)) {
      Serial.println(F("Failed"));
    } else {
      Serial.println(F("OK!"));
    }
  }

  // ping the server to keep the mqtt connection alive
  // NOT required if you are publishing once every KEEPALIVE seconds
/*
  if(! mqtt.ping()) {
    mqtt.disconnect();
  }
  */
}

void handleInterrupt() {
 static unsigned long last_interrupt_time = 0;
 unsigned long interrupt_time = millis();
  if (interrupt_time - last_interrupt_time > 200) 
 {
  if (outputState == 0) {
    outputState = 1;
    digitalWrite(LED_BUILTIN, LOW);
  }
  else {
    outputState = 0;
    digitalWrite(LED_BUILTIN, HIGH);
  }
   }
 last_interrupt_time = interrupt_time;
}

// Function to connect and reconnect as necessary to the MQTT server.
// Should be called in the loop function and it will take care if connecting.
void MQTT_connect() {
  int8_t ret;

  // Stop if already connected.
  if (mqtt.connected()) {
    return;
  }

  Serial.print("Connecting to MQTT... ");

  uint8_t retries = 3;
  while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
       Serial.println(mqtt.connectErrorString(ret));
       Serial.println("Retrying MQTT connection in 5 seconds...");
       mqtt.disconnect();
       delay(5000);  // wait 5 seconds
       retries--;
       if (retries == 0) {
         // basically die and wait for WDT to reset me
         while (1);
       }
  }
  Serial.println("MQTT Connected!");
}

/*
void prinScanResult(int networksFound)
{
  Serial.printf("%d network(s) found\n", networksFound);
  for (int i = 0; i < networksFound; i++)
  {
    Serial.printf("%d: %s, Ch:%d (%ddBm) (%s) %s\n", i + 1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str(), WiFi.encryptionType(i) == ENC_TYPE_NONE ? "open" : "");
  }
}
*/
