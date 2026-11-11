const SfxDef& definition(uint8_t sound, bool preview) {
    switch (sound) {
    case 0: return preview ? sfx::kSfxAlarmClassicPreview : sfx::kSfxAlarmClassicRing;
    case 1: return preview ? sfx::kSfxAlarmChimePreview : sfx::kSfxAlarmChimeRing;
    case 2: return preview ? sfx::kSfxAlarmDigitalPreview : sfx::kSfxAlarmDigitalRing;
    default: return preview ? sfx::kSfxAlarmGentlePreview : sfx::kSfxAlarmGentleRing;
    }
}
