/* handle the touch screen
 */



#include "HamClock.h"


/* read keyboard char and check for warp cursor if hjkl or engage cr/lf/space
 * N.B. ignore multiple rapid engages
 */
TouchType checkKBWarp (SCoord &s)
{
    TouchType tt = TT_NONE;
    s.x = s.y = 0;

#if !defined(_WEB_ONLY)

    // ignore if don't want warping
    if (!want_kbcursor)
        return (TT_NONE);

    bool control, shift;
    char c = tft.getChar (&control, &shift);
    if (c) {

        switch (c) {

        case CHAR_LEFT: case CHAR_DOWN: case CHAR_UP: case CHAR_RIGHT:
            // warp
            {
                unsigned n = 1;
                if (shift)
                    n *= 2;
                if (control)
                    n *= 4;
                int x, y;
                if (tft.warpCursor (c, n, &x, &y)) {
                    s.x = x;
                    s.y = y;
                }
            }
            break;

        case CHAR_CR: case CHAR_NL: case CHAR_SPACE:
            // engage
            {
                static uint32_t prev_engage_ms;
                static int n_fast_engages;
                uint32_t engage_ms = millis();
                bool engage_rate_ok = engage_ms - prev_engage_ms > 1000;
                bool engage_ok = engage_rate_ok || ++n_fast_engages < 10;

                if (engage_ok && tft.getMouse (&s.x, &s.y))
                    tt = TT_TAP;
                if (engage_rate_ok)
                    n_fast_engages = 0;
                else if (!engage_ok)
                    Serial.printf (F("Keyboard functions are too fast\n"));

                prev_engage_ms = engage_ms;
            }
            break;

        default:
            // ignore all other chars
            break;

        }
    }
#endif // !_WEB_ONLY

    return (tt);
}

/* read the touch screen or mouse.
 * pass back calibrated screen coordinate and return a TouchType.
 */
TouchType readCalTouch (SCoord &s)
{
    // fast return if none
    if (!tft.touched())
        return (TT_NONE);

    int mb;
    while (tft.touched())
        tft.touchRead (&s.x, &s.y, &mb);

    TouchType tt = mb == 1 ? TT_TAP : TT_TAP_BX;

    Serial.printf("Touch: \t%4d %4d\ttype %d\n", s.x, s.y, (int)tt);

    // return tap type
    return (tt);
}


/* wait for no touch events, need time also since the resistance film seems to be sticky
 */
void drainTouch()
{
    resetWatchdog();
    uint32_t t0 = millis();
    bool touched = false;
    while (millis() - t0 < 100 || touched) {
        if ((touched = tft.touched()) == true) {
            uint16_t tx, ty;
            tft.touchRead (&tx, &ty, NULL);
        }
    }
    // Serial.println (F("Drain complete"));
    resetWatchdog();
}
