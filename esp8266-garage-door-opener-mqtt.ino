#ifdef DEBUG_ESP_PORT
  #define DEBUG_MSG(...) Serial.println( __VA_ARGS__ )
  #define DEBUG_WRITE(...) Serial.write( __VA_ARGS__ )
  #define DEBUG_MSG_(...) Serial.print( __VA_ARGS__ )
  #define DEBUG_MSGF(...) Serial.printf( __VA_ARGS__ )
#else
  #define DEBUG_MSG(...)
  #define DEBUG_WRITE(...)
  #define DEBUG_MSG_(...)
  #define DEBUG_MSGF(...) 
#endif

#include <Arduino.h>

// Copy config.tmpl.h to config.h and replace with your own values
#include "config.h"


#define SECONDARY_LED D4
#define OUTPUT_PIN D1
#define INPUT_PIN D2

#include "ConnectionManager.h"

void processStateUpdate(const DynamicJsonDocument& payload);
void publish();

SSLSocketIOclient socketIO;

class MyConnectionManager : public ConnectionManager {
  public: 
  MyConnectionManager(SSLSocketIOclient &client) : 
    ConnectionManager(client) {}
  
  virtual void onConnect() {
    publish();
  }

  virtual void processJsonEvent(String& eventName, const DynamicJsonDocument& payload) {
    if (eventName == "update") {
      processStateUpdate(payload);
    }    
  }
};

MyConnectionManager conn(socketIO);

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

bool lastNotifiedValue = false;
bool shouldPrintCurrentState = true;
DoorState* currentState = &DOOR_WILL_OPEN_ON_TOGGLE;

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

  conn.setup("garage");
  
}


void publish() {
  DynamicJsonDocument doc(JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(2));
  JsonArray payload = doc.to<JsonArray>();

  // Socket.IO "eventName"
  payload.add("update");

  JsonObject envelope = payload.createNestedObject();
  JsonObject state = envelope.createNestedObject(DEVICE_ID);

  state["online"] = true;
  state["on"] = currentState->valueToNotify;

  String json;
  serializeJson(doc, json);

  DEBUG_MSG(json);

  if (socketIO.sendEVENT(json)) {
    lastNotifiedValue = currentState->valueToNotify;
  } else {  
    DEBUG_MSG("failed to send");
  }
}

void publishIfNeeded() {

  if (currentState->valueToNotify != lastNotifiedValue) {

    publish();

  }
}



void loop() {

  conn.reconnect();

  if (shouldPrintCurrentState) {
    shouldPrintCurrentState = false;
    DEBUG_MSG(currentState->name);
  }    

  publishIfNeeded();
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

void updateCurrentState(struct DoorState *nextState) {
  DEBUG_MSG(nextState->name);
  if(nextState != currentState) {
    shouldPrintCurrentState = true;
    currentState = nextState;
  }
}

void ICACHE_RAM_ATTR motorChanged() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime > 200) {
    bool value = digitalRead(INPUT_PIN);
    digitalWrite(SECONDARY_LED, value);
    if (value == LOW) { // input is inverted
      updateCurrentState(currentState->motorStarted);
    } else {
      updateCurrentState(currentState->motorStopped);  
    }
  }
  lastInterruptTime = interruptTime;
  
}


void processStateUpdate(const DynamicJsonDocument& payload) {
  if (payload.containsKey(DEVICE_ID)) {
    JsonObjectConst state = payload[DEVICE_ID].as<JsonObject>();
    if (payload.containsKey("on")) {
      if (payload["ok"]) {
        currentState->receivedRequestToOpen();
      } else {
        currentState->receivedRequestToClose();
      }
    }
  }
}
