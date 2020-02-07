#include "certificates.h"

#include <ESP8266WiFi.h>

#include <ESP8266mDNS.h>

#include <WebSocketsClient.h>
#include <SocketIOclient.h>

#include <ArduinoJson.h>


class SSLSocketIOclient : public SocketIOclient {
  public:
    void beginSocketIOSSL(const char * host, uint16_t port, const char * url, const char* CAcert) {
      WebSocketsClient::beginSocketIOSSLWithCA(host, port, url, CAcert);
    }

};

class ConnectionManager {

  protected:
    SSLSocketIOclient &client;
    bool connectingLed = false;
    bool hasEverConnected = false;
    bool configuringWifi = false;
    unsigned long lastLoopMillis = 0;
    unsigned long nextDelayMillis = 500;
    
  public:
    ConnectionManager(SSLSocketIOclient &client) :
      client(client) {}

    void setup(String hostname) {
      // Connect to WiFi access point.

      WiFi.mode(WIFI_STA);
      WiFi.hostname(hostname);

#ifndef WLAN_SSID
      if (WiFi.SSID() != "") {
        DEBUG_MSG("Connecting to ");
        DEBUG_MSG(WiFi.SSID());
        WiFi.begin();
      } else {
        WiFi.beginSmartConfig();
        configuringWifi = true;
        nextDelayMillis = 200;
      }
#else 
      WiFi.begin(WLAN_SSID, WLAN_PASS);
#endif

      //WiFi.scanNetworksAsync(prinScanResult);
    
      configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");    
      
      client.beginSocketIOSSL(NORA_HOST, NORA_PORT, NORA_URL, ca_cert[0]);

      client.onEvent([this](socketIOmessageType_t type, uint8_t * payload, size_t length) {
        onEvent(type, payload, length);
      });
        
    }

    void reconnect() {

      unsigned long currentTimeMillis = millis();
    
      // "delay"
      if (!client.isConnected()) {
        if (currentTimeMillis < (lastLoopMillis + nextDelayMillis)) {
          return;
        }
      }  

      lastLoopMillis = currentTimeMillis;
      int wifiStatus;

      if (configuringWifi) {
        if(!WiFi.smartConfigDone()) {
          connectingLed = !connectingLed;
          digitalWrite(LED_BUILTIN, connectingLed ? HIGH : LOW);
          DEBUG_MSG_(".");
          return;
        }
        configuringWifi = false;
        WiFi.stopSmartConfig();
        nextDelayMillis = 500;
      }
    
      if ((wifiStatus = WiFi.status()) != WL_CONNECTED) {
        connectingLed = !connectingLed;
        digitalWrite(LED_BUILTIN, connectingLed ? HIGH : LOW);
        hasEverConnected = false;
        DEBUG_MSG_("Wifi status: ");
        DEBUG_MSG(wifiStatus);
        return;
      }

      if (!hasEverConnected) {
        hasEverConnected = true;
        DEBUG_MSG("WiFi connected");
        digitalWrite(LED_BUILTIN, HIGH);
        DEBUG_MSG("IP address: "); 
        DEBUG_MSG(WiFi.localIP());

        if(!MDNS.begin(WiFi.hostname())) {
          DEBUG_MSG("can't init MDNS");
        }

        MDNS.enableArduino(8266, true);
        //MDNS.addService("http", "tcp", 80);
      }

      client.loop();

      if (!client.isConnected()) {
        connectingLed = !connectingLed;
        digitalWrite(LED_BUILTIN, connectingLed ? LOW : HIGH);
      } else {
        MDNS.update();      
      }

    }

    void onEvent(socketIOmessageType_t type, uint8_t * payload, size_t length) {
        switch(type) {
            case sIOtype_DISCONNECT:
                DEBUG_MSGF("[IOc] Disconnected!\n");
                break;
            case sIOtype_CONNECT:
                DEBUG_MSGF("[IOc] Connected");
                this->onConnect();
                break;
            case sIOtype_EVENT:
                handleStringEvent((char*)payload, length);
                break;
            case sIOtype_ACK:
                DEBUG_MSGF("[IOc] get ack: %u\n", length);
                hexdump(payload, length);
                break;
            case sIOtype_ERROR:
                DEBUG_MSGF("[IOc] get error: %u\n", length);
                DEBUG_WRITE(payload, length);
                DEBUG_MSG();
                break;
            case sIOtype_BINARY_EVENT:
                DEBUG_MSGF("[IOc] get binary: %u\n", length);
                handleBinaryEvent(payload, length);
                break;
            case sIOtype_BINARY_ACK:
                DEBUG_MSGF("[IOc] get binary ack: %u\n", length);
                hexdump(payload, length);
                break;
        }
    }
    
    
    void handleStringEvent(char* payload, size_t length) {
    
      char * sptr = NULL;
      int id = strtol(payload, &sptr, 10);
      DEBUG_MSGF("[IOc] get event: %s id: %d\n", payload, id);
      if(id) {
          payload = sptr;
      }
      DynamicJsonDocument doc(JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(20));
      DeserializationError error = deserializeJson(doc, payload, length);
    
      if(error) {
          DEBUG_MSG(F("deserializeJson() failed: "));
          DEBUG_MSG(error.c_str());
          return;
      }
      
      String eventName = doc[0];
      DEBUG_MSGF("[IOc] event name: %s\n", eventName.c_str());
    
      processJsonEvent(eventName, doc);
    
      // Message Includes a ID for a ACK (callback)
      if(id) {
          // creat JSON message for Socket.IO (ack)
          DynamicJsonDocument docOut(1024);
          JsonArray array = docOut.to<JsonArray>();
          
          // add payload (parameters) for the ack (callback function)
          JsonObject param1 = array.createNestedObject();
          param1["now"] = millis();
    
          // JSON to String (serializion)
          String output;
          output += id;
          serializeJson(docOut, output);
    
          // Send event        
          /*if(!socketIO.send(sIOtype_ACK, output)) {
            DEBUG_MSG("failed to ack");
          }*/
      }
    
    }

    virtual void handleBinaryEvent(uint8_t * payload, size_t length) {
      hexdump(payload, length);
    }
    
    virtual void processJsonEvent(String& eventName, const DynamicJsonDocument& payload) {}
    
    virtual void onConnect() {}

    

};

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
