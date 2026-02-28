#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>

const char* ssid = "Leos IoT 2,4GHz";
const char* password = "leonardwecke";

// Static IP configuration
IPAddress staticIP(192, 168, 1, 50);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// GPIO pins
// DOWN: GPIO4 (D2) - open-drain: OUTPUT+LOW = ON, INPUT = OFF
// UP:   GPIO5 (D1) - open-drain: OUTPUT+LOW = ON, INPUT = OFF
const int pinVolumeDown = 4;
const int pinVolumeUp   = 5;
const int pinLed        = 2;

const int PRESS_DURATION_MS = 100;
const int PRESS_PAUSE_MS    = 100;
const int MAX_VOLUME        = 15;
const int DEFAULT_VOLUME    = 9;
const int TEST_HOLD_MS      = 2000;

int currentVolume = 0;

// Open-drain: LOW = switch closed, INPUT = switch open (high-Z)
void pinOn(int pin)  { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
void pinOff(int pin) { pinMode(pin, INPUT); }

// --- Non-blocking state machine ---

enum PressState { IDLE, PIN_ON, PIN_OFF, TEST_DOWN_ON, TEST_DOWN_OFF, TEST_UP_ON, TEST_UP_OFF };

struct PressJob {
    int pin;
    int remaining;       // how many presses left
    int targetVolume;    // volume after all presses
    bool isUp;           // true=up, false=down
    AsyncClient* client; // respond to this client when done (may be null)
};

PressState pressState = IDLE;
PressJob   activeJob  = {0, 0, 0, false, nullptr};
unsigned long pressTimer = 0;

void startJob(int pin, int count, int target, bool up, AsyncClient* client) {
    if (count <= 0) {
        // Nothing to do — send response immediately if client alive
        if (client && client->connected() && client->canSend()) {
            String r = "OK volume=" + String(target) + "\n";
            client->write(r.c_str(), r.length());
        }
        currentVolume = target;
        return;
    }
    activeJob = {pin, count, target, up, client};
    pressState = PIN_ON;
    pinOn(pin);
    pressTimer = millis();
}

void tickStateMachine() {
    if (pressState == IDLE) return;

    unsigned long now = millis();

    if (pressState == PIN_ON) {
        if (now - pressTimer >= PRESS_DURATION_MS) {
            pinOff(activeJob.pin);
            pressState = PIN_OFF;
            pressTimer = now;
        }
    } else if (pressState == PIN_OFF) {
        if (now - pressTimer >= PRESS_PAUSE_MS) {
            activeJob.remaining--;

            if (activeJob.remaining > 0) {
                pinOn(activeJob.pin);
                pressState = PIN_ON;
                pressTimer = now;
            } else {
                currentVolume = activeJob.targetVolume;
                digitalWrite(pinLed, HIGH);
                Serial.printf("Job done. volume=%d\n", currentVolume);

                if (activeJob.client && activeJob.client->connected() && activeJob.client->canSend()) {
                    String r = "OK volume=" + String(currentVolume) + "\n";
                    activeJob.client->write(r.c_str(), r.length());
                }
                activeJob.client = nullptr;
                pressState = IDLE;
            }
        }
    }
    // TEST mode: 2s DOWN, pause, 2s UP
    else if (pressState == TEST_DOWN_ON) {
        if (now - pressTimer >= TEST_HOLD_MS) {
            pinOff(pinVolumeDown);
            pressState = TEST_DOWN_OFF;
            pressTimer = now;
            Serial.println("TEST: DOWN released, pausing...");
        }
    }
    else if (pressState == TEST_DOWN_OFF) {
        if (now - pressTimer >= 500) {
            pinOn(pinVolumeUp);
            pressState = TEST_UP_ON;
            pressTimer = now;
            Serial.println("TEST: UP active...");
        }
    }
    else if (pressState == TEST_UP_ON) {
        if (now - pressTimer >= TEST_HOLD_MS) {
            pinOff(pinVolumeUp);
            pressState = TEST_UP_OFF;
            pressTimer = now;
            Serial.println("TEST: UP released");
        }
    }
    else if (pressState == TEST_UP_OFF) {
        if (now - pressTimer >= 200) {
            if (activeJob.client && activeJob.client->connected() && activeJob.client->canSend()) {
                activeJob.client->write("OK test done\n", 13);
            }
            activeJob.client = nullptr;
            pressState = IDLE;
            Serial.println("TEST done.");
        }
    }
}

// --- SYNC sequence: DOWN phase then UP phase ---

enum SyncPhase { SYNC_NONE, SYNC_DOWN, SYNC_UP };
SyncPhase syncPhase = SYNC_NONE;
int syncTarget = 0;
AsyncClient* syncClient = nullptr;

void startSync(int target, AsyncClient* client) {
    if (pressState != IDLE || syncPhase != SYNC_NONE) return; // busy
    syncTarget = target;
    syncClient = client;
    syncPhase = SYNC_DOWN;
    digitalWrite(pinLed, LOW);
    Serial.printf("SYNC start -> target=%d\n", target);
    startJob(pinVolumeDown, MAX_VOLUME + 1, 0, false, nullptr);
}

void tickSync() {
    if (syncPhase == SYNC_NONE) return;
    if (pressState != IDLE) return; // still pressing

    if (syncPhase == SYNC_DOWN) {
        // DOWN phase complete, now go UP
        syncPhase = SYNC_UP;
        // +1 extra UP press: first press after full-down is consistently ignored by hardware
        int upPresses = syncTarget > 0 ? syncTarget + 1 : 0;
        startJob(pinVolumeUp, upPresses, syncTarget, true, syncClient);
        syncClient = nullptr;
        syncPhase = SYNC_NONE;
    }
}

bool isBusy() {
    return pressState != IDLE || syncPhase != SYNC_NONE;
}

// --- TCP command handling ---

void handleCommand(const String& cmd, AsyncClient* client) {
    if (isBusy()) {
        if (client->connected() && client->canSend())
            client->write("ERR busy\n", 9);
        return;
    }

    if (cmd == "UP") {
        if (currentVolume >= MAX_VOLUME) {
            if (client->connected() && client->canSend())
                client->write("ERR already at max (15)\n", 23);
            return;
        }
        startJob(pinVolumeUp, 1, currentVolume + 1, true, client);
    }
    else if (cmd == "DOWN") {
        if (currentVolume <= 0) {
            if (client->connected() && client->canSend())
                client->write("ERR already at min (0)\n", 22);
            return;
        }
        startJob(pinVolumeDown, 1, currentVolume - 1, false, client);
    }
    else if (cmd.startsWith("SET:")) {
        int target = cmd.substring(4).toInt();
        if (target < 0 || target > MAX_VOLUME) {
            if (client->connected() && client->canSend())
                client->write("ERR range 0-15\n", 15);
            return;
        }
        startSync(target, client);
    }
    else if (cmd == "SYNC") {
        startSync(DEFAULT_VOLUME, client);
    }
    else if (cmd == "GET") {
        String r = "OK volume=" + String(currentVolume) + "\n";
        if (client->connected() && client->canSend())
            client->write(r.c_str(), r.length());
    }
    else if (cmd == "TEST") {
        // 2s DOWN pin active, 500ms pause, 2s UP pin active
        Serial.println("TEST: DOWN active...");
        activeJob.client = client;
        pinOn(pinVolumeDown);
        pressState = TEST_DOWN_ON;
        pressTimer = millis();
    }
    else {
        if (client->connected() && client->canSend())
            client->write("ERR unknown command\n", 20);
    }
}

AsyncServer tcpServer(42069);

void setup() {
    Serial.begin(115200);

    pinMode(pinVolumeDown, INPUT);  // off = high-Z
    pinMode(pinVolumeUp, INPUT);    // off = high-Z
    pinMode(pinLed, OUTPUT);
    digitalWrite(pinLed, HIGH);

    WiFi.config(staticIP, gateway, subnet);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.println("IP: " + WiFi.localIP().toString());

    // Boot SYNC (blocking here is fine — no clients connected yet)
    Serial.println("Boot SYNC...");
    for (int i = 0; i < MAX_VOLUME + 1; i++) {
        pinOn(pinVolumeDown);
        delay(PRESS_DURATION_MS);
        pinOff(pinVolumeDown);
        delay(PRESS_PAUSE_MS);
    }
    delay(500);
    for (int i = 0; i < DEFAULT_VOLUME + 1; i++) {
        pinOn(pinVolumeUp);
        delay(PRESS_DURATION_MS);
        pinOff(pinVolumeUp);
        delay(PRESS_PAUSE_MS);
    }
    currentVolume = DEFAULT_VOLUME;
    Serial.printf("Boot SYNC done. volume=%d\n", currentVolume);

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
            // Clear client ref if it's our active one
            if (activeJob.client == c) activeJob.client = nullptr;
            if (syncClient == c) syncClient = nullptr;
            delete c;
        }, nullptr);

    }, nullptr);

    tcpServer.begin();
    Serial.println("TCP server ready on port 42069");
}

void loop() {
    tickStateMachine();
    tickSync();
}
