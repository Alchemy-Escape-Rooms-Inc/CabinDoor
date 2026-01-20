/*
 * ============================================================
 * Cabin Door Controller
 * Alchemy Escape Rooms - A Mermaid's Tale
 * ============================================================
 * 
 * Platform: ESP32-S3
 * Hardware: 2-Channel Relay Module + Electric Piston
 *   - Relay 1: Extend piston (open door)
 *   - Relay 2: Retract piston (close door)
 * 
 * MQTT Topics:
 *   Subscribe: MermaidsTale/CabinDoor/command
 *   Publish:   MermaidsTale/CabinDoor/command  (PONG, state responses)
 *              MermaidsTale/CabinDoor/status   (ONLINE, state changes)
 *              MermaidsTale/CabinDoor/log      (debug output)
 * 
 * Commands:
 *   PING         -> PONG (health check)
 *   STATUS       -> Current state
 *   RESET        -> Reboot device
 *   PUZZLE_RESET -> Reset to closed state
 *   OPEN         -> Extend piston for 8 seconds
 *   CLOSE        -> Retract piston for 8 seconds
 *   STOP         -> Stop piston immediately
 * 
 * Version: 1.0.0
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <stdarg.h>

// ============================================================
// CONFIGURATION
// ============================================================

#define VERSION "1.0.0"

#define GAME_NAME "MermaidsTale"
#define PROP_NAME "CabinDoor"

#define MQTT_TOPIC_COMMAND "MermaidsTale/CabinDoor/command"
#define MQTT_TOPIC_STATUS  "MermaidsTale/CabinDoor/status"
#define MQTT_TOPIC_LOG     "MermaidsTale/CabinDoor/log"

// WiFi credentials
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// MQTT broker
const char* MQTT_SERVER = "10.1.10.115";
const int MQTT_PORT = 1883;

// Relay pins - ESP32-S3 safe GPIO pins
// Avoid: 0, 3, 19, 20, 45, 46 (strapping/USB/JTAG)
const int PIN_RELAY_EXTEND  = 4;   // Relay 1: Extend piston (open)
const int PIN_RELAY_RETRACT = 5;   // Relay 2: Retract piston (close)

// Relay logic - most modules are ACTIVE LOW
const bool RELAY_ACTIVE_LOW = true;

// Timing
const unsigned long PISTON_RUN_TIME_MS  = 8000;   // 8 seconds
const unsigned long RELAY_SWITCH_DELAY  = 100;    // Safety delay between relay switches

// ============================================================
// STATE
// ============================================================

enum DoorState {
    DOOR_STOPPED,
    DOOR_OPENING,
    DOOR_CLOSING
};

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

DoorState currentState = DOOR_STOPPED;
unsigned long actionStartTime = 0;
bool pistonRunning = false;

// ============================================================
// RELAY HELPERS
// ============================================================

void relayOn(int pin) {
    digitalWrite(pin, RELAY_ACTIVE_LOW ? LOW : HIGH);
}

void relayOff(int pin) {
    digitalWrite(pin, RELAY_ACTIVE_LOW ? HIGH : LOW);
}

void allRelaysOff() {
    relayOff(PIN_RELAY_EXTEND);
    relayOff(PIN_RELAY_RETRACT);
    pistonRunning = false;
    Serial.println("[PISTON] All relays OFF");
}

// ============================================================
// MQTT LOGGING HELPER
// ============================================================

void mqttLogf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    mqttClient.publish(MQTT_TOPIC_LOG, buffer);
    Serial.println(buffer);
}

// ============================================================
// STATE HELPERS
// ============================================================

const char* stateToString(DoorState state) {
    switch (state) {
        case DOOR_STOPPED:  return "STOPPED";
        case DOOR_OPENING:  return "OPENING";
        case DOOR_CLOSING:  return "CLOSING";
        default:            return "UNKNOWN";
    }
}

void publishState() {
    mqttClient.publish(MQTT_TOPIC_STATUS, stateToString(currentState));
}

// ============================================================
// PISTON CONTROL
// ============================================================

void extendPiston() {
    // Safety: Turn off retract relay first
    relayOff(PIN_RELAY_RETRACT);
    delay(RELAY_SWITCH_DELAY);

    // Activate extend relay
    relayOn(PIN_RELAY_EXTEND);

    currentState = DOOR_OPENING;
    pistonRunning = true;
    actionStartTime = millis();

    Serial.println("[PISTON] Extending (OPEN)");
    mqttLogf("Piston extending - door opening");
    publishState();
}

void retractPiston() {
    // Safety: Turn off extend relay first
    relayOff(PIN_RELAY_EXTEND);
    delay(RELAY_SWITCH_DELAY);

    // Activate retract relay
    relayOn(PIN_RELAY_RETRACT);

    currentState = DOOR_CLOSING;
    pistonRunning = true;
    actionStartTime = millis();

    Serial.println("[PISTON] Retracting (CLOSE)");
    mqttLogf("Piston retracting - door closing");
    publishState();
}

void stopPiston() {
    allRelaysOff();
    currentState = DOOR_STOPPED;
    Serial.println("[PISTON] Stopped");
    mqttLogf("Piston stopped");
    publishState();
}

// ============================================================
// MQTT CALLBACK - WATCHTOWER COMPLIANT
// ============================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // ============================================================
    // CRITICAL: Copy topic to local buffer FIRST
    // This prevents stack corruption from overwriting the topic pointer
    // ============================================================
    char topicBuf[128];
    strncpy(topicBuf, topic, sizeof(topicBuf) - 1);
    topicBuf[sizeof(topicBuf) - 1] = '\0';

    // Now safe to declare other variables
    char message[128];
    if (length >= sizeof(message)) {
        length = sizeof(message) - 1;
    }
    memcpy(message, payload, length);
    message[length] = '\0';

    // Trim whitespace
    char* msg = message;
    while (*msg == ' ' || *msg == '\t' || *msg == '\r' || *msg == '\n') msg++;
    char* end = msg + strlen(msg) - 1;
    while (end > msg && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }

    // Log received command
    Serial.printf("[MQTT] Received on %s: %s\n", topicBuf, msg);

    // Only process commands on our command topic
    if (strcmp(topicBuf, MQTT_TOPIC_COMMAND) != 0) {
        return;
    }

    // ============================================================
    // REQUIRED COMMANDS - All props must implement these
    // ============================================================

    // PING - Health check for System Checker
    if (strcmp(msg, "PING") == 0) {
        mqttClient.publish(MQTT_TOPIC_COMMAND, "PONG");
        Serial.println("[MQTT] PING -> PONG");
        return;
    }

    // STATUS - Report current state
    if (strcmp(msg, "STATUS") == 0) {
        mqttClient.publish(MQTT_TOPIC_COMMAND, stateToString(currentState));
        Serial.printf("[MQTT] STATUS -> %s\n", stateToString(currentState));
        return;
    }

    // RESET - Reboot the device
    if (strcmp(msg, "RESET") == 0) {
        allRelaysOff();  // Safety: stop piston before reboot
        mqttClient.publish(MQTT_TOPIC_COMMAND, "OK");
        Serial.println("[MQTT] RESET -> Rebooting...");
        delay(100);
        ESP.restart();
        return;
    }

    // PUZZLE_RESET - Reset to closed state without rebooting
    if (strcmp(msg, "PUZZLE_RESET") == 0) {
        stopPiston();
        mqttClient.publish(MQTT_TOPIC_COMMAND, "OK");
        Serial.println("[MQTT] PUZZLE_RESET -> OK");
        return;
    }

    // ============================================================
    // PROP-SPECIFIC COMMANDS
    // ============================================================

    // OPEN - Extend piston to open door
    if (strcmp(msg, "OPEN") == 0) {
        extendPiston();
        mqttClient.publish(MQTT_TOPIC_COMMAND, "OPENING");
        return;
    }

    // CLOSE - Retract piston to close door
    if (strcmp(msg, "CLOSE") == 0) {
        retractPiston();
        mqttClient.publish(MQTT_TOPIC_COMMAND, "CLOSING");
        return;
    }

    // STOP - Stop piston immediately
    if (strcmp(msg, "STOP") == 0) {
        stopPiston();
        mqttClient.publish(MQTT_TOPIC_COMMAND, "STOPPED");
        return;
    }

    // Unknown command
    Serial.printf("[MQTT] Unknown command: %s\n", msg);
}

// ============================================================
// WIFI CONNECTION
// ============================================================

void connectWiFi() {
    Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println();
        Serial.println("[WIFI] Connection failed. Rebooting...");
        delay(1000);
        ESP.restart();
    }
}

// ============================================================
// MQTT CONNECTION
// ============================================================

void connectMQTT() {
    while (!mqttClient.connected()) {
        Serial.print("[MQTT] Connecting...");

        String clientId = PROP_NAME;
        clientId += "_";
        clientId += String(random(0xffff), HEX);

        if (mqttClient.connect(clientId.c_str())) {
            Serial.println("connected!");

            // Subscribe to command topic
            mqttClient.subscribe(MQTT_TOPIC_COMMAND);

            // Announce we're online
            mqttClient.publish(MQTT_TOPIC_STATUS, "ONLINE");
            mqttLogf("%s v%s online - IP: %s", PROP_NAME, VERSION, WiFi.localIP().toString().c_str());

        } else {
            Serial.printf("failed (rc=%d), retrying in 5s\n", mqttClient.state());
            delay(5000);
        }
    }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    // ESP32-S3 USB CDC serial
    Serial.begin(115200);
    
    // Wait for serial on ESP32-S3 (USB CDC can take a moment)
    unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart < 3000)) {
        delay(10);
    }
    delay(500);

    Serial.println();
    Serial.println("================================");
    Serial.printf("%s v%s\n", PROP_NAME, VERSION);
    Serial.println("Platform: ESP32-S3");
    Serial.println("Alchemy Escape Rooms");
    Serial.println("Watchtower Protocol");
    Serial.println("================================");

    // Initialize relay pins - OFF first for safety
    pinMode(PIN_RELAY_EXTEND, OUTPUT);
    pinMode(PIN_RELAY_RETRACT, OUTPUT);
    allRelaysOff();

    Serial.printf("Relay pins: EXTEND=%d, RETRACT=%d\n", PIN_RELAY_EXTEND, PIN_RELAY_RETRACT);
    Serial.printf("Relay logic: %s\n", RELAY_ACTIVE_LOW ? "ACTIVE LOW" : "ACTIVE HIGH");
    Serial.printf("Piston run time: %lu ms\n", PISTON_RUN_TIME_MS);

    // Connect to network
    connectWiFi();

    // Setup MQTT
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);

    connectMQTT();

    Serial.println("Setup complete. Listening for commands...");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    // Maintain MQTT connection
    if (!mqttClient.connected()) {
        allRelaysOff();  // Safety: stop piston on disconnect
        connectMQTT();
    }
    mqttClient.loop();

    // Check piston timeout
    if (pistonRunning && (millis() - actionStartTime >= PISTON_RUN_TIME_MS)) {
        Serial.println("[INFO] Piston timeout reached. Stopping.");

        const char* completedState = (currentState == DOOR_OPENING) ? "OPEN_COMPLETE" : "CLOSE_COMPLETE";
        allRelaysOff();
        currentState = DOOR_STOPPED;

        mqttClient.publish(MQTT_TOPIC_STATUS, completedState);
        mqttLogf("Piston %s", completedState);
    }
}
