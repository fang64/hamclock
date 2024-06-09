/* support for the ADIF file pane and mapping.
 * based on https://www.adif.org/314/ADIF_314.htm
 * pane layout and operations are the same as dxcluster.
 */

#include "HamClock.h"

// #define _TRACE                                       // debug RBF

bool from_set_adif;                                     // set when spots are loaded via RESTful set_adif

#define ADIF_COLOR      RGB565 (255,228,225)            // misty rose
#define FILENM_Y0       32                              // file name down from box top
#define MAX_SPOTS       1000                            // max n spots to keep

static DXSpot *adif_spots;                       // malloced list
static ScrollState adif_ss;                             // scroll controller

typedef uint8_t crc_t;                                  // CRC data type
static crc_t prev_crc;                                  // detect crc change from one file to the next


/***********************************************************************************************************
 *
 * ADIF parser
 *
 ***********************************************************************************************************/


typedef enum {
    ADIFPS_STARTFILE,                                   // initialize all
    ADIFPS_STARTSPOT,                                   // initialize parser and spot candidate
    ADIFPS_STARTSEARCH,                                 // initialize parser, retain spot so far
    ADIFPS_SEARCHING,                                   // looking for opening <
    ADIFPS_INNAME,                                      // after < collecting field name until :
    ADIFPS_INLENGTH,                                    // after first : building value_len until : or >
    ADIFPS_INTYPE,                                      // after second : skipping type until >
    ADIFPS_INVALUE,                                     // after > now collecting value
    ADIFPS_FINISHED,                                    // spot is complete
} ADIFParseState;

typedef struct {
    ADIFParseState ps;                                  // what is happening now
    int line_n;                                         // line number for diagnostics
    crc_t crc;                                          // running checksum
    char name[20];                                      // field name so far, always includes EOS
    char value[20];                                     // field value so far, always includes EOS
    unsigned name_seen;                                 // n name chars seen so far (avoids strlen(name))
    unsigned value_len;                                 // value length so far from field defn
    unsigned value_seen;                                // n value chars seen so far (avoids strlen(value))
    char date_or_time[10];                              // temp QSO_DATE or TIME_ON whichever came first
} ADIFParser;

// YYYYMMDD HHMM[SS]
static bool parseDT2UNIX (const char *date, const char *tim, const char *call, time_t &unix)
{
    int yr, mo, dd, hh, mm, ss = 0;
    if (sscanf (date, "%4d%2d%2d", &yr, &mo, &dd) != 3 || sscanf (tim, "%2d%2d%2d", &hh, &mm, &ss) < 2) {
        Serial.printf ("ADIF: bogus date %s time %s for %s\n", date, tim, call);
        return (false);
    }

    tmElements_t tm;
    tm.Year = yr - 1970;                                // 1970-based
    tm.Month = mo;                                      // 1-based
    tm.Day = dd;                                        // 1-based
    tm.Hour = hh;
    tm.Minute = mm;
    tm.Second = ss;
    unix = makeTime(tm);

    return (true);
}



typedef struct {
    const char name[7];         // just long enough for longest band
    float MHz;
} ADIFBand;

/* convert BAND ADIF enumeration to lower frequency in kHz.
 * return whether recognized
 */
static bool parseADIFBand (const char *band, float &kHz)
{
    static ADIFBand bands[] PROGMEM = {
        { "2190m",       0.1357	},
        { "630m",        0.472	},
        { "560m",        0.501	},
        { "160m",        1.8	},
        { "80m",         3.5	},
        { "60m",         5.06	},
        { "40m",         7.0	},
        { "30m",        10.1	},
        { "20m",        14.0	},
        { "17m",        18.068	},
        { "15m",        21.0	},
        { "12m",        24.890	},
        { "10m",        28.0	},
        { "8m",         40	},
        { "6m",         50	},
        { "5m",         54	},
        { "4m",         70	},
        { "2m",        144	},
        { "1.25m",     222	},
        { "70cm",      420	},
        { "33cm",      902	},
        { "23cm",     1240	},
        { "13cm",     2300	},
        { "9cm",      3300	},
        { "6cm",      5650	},
        { "3cm",     10000	},
        { "1.25cm",  24000	},
        { "6mm",     47000	},
        { "4mm",     75500	},
        { "2.5mm",  119980	},
        { "2mm",    134000	},
        { "1mm",    241000	},
        { "submm",  300000	},
    };

    for (int i = 0; i < NARRAY(bands); i++) {
        if (strcmp_P (band, bands[i].name) == 0) {
            kHz = 1e3 * pgm_read_float(&bands[i].MHz);
            return (true);
        }
    }
    return (false);
}


/* crack a lat/long location of the form XDDD MM.MMM to degrees +N +E.
 */
static bool parseADIFLocation (const char *loc, float &degs)
{
    char dir;
    int deg;
    float min;
    if (sscanf (loc, "%c%d %f", &dir, &deg, &min) != 3)
        return (false);

    degs = deg + min/60;
    if (tolower(dir)=='w' || tolower(dir) == 's')
        degs = -degs;

    return (true);
}

/* strncpy that insures "to" has EOS (and avoids the g++ fussing)
 */
static void quietStrncpy (char *to, char *from, int len)
{
    snprintf (to, len, "%.*s", len-1, from);
}

/* add a completed ADIF field to spot if useful.
 */
static void addADIFFIeld (ADIFParser &adif, DXSpot &spot)
{
    if (!strcasecmp (adif.name, "OPERATOR") || !strcasecmp (adif.name, "STATION_CALLSIGN")) {
        quietStrncpy (spot.rx_call, adif.value, sizeof(spot.rx_call));



    } else if (!strcasecmp (adif.name, "MY_GRIDSQUARE")) {
        quietStrncpy (spot.rx_grid, adif.value, sizeof(spot.rx_grid));



    } else if (!strcasecmp (adif.name, "MY_LAT")) {
        float lat_d = 0;
        if (!parseADIFLocation (adif.value, lat_d) || lat_d < -90 || lat_d > 90) {
            Serial.printf ("ADIF: bogus MY_LAT %s for %s\n", adif.value, spot.tx_call);
            return;
        }
        spot.rx_ll.lat_d = lat_d;

        // set grid if not already and we have lng
        if (!spot.rx_grid[0] && spot.rx_ll.lng_d != 0) {
            normalizeLL(spot.rx_ll);
            ll2maidenhead (spot.rx_grid, spot.rx_ll);
        }



    } else if (!strcasecmp (adif.name, "MY_LON")) {
        float lng_d = 0;
        if (!parseADIFLocation (adif.value, lng_d) || lng_d < -180 || lng_d > 180) {
            Serial.printf ("ADIF: bogus MY_LON %s for %s\n", adif.value, spot.tx_call);
            return;
        }
        spot.rx_ll.lng_d = lng_d;

        // set grid if not already and we have lat
        if (!spot.rx_grid[0] && spot.rx_ll.lat_d != 0) {
            normalizeLL(de_ll);
            ll2maidenhead (spot.rx_grid, de_ll);
        }


    } else if (!strcasecmp (adif.name, "CALL") || !strcasecmp (adif.name, "CONTACTED_OP")) {
        quietStrncpy (spot.tx_call, adif.value, sizeof(spot.tx_call));



    } else if (!strcasecmp (adif.name, "QSO_DATE")) {
        // crack and reset if have both date and time, else save for later
        if (adif.date_or_time[0]) {
            bool parse_ok = parseDT2UNIX (adif.value, adif.date_or_time, spot.tx_call, spot.spotted);
            adif.date_or_time[0] = '\0';
            if (!parse_ok)
                return;
        } else
            quietStrncpy (adif.date_or_time, adif.value, sizeof(adif.date_or_time));



    } else if (!strcasecmp (adif.name, "TIME_ON")) {
        // crack and reset if have both date and time, else save for later
        if (adif.date_or_time[0]) {
            bool parse_ok = parseDT2UNIX (adif.date_or_time, adif.value, spot.tx_call, spot.spotted);
            adif.date_or_time[0] = '\0';
            if (!parse_ok)
                return;
        } else
            quietStrncpy (adif.date_or_time, adif.value, sizeof(adif.date_or_time));



    } else if (!strcasecmp (adif.name, "BAND")) {
        // ignore if kHz already set
        if (spot.kHz == 0) {
            if (!parseADIFBand (adif.value, spot.kHz)) {
                Serial.printf ("ADIF: unknown band %s for %s\n", adif.value, spot.tx_call);
                return;
            }
        }



    } else if (!strcasecmp (adif.name, "FREQ")) {
        spot.kHz = 1e3 * atof(adif.value); // ADIF stores MHz



    } else if (!strcasecmp (adif.name, "MODE")) {
        quietStrncpy (spot.mode, adif.value, sizeof(spot.mode));


    } else if (!strcasecmp (adif.name, "GRIDSQUARE")) {
        LatLong ll;
        if (maidenhead2ll (ll, adif.value)) {
            quietStrncpy (spot.tx_grid, adif.value, sizeof(spot.tx_grid));
            spot.tx_ll = ll;
        } else {
            Serial.printf ("ADIF: %s GRIDSQUARE%s for %s\n",
                        adif.value_seen > 0 ? " bogus" : " empty", adif.value, spot.tx_call);
            return;
        }



    } else if (!strcasecmp (adif.name, "LAT")) {
        float lat_d = 0;
        if (!parseADIFLocation (adif.value, lat_d) || lat_d < -90 || lat_d > 90) {
            Serial.printf ("ADIF: bogus LAT %s for %s\n", adif.value, spot.tx_call);
            return;
        }
        spot.tx_ll.lat_d = lat_d;

        // set grid if not already and we have lng
        if (!spot.tx_grid[0] && spot.tx_ll.lng_d != 0) {
            normalizeLL(spot.tx_ll);
            ll2maidenhead (spot.tx_grid, spot.tx_ll);
        }



    } else if (!strcasecmp (adif.name, "LON")) {
        float lng_d = 0;
        if (!parseADIFLocation (adif.value, lng_d) || lng_d < -180 || lng_d > 180) {
            Serial.printf ("ADIF: bogus LON %s for %s\n", adif.value, spot.tx_call);
            return;
        }
        spot.tx_ll.lng_d = lng_d;

        // set grid if not already and we have lat
        if (!spot.tx_grid[0] && spot.tx_ll.lat_d != 0) {
            normalizeLL(spot.tx_ll);
            ll2maidenhead (spot.tx_grid, spot.tx_ll);
        }
    }
}

/* return whether the given spot looks good-to-go
 */
static bool spotLooksGood (DXSpot &spot)
{
    bool ok = spot.tx_call[0] && spot.tx_grid[0] && (spot.tx_ll.lat_d != 0 || spot.tx_ll.lng_d != 0)
                            && spot.mode[0] && spot.kHz != 0 && spot.spotted != 0;

    if (ok) {
        // all good, just tidy up call a bit
        strtoupper (spot.tx_call);
    } else {
        // sorry
        Serial.printf ("ADIF: rejected spot: %s %s %g,%g %s %g %ld\n", spot.tx_call, spot.tx_grid,
                        spot.tx_ll.lat_d, spot.tx_ll.lng_d, spot.mode, spot.kHz, spot.spotted);
    }

    return (ok);
}

/* update crc with the next byte
 * adapted from pycrc --model=crc-8 --algorithm=bbf --generate c
 */
static void updateCRC (crc_t &crc, uint8_t byte)
{
    for (int i = 0x80; i > 0; i >>= 1) {
        crc_t bit = (crc & 0x80) ^ ((byte & i) ? 0x80 : 0);
        crc <<= 1;
        if (bit)
            crc ^= 0x07;
    }
}

/* parse the next character of an ADIF file, updating as we go along. spot is gradually filled as fields
 * are recognized. call with adif.ps = ADIFPS_STARTFILE the first time. adif.ps is set to ADIFPS_FINISHED
 * when spot is complete; no need to mess with adif.ps for any subsequent calls.
 *
 * return true as long as parsing is going well else false with brief reason in ynot
 * 
 * Required ADIF fields:
 *   CALL or CONTACTED_OP
 *   QSO_DATE
 *   TIME_ON
 *   BAND or FREQ
 *   MODE
 *   GRIDSQUARE or LAT and LON
 *   OPERATOR STATION_CALLSIGN else getCallsign()
 *   MY_GRIDSQUARE else use NV_DE_GRID
 *   MY_LAT and MY_LON else use de_ll
 *
 */
static bool parseADIF (char c, ADIFParser &adif, DXSpot &spot, char *ynot, int n_ynot)
{
    // update running line count and crc
    if (c == '\n')
        adif.line_n++;
    updateCRC (adif.crc, (uint8_t)c);

    // next action depends on current state

    switch (adif.ps) {

    case ADIFPS_STARTFILE:
        // full init
        memset (&adif, 0, sizeof(adif));
        memset (&spot, 0, sizeof(spot));
        // fallthru

    case ADIFPS_FINISHED:
        // putting FINISHED here allows caller to not have to change ps to look for the next spot
        // fallthru

    case ADIFPS_STARTSPOT:
        // init spot
        memset (&spot, 0, sizeof(spot));
        // fallthru

    case ADIFPS_STARTSEARCH:
        // init parser to look for a new field
        adif.name[0] = '\0';
        adif.value[0] = '\0';
        adif.name_seen = 0;
        adif.value_len = 0;
        adif.value_seen = 0;
        // fallthru

    case ADIFPS_SEARCHING:
        if (c == '<')
            adif.ps = ADIFPS_INNAME;                    // found opening <, start looking for field name
        else
            adif.ps = ADIFPS_SEARCHING;                 // in case we got here via a fallthru
        break;

    case ADIFPS_INNAME:
        if (c == ':') {
            // finish field name, start building value length until find > or optionl type :
            adif.value_len = 0;
            adif.ps = ADIFPS_INLENGTH;
        } else if (c == '>') {
            // bogus unless EOH or EOF
            if (!strcasecmp (adif.name, "EOH")) {
                adif.ps = ADIFPS_STARTSPOT;
            } else if (!strcasecmp (adif.name, "EOR")) {
                // yah! finished if spot is good else start fresh
                if (spotLooksGood(spot))
                    adif.ps = ADIFPS_FINISHED;
                else
                    adif.ps = ADIFPS_STARTSPOT;
            } else {
                snprintf (ynot, n_ynot, "line %d: no length with field %s", adif.line_n+1, adif.name);
                return (false);
            }
        } else if (adif.name_seen > sizeof(adif.name)-1) {
            // too long for name[] but none of the field names we care about will overflow so just skip it
            adif.ps = ADIFPS_STARTSEARCH;
        } else {
            // append next character to field name, maintaining EOS
            adif.name[adif.name_seen] = c;
            adif.name[++adif.name_seen] = '\0';
        }
        break;

    case ADIFPS_INLENGTH:
        if (c == ':') {
            // finish value length, start skipping optional data type. TODO?
            adif.ps = ADIFPS_INTYPE;
        } else if (c == '>') {
            // finish value length, start collecting value_len chars for field value
            adif.value[0] = '\0';
            adif.value_seen = 0;
            adif.ps = ADIFPS_INVALUE;
        } else if (isdigit(c)) {
            // fold c as int into value_len
            adif.value_len = 10*adif.value_len + (c - '0');
        } else {
            snprintf (ynot, n_ynot, "line %d: non-digit %c in field %s length\n", adif.line_n+1, c,
                                                                                adif.name);
            return (false);
        }
        break;

    case ADIFPS_INTYPE:
        // just skip until see >
        if (c == '>') {
            // finish optional type length, start collecting value_len chars for field value
            adif.value[0] = '\0';
            adif.value_seen = 0;
            adif.ps = ADIFPS_INVALUE;
        }
        break;

    case ADIFPS_INVALUE:
        if (adif.value_seen > sizeof(adif.value)-1) {
            // too long for value[] but none of the field values we care about will overflow so just skip it
            adif.ps = ADIFPS_STARTSEARCH;
        } else if (adif.value_seen < adif.value_len) {
            // append next character to field value, maintaining EOS
            adif.value[adif.value_seen] = c;
            adif.value[++adif.value_seen] = '\0';
        } else {
            // end of value, see if it helps spot then look for another field
            addADIFFIeld (adif,spot);
            adif.ps = ADIFPS_STARTSEARCH;
        }
        break;
    }

    // ok so far
    return (true);
}

/* expand any ENV in the given file.
 * return malloced result -- caller must free!
 */
static const char *expandENV (const char *fn)
{
    // build fn with any embedded info expanded
    char *fn_exp = NULL;
    int exp_len = 0;
    for (const char *fn_walk = fn; *fn_walk; fn_walk++) {

        // check for embedded terms

        char *embed_value = NULL;
        if (*fn_walk == '$') {
            // expect ENV all caps, digits or _
            const char *dollar = fn_walk;
            const char *env = dollar + 1;
            while (isupper(*env) || isdigit(*env) || *env=='_')
                env++;
            int env_len = env - dollar - 1;             // env now points to first invalid char; len w/o EOS
            StackMalloc env_mem(env_len+1);             // +1 for EOS
            char *env_tmp = (char *) env_mem.getMem();
            memcpy (env_tmp, dollar+1, env_len);
            env_tmp[env_len] = '\0';
            embed_value = getenv(env_tmp);
            fn_walk += env_len;

        } else if (*fn_walk == '~') {
            // expand ~ as $HOME
            embed_value = getenv("HOME");
            // fn_walk++ in for loop is sufficient
        }

        // append to fn_exp
        if (embed_value) {
            // add embedded value to fn_exp
            int val_len = strlen (embed_value);
            fn_exp = (char *) realloc (fn_exp, exp_len + val_len);
            memcpy (fn_exp + exp_len, embed_value, val_len);
            exp_len += val_len;
        } else {
            // no embedded found, just add fn_walk to fn_exp
            fn_exp = (char *) realloc (fn_exp, exp_len + 1);
            fn_exp[exp_len++] = *fn_walk;
        }
    }

    // add EOS
    fn_exp = (char *) realloc (fn_exp, exp_len + 1);
    fn_exp[exp_len++] = '\0';

    // ok
    return (fn_exp);
}


/***********************************************************************************************************
 *
 * ADIF pane and mapping control
 *
 ***********************************************************************************************************/

/* draw all currently visible spot then update scroll markers
 */
static void drawAllVisADIFSpots (const SBox &box)
{
    // show all visible adif_spots
    int min_i, max_i;
    if (adif_ss.getVisIndices (min_i, max_i) > 0) {
        for (int i = min_i; i <= max_i; i++) {
            const DXSpot &spot = adif_spots[i];
            uint16_t bg_col = onSPOTAWatchList (spot.tx_call) ? RA8875_RED : RA8875_BLACK;
            drawSpotOnList (box, spot, adif_ss.getDisplayRow(i), bg_col);
        }
    }

    // show scroll controls
    adif_ss.drawScrollDownControl (box, ADIF_COLOR);
    adif_ss.drawScrollUpControl (box, ADIF_COLOR);
}

/* shift the visible list to show older spots, if appropriate
 */
static void scrollADIFUp (const SBox &box)
{
    if (adif_ss.okToScrollUp ()) {
        adif_ss.scrollUp ();
        drawAllVisADIFSpots (box);
    }
}

/* shift the visible list to show newer spots, if appropriate
 */
static void scrollADIFDown (const SBox &box)
{
    if (adif_ss.okToScrollDown()) {
        adif_ss.scrollDown ();
        drawAllVisADIFSpots (box);
    }
}

static void resetADIFSpots(void)
{
    free (adif_spots);
    adif_spots = NULL;
    adif_ss.n_data = 0;
    adif_ss.top_vis = 0;
    prev_crc = 0;
}

/* draw complete ADIF pane in the given box
 */
static void drawADIFPane (const SBox &box, const char *filename)
{
    // prep
    prepPlotBox(box);

    // title
    const char *title = "ADIF";
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor(ADIF_COLOR);
    uint16_t tw = getTextWidth (title);
    tft.setCursor (box.x + (box.w-tw)/2, box.y + PANETITLE_H);
    tft.print (title);

    // show only the base filename
    const char *fn_basename = strrchr (filename, '/');
    if (fn_basename)
        fn_basename += 1;                       // skip past /
    else
        fn_basename = filename;                 // no change
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    uint16_t fnbw = getTextWidth (fn_basename);
    tft.setCursor (box.x + (box.w-fnbw)/2, box.y + FILENM_Y0);
    tft.print (fn_basename);

    // draw spots
    drawAllVisADIFSpots (box);
}

/* add another spot to adif_spots[] unless older than oldest so far.
 * maintain sorted order of oldest spot first.
 * only way we can fail is run out of memory.
 */
static void addADIFSpot (const DXSpot &spot)
{
    #if defined(_TRACE)
        printf ("new spot: %s %s %s %s %g %g %s %g %ld\n", 
            spot.rx_call, spot.rx_grid, spot.tx_call, spot.tx_grid, spot.tx_ll.lat_d,
            spot.tx_ll.lng_d, spot.mode, spot.kHz, spot.spotted);
    #endif

    // if already full, just discard spot if older than oldest
    if (adif_ss.n_data == MAX_SPOTS && spot.spotted < adif_spots[0].spotted)
        return;

    // assuming file probably has oldest entries first then each spot is probably newer than any so far,
    // so work back from the end to find the newest older entry
    int new_i;                                  // will be the index of the newest entry older than spot
    for (new_i = adif_ss.n_data; --new_i >= 0 && spot.spotted < adif_spots[new_i].spotted; )
        continue;

    if (adif_ss.n_data == MAX_SPOTS) {
        // adif_spots is already full: make room by shifting out the oldest up through new_i
        memmove (adif_spots, &adif_spots[1], new_i * sizeof(DXSpot));
    } else {
        // make room by moving existing entries newer than new_i
        memmove (&adif_spots[new_i+2], &adif_spots[new_i+1], (adif_ss.n_data-new_i-1)*sizeof(DXSpot));
        adif_ss.n_data += 1;                    // we've made room for spot
        new_i += 1;                             // put it 1 past the older entry 
    }

    // place new spot at new_i
    DXSpot &new_spot = adif_spots[new_i];
    new_spot = spot;

    // supply missing fields from DE
    if (!new_spot.rx_call[0])
        snprintf (new_spot.rx_call, sizeof(new_spot.rx_call), "%s", getCallsign());
    if (!new_spot.rx_grid[0]) {
        char de_grid[MAID_CHARLEN];
        getNVMaidenhead (NV_DE_GRID, de_grid);
        snprintf (new_spot.rx_grid, sizeof(new_spot.rx_grid), "%s", de_grid);
    }
    if (new_spot.rx_ll.lat == 0 && new_spot.rx_ll.lng == 0) {
        if (!maidenhead2ll (new_spot.rx_ll, new_spot.rx_grid))
            new_spot.rx_ll = de_ll;
    }
}

/* replace adif_spots with those found in the given open file.
 * return new count else -1 with short reason in ynot[] and adif_spots reset.
 * N.B. we clear from_set_adif.
 * N.B. caller must close fp
 * N.B. silently trucated to newest MAX_SPOTS
 * N.B. errors only reported for broken adif, not missing fields
 */
static int readADIFile (FILE *fp, char ynot[], int n_ynot)
{
    // restart list at full capacity
    adif_spots = (DXSpot *) realloc (adif_spots, MAX_SPOTS * sizeof(DXSpot));
    if (!adif_spots)
        fatalError ("ADIF: no memory for new spots\n");
    adif_ss.n_data = 0;

    // struct timeval t0, t1;
    // gettimeofday (&t0, NULL);

    // crack entire file, but keep only up to MAX_SPOTS newest
    DXSpot spot;
    ADIFParser adif;
    adif.ps = ADIFPS_STARTFILE;
    for (int c; (c = getc(fp)) != EOF; ) {
        if (!parseADIF((char)c, adif, spot, ynot, n_ynot)) {
            resetADIFSpots();
            return (-1);
        }
        if (adif.ps == ADIFPS_FINISHED) {
            if (!showOnlyOnSPOTAWatchList() || onSPOTAWatchList(spot.tx_call))
                addADIFSpot (spot);
            #if defined(_TRACE)
            else
                printf ("not on watch list: %s %s %s %s %g %g %s %g %ld\n", 
                        spot.rx_call, spot.rx_grid, spot.tx_call, spot.tx_grid, spot.tx_ll.lat_d,
                        spot.tx_ll.lng_d, spot.mode, spot.kHz, spot.spotted);
            #endif
        }
    }

    // gettimeofday (&t1, NULL);
    // printf ("file update %ld us\n", TVDELUS (t0, t1));

    // note these spots came from a file
    from_set_adif = false;

    // scroll all the way up unless likely the same list
    Serial.printf ("ADIF: crc %d previous %d\n", adif.crc, prev_crc);
    if (adif.crc != prev_crc) {
        adif_ss.scrollToNewest();
        prev_crc = adif.crc;
    }

    // shrink back to just what we need
    adif_spots = (DXSpot *) realloc (adif_spots, adif_ss.n_data * sizeof(DXSpot));

    // ok
    return (adif_ss.n_data);
}



/* replace adif_spots with those found in the given network connection.
 * return new count else -1 with short reason in ynot[] and adif_spots reset.
 * N.B. we set from_set_adif.
 * N.B. caller must close connection.
 * N.B. silently trucated to newest MAX_SPOTS
 * N.B. errors only reported for broken adif, not missing fields
 */
int readADIFWiFiClient (WiFiClient &client, long content_length, char ynot[], int n_ynot)
{
    // restart list at full capacity
    adif_spots = (DXSpot *) realloc (adif_spots, MAX_SPOTS * sizeof(DXSpot));
    if (!adif_spots)
        fatalError ("ADIF: no memory for new spots\n");
    adif_ss.n_data = 0;

    // struct timeval t0, t1;
    // gettimeofday (&t0, NULL);

    // crack entire stream, but keep only a max of the MAX_SPOTS newest
    DXSpot spot;
    ADIFParser adif;
    adif.ps = ADIFPS_STARTFILE;
    char c;
    for (long nr = 0; (!content_length || nr < content_length) && getTCPChar (client, &c); nr++) {
        if (!parseADIF(c, adif, spot, ynot, n_ynot)) {
            resetADIFSpots();
            return (-1);
        }
        if (adif.ps == ADIFPS_FINISHED) {
            if (!showOnlyOnSPOTAWatchList() || onSPOTAWatchList(spot.tx_call))
                addADIFSpot (spot);
        }
    }

    // gettimeofday (&t1, NULL);
    // printf ("file update %ld us\n", TVDELUS (t0, t1));

    // note spots came from network
    from_set_adif = true;

    // scroll all the way down unless likely the same list
    Serial.printf ("ADIF: crc %d previous %d\n", adif.crc, prev_crc);
    if (adif.crc != prev_crc) {
        adif_ss.scrollToNewest();
        prev_crc = adif.crc;
    }

    // shrink back to just what we need
    adif_spots = (DXSpot *) realloc (adif_spots, adif_ss.n_data * sizeof(DXSpot));

    // ok
    return (adif_ss.n_data);
}


/* called occasionally to show ADIF records.
 * if records were set via set_adif or no file name is available set name to "set_adif".
 * once records are set via set_adif, must tap the pane to read file again.
 */
void updateADIF (const SBox &box)
{
    // init data size and scrolling parameters
    int max_vis = (box.h - LISTING_Y0)/LISTING_DY;
    if (max_vis != adif_ss.max_vis) {
        adif_ss.max_vis = max_vis;
        adif_ss.top_vis = 0;
    }
    adif_ss.n_data = 0;

    // get full file name, or show web hint
    const char *fn = getADIFilename();
    const char *fn_exp;
    bool fn_malloced;
    if (!fn || from_set_adif) {
        fn_exp = "set_adif";
        from_set_adif = true;
        fn_malloced = false;
    } else {
        fn_exp = expandENV (fn);
        fn_malloced = true;
    }

    // read file unless spots are from_set_adif
    bool showing_errmsg = false;
    if (!from_set_adif) {
        char errmsg[100];
        FILE *fp = fopen (fn_exp, "r");
        if (!fp) {
            snprintf (errmsg, sizeof(errmsg), "%s: %s", fn_exp, strerror(errno));
            plotMessage (box, RA8875_RED, errmsg);
            showing_errmsg = true;
        } else {
            // prefill errmsg with basename in case of error
            const char *fn_basename = strrchr (fn_exp, '/');
            if (fn_basename)
                fn_basename += 1;                       // skip past /
            else
                fn_basename = fn_exp;                   // use as-is
            int prefix_l = snprintf (errmsg, sizeof(errmsg), "%s: ", fn_basename);
            int n = readADIFile (fp, errmsg + prefix_l, sizeof(errmsg) - prefix_l);
            if (n < 0) {
                plotMessage (box, RA8875_RED, errmsg);
                showing_errmsg = true;
            } else
                Serial.printf ("ADIF: found %d spots in %s\n", n, fn_exp);
            fclose (fp);
        }
    }

    // draw spots unless we are showing an err msg
    if (!showing_errmsg)
        drawADIFPane (box, fn_exp);

    // clean up if our malloc
    if (fn_malloced)
        free ((void*)fn_exp);
}

/* draw each entry, if enabled
 */
void drawADIFSpotsOnMap()
{
    if (findPaneForChoice(PLOT_CH_ADIF) == PANE_NONE)
        return;

    // paths first then labels looks better
    for (int i = 0; i < adif_ss.n_data; i++) {
        DXSpot &si = adif_spots[i];
        drawSpotPathOnMap (si);
    }
    for (int i = 0; i < adif_ss.n_data; i++) {
        DXSpot &si = adif_spots[i];
        drawSpotLabelOnMap (si, LOM_TXEND, LOM_ALL);
        drawSpotLabelOnMap (si, LOM_RXEND, LOM_ALL);
    }
}

/* check for touch at s in the ADIF pane located in the given box.
 * return true if touch is for us else false so mean user wants to change pane selection.
 */
bool checkADIFTouch (const SCoord &s, const SBox &box)
{
    if (s.y < box.y + PANETITLE_H) {

        // scroll up?
        if (adif_ss.checkScrollUpTouch (s, box)) {
            scrollADIFUp (box);
            return (true);
        }

        // scroll down?
        if (adif_ss.checkScrollDownTouch (s, box)) {
            scrollADIFDown (box);
            return (true);
        }

        // else in title
        return (false);
    }

    else if (s.y < box.y + FILENM_Y0 + 10) {

        // tap file name to force immediate re-read
        if (getADIFilename()) {
            from_set_adif = false;
            scheduleNewPlot(PLOT_CH_ADIF);
        }

        // our touch regardless of file success
        return (true);
    }

    else {

        // tapped entry to set DX

        int row_i = (s.y - box.y - LISTING_Y0)/LISTING_DY;
        int data_i;
        if (adif_ss.findDataIndex (row_i, data_i))
            newDX (adif_spots[data_i].tx_ll, NULL, adif_spots[data_i].tx_call);

        // our touch regardless of whether row was occupied
        return (true);
    }
}

/* find closest spot and location on either end to given ll, if any.
 */
bool getClosestADIFSpot (const LatLong &ll, DXSpot *sp, LatLong *llp)
{
    return (getClosestSpot (adif_spots, adif_ss.n_data, ll, sp, llp));
}

/* call to clean up if not in use, get out fast if nothing to do.
 */
void checkADIF()
{
    if (adif_spots && findPaneForChoice(PLOT_CH_ADIF) == PANE_NONE)
        resetADIFSpots();
}
