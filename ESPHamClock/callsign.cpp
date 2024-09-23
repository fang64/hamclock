/* this code manages the call sign and ON THE AIR experience.
 */


#include "HamClock.h"

CallsignInfo cs_info;                                   // state
static const char on_air_msg[] = "ON THE AIR";          // default ota message


#define DEFCALL_FG      RA8875_WHITE                    // default callsign fg color
#define DEFCALL_BG      RA8875_BLACK                    // default callsign bg color
#define DEFONAIR_FG     RA8875_WHITE                    // default OTA fg color
#define DEFONAIR_BG     RA8875_RED                      // default OTA bg color

static const char def_oatitle[] = "ON THE AIR";         // default oa string
static bool onair_sw, onair_hw;                         // sw and hw ON AIR states


/* draw full spectrum in the given box.
 */
static void drawRainbow (SBox &box)
{
    // sweep full range of hue starting at random x within box
    uint8_t x0 = random(box.w);
    for (uint16_t x = box.x; x < box.x+box.w; x++) {
        uint8_t h = 255*((x+x0-box.x)%box.w)/box.w;
        tft.fillRect (x, box.y, 1, box.h, HSV_2_RGB565(h,255,255));
    }
}

/* draw the given string centered in the given box using the current font and given color.
 * if it fits in one line, set y to box.y + l1dy.
 * if fits as two lines draw set their y to box.y + l12dy and l22dy.
 * if latter two are 0 then don't even try 2 lines.
 * if it won't fit in 2 lines and anyway is set, draw as much as possible.
 * if shadow then draw a black background shadow.
 * return whether it all fit some way.
 */
static bool drawBoxText (bool anyway, const SBox &box, const char *str, uint16_t color,
uint16_t l1dy, uint16_t l12dy, uint16_t l22dy, bool shadow)
{
    // try as one line
    uint16_t w = getTextWidth (str);
    if (w < box.w) {
        shadowString (str, shadow, color, box.x + (box.w-w)/2, box.y + l1dy);
        return (true);
    }

    // bale if don't even want to try 2 lines
    if (l12dy == 0 || l22dy == 0)
        return (false);

    // try splitting into 2 lines
    StackMalloc str_copy0(str);
    char *s0 = (char *) str_copy0.getMem();
    for (char *space = strrchr (s0,' '); space; space = strrchr (s0,' ')) {
        *space = '\0';
        uint16_t w0 = getTextWidth (s0);
        if (w0 < box.w) {
            char *s1 = space + 1;
            strcpy (s1, str + (s1 - s0));               // restore zerod spaces
            uint16_t w1 = getTextWidth (s1);
            if (w1 < box.w) {
                // 2 lines fit
                shadowString (s0, shadow, color, box.x + (box.w - w0)/2, box.y + l12dy);
                shadowString (s1, shadow, color, box.x + (box.w - w1)/2, box.y + l22dy);
                return (true);
            } else if (anyway) {
                // print 1st line and as AMAP of 2nd
                shadowString (s0, shadow, color, box.x + (box.w - w0)/2, box.y + l12dy);
                w1 = maxStringW (s1, box.w);
                shadowString (s1, shadow, color, box.x + (box.w - w1)/2, box.y + l22dy);
                return (false);
            }
        }
    }

    // no way
    return (false);
}


/* return next color after current in basic series of primary colors that is nicely different from contrast.
 */
static uint16_t getNextColor (uint16_t current, uint16_t contrast)
{
    static uint16_t colors[] = {
        RA8875_RED, RA8875_GREEN, RA8875_BLUE, RA8875_CYAN,
        RA8875_MAGENTA, RA8875_YELLOW, RA8875_WHITE, RA8875_BLACK,
        DE_COLOR
    };
    #define NCOLORS NARRAY(colors)
    
    // find index of current color, ok if not found
    unsigned current_i;
    for (current_i = 0; current_i < NCOLORS; current_i++)
        if (colors[current_i] == current)
            break;

    // scan forward from current until find one nicely different from contrast
    for (unsigned cdiff_i = 1; cdiff_i < NCOLORS; cdiff_i++) {
        uint16_t next_color = colors[(current_i + cdiff_i) % NCOLORS];

        // certainly doesn't work if same as contrast
        if (next_color == contrast)
            continue;

        // continue scanning if bad combination
        switch (next_color) {
        case RA8875_RED:
            if (contrast == RA8875_MAGENTA || contrast == DE_COLOR)
                continue;
            break;
        case RA8875_GREEN:      // fallthru
        case RA8875_BLUE:
            if (contrast == RA8875_CYAN)
                continue;
            break;
        case RA8875_CYAN:
            if (contrast == RA8875_GREEN || contrast == RA8875_BLUE)
                continue;
            break;
        case RA8875_MAGENTA:
            if (contrast == RA8875_RED || contrast == DE_COLOR)
                continue;
            break;
        case RA8875_YELLOW:
            if (contrast == RA8875_WHITE)
                continue;
            break;
        case RA8875_WHITE:
            if (contrast == RA8875_YELLOW)
                continue;
            break;
        case RA8875_BLACK:
            // black goes with anything
            break;
        case DE_COLOR:
            if (contrast == RA8875_RED || contrast == RA8875_MAGENTA)
                continue;
            break;
        }

        // no complaints
        return (next_color);
    }

    // default 
    return (colors[0]);
}

/* load cs_info color settings from NV and set default ON AIR text
 */
void initCallsignInfo()
{
    if (!NVReadUInt16 (NV_CALL_FG_COLOR, &cs_info.call_fg)) {
        cs_info.call_fg = DEFCALL_FG;
        NVWriteUInt16 (NV_CALL_FG_COLOR, cs_info.call_fg);
    }
    if (!NVReadUInt16 (NV_CALL_BG_COLOR, &cs_info.call_bg)) {
        cs_info.call_bg = DEFCALL_BG;
        NVWriteUInt16 (NV_CALL_BG_COLOR, cs_info.call_bg);
    }
    if (!NVReadUInt8 (NV_CALL_BG_RAINBOW, &cs_info.call_bg_rainbow)) {
        cs_info.call_bg_rainbow = 0;
        NVWriteUInt8 (NV_CALL_BG_RAINBOW, cs_info.call_bg_rainbow);
    }

    if (!NVReadUInt16 (NV_OA_FG_COLOR, &cs_info.oa_fg)) {
        cs_info.oa_fg = DEFONAIR_FG;
        NVWriteUInt16 (NV_OA_FG_COLOR, cs_info.oa_fg);
    }
    if (!NVReadUInt16 (NV_OA_BG_COLOR, &cs_info.oa_bg)) {
        cs_info.oa_bg = DEFONAIR_BG;
        NVWriteUInt16 (NV_OA_BG_COLOR, cs_info.oa_bg);
    }
    if (!NVReadUInt8 (NV_OA_BG_RAINBOW, &cs_info.oa_bg_rainbow)) {
        cs_info.oa_bg_rainbow = 0;
        NVWriteUInt8 (NV_OA_BG_RAINBOW, cs_info.oa_bg_rainbow);
    }

    // always start showing call sign but prep oa_title regardless
    cs_info.showing_oa = false;
    setOnAirText (NULL);
}

/* draw callsign using cs_info.
 * draw everything if all, else just fg text as when just changing text color.
 */
void drawCallsign (bool all)
{
    // handy
    uint16_t fg_c = cs_info.showing_oa ? cs_info.oa_fg : cs_info.call_fg;
    bool rainbow = (cs_info.showing_oa && cs_info.oa_bg_rainbow)
                                                        || (!cs_info.showing_oa && cs_info.call_bg_rainbow);

    // start with background iff all
    if (all) {
        if (rainbow)
            drawRainbow (cs_info.box);
        else {
            uint16_t bg = cs_info.showing_oa ? cs_info.oa_bg : cs_info.call_bg;
            fillSBox (cs_info.box, bg);
        }
    }

    // set text color
    tft.setTextColor(fg_c);

    // get desired string
    const char *str = cs_info.showing_oa ? cs_info.oa_title : cs_info.call;

    // copy str to slash0 with each '0' replaced with DEL which has been hacked into a slashed-0 in BOLD/LARGE
    StackMalloc call_slash0(str);
    char *slash0 = (char *) call_slash0.getMem();
    for (char *z = slash0; *z != '\0' ; z++) {
        if (*z == '0')
            *z = 127;   // del
    }

    // start with largest font and keep shrinking and trying 2 lines until fits
    const SBox &box = cs_info.box;              // handy
    selectFontStyle (BOLD_FONT, LARGE_FONT);
    if (!drawBoxText (false, box, slash0, fg_c, box.h/2+20, 0, 0, rainbow)) {
        // try smaller font
        selectFontStyle (BOLD_FONT, SMALL_FONT);
        if (!drawBoxText (false, box, str, fg_c, box.h/2+10, 0, 0, rainbow)) {
            // try smaller font
            selectFontStyle (LIGHT_FONT, SMALL_FONT);
            if (!drawBoxText (false, box, str, fg_c, box.h/2+10, 0, 0, rainbow)) {
                // try all upper case to allow 2 lines without regard to descenders
                StackMalloc call_uc(str);
                char *uc = (char *) call_uc.getMem();
                for (char *z = uc; *z != '\0' ; z++)
                    *z = toupper(*z);
                if (!drawBoxText (false, box, uc, fg_c, box.h/2+10, box.h/2-2, box.h-3, rainbow)) {
                    // try smallest font
                    selectFontStyle (LIGHT_FONT, FAST_FONT);
                    (void) drawBoxText (true, box, str, fg_c, box.h/2-10, box.h/2-14, box.h/2+4, rainbow);
                }
            }
        }
    }
}

/* set the ON AIR text, else set it to the default.
 * N.B. this neither sets showing_ao nor displays the message.
 */
void setOnAirText (const char *s)
{
    free (cs_info.oa_title);
    if (s)
        cs_info.oa_title = strdup (s);
    else
        cs_info.oa_title = strdup (def_oatitle);
}

/* common test for setOnAirHW() and setOnAirSW().
 * N.B. only draw when aggregate state changes to avoid flashing.
 */
static void setOnAir(void)
{
    if (onair_hw || onair_sw) {
        if (!cs_info.showing_oa) {
            cs_info.showing_oa = true;
            drawCallsign (true);
            Serial.printf ("ONAIR: on\n");
        }
    } else if (cs_info.showing_oa) {
        cs_info.showing_oa = false;
        drawCallsign (true);
        Serial.printf ("ONAIR: off\n");
    }
}

/* set ON AIR message state from hardware input -- cooperates with setOnAirSW()
 */
void setOnAirHW (bool on)
{
    onair_hw = on;
    setOnAir();
}

/* set ON AIR message state from software input -- cooperates with setOnAirSH()
 */
void setOnAirSW (bool on)
{
    onair_sw = on;
    setOnAir();
}


/* given a touch location check if Op wants to change callsign fg.
 * if so then update cs_info and return true else false.
 */
bool checkCallsignTouchFG (const SCoord &b)
{
    SBox left_half = cs_info.box;
    left_half.w /=2;

    if (inBox (b, left_half)) {
        // assume black background when over rainbow
        if (cs_info.showing_oa) {
            uint16_t bg = cs_info.oa_bg_rainbow ? RA8875_BLACK : cs_info.oa_bg;
            cs_info.oa_fg = getNextColor (cs_info.oa_fg, bg);
            NVWriteUInt16 (NV_OA_FG_COLOR, cs_info.oa_fg);
        } else {
            uint16_t bg = cs_info.call_bg_rainbow ? RA8875_BLACK : cs_info.call_bg;
            cs_info.call_fg = getNextColor (cs_info.call_fg, bg);
            NVWriteUInt16 (NV_CALL_FG_COLOR, cs_info.call_fg);
        }
        return (true);
    }
    return (false);
}


/* given a touch location check if Op wants to change callsign bg.
 * if so then update cs_info and return true else false.
 */
bool checkCallsignTouchBG (const SCoord &b)
{
    SBox right_half = cs_info.box;
    right_half.w /=2;
    right_half.x += right_half.w;

    if (inBox (b, right_half)) {
        // cycle through rainbow when current bg is white
        if (cs_info.showing_oa) {
            if (cs_info.oa_bg_rainbow) {
                cs_info.oa_bg_rainbow = 0;
                cs_info.oa_bg = getNextColor (cs_info.oa_bg, cs_info.oa_fg);
            } else if (cs_info.oa_bg == RA8875_WHITE) {
                cs_info.oa_bg_rainbow = 1;
                // leave cs_info.oa_bg to resume color scan when rainbow turned off
            } else {
                cs_info.oa_bg = getNextColor (cs_info.oa_bg, cs_info.oa_fg);
            }
            NVWriteUInt16 (NV_OA_BG_COLOR, cs_info.oa_bg);
            NVWriteUInt8 (NV_OA_BG_RAINBOW, cs_info.oa_bg_rainbow);
        } else {
            if (cs_info.call_bg_rainbow) {
                cs_info.call_bg_rainbow = 0;
                cs_info.call_bg = getNextColor (cs_info.call_bg, cs_info.call_fg);
            } else if (cs_info.call_bg == RA8875_WHITE) {
                cs_info.call_bg_rainbow = 1;
                // leave cs_info.call_bg to resume color scan when rainbow turned off
            } else {
                cs_info.call_bg = getNextColor (cs_info.call_bg, cs_info.call_fg);
            }
            NVWriteUInt16 (NV_CALL_BG_COLOR, cs_info.call_bg);
            NVWriteUInt8 (NV_CALL_BG_RAINBOW, cs_info.call_bg_rainbow);
        }
        return (true);
    }

    return (false);
}

/* set and save new cs_info parameters.
 * all args are optional but whether msg determines whether remaining args apply to oa or call.
 * rainbow supercedes bg if both are present.
 */
void setCallsignInfo (const char *oa_msg, uint16_t *fg, uint16_t *bg, uint8_t *rainbow)
{
    if (oa_msg) {
        // affect ON AIR message
        setOnAirText (oa_msg);
        cs_info.showing_oa = true;
        if (fg) {
            cs_info.oa_fg = *fg;
        }
        if (rainbow) {
            cs_info.oa_bg_rainbow = *rainbow;
        } else if (bg) {
            cs_info.oa_bg = *bg;
            cs_info.oa_bg_rainbow = 0;
        }
        NVWriteUInt16 (NV_OA_FG_COLOR, cs_info.oa_fg);
        NVWriteUInt16 (NV_OA_BG_COLOR, cs_info.oa_bg);
        NVWriteUInt8 (NV_OA_BG_RAINBOW, cs_info.oa_bg_rainbow);
    } else {
        // affect normal call sign
        cs_info.showing_oa = false;
        if (fg) {
            cs_info.call_fg = *fg;
        }
        if (rainbow) {
            cs_info.call_bg_rainbow = *rainbow;
        } else if (bg) {
            cs_info.call_bg = *bg;
            cs_info.call_bg_rainbow = 0;
        }
        NVWriteUInt16 (NV_CALL_FG_COLOR, cs_info.call_fg);
        NVWriteUInt16 (NV_CALL_BG_COLOR, cs_info.call_bg);
        NVWriteUInt8 (NV_CALL_BG_RAINBOW, cs_info.call_bg_rainbow);
    }
}
