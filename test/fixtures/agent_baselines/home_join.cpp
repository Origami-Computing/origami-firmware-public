    if (!hal::wifi::connected()) {
        Serial.println("[agent] enter(): not connected -> hal::wifi::connect()");
        (void)hal::wifi::connect();
    }
