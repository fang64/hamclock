/* EME planning tool
 */


#include "HamClock.h"


// moon elevation plot parameters and handy helpers
#define MP_TB           60                                      // top plot border
#define MP_LB           60                                      // left plot border
#define MP_RB           20                                      // right plot border
#define MP_BB           50                                      // bottom plot border
#define MP_NI           10                                      // next-both-up table indent
#define MP_MT           5                                       // up marker line thickness
#define MP_TL           2                                       // tick length
#define MP_PH           (map_b.h - MP_TB - MP_BB)               // plot height
#define MP_PW           (map_b.w - MP_LB - MP_RB)               // plot width
#define MP_X0           (map_b.x + MP_LB)                       // x coord of plot left
#define MP_DUR          (2*24*3600)                             // plot duration, seconds
#define MP_DT           (MP_DUR/100)                            // plot step size, seconds
#define MP_US           60                                      // micro step refined time, seconds
#define MP_TO           (30*1000)                               // time out, millis
#define MP_FC           RGB565(65,200,65)                       // fill color 
#define MP_TT           7                                       // timeline marker thickness
#define MP_E2Y(E)       ((uint16_t)(map_b.y+MP_TB + MP_PH*(M_PI_2F-(E))/M_PIF + 0.5F)) // elev to y coord
#define MP_T2X(T)       ((uint16_t)(MP_X0 + MP_PW*((T)-t0)/MP_DUR))     // time_t to x coord
#define MP_X2T(X)       ((time_t)(t0 + MP_DUR*((X)-MP_X0)/MP_PW))       // x coord to time_t


/* draw everything in the moon EME plot except the elevation plots, Resume button and the "Next Up" table.
 * t0 is nowWO()
 */
static void drawMPSetup (time_t t0)
{
        resetWatchdog();

        // grid lines color
        const uint16_t dark = RGB565(50,50,50);

        // title
        const char *title = _FX("Lunar Elevation at DE and DX");
        selectFontStyle (LIGHT_FONT, SMALL_FONT);
        uint16_t tw = getTextWidth(title);
        tft.setCursor (map_b.x + (map_b.w-tw)/2, map_b.y + 30);
        tft.setTextColor (RA8875_WHITE);
        tft.print (title);

        // x and y axes
        tft.drawLine (MP_X0, MP_E2Y(-M_PI_2F), MP_X0, MP_E2Y(M_PI_2F), BRGRAY);
        tft.drawLine (MP_X0, MP_E2Y(-M_PI_2F), MP_X0 + MP_PW, MP_E2Y(-M_PI_2F), BRGRAY);

        // center line
        tft.drawLine (MP_X0, MP_E2Y(0), MP_X0+MP_PW, MP_E2Y(0), GRAY);

        // horizontal grid lines
        for (int i = -80; i <= 90; i += 10)
            tft.drawLine (MP_X0+MP_TL, MP_E2Y(deg2rad(i)), MP_X0 + MP_PW, MP_E2Y(deg2rad(i)), dark);
        tft.drawLine (MP_X0+MP_TL, MP_E2Y(0), MP_X0 + MP_PW, MP_E2Y(0), GRAY);

        // y labels
        selectFontStyle (LIGHT_FONT, FAST_FONT);
        tft.setTextColor(BRGRAY);
        tft.setCursor (MP_X0 - 20, MP_E2Y(M_PI_2F) - 4);
        tft.print ("+90");
        tft.setCursor (MP_X0 - 10, MP_E2Y(0) - 4);
        tft.print ("0");
        tft.setCursor (MP_X0 - 20, MP_E2Y(-M_PI_2F) - 4);
        tft.print ("-90");
        tft.setCursor(MP_X0-17, MP_E2Y(deg2rad(50)));
        tft.print(F("Up"));
        tft.setCursor(MP_X0-29, MP_E2Y(deg2rad(-45)));
        tft.print(F("Down"));
        const char estr[] = "Elevation";
        const int estr_l = strlen(estr);
        for (int i = 0; i < estr_l; i++) {
            tft.setCursor(MP_X0-42, MP_E2Y(deg2rad(45-10*i)));
            tft.print(estr[i]);
        }

        // y tick marks
        for (int i = -80; i <= 90; i += 10)
            tft.drawLine (MP_X0, MP_E2Y(deg2rad(i)), MP_X0+MP_TL, MP_E2Y(deg2rad(i)), BRGRAY);

        // time zone labels
        uint16_t de_y = MP_E2Y(-M_PI_2F) + 6;
        uint16_t dx_y = MP_E2Y(-M_PI_2F) + MP_BB/2 - 4;
        uint16_t utc_y = MP_E2Y(-M_PI_2F) + MP_BB - 6-8;
        tft.setTextColor (DE_COLOR);
        tft.setCursor (MP_X0-53, de_y);
        tft.print ("DE hour");
        tft.setTextColor (DX_COLOR);
        tft.setCursor (MP_X0-53, dx_y);
        tft.print ("DX");
        tft.setTextColor (RA8875_WHITE);
        tft.setCursor (MP_X0-53, utc_y);
        tft.print ("UTC");

        // x axis time line and vertical grid lines, mark each even hour.
        // N.B. check every 15 minutes for oddball time zones (looking at you Australia)
        int detz = getTZ (de_tz);
        int dxtz = getTZ (dx_tz);
        tft.drawLine (MP_X0, dx_y-3, MP_X0+MP_PW, dx_y-3, BRGRAY);
        tft.drawLine (MP_X0, utc_y-3, MP_X0+MP_PW, utc_y-3, BRGRAY);
        int prev_de_hr = hour (t0 + detz);
        int prev_dx_hr = hour (t0 + dxtz);
        int prev_utc_hr = hour (t0);
        for (time_t t = 900*(t0/900+1); t < t0 + MP_DUR; t += 900) {

            // get x coord of this time
            uint16_t x = MP_T2X(t);

            // get times in each zone
            int de_hr = hour (t + detz);
            int dx_hr = hour (t + dxtz);
            int utc_hr = hour (t);

            // plot each time zone every 2 hours
            if (prev_de_hr != de_hr && (de_hr%2)==0) {
                tft.drawLine (x, MP_E2Y(-M_PI_2F), x, MP_E2Y(M_PI_2F), dark);
                tft.drawLine (x, MP_E2Y(-M_PI_2F), x, MP_E2Y(-M_PI_2F)-MP_TL, RA8875_WHITE);
                tft.setTextColor (DE_COLOR);
                tft.setCursor (x-(de_hr<10?3:6), de_y);         // center X or XX
                tft.print (de_hr);
            }
            if (prev_dx_hr != dx_hr && (dx_hr%2)==0) {
                tft.drawLine (x, dx_y-3, x, dx_y-3-MP_TL, RA8875_WHITE);
                tft.setTextColor (DX_COLOR);
                tft.setCursor (x-(dx_hr<10?3:6), dx_y);
                tft.print (dx_hr);
            }
            if (prev_utc_hr != utc_hr && (utc_hr%2)==0) {
                tft.drawLine (x, utc_y-3, x, utc_y-3-MP_TL, RA8875_WHITE);
                tft.setTextColor (RA8875_WHITE);
                tft.setCursor (x-(utc_hr<10?3:6), utc_y);
                tft.print (utc_hr);
            }

            // retain for next loop
            prev_de_hr = de_hr;
            prev_dx_hr = dx_hr;
            prev_utc_hr = utc_hr;
        }
}

/* draw both elevation plots.
 * return rough start and end times +- MP_DT of first period in which moon is up for both, with complications:
 *   start == t0 means plot period started both-up;
 *   end == 0 means both-up never ended within plot duration;
 *   both above means always both-up;
 *   start == 0 means never both-up, end has no meaning
 * t0 is nowWO()
 */
static void drawMPElPlot (time_t t0, time_t &t_start, time_t &t_end)
{
        resetWatchdog();

        // reset start/end so we can set with first occurance
        t_start = t_end = 0;

        // previous location in order to build line segments and find when both just up or down
        uint16_t prev_x = 0, prev_de_y = 0, prev_dx_y = 0;
        bool prev_both_up = false;

        // handy
        const uint16_t x_step = MP_T2X(MP_DT) - MP_T2X(0);    // time step x change
        const uint16_t elm90y = MP_E2Y(deg2rad(-90));         // y of -90 el

        // work across plot
        for (time_t t = t0; t <= t0 + MP_DUR; t += MP_DT) {
            resetWatchdog();

            // find circumstances at time t
            AstroCir de_ac, dx_ac;
            getLunarCir (t, de_ll, de_ac);
            getLunarCir (t, dx_ll, dx_ac);
            uint16_t de_y = MP_E2Y(de_ac.el);
            uint16_t dx_y = MP_E2Y(dx_ac.el);
            uint16_t x = MP_T2X(t);

            // check both_up_now
            bool both_up_now = de_ac.el > 0 && dx_ac.el > 0;

            // emphasize when both up
            if (!prev_both_up && both_up_now) {
                // approximate this starting half step left of x .. beware left edge
                uint16_t left_x = x - x_step/2;
                if (left_x < MP_X0)
                    left_x = MP_X0;
                tft.fillRect (left_x, elm90y-MP_TT, x_step/2 + 1, MP_TT, MP_FC);
            } else if (prev_both_up && both_up_now) {
                // mark entire step
                tft.fillRect (prev_x, elm90y-MP_TT, x_step + 1, MP_TT, MP_FC);
            } else if (prev_both_up && !both_up_now) {
                // approximate this stopping half step right of prev_x .. beware right edge
                uint16_t width = x_step/2;
                if (x + width > MP_X0 + MP_PW)
                    width = MP_X0 + MP_PW - x;
                tft.fillRect (prev_x, elm90y-MP_TT, width, MP_TT, MP_FC);
            }

            // continue line segment connected to previous location
            if (t > t0) {
                tft.drawLine (prev_x, prev_de_y, x, de_y, DE_COLOR);
                tft.drawLine (prev_x, prev_dx_y, x, dx_y, DX_COLOR);
            }

            // note when first both up or down
            if (both_up_now) {
                if (t_start == 0)
                    t_start = t;
            } else if (prev_both_up) {
                if (t_end == 0)
                    t_end = t;
            }

            // save for next iteration
            prev_x = x;
            prev_de_y = de_y;
            prev_dx_y = dx_y;
            prev_both_up = both_up_now;
        }

        Serial.printf (_FX("MP: rough start %02d:%02d end %02d:%02d\n"),
                                hour(t_start), minute(t_start),
                                hour(t_end), minute(t_end));
}

/* given plot start time and approximate times for both-up start and end, refine and draw table.
 * N.B. see drawMPElPlot comments for special cases.
 */
static void drawMPBothUpTable (time_t t0, time_t t_start, time_t t_end)
{
        bool always_both_up = t_start == t0 && !t_end;
        bool never_both_up = t_start == 0;
        bool finite_both_up = !always_both_up && !never_both_up;
        char buf[50];

        // search around times in finer steps to refine to nearest MP_US
        time_t better_start = 0, better_end = 0;
        if (finite_both_up) {
            AstroCir de_ac, dx_ac;

            // find better start unless now
            if (t_start > t0) {
                bool both_up_now = true;
                for (better_start = t_start - MP_US; both_up_now; better_start -= MP_US) {
                    getLunarCir (better_start, de_ll, de_ac);
                    getLunarCir (better_start, dx_ll, dx_ac);
                    both_up_now = de_ac.el > 0 && dx_ac.el > 0;
                }
                better_start += 2*MP_US;            // return to last known both_up_now
            } else {
                better_start = t0;
            }

            // find better end
            bool both_up_now = false;
            for (better_end = t_end - MP_US; !both_up_now; better_end -= MP_US) {
                getLunarCir (better_end, de_ll, de_ac);
                getLunarCir (better_end, dx_ll, dx_ac);
                both_up_now = de_ac.el > 0 && dx_ac.el > 0;
            }
            better_end += 2*MP_US;              // return to last known !both_up_now

            Serial.printf (_FX("MP: better start %02d:%02d end %02d:%02d\n"),
                                hour(better_start), minute(better_start),
                                hour(better_end), minute(better_end));
        }

        // table title
        selectFontStyle (LIGHT_FONT, FAST_FONT);
        tft.setTextColor (RA8875_WHITE);
        tft.setCursor (map_b.x+MP_NI, map_b.y+5);
        if (always_both_up) {
            tft.print (F("Both always up"));
            return;
        }
        if (never_both_up) {
            tft.print (F("Never both up"));
            return;
        }
        int dt = better_end - better_start;
        snprintf (buf, sizeof(buf), _FX("Next both up %02dh%02d"), dt/3600, (dt%3600)/60);
        tft.print (buf);


        // DE row
        int detz = getTZ (de_tz);
        if (better_start == t0)  {
            snprintf (buf, sizeof(buf), _FX("DE    now    %02d:%02d"),
                    hour(better_end+detz), minute(better_end+detz));
        } else {
            snprintf (buf, sizeof(buf), _FX("DE   %02d:%02d   %02d:%02d"),
                    hour(better_start+detz), minute(better_start+detz),
                    hour(better_end+detz), minute(better_end+detz));
        }
        tft.setTextColor (DE_COLOR);
        tft.setCursor (map_b.x+MP_NI, map_b.y+15);
        tft.print (buf);

        // DX row
        int dxtz = getTZ (dx_tz);
        if (better_start == t0)  {
            snprintf (buf, sizeof(buf), _FX("DX    now    %02d:%02d"),
                    hour(better_end+dxtz), minute(better_end+dxtz));
        } else {
            snprintf (buf, sizeof(buf), _FX("DX   %02d:%02d   %02d:%02d"),
                    hour(better_start+dxtz), minute(better_start+dxtz),
                    hour(better_end+dxtz), minute(better_end+dxtz));
        }
        tft.setTextColor (DX_COLOR);
        tft.setCursor (map_b.x+MP_NI, map_b.y+25);
        tft.print (buf);

        // UTC rows
        if (better_start == t0)  {
            snprintf (buf, sizeof(buf), _FX("UTC   now    %02d:%02d"),
                    hour(better_end), minute(better_end));
        } else {
            snprintf (buf, sizeof(buf), _FX("UTC  %02d:%02d   %02d:%02d"),
                    hour(better_start), minute(better_start),
                    hour(better_end), minute(better_end));
        }
        tft.setTextColor (RA8875_WHITE);
        tft.setCursor (map_b.x+MP_NI, map_b.y+35);
        tft.print (buf);
}

/* draw popup in the given box for time t
 */
static void drawMPPopup (const time_t t, const SBox &popup_b)
{
        resetWatchdog();

        // circumstances at t
        AstroCir de_ac, dx_ac;
        getLunarCir (t, de_ll, de_ac);
        getLunarCir (t, dx_ll, dx_ac);

        // prep popup rectangle
        fillSBox (popup_b, RA8875_BLACK);
        drawSBox (popup_b, RA8875_WHITE);

        // draw column headings
        tft.setTextColor (RA8875_WHITE);
        selectFontStyle (LIGHT_FONT, FAST_FONT);
        tft.setCursor (popup_b.x+4, popup_b.y+2);
        tft.print (F("     Time   Az   El"));

        // draw time, el and az at each location
        char buf[100];

        int detz = getTZ (de_tz);
        snprintf (buf, sizeof(buf), _FX("DE  %02d:%02d  %3.0f %4.0f"), hour(t+detz),
                minute(t+detz), rad2deg(de_ac.az), rad2deg(de_ac.el));
        tft.setCursor (popup_b.x+4, popup_b.y+14);
        tft.setTextColor(DE_COLOR);
        tft.print (buf);

        int dxtz = getTZ (dx_tz);
        snprintf (buf, sizeof(buf), _FX("DX  %02d:%02d  %3.0f %4.0f"), hour(t+dxtz),
                minute(t+dxtz), rad2deg(dx_ac.az), rad2deg(dx_ac.el));
        tft.setCursor (popup_b.x+4, popup_b.y+24);
        tft.setTextColor(DX_COLOR);
        tft.print (buf);

        snprintf (buf, sizeof(buf), _FX("UTC %02d:%02d"), hour(t), minute(t));
        tft.setCursor (popup_b.x+4, popup_b.y+34);
        tft.setTextColor(RA8875_WHITE);
        tft.print (buf);

        // now
        tft.drawPR();
}

/* plot lunar elevation vs time on map_b. time goes forward a few days. label in DE DX local and UTC.
 */
void drawEMETool()
{
        // start now
        time_t t0 = nowWO();

        // erase
        fillSBox (map_b, RA8875_BLACK);

        // draw boilerplate
        drawMPSetup (t0);

        // draw elevation plot, find first period when both up
        time_t t_start, t_end;
        drawMPElPlot (t0, t_start, t_end);

        // refine and draw both-up table
        drawMPBothUpTable (t0, t_start, t_end);

        // create resume button box
        SBox resume_b;
        resume_b.w = 100;
        resume_b.x = map_b.x + map_b.w - resume_b.w - MP_RB;
        resume_b.h = 40;
        resume_b.y = map_b.y + 4;
        const char button_name[] = "Resume";
        selectFontStyle (LIGHT_FONT, SMALL_FONT);
        drawStringInBox (button_name, resume_b, false, RA8875_GREEN);

        // see it all now
        tft.drawPR();

        // popup history for erasing
        bool popup_is_up = false;
        SBox popup_b = {0,0,0,0};

        // report info for tap times until time out or tap Resume button
        SCoord s;
        char c;
        UserInput ui = {
            map_b,
            UI_UFuncNone,
            UF_UNUSED,
            MP_TO,
            UF_CLOCKSOK,
            s,
            c,
            false,
            false
        };
        while (waitForUser(ui)) {

            // done if return, esc or tap Resume button or tap outside box
            if (c == CHAR_CR || c == CHAR_NL || c == CHAR_ESC || inBox (s, resume_b) || 
                                                    (c == CHAR_NONE && !inBox (s, map_b)))
                break;

            // first erase previous popup, if any
            if (popup_is_up) {
                fillSBox (popup_b, RA8875_BLACK);
                drawMPSetup (t0);
                drawMPElPlot (t0, t_start, t_end);
                popup_is_up = false;
            }

            // show new popup if tap within the plot area
            if (s.x > MP_X0 && s.x < MP_X0 + MP_PW && s.y > MP_E2Y(M_PI_2F) && s.y < MP_E2Y(-M_PI_2F)) {

                resetWatchdog();

                // popup at s
                popup_b.x = s.x;
                popup_b.y = s.y;
                popup_b.w = 122;
                popup_b.h = 45;

                // insure entirely over plot
                if (popup_b.x + popup_b.w > MP_X0 + MP_PW)
                    popup_b.x = MP_X0 + MP_PW - popup_b.w;
                if (popup_b.y + popup_b.h > MP_E2Y(-M_PI_2F) - MP_MT)
                    popup_b.y = MP_E2Y(-M_PI_2F) - MP_MT - popup_b.h;

                // draw popup
                drawMPPopup (MP_X2T(s.x), popup_b);

                // note popup is now up
                popup_is_up = true;
            }
        }

        // ack
        selectFontStyle (LIGHT_FONT, SMALL_FONT);
        drawStringInBox (button_name, resume_b, true, RA8875_GREEN);
        tft.drawPR();
}
