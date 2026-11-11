void listRow(TFT_eSPI& c, int x, int y, int w, int h,
             const char* text, bool selected,
             uint16_t selBg, uint16_t selFg, uint16_t bg, uint16_t fg,
             bool choice, bool checked, uint16_t indicatorFg, bool opensScreen) {
    if (safe_area::applies(c)) {
        const auto row = safe_area::fitBox({x,y,w,h});
        x=row.x; y=row.y; w=row.w; h=row.h;
    }
    uint16_t rowBg = selected ? selBg : bg;
    uint16_t rowFg = selected ? selFg : fg;
    c.fillRect(x, y, w, h, rowBg);
    c.setTextFont(1);
    c.setTextDatum(TL_DATUM);
    c.setTextSize(2);
    c.setTextColor(rowFg, rowBg);

    // font 1 × size 2 = 12 px per character, 16 px tall.
    static constexpr int kCharW   = 12;
    static constexpr int kIndW    = layout::kListIndicatorW;  // indicator zone width (for '>' or '●')
    static constexpr int kIndPad  = layout::kListIndicatorInset;   // gap between indicator and right edge of row
    static constexpr int kTextL   = layout::kListTextInset;   // left text indent
    static constexpr int kGap     = layout::kListTextGap;   // gap between text and indicator zone

    const int textY    = y + (h - 16) / 2;
    const int indRight = x + w - kIndPad;          // right edge of indicator zone
    const int indLeft  = indRight - kIndW;          // left edge of indicator zone
    const int maxChars = (indLeft - (x + kTextL) - kGap) / kCharW;

    // Label — truncated with ".." if longer than the available text zone.
    if (text && text[0]) {
        c.setCursor(x + kTextL, textY);
        int len = (int)strlen(text);
        if (len <= maxChars) {
            c.print(text);
        } else {
            char buf[80];
            int n = (maxChars >= 3) ? maxChars - 2 : 0;
            if (n >= (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
            strncpy(buf, text, (size_t)n);
            buf[n] = 0;
            strcat(buf, "..");
            c.print(buf);
        }
    }

    // Right-side indicator — inverts automatically because it uses rowFg/rowBg.
    if (!choice && opensScreen) {
        // Navigation row: always show '>'.
        c.setCursor(indLeft, textY);
        c.print(">");
    } else if (checked) {
        // Radio row, active choice: filled circle dot centred in the indicator zone.
        const int cx = indLeft + kIndW / 2;
        const int cy = y + h / 2;
        c.fillCircle(cx, cy, 4, indicatorFg ? indicatorFg : rowFg);
    }
    // choice && !checked: no indicator — empty right zone.
}

