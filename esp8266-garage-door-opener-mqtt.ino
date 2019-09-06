#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// Copy config.tmpl.h to config.h and replace with your own values
#include "config.h"

#include "certificates.h"

#define SECONDARY_LED D4
#define OUTPUT_PIN D1
#define INPUT_PIN D2



/************ Global State (you don't need to change this!) ******************/

// Create an ESP8266 WiFiClient class to connect to the MQTT server.
WiFiClientSecure client;
BearSSL::X509List certificates(ca_cert[0]);

// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details.
Adafruit_MQTT_Client mqtt(&client, MQTT_SERVER, MQTT_SERVERPORT, MQTT_USERNAME, MQTT_PASS);

/****************************** Feeds ***************************************/

Adafruit_MQTT_Publish onOffSetFeed = Adafruit_MQTT_Publish(&mqtt, ON_OFF_SET_FEED);
Adafruit_MQTT_Subscribe onOffFeed = Adafruit_MQTT_Subscribe(&mqtt, ON_OFF_FEED);

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
  char valueToNotify;
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

void setClock();
void MQTT_connect();
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
  Serial.println("IP address: "); Serial.println(WiFi.localIP());

  setClock();

  for (int i = 1; i < CA_CERT_COUNT; i++) {
    certificates.append(ca_cert[i]);
  }

  client.setTrustAnchors(&certificates);

  // Setup MQTT subscription for onoff feed.
  mqtt.subscribe(&onOffFeed);
}

uint32_t x = 0;

void loop() {
  // Ensure the connection to the MQTT server is alive (this will make the first
  // connection and automatically reconnect when disconnected).  See the MQTT_connect
  // function definition further below.
  MQTT_connect();

  if (shouldPrintCurrentState) {
    shouldPrintCurrentState = false;
    Serial.println(currentState->name);
  }    

  // this is our 'wait for incoming subscription packets' busy subloop
  // try to spend your time here

  Adafruit_MQTT_Subscribe *subscription;
  while ((subscription = mqtt.readSubscription(3000))) {
    if (subscription == &onOffFeed) {
      Serial.print(F("Got: "));
      Serial.println((char *)onOffFeed.lastread);
      if (strcmp((char *)onOffFeed.lastread, "1") == 0) {
        currentState->receivedRequestToOpen();
      }
      if (strcmp((char *)onOffFeed.lastread, "0") == 0) {
        currentState->receivedRequestToClose();
      }
    }
  }

    

  char nextValue = currentState->valueToNotify;
  // Now we can publish stuff!
  if (nextValue != lastNotifiedValue) {
    Serial.print(F("\nSending state val "));
    Serial.print(nextValue, BIN);
    Serial.print("...");
    if (!onOffSetFeed.publish(nextValue)) {
      Serial.println(F("Failed"));
    } else {
      lastNotifiedValue = nextValue;
      Serial.println(F("OK!"));
    }
  }

  if (!mqtt.ping()) {
    mqtt.disconnect();
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
    char err_buf[256];
    client.getLastSSLError(err_buf, 256);
    Serial.println(err_buf);
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
