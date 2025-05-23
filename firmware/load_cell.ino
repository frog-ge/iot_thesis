#include <WiFi.h>
#include <MQTTClient.h>
#include "HX711.h"
#include <DHT.h> 

// Wi-Fi credentials
#define WIFI_SSID  ""     // Your Wi-Fi SSID
#define WIFI_PASSWORD ""  // Your Wi-Fi Password

// MQTT broker info
const char* mqttHost = "" ;//Your host
int         mqttPort = 80; // Standard MQTT port is 1883, 80 if proxied
const char* mqttUser = "" ;//Your user
const char* mqttPass = "" ;//Your password

// MQTT Topics
const char* mqttTensionTopic     = "esp32/tension";     // Topic to publish weight readings
const char* mqttTemperatureTopic = "esp32/temperature"; // Topic to publish temperature readings
const char* mqttCommandTopic     = "esp32/command";     // Topic to listen for commands (e.g., tare)
const char* mqttStatusTopic      = "esp32/status";      // Topic to publish status messages (e.g., tare confirmation)

// Plain WiFi client (no TLS)
WiFiClient net;
MQTTClient mqttClient(256);

// HX711 setup
#define DOUT  23  // HX711 Data Out pin
#define CLK   17  // HX711 Clock pin
#define CALIBRATION_FACTOR 73060 // This factor needs to be calibrated for your specific setup
HX711 scale;

// DHT11 Sensor Setup
#define DHTPIN 13      // DHT11 data pin connected to GPIO13
#define DHTTYPE DHT11  // We are using a DHT11 sensor
DHT dht(DHTPIN, DHTTYPE);

// Forward declaration for the MQTT message callback
void messageReceived(String &topic, String &payload);

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("--- ESP32 Load Cell & DHT11 (Temperature) with MQTT ---");

  // 1) Connect to Wi-Fi
  Serial.printf("Connecting to WiFi SSID: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int wifiConnectStartTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiConnectStartTime > 20000) { // 20 second timeout
        Serial.println("\n[WiFi] Failed to connect. Please check credentials and signal. Restarting...");
        ESP.restart();
    }
  }
  Serial.println("\n[WiFi] Connected.");
  Serial.print("[WiFi] IP Address: ");
  Serial.println(WiFi.localIP());

  // 2) Initialize HX711
  Serial.println("[HX711] Initializing scale...");
  scale.begin(DOUT, CLK);
  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare(); // Perform initial tare
  Serial.println("[HX711] Scale ready and tared.");
  long zero_factor = scale.read_average();
  Serial.print("[HX711] Initial zero factor (raw reading): ");
  Serial.println(zero_factor);

  // 3) Initialize DHT11 Sensor
  Serial.println("[DHT11] Initializing sensor...");
  dht.begin();
  Serial.println("[DHT11] Sensor initialized for temperature reading.");


  // 4) Setup MQTT client
  Serial.printf("[MQTT] Setting up MQTT client for %s:%d\n", mqttHost, mqttPort);
  mqttClient.begin(mqttHost, mqttPort, net);
  mqttClient.onMessage(messageReceived);

  // 5) Connect to the MQTT broker
  Serial.printf("[MQTT] Connecting to broker as ESP32Client...\n");
  unsigned long mqttConnectStartTime = millis();
  while (!mqttClient.connect("ESP32Client", mqttUser, mqttPass)) {
    Serial.print(".");
    delay(1000);
    if (millis() - mqttConnectStartTime > 20000) {
        Serial.println("\n[MQTT] Failed to connect to broker. Check host, port, credentials. Restarting...");
        ESP.restart();
    }
  }
  Serial.println("\n[MQTT] Connected to broker!");

  // 6) Subscribe to the command topic
  if (mqttClient.subscribe(mqttCommandTopic)) {
    Serial.printf("[MQTT] Subscribed to command topic: %s\n", mqttCommandTopic);
    mqttClient.publish(mqttStatusTopic, "ESP32 online (Load Cell & DHT11 Temp). Listening for commands.");
  } else {
    Serial.printf("[MQTT] Failed to subscribe to command topic: %s\n", mqttCommandTopic);
  }

  Serial.println("\nSetup complete. Entering loop.\n");
}

void messageReceived(String &topic, String &payload) {
  Serial.println("[MQTT] Incoming message...");
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(payload);

  if (topic == mqttCommandTopic) {
    if (payload == "tare") {
      Serial.println("[HX711] Tare command received. Taring scale...");
      scale.tare();
      long new_zero_factor = scale.read_average();
      Serial.print("[HX711] Scale tared. New zero factor (raw reading): ");
      Serial.println(new_zero_factor);
      mqttClient.publish(mqttStatusTopic, "Scale tared successfully.");
    } else {
      Serial.print("[MQTT] Unknown command on command topic: ");
      Serial.println(payload);
      String unknownCmdMsg = "Unknown command received: " + payload;
      mqttClient.publish(mqttStatusTopic, unknownCmdMsg);
    }
  }
}

void loop() {
  bool initialConnectionState = mqttClient.connected();

  // Attempt to reconnect if not connected
  if (!initialConnectionState) {
    Serial.println("[MQTT] Disconnected from broker. Attempting to reconnect...");
    unsigned long mqttReconnectStartTime = millis();
    while (!mqttClient.connect("ESP32Client", mqttUser, mqttPass)) {
        Serial.print(".");
        delay(1000);
        if (millis() - mqttReconnectStartTime > 20000) { // 20 second timeout for reconnect
            Serial.println("\n[MQTT] Failed to reconnect to broker. Will try again later this cycle.");
            break; // Exit the while loop, mqttClient.connected() will still be false
        }
    }
    // If reconnection was successful
    if (mqttClient.connected()) {
        Serial.println("\n[MQTT] Reconnected to broker!");
        // Re-subscribe if necessary
        if (mqttClient.subscribe(mqttCommandTopic)) {
            Serial.printf("[MQTT] Re-subscribed to command topic: %s\n", mqttCommandTopic);
        } else {
            Serial.printf("[MQTT] Failed to re-subscribe to command topic: %s\n", mqttCommandTopic);
        }
    }
  }

  // mqttClient.loop() should be called regularly to process messages and keepalives
  mqttClient.loop();

  // Only proceed with sensor readings and publishing if connected
  if (mqttClient.connected()) {
    // Read from HX711 (Load Cell)
    if (scale.is_ready()) {
      float reading = scale.get_units(5); // 'reading' is local to this block
      Serial.print("[HX711] Weight: ");
      Serial.print(reading, 2);
      Serial.println(" kg");

      String tensionPayload = String(reading, 2);
      if (!mqttClient.publish(mqttTensionTopic, tensionPayload)) {
          Serial.println("[MQTT] Failed to publish tension reading.");
      }
    } else {
      Serial.println("[HX711] Scale not ready.");
    }

    // Read Temperature from DHT11 Sensor
    // 'temperature_c' is now declared inside this block, so no initialization is crossed
    float temperature_c = dht.readTemperature();
    if (isnan(temperature_c)) {
      Serial.println("[DHT11] Failed to read temperature from DHT sensor!");
    } else {
      Serial.print("[DHT11] Temperature: ");
      Serial.print(temperature_c);
      Serial.println(" °C");

      String temperaturePayload = String(temperature_c, 1);
      if (!mqttClient.publish(mqttTemperatureTopic, temperaturePayload)) {
          Serial.println("[MQTT] Failed to publish temperature reading.");
      }
    }
  } else {
    // This message will be printed if it was disconnected at the start of loop()
    // and the reconnection attempt failed.
    if(!initialConnectionState) {
        Serial.println("[MQTT] Still disconnected. Skipping sensor readings and publishing for this cycle.");
    }
  }

  // The delay happens regardless of MQTT connection state
  delay(5000); // Wait 5 seconds before next reading cycle
}