void refreshQuote() {
  s_quoteAvailable = services::morning_quote::getForToday(&s_quote);
  if (s_quoteAvailable) s_quoteRetrying = false;
  markRedraw();
}

void enterQuote() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  s_devQuote=false;
#endif
  s_active = true;
  // Catch up after a powered-off night, even when the next alarm is tomorrow.
  services::morning_quote::requestToday();
  s_quoteRetrying = false;
  refreshQuote();
  s_lastQuoteStatus=services::morning_quote::status();
  s_quoteRetrying=!s_quoteAvailable &&
      (s_lastQuoteStatus==services::morning_quote::Status::Fetching ||
       s_lastQuoteStatus==services::morning_quote::Status::Idle);
}

