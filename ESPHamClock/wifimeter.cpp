/* show a WiFi full-screen signal strength meter
 */

#include "HamClock.h"

#define WN_MIN_DB   (-80)                       // minimum graphed dBm
#define WN_MAX_DB   (-20)                       // max graphed dBm
#define WN_TRIS     10                          // current rssi triangle marker half-size
#define WN_MMTRIS   5                           // min/max rssi triangle marker half-size
#define MM_COLOR    RA8875_WHITE                // min/max triangle color
#define BUTTON_W    120                         // button width
#define BUTTON_H    35                          // button height
#define BUTTON_M    40                          // button l-r margin

// handy conversion of RSSI to graphics x
#define RSSI_X(R)   ((uint16_t)(rssi_b.x + rssi_b.w*(CLAMPF((R),WN_MIN_DB,WN_MAX_DB) - WN_MIN_DB)/(WN_MAX_DB-WN_MIN_DB)))

// state
static bool wifi_meter_up;              // whether visible now
static int rssi_min, rssi_max;          // range seen so far


/* given a screen coord x, return its wifi power color for drawing in the given box
 */
static uint16_t getWiFiMeterColor (const SBox &box, uint16_t x)
{
    // make color scale from red @ WN_MIN_DB, yellow @ MIN_WIFI_RSSI and green @ WN_MAX_DB.
    // use power scale because we don't want to assume MIN_WIFI_RSSI is midway.
    static float pwr;
    if (pwr == 0) {
        // first call, find power that puts yellow at MIN_WIFI_RSSI, where is halfway from red to green
        pwr = logf (0.5F) / logf ((float)(MIN_WIFI_RSSI-WN_MIN_DB)/(WN_MAX_DB-WN_MIN_DB));
        // printf ("*************** pwr %g\n", pwr);
    }

    if (x <= box.x)
        return (RA8875_RED);
    if (x >= box.x + box.w)
        return (RA8875_GREEN);
    float frac = powf((float)(x - box.x)/box.w, pwr);
    uint8_t h = 85*frac;                // hue 0 is red, 255/3 = 85 is green
    return (HSV_2_RGB565(h,255,255));
}


/* draw min/max marker above y centered at x in the given color
 */
static void drawMMMarker (uint16_t y, uint16_t x, uint16_t color)
{
    uint16_t top_y = y - 2*WN_MMTRIS;
    tft.fillTriangle (x, y-1, x - WN_MMTRIS, top_y, x + WN_MMTRIS, top_y, color);
}


/* draw current marker below y centered at x in the given color
 */
static void drawCMarker (uint16_t y, uint16_t x, uint16_t color)
{
    uint16_t bot_y = y + 2*WN_TRIS;
    tft.fillTriangle (x, y+1, x - WN_TRIS, bot_y, x + WN_TRIS, bot_y, color);
}


/* read wifi signal strength.
 * also set min and max so it's updated even when we're not up.
 * return whether ok.
 */
bool readWiFiRSSI(int &rssi)
{
    int r = WiFi.RSSI();

    // r = WN_MIN_DB + millis()*random(WN_MAX_DB-WN_MIN_DB)/100000;     // RBF

    if (r < 10) {                       // 10 is crazy hi
        rssi = r;
        if (rssi_min == 0 || r < rssi_min)
            rssi_min = rssi;
        if (rssi_max == 0 || r > rssi_max)
            rssi_max = rssi;
        return (true);

    } else {
        return (false);
    }
}

/* run the wifi meter screen until op taps Dismiss or Ignore.
 * show ignore based on incoming ignore_on and update if user changes.
 */
void runWiFiMeter(bool warn, bool &ignore_on)
{
    // prep
    Serial.printf ("WiFiM: start with ignore %s\n", ignore_on ? "on" : "off");
    wifi_meter_up = true;
    eraseScreen();
    closeGimbal();

    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    uint16_t y = 35;

    // title
    if (warn) {
        tft.setCursor (75, y);
        tft.setTextColor(RA8875_WHITE);
        tft.printf ("WiFi signal strength is too low -- recommend at least %d dBm", MIN_WIFI_RSSI);
    } else {
        tft.setCursor (100, y);
        tft.setTextColor(RA8875_WHITE);
        tft.printf ("WiFi Signal Strength Meter -- to improve signal strength:");
    }

    y += 25;

    // tips
    static const char *tips[] = {
        "Try different HamClock orientations",
        "Try different WiFi router orientations",
        "Try moving closer to WiFi router",
        "Try increasing WiFi router power",
        "Try adding a WiFi repeater",
        "Try experimenting with a foil reflector",
        "Try moving metal objects out of line-of-sight with router",
    };
    for (unsigned i = 0; i < NARRAY(tips); i++) {
        tft.setCursor (50, y += 32);
        tft.print (tips[i]);
    }

    y += 60;

    // resume button
    static const char resume_lbl[] = "Resume";
    SBox resume_b;
    resume_b.x = BUTTON_M;
    resume_b.y = y;
    resume_b.w = BUTTON_W;
    resume_b.h = BUTTON_H;
    drawStringInBox (resume_lbl, resume_b, false, RA8875_GREEN);

    // ignore button
    static const char ignore_lbl[] = "Ignore";
    SBox ignore_b;
    ignore_b.x = tft.width() - (BUTTON_W+BUTTON_M);
    ignore_b.y = y;
    ignore_b.w = BUTTON_W;
    ignore_b.h = BUTTON_H;
    drawStringInBox (ignore_lbl, ignore_b, ignore_on, RA8875_GREEN);

    // reset limits button
    static const char reset_lbl[] = "Reset Hi-Lo";
    SBox reset_b;
    reset_b.x = ignore_b.x;
    reset_b.y = y + ignore_b.h + 10;
    reset_b.w = BUTTON_W;
    reset_b.h = BUTTON_H;
    drawStringInBox (reset_lbl, reset_b, false, RA8875_GREEN);

    // signal strength scale
    SBox rssi_b;
    rssi_b.x = resume_b.x + resume_b.w + 50;
    rssi_b.y = y;
    rssi_b.w = ignore_b.x - rssi_b.x - 50;
    rssi_b.h = 17;

    // draw scale
    tft.setTextColor(RA8875_WHITE);
    for (uint16_t i = rssi_b.x; i < rssi_b.x + rssi_b.w; i++)
        tft.fillRect (i, rssi_b.y, 1, rssi_b.h, getWiFiMeterColor (rssi_b,i));
    for (int rssi = WN_MIN_DB+10; rssi < WN_MAX_DB; rssi += 10) {
        uint16_t x = RSSI_X(rssi);
        tft.drawLine (x, rssi_b.y, x, rssi_b.y+rssi_b.h/5, RA8875_BLACK);
    }

    // draw labels
    tft.setCursor (rssi_b.x - 20, rssi_b.y + rssi_b.h + 2*WN_TRIS + 25);
    tft.print (WN_MIN_DB); tft.print (" dBm");
    tft.setCursor (rssi_b.x + rssi_b.w - 20, rssi_b.y + rssi_b.h + 2*WN_TRIS + 25);
    tft.print (WN_MAX_DB);

    uint16_t ok_x = RSSI_X(MIN_WIFI_RSSI);
    tft.setCursor (ok_x - 20, rssi_b.y - 2*WN_MMTRIS-5);
    tft.print (MIN_WIFI_RSSI);
    tft.drawLine (ok_x, rssi_b.y, ok_x, rssi_b.y + rssi_b.h, RA8875_BLACK);

    // draw initial min/max
    uint16_t min_x = RSSI_X(rssi_min);
    uint16_t max_x = RSSI_X(rssi_max);
    uint16_t marker_y = rssi_b.y;
    drawMMMarker (marker_y, min_x, getWiFiMeterColor (rssi_b,min_x));
    drawMMMarker (marker_y, max_x, getWiFiMeterColor (rssi_b,max_x));


    // real-time loop

    bool done = false;
    uint32_t log_t = millis();
    int prev_rssi_x = rssi_b.x + 1;
    int rssi = 0;
    do {

        // erase real-time marker and value
        drawCMarker (rssi_b.y+rssi_b.h, prev_rssi_x, RA8875_BLACK);
        tft.fillRect (rssi_b.x + rssi_b.w/2 - 20, rssi_b.y + rssi_b.h + 2*WN_TRIS + 1, 40, 30, RA8875_BLACK);

        // read and update
        if (readWiFiRSSI(rssi)) {

            // update min/max if changed -- do both in case they overlap
            uint16_t new_min_x = RSSI_X(rssi_min);
            uint16_t new_max_x = RSSI_X(rssi_max);
            if (new_min_x != min_x || new_max_x != max_x) {
                drawMMMarker (marker_y, min_x, RA8875_BLACK);
                drawMMMarker (marker_y, max_x, RA8875_BLACK);
                min_x = new_min_x;
                max_x = new_max_x;
                drawMMMarker (marker_y, min_x, getWiFiMeterColor (rssi_b,min_x));
                drawMMMarker (marker_y, max_x, getWiFiMeterColor (rssi_b,max_x));
            }

            // show current
            uint16_t rssi_x = RSSI_X(rssi);
            drawCMarker (rssi_b.y+rssi_b.h, rssi_x, getWiFiMeterColor (rssi_b,rssi_x));
            tft.setCursor (rssi_b.x + rssi_b.w/2 - 20, rssi_b.y + rssi_b.h + 2*WN_TRIS + 25);
            tft.setTextColor(RA8875_WHITE);
            tft.print (rssi);
            prev_rssi_x = rssi_x;

            // log occassionally
            if (timesUp (&log_t, 5000))
                Serial.printf ("WiFiM: %d .. %d .. %d\n", rssi_min, rssi, rssi_max);
        }

        // update BME and brightness
        readBME280();
        followBrightness();

        // check touch
        SCoord tap;
        TouchType tt = readCalTouchWS (tap);
        if (tt != TT_NONE) {

            // bale if tap restores full brightness
            if (brightnessOn())
                continue;

            // check controls
            if (inBox (tap, resume_b)) {
                Serial.printf ("WiFiM: Resume\n");
                drawStringInBox (resume_lbl, resume_b, true, RA8875_GREEN);
                wdDelay (300);
                done = true;
            } else if (inBox (tap, ignore_b)) {
                ignore_on = !ignore_on;
                drawStringInBox (ignore_lbl, ignore_b, ignore_on, RA8875_GREEN);
                Serial.printf ("WiFiM: Ignore %s\n", ignore_on ? "on" : "off");
            } else if (inBox (tap, reset_b)) {
                Serial.printf ("WiFiM: Reset\n");
                drawStringInBox (reset_lbl, reset_b, true, RA8875_GREEN);
                wdDelay (300);
                drawStringInBox (reset_lbl, reset_b, false, RA8875_GREEN);
                rssi_min = rssi_max = 0;
                int rssi;
                (void) readWiFiRSSI(rssi);
            }

        } else

            wdDelay (100);

    } while (!done);

    // new state
    wifi_meter_up = false;
    initScreen();
}
