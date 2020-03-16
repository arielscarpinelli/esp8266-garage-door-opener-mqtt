#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// Copy config.tmpl.h to config.h and replace with your own values
#include "config.h"

#include "certificates.h"

#define SECONDARY_LED D4
#define OUTPUT_PIN D1
#define INPUT_PIN D2

// Poorman's state pattern
void toggle();
void reverseToggle();
void noop() {};

typedef struct DoorState {
  String name;
  DoorState* motorStarted;
  DoorState* motorStopped;
  void (*receivedRequestToOpen)(void);
  void (*receivedRequestToClose)(void);
  bool valueToNotify;
} DoorState;

DoorState DOOR_CLOSING = {
  .name = "CLOSING",
  .motorStarted = &DOOR_CLOSING,
  .motorStopped = &DOOR_CLOSING, // redefined below
  .receivedRequestToOpen = reverseToggle,
  .receivedRequestToClose = noop,
  .valueToNotify = true
};

DoorState DOOR_WILL_CLOSE_ON_TOGGLE = {
  .name = "WILL_CLOSE_ON_TOGGLE",
  .motorStarted = &DOOR_CLOSING,
  .motorStopped = &DOOR_WILL_CLOSE_ON_TOGGLE,
  .receivedRequestToOpen = noop,
  .receivedRequestToClose = toggle,
  .valueToNotify = true
};

DoorState DOOR_OPENING = {
  .name = "DOOR_OPENING",
  .motorStarted = &DOOR_OPENING,
  .motorStopped = &DOOR_WILL_CLOSE_ON_TOGGLE,
  .receivedRequestToOpen = noop,
  .receivedRequestToClose = reverseToggle,
  .valueToNotify = true
};

DoorState DOOR_WILL_OPEN_ON_TOGGLE = {
  .name = "DOOR_WILL_OPEN_ON_TOGGLE",
  .motorStarted = &DOOR_OPENING,
  .motorStopped = &DOOR_WILL_OPEN_ON_TOGGLE,
  .receivedRequestToOpen = toggle,
  .receivedRequestToClose = noop,
  .valueToNotify = false
};

DoorState* currentState = &DOOR_WILL_OPEN_ON_TOGGLE;

WebSocketsClient webSocket;

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void publishState();

bool connected = false;
bool justConnected = false;
bool connectingLed = false;

bool lastNotifiedValue = false;

void ICACHE_RAM_ATTR motorChanged();

void setup() {

  DOOR_CLOSING.motorStopped = &DOOR_WILL_OPEN_ON_TOGGLE;

  Serial.begin(115200);
  delay(10);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(SECONDARY_LED, OUTPUT); // NodeMCU secondary led
  digitalWrite(SECONDARY_LED, HIGH);
  pinMode(OUTPUT_PIN, OUTPUT);
  pinMode(INPUT_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(INPUT_PIN), motorChanged, CHANGE);

  WiFi.begin(WLAN_SSID, WLAN_PASS);

  // Set time via NTP, as required for x.509 validation
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  webSocket.beginSslWithCA("iotmaster.dev", 443, "/api/ws?deviceid=" DEVICE_ID "&apikey=" API_KEY, ca_cert[0]);
  webSocket.onEvent(webSocketEvent);

}

void loop() {

  updateConnectingLed();
  webSocket.loop();

  if (currentState->valueToNotify != lastNotifiedValue) {
    publishState();
    Serial.println(currentState->name);
  }

}

void toggle() {
  digitalWrite(LED_BUILTIN, LOW);  
  digitalWrite(OUTPUT_PIN, HIGH);
  delay(500);
  digitalWrite(OUTPUT_PIN, LOW);
  digitalWrite(LED_BUILTIN, HIGH);  
}

void reverseToggle() {
  // first stop
  toggle();
  delay(500);
  // I think that the interrupt should have changed the value of the current state in the middle, but shouldn't stop us...
  // then restart
  toggle();
}

void ICACHE_RAM_ATTR motorChanged() {
  static unsigned long last_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  if (interrupt_time - last_interrupt_time > 200) {
    bool value = digitalRead(INPUT_PIN);
    digitalWrite(SECONDARY_LED, value);
    if (value == LOW) { // input is inverted
      currentState = currentState->motorStarted;
    } else {
      currentState = currentState->motorStopped;
    }
  }
  last_interrupt_time = interrupt_time;
  
}

void updateConnectingLed() {

  static unsigned long lastLoopMillis = 0; 
  
  if (!connected) {
    unsigned long currentTimeMillis = millis();
    if (currentTimeMillis > (lastLoopMillis + 500)) {
      lastLoopMillis = currentTimeMillis;
      connectingLed = !connectingLed;
      digitalWrite(LED_BUILTIN, connectingLed ? LOW : HIGH);
    }
  }

  if (justConnected) {
    connectingLed = false;
    justConnected = false;
    digitalWrite(LED_BUILTIN, HIGH);
  }
      
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {

  switch(type) {

    case WStype_DISCONNECTED:
      Serial.println("[WSc] Disconnected!");
      connected = false;
      break;

    case WStype_CONNECTED:
      Serial.println("[WSc] Connected to url:");
      Serial.write(payload, length);
      connected = true;
      justConnected = true;
      publishState();   
      break;

    case WStype_TEXT:
      Serial.println("[WSc] got text:");
      Serial.write(payload, length);

      const size_t capacity = JSON_OBJECT_SIZE(20);
      DynamicJsonDocument doc(capacity);

      // Deserialize the JSON document
      DeserializationError error = deserializeJson(doc, payload, length);

      // Test if parsing succeeds.
      if (error) {
          Serial.print(F("deserializeJson() failed: "));
          Serial.println(error.c_str());
          return;
      }

      if (doc["action"] == "update") {
          const char *sequence = doc["sequence"];
          JsonObject params = doc["params"];
          
          if (params.containsKey("on")) {
            if (params["on"]) {
              currentState->receivedRequestToOpen();
            } else {
              currentState->receivedRequestToClose();
            }
            sendRecipt(sequence);
          }
          
      }
      
      break;

  }

}

void publishState() {

  bool nextValue = currentState->valueToNotify;

  const size_t capacity = JSON_OBJECT_SIZE(20);
  DynamicJsonDocument doc(capacity);

  doc["action"] = "update";
  doc["deviceid"] = DEVICE_ID;
  doc["apikey"] = API_KEY;
  JsonObject params  = doc.createNestedObject("params");
  params["on"] = nextValue;

  String json;
  serializeJson(doc, json);  
  
  if(webSocket.sendTXT(json)) {
    lastNotifiedValue = nextValue;
  }
  
}

void sendRecipt(const char *sequence) {

    const size_t capacity = JSON_OBJECT_SIZE(20);
    DynamicJsonDocument doc(capacity);

    doc["error"] = 0;
    doc["sequence"] = sequence;

    String json;
    serializeJson(doc, json);  
    
    webSocket.sendTXT(json);
  
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
