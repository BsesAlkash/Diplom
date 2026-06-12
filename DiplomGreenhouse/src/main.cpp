#include <Arduino.h>
#include "Greenhouse.h"

Greenhouse greenhouse;

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== MAIN STARTED ===");

    greenhouse.begin();

    Serial.println("=== GREENHOUSE INIT FINISHED ===");
}

void loop() {
    greenhouse.update();
}