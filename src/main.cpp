#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>

const char* ssid = "Leos IoT 2,4GHz";
const char* password = "leonardwecke";

// Static IP configuration
IPAddress staticIP(192, 168, 1, 50);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// GPIO pins - inverted logic (LOW = transistor ON)
const int pinVolumeDown = 4;  // GPIO4 - D2
const int pinVolumeUp   = 5;  // GPIO5 - D1
const int pinLed        = 2;  // GPIO2 - onboard LED

const int PRESS_DURATION_MS = 50;   // how long the button is "held"
const int PRESS_PAUSE_MS    = 100;  // pause between presses
const int MAX_VOLUME        = 15;
const int DEFAULT_VOLUME    = 9;

int currentVolume = 0;

AsyncServer tcpServer(42069);

// --- Button press simulation ---

void pressButton(int pin) {
    digitalWrite(pin, LOW);   // ON (inverted)
    delay(PRESS_DURATION_MS);
    digitalWrite(pin, HIGH);  // OFF
    delay(PRESS_PAUSE_MS);
}

void volumeDownOnce() {
    pressButton(pinVolumeDown);
    if (currentVolume > 0) currentVolume--;
    Serial.printf("DOWN -> volume=%d\n", currentVolume);
}

void volumeUpOnce() {
    pressButton(pinVolumeUp);
    if (currentVolume < MAX_VOLUME) currentVolume++;
    Serial.printf("UP -> volume=%d\n", currentVolume);
}

// SYNC: 15x down to guarantee level 0, then go to target
void syncToVolume(int target) {
    if (target < 0) target = 0;
    if (target > MAX_VOLUME) target = MAX_VOLUME;

    Serial.printf("SYNC: resetting to 0 (15x DOWN)...\n");
    digitalWrite(pinLed, LOW);  // LED on during sync

    for (int i = 0; i < MAX_VOLUME; i++) {
        pressButton(pinVolumeDown);
    }
    currentVolume = 0;

    Serial.printf("SYNC: going to %d (%dx UP)...\n", target, target);
    for (int i = 0; i < target; i++) {
        pressButton(pinVolumeUp);
    }
    currentVolume = target;

    digitalWrite(pinLed, HIGH);  // LED off
    Serial.printf("SYNC done. volume=%d\n", currentVolume);
}

// --- TCP command handling ---

void handleCommand(const String& cmd, AsyncClient* client) {
    String response;

    if (cmd == "UP") {
        if (currentVolume < MAX_VOLUME) {
            volumeUpOnce();
            response = "OK volume=" + String(currentVolume) + "\n";
        } else {
            response = "ERR already at max (15)\n";
        }
    }
    else if (cmd == "DOWN") {
        if (currentVolume > 0) {
            volumeDownOnce();
            response = "OK volume=" + String(currentVolume) + "\n";
        } else {
            response = "ERR already at min (0)\n";
        }
    }
    else if (cmd.startsWith("SET:")) {
        int target = cmd.substring(4).toInt();
        if (target < 0 || target > MAX_VOLUME) {
            response = "ERR range 0-15\n";
        } else {
            syncToVolume(target);
            response = "OK volume=" + String(currentVolume) + "\n";
        }
    }
    else if (cmd == "SYNC") {
        syncToVolume(DEFAULT_VOLUME);
        response = "OK synced to " + String(DEFAULT_VOLUME) + "\n";
    }
    else if (cmd == "GET") {
        response = "OK volume=" + String(currentVolume) + "\n";
    }
    else {
        response = "ERR unknown command. USE: UP, DOWN, SET:0-15, SYNC, GET\n";
    }

    if (client->canSend()) {
        client->write(response.c_str(), response.length());
    }
    Serial.print("-> " + response);
}

// --- Setup & Loop ---

void setup() {
    Serial.begin(115200);

    pinMode(pinVolumeDown, OUTPUT);
    pinMode(pinVolumeUp, OUTPUT);
    pinMode(pinLed, OUTPUT);

    // All OFF at startup
    digitalWrite(pinVolumeDown, HIGH);
    digitalWrite(pinVolumeUp, HIGH);
    digitalWrite(pinLed, HIGH);

    // Connect WiFi with static IP
    WiFi.config(staticIP, gateway, subnet);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // SYNC on boot -> default volume
    Serial.println("Boot SYNC...");
    syncToVolume(DEFAULT_VOLUME);

    // TCP server
    tcpServer.onClient([](void* arg, AsyncClient* client) {
        Serial.println("Client connected");

        client->onData([](void* arg, AsyncClient* c, void* data, size_t len) {
            String cmd = String((char*)data).substring(0, len);
            cmd.trim();
            Serial.printf("CMD: '%s'\n", cmd.c_str());
            handleCommand(cmd, c);
        }, nullptr);

        client->onDisconnect([](void* arg, AsyncClient* c) {
            Serial.println("Client disconnected");
            delete c;
        }, nullptr);

    }, nullptr);

    tcpServer.begin();
    Serial.println("TCP server on port 42069 ready");
    Serial.printf("Volume: %d/%d\n", currentVolume, MAX_VOLUME);
}

void loop() {
    // nothing needed - async TCP handles everything
}
