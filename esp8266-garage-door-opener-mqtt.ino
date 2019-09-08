#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// Copy config.tmpl.h to config.h and replace with your own values
#include "config.h"

#include "certificates.h"

#define SECONDARY_LED D4
#define OUTPUT_PIN D1
#define INPUT_PIN D2


void onMqttMessage(char* topic, byte* payload, unsigned int length);
/************ Global State (you don't need to change this!) ******************/

// Create an ESP8266 WiFiClient class to connect to the MQTT server.
WiFiClientSecure client;
BearSSL::X509List certificates(ca_cert[0]);

// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details.
PubSubClient mqtt(MQTT_SERVER, MQTT_SERVERPORT, onMqttMessage, client);

/*************************** Sketch Code ************************************/
void toggle();
void reverseToggle();

void noop() {};

typedef struct DoorState {
  String name;
  DoorState* motorStarted;
  DoorState* motorStopped;
  void (*receivedRequestToOpen)(void);
  void (*receivedRequestToClose)(void);
  int32_t valueToNotify;
} DoorState;

DoorState DOOR_CLOSING = {
  .name = "CLOSING",
  .motorStarted = &DOOR_CLOSING,
  .motorStopped = &DOOR_CLOSING, // redefined below
  .receivedRequestToOpen = reverseToggle,
  .receivedRequestToClose = noop,
  .valueToNotify = 1
};

DoorState DOOR_WILL_CLOSE_ON_TOGGLE = {
  .name = "WILL_CLOSE_ON_TOGGLE",
  .motorStarted = &DOOR_CLOSING,
  .motorStopped = &DOOR_WILL_CLOSE_ON_TOGGLE,
  .receivedRequestToOpen = noop,
  .receivedRequestToClose = toggle,
  .valueToNotify = 1
};

DoorState DOOR_OPENING = {
  .name = "DOOR_OPENING",
  .motorStarted = &DOOR_OPENING,
  .motorStopped = &DOOR_WILL_CLOSE_ON_TOGGLE,
  .receivedRequestToOpen = noop,
  .receivedRequestToClose = reverseToggle,
  .valueToNotify = 1
};

DoorState DOOR_WILL_OPEN_ON_TOGGLE = {
  .name = "DOOR_WILL_OPEN_ON_TOGGLE",
  .motorStarted = &DOOR_OPENING,
  .motorStopped = &DOOR_WILL_OPEN_ON_TOGGLE,
  .receivedRequestToOpen = toggle,
  .receivedRequestToClose = noop,
  .valueToNotify = 0
};

char lastNotifiedValue = 0;
bool shouldPrintCurrentState = true;
DoorState* currentState = &DOOR_WILL_OPEN_ON_TOGGLE;

void setupWifi();
void setClock();
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

  setupWifi();

  setClock();

  for (int i = 1; i < CA_CERT_COUNT; i++) {
    certificates.append(ca_cert[i]);
  }

  client.setTrustAnchors(&certificates);

}

void reconnect() {
  // Loop until we're reconnected
  bool connectingLed = false;
  while (!mqtt.connected()) {
    connectingLed = !connectingLed;
    digitalWrite(LED_BUILTIN, connectingLed ? LOW : HIGH);
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqtt.connect("DoorOpener", MQTT_USERNAME, MQTT_PASS)) {
      Serial.println("connected");
      mqtt.subscribe(ON_OFF_FEED);
    } else {
      Serial.print("failed, rc=");
      Serial.println(mqtt.state());
      delay(500);
    }
  }
  if (connectingLed) {
    digitalWrite(LED_BUILTIN, HIGH);
  }  
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.print(F("Got: "));
  Serial.write(payload, length);
  Serial.println();
  if (payload[0] == '1') {
    currentState->receivedRequestToOpen();
  }
  if (payload[0] == '0') {
    currentState->receivedRequestToClose();
  }
}

void publishIfNeeded() {

  int32_t nextValue = currentState->valueToNotify;
  
  if (nextValue != lastNotifiedValue) {
    char payload[12];
    ltoa(nextValue, payload, 10);

    Serial.print("Sending ");
    Serial.println(payload);

    if (!mqtt.publish(ON_OFF_SET_FEED, payload)) {
      Serial.println(F("Failed"));
    } else {
      lastNotifiedValue = nextValue;
      Serial.println(F("OK!"));
    }
  }
}

void loop() {

  reconnect();
  mqtt.loop();

  if (shouldPrintCurrentState) {
    shouldPrintCurrentState = false;
    Serial.println(currentState->name);
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
  Serial.println(nextState->name);
  if(nextState != currentState) {
    shouldPrintCurrentState = true;
    currentState = nextState;
  }
}

void ICACHE_RAM_ATTR motorChanged() {
  static unsigned long last_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  if (interrupt_time - last_interrupt_time > 200) {
    bool value = digitalRead(INPUT_PIN);
    digitalWrite(SECONDARY_LED, value);
    if (value == LOW) { // input is inverted
      updateCurrentState(currentState->motorStarted);
    } else {
      updateCurrentState(currentState->motorStopped);  
    }
  }
  last_interrupt_time = interrupt_time;
  
}

void setupWifi() {
    // Connect to WiFi access point.
  Serial.print("Connecting to ");
  Serial.println(WLAN_SSID);

  WiFi.begin(WLAN_SSID, WLAN_PASS);
  //WiFi.scanNetworksAsync(prinScanResult);

  int wifiStatus;
  bool connectingLed = false;

  while ((wifiStatus = WiFi.status()) != WL_CONNECTED) {
    connectingLed = !connectingLed;
    digitalWrite(LED_BUILTIN, connectingLed ? HIGH : LOW);
    delay(500);
    Serial.print(wifiStatus);
    Serial.print(" ");
  }

  Serial.println();

  Serial.println("WiFi connected");
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("IP address: "); 
  Serial.println(WiFi.localIP());

}

// Set time via NTP, as required for x.509 validation
void setClock() {
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("");
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
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
