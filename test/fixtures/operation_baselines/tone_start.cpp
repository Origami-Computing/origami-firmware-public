        hal::audio::hardwareReset(); delay(50);
        hal::audio::initCodec(0x18);
        hal::audio::configureForPlayback();
        hal::audio::txBegin(44100);
        s_toneActive = true; s_tonePhase = 0.0f;