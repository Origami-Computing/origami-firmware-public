static const SfxDef& ringDef(services::automation::AlertSound sound) {
  switch (sound) {
    case services::automation::AlertSound::Classic: return sfx::kSfxAlarmClassicRing;
    case services::automation::AlertSound::Chime: return sfx::kSfxAlarmChimeRing;
    case services::automation::AlertSound::Digital: return sfx::kSfxAlarmDigitalRing;
    case services::automation::AlertSound::Gentle: return sfx::kSfxAlarmGentleRing;
  }
  return sfx::kSfxAlarmClassicRing;
}

static uint16_t ringGapMs(services::automation::AlertSound sound) {
  switch (sound) {
    case services::automation::AlertSound::Classic: return 1050;
    case services::automation::AlertSound::Chime: return 1150;
    case services::automation::AlertSound::Digital: return 900;
    case services::automation::AlertSound::Gentle: return 1300;
  }
  return 1050;
}
