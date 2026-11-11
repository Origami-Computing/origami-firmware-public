static const char* soundName(services::automation::AlertSound sound) {
  switch (sound) {
    case services::automation::AlertSound::Classic: return "Daybreak";
    case services::automation::AlertSound::Chime: return "Sunrise";
    case services::automation::AlertSound::Digital: return "Morning Steps";
    case services::automation::AlertSound::Gentle: return "Gentle Bells";
  }
  return "Daybreak";
}

static void previewSound(services::automation::AlertSound sound) {
  switch (sound) {
    case services::automation::AlertSound::Classic:
      sfx::play(sfx::kSfxAlarmClassicPreview);
      break;
    case services::automation::AlertSound::Chime:
      sfx::play(sfx::kSfxAlarmChimePreview);
      break;
    case services::automation::AlertSound::Digital:
      sfx::play(sfx::kSfxAlarmDigitalPreview);
      break;
    case services::automation::AlertSound::Gentle:
      sfx::play(sfx::kSfxAlarmGentlePreview);
      break;
  }
}
