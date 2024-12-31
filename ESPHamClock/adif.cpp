/* support for the ADIF file pane and mapping.
 * based on https://www.adif.org/314/ADIF_314.htm
 * pane layout and operations are similar to dxcluster.
 * 
 * debug guidelines:
 *   level 1: log each successful spot, no errors
 *   level 2: 1+ errors in final check
 *   level 3: 2+ all else
 */

#include "HamClock.h"


#define ADIF_COLOR      RGB565 (255,228,225)            // misty rose


// tables of sort enum ids and corresponding qsort compare functions -- use X trick to insure in sync

#define ADIF_SORTS              \
    X(ADS_AGE, qsDXCSpotted)    \
    X(ADS_DIST, qsDXCDist)      \
    X(ADS_CALL, qsDXCTXCall)    \
    X(ADS_BAND, qsDXCFreq)

#define X(a,b) a,                                       // expands X to each enum value and comma
typedef enum {
    ADIF_SORTS
    ADIF_SORTS_N
} ADIFSorts;                                            // enum of each sort function
#undef X

#define X(a,b) b,                                       // expands X to each function pointer and comma
static PQSF adif_pqsf[ADIF_SORTS_N] = {                 // sort functions, in order of ADIFSorts
    ADIF_SORTS
};
#undef X



// class to organize testing whether a file has changed
class FileSignature
{
    public:

        // init
        FileSignature(void) {
            reset();
        }

        // reset so any subsequent file will appear to have changed
        void reset() {
            mtime = 0;
            len = 0;
        }

        // return whether the given file appears to be different than the current state.
        // if so, save the new file info as the current state.
        bool fileChanged (const char *fn) {

            // get info for fn
            struct stat s;
            bool changed = true;
            if (::stat (fn, &s) == 0) {
                time_t fn_mtime = s.st_mtime;
                long fn_len = s.st_size;
                changed = fn_mtime != mtime || fn_len != len;
                mtime = fn_mtime;
                len = fn_len;
            } else {
                Serial.printf ("ADIF: stat(%s): %s\n", fn, strerror(errno));
                reset();
            }
            return (changed);
        }

    private:

        // current state
        time_t mtime;                                   // modification time
        long len;                                       // file length

};


// state
static ADIFSorts adif_sort;                             // current sort code index into adif_pqsf
static DXSpot *adif_spots;                              // malloced
static ScrollState adif_ss;                             // scroll controller, n_data is count
static bool showing_set_adif;                           // set when not checking for local file
static bool newfile_pending;                            // set when find new file while scrolled away
static FileSignature fsig;                              // used to decide whether to read file again
static int n_adif_bad;                                  // n bad spots found, global to maintain context


/***********************************************************************************************************
 *
 * ADIF parser
 *
 ***********************************************************************************************************/


typedef enum {
    ADIFPS_STARTFILE,                                   // initialize all
    ADIFPS_STARTSPOT,                                   // initialize parser and spot candidate
    ADIFPS_STARTFIELD,                                  // initialize parser for next field,retain spot so far
    ADIFPS_SEARCHING,                                   // looking for opening <
    ADIFPS_INNAME,                                      // after < collecting field name until :
    ADIFPS_INLENGTH,                                    // after first : building value_len until : or >
    ADIFPS_INTYPE,                                      // after second : skipping type until >
    ADIFPS_INVALUE,                                     // after > now collecting value
    ADIFPS_FINISHED,                                    // spot is complete
    ADIFPS_SKIPTOEOR,                                   // skip to EOR after finding an error
} ADIFParseState;

typedef enum {
    AFB_BAND,
    AFB_CALL,
    AFB_DXCC,
    AFB_CONTACTED_OP,
    AFB_FREQ,
    AFB_GRIDSQUARE,
    AFB_LAT,
    AFB_LON,
    AFB_MODE,
    AFB_MY_GRIDSQUARE,
    AFB_MY_LAT,
    AFB_MY_LON,
    AFB_MY_DXCC,
    AFB_OPERATOR,
    AFB_QSO_DATE,
    AFB_STATION_CALLSIGN,
    AFB_TIME_ON,
} ADIFFieldBit;

typedef struct {
    // running state
    ADIFParseState ps;                                  // what is happening now
    int line_n;                                         // line number for diagnostics

    // per-field state
    char name[20];                                      // field name so far, always includes EOS
    char value[20];                                     // field value so far, always includes EOS
    unsigned name_seen;                                 // n name chars seen so far (avoids strlen(name))
    unsigned value_len;                                 // claimed value length so far from field defn
    unsigned value_seen;                                // n value chars seen so far (avoids strlen(value))

    // per-spot state
    uint32_t fields;                                    // bit mask of 1 << ADIFFieldBit seen
    char qso_date[10];                                  // temp QSO_DATE .. need both to get UNIX time
    char time_on[10];                                   // temp TIME_ON .. need both to get UNIX time
} ADIFParser;

#define CHECK_AFB(a,b)   ((a).fields & (1 << (b)))      // handy test for ADIFFieldBit
#define ADD_AFB(a,b)     ((a).fields |= (1 << (b)))     // handy way to add one ADIFFieldBit


// YYYYMMDD HHMM[SS]
static bool parseDT2UNIX (const char *date, const char *tim, time_t &unix)
{
    int yr, mo, dd, hh, mm, ss = 0;
    if (sscanf (date, "%4d%2d%2d", &yr, &mo, &dd) != 3 || sscanf (tim, "%2d%2d%2d", &hh, &mm, &ss) < 2) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: parseDT2UNIX(%s, %s) failed\n", date, tim);
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

    if (debugLevel (DEBUG_ADIF, 3))
        Serial.printf ("ADIF: spotted %s %s -> %ld\n", date, tim, (long)unix);

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
    static ADIFBand bands[] = {
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
        if (strcasecmp (band, bands[i].name) == 0) {
            kHz = 1e3 * bands[i].MHz;
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

/* add a completed ADIF name/value pair to spot and update fields mask if qualifies.
 * return false if outright syntax error.
 * N.B. within spot we assign "my" fields to be rx, the "other" guy to be tx.
 */
static bool addADIFFIeld (ADIFParser &adif, DXSpot &spot)
{
    // false if fall thru all the tests
    bool useful_field = true;

    if (!strcasecmp (adif.name, "OPERATOR")) {
        ADD_AFB (adif, AFB_OPERATOR);
        quietStrncpy (spot.rx_call, adif.value, sizeof(spot.rx_call));


    } else if (!strcasecmp (adif.name, "STATION_CALLSIGN")) {
        ADD_AFB (adif, AFB_STATION_CALLSIGN);
        quietStrncpy (spot.rx_call, adif.value, sizeof(spot.rx_call));


    } else if (!strcasecmp (adif.name, "MY_GRIDSQUARE")) {
        if (!maidenhead2ll (spot.rx_ll, adif.value)) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus MY_GRIDSQUARE %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_MY_GRIDSQUARE);
        quietStrncpy (spot.rx_grid, adif.value, sizeof(spot.rx_grid));


    } else if (!strcasecmp (adif.name, "MY_LAT")) {
        if (!parseADIFLocation (adif.value, spot.rx_ll.lat_d)
                                        || spot.rx_ll.lat_d < -90 || spot.rx_ll.lat_d > 90) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus MY_LAT %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_MY_LAT);

    } else if (!strcasecmp (adif.name, "MY_LON")) {
        if (!parseADIFLocation (adif.value, spot.rx_ll.lng_d)
                                        || spot.rx_ll.lng_d < -180 || spot.rx_ll.lng_d > 180) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus MY_LON %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_MY_LON);

    } else if (!strcasecmp (adif.name, "MY_DXCC")) {
        ADD_AFB (adif, AFB_MY_DXCC);
        spot.rx_dxcc = atoi (adif.value);

    } else if (!strcasecmp (adif.name, "DXCC")) {
        ADD_AFB (adif, AFB_DXCC);
        spot.tx_dxcc = atoi (adif.value);

    } else if (!strcasecmp (adif.name, "CALL")) {
        ADD_AFB (adif, AFB_CALL);
        quietStrncpy (spot.tx_call, adif.value, sizeof(spot.tx_call));


    } else if (!strcasecmp (adif.name, "CONTACTED_OP")) {
        ADD_AFB (adif, AFB_CONTACTED_OP);
        quietStrncpy (spot.tx_call, adif.value, sizeof(spot.tx_call));


    } else if (!strcasecmp (adif.name, "QSO_DATE")) {
        if (CHECK_AFB (adif, AFB_TIME_ON)) {
            if (!parseDT2UNIX (adif.value, adif.time_on, spot.spotted)) {
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d bogus QSO_DATE %s or TIME_ON %s\n", adif.line_n,
                                                adif.value, adif.time_on);
                return (false);
            }
        }
        ADD_AFB (adif, AFB_QSO_DATE);
        quietStrncpy (adif.qso_date, adif.value, sizeof(adif.qso_date));


    } else if (!strcasecmp (adif.name, "TIME_ON")) {
        if (CHECK_AFB (adif, AFB_QSO_DATE)) {
            if (!parseDT2UNIX (adif.qso_date, adif.value, spot.spotted)) {
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d bogus TIME_ON %s or QSO_DATE %s\n", adif.line_n,
                                                adif.value, adif.qso_date);
                return (false);
            }
        }
        ADD_AFB (adif, AFB_TIME_ON);
        quietStrncpy (adif.time_on, adif.value, sizeof(adif.time_on));


    } else if (!strcasecmp (adif.name, "BAND")) {
        // don't use BAND if FREQ already set
        if (!CHECK_AFB (adif, AFB_FREQ) && !parseADIFBand (adif.value, spot.kHz)) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d unknown band %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_BAND);


    } else if (!strcasecmp (adif.name, "FREQ")) {
        spot.kHz = 1e3 * atof(adif.value); // ADIF stores MHz
        if (findHamBand (spot.kHz) == HAMBAND_NONE) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus FREQ %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_FREQ);


    } else if (!strcasecmp (adif.name, "MODE")) {
        ADD_AFB (adif, AFB_MODE);
        quietStrncpy (spot.mode, adif.value, sizeof(spot.mode));


    } else if (!strcasecmp (adif.name, "GRIDSQUARE")) {
        if (!maidenhead2ll (spot.tx_ll, adif.value)) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus GRIDSQUARE %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_GRIDSQUARE);

        quietStrncpy (spot.tx_grid, adif.value, sizeof(spot.tx_grid));


    } else if (!strcasecmp (adif.name, "LAT")) {
        if (!parseADIFLocation (adif.value, spot.tx_ll.lat_d)
                                        || spot.tx_ll.lat_d < -90 || spot.tx_ll.lat_d > 90) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus LAT %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_LAT);


    } else if (!strcasecmp (adif.name, "LON")) {
        if (!parseADIFLocation (adif.value, spot.tx_ll.lng_d)
                                        || spot.tx_ll.lng_d < -180 || spot.tx_ll.lng_d > 180) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus LON %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_LON);

    } else 
        useful_field = false;

    if (debugLevel (DEBUG_ADIF, 3)) {
        if (useful_field)
            Serial.printf ("ADIF: added <%s:%d>%s\n", adif.name, adif.value_seen, adif.value);
        else
            Serial.printf ("ADIF: unused field <%s:%d>%s\n", adif.name, adif.value_seen, adif.value);
    }

    // keep going
    return (true);
}

/* make sure spot is complete and ready to use.
 * return whether spot is good to go.
 */
static bool spotLooksGood (ADIFParser &adif, DXSpot &spot)
{
    if (! (CHECK_AFB(adif, AFB_CALL) || CHECK_AFB(adif, AFB_CONTACTED_OP)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No CALL or CONTACTED_OP\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_FREQ) || CHECK_AFB(adif, AFB_BAND)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No FREQ or BAND\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_QSO_DATE)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No QSO_DATE\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_TIME_ON)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No TIME_ON\n", adif.line_n);
        return (false);
    }

    // must have tx location for plotting
    if (!CHECK_AFB(adif,AFB_LAT) || !CHECK_AFB(adif,AFB_LON)) {
        if (CHECK_AFB(adif,AFB_GRIDSQUARE)) {
            if (!maidenhead2ll (spot.tx_ll, spot.tx_grid)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d bogus grid %s for %s\n", adif.line_n,spot.tx_grid,spot.tx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from grid %s\n", adif.line_n, spot.tx_call,
                                            spot.tx_grid);
        } else {
            if (!call2LL (spot.tx_call, spot.tx_ll)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d No GRIDSQUARE LAT or LON and cty lookup for %s failed\n",
                                    adif.line_n, spot.tx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from cty\n", adif.line_n, spot.tx_call);
        }
    }

    // at this point we know we have tx_ll so might as well add tx_grid if not already
    if (!CHECK_AFB(adif,AFB_GRIDSQUARE)) {
        ll2maidenhead (spot.tx_grid, spot.tx_ll);
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: line %d: add grid %s for %s from ll\n", adif.line_n, spot.tx_grid,
                                            spot.tx_call);
    }

    if (!CHECK_AFB(adif, AFB_MODE)) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No MODE\n", adif.line_n);
        return (false);
    }


    if (! (CHECK_AFB(adif, AFB_OPERATOR) || CHECK_AFB(adif, AFB_STATION_CALLSIGN)) ) {
        // assume us?
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: assuming RX is %s\n", getCallsign());
        quietStrncpy (spot.rx_call, getCallsign(), sizeof(spot.rx_call));
        spot.rx_ll = de_ll;
        ll2maidenhead (spot.rx_grid, spot.rx_ll);
        ADD_AFB (adif, AFB_MY_LAT);
        ADD_AFB (adif, AFB_MY_LON);
        ADD_AFB (adif, AFB_MY_GRIDSQUARE);
    }


    // must have rx location for plotting
    if (!CHECK_AFB(adif,AFB_MY_LAT) || !CHECK_AFB(adif,AFB_MY_LON)) {
        if (CHECK_AFB(adif,AFB_MY_GRIDSQUARE)) {
            if (!maidenhead2ll (spot.rx_ll, spot.rx_grid)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d bogus grid %s for %s\n", adif.line_n,spot.rx_grid,spot.rx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from grid %s\n", adif.line_n, spot.rx_call,
                                            spot.rx_grid);
        } else {
            if (!call2LL (spot.rx_call, spot.rx_ll)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d No GRIDSQUARE LAT or LON and cty lookup for %s failed\n",
                                    adif.line_n, spot.rx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from cty\n", adif.line_n, spot.rx_call);
        }
    }

    // at this point we know we have rx_ll so might as well add rx_grid if not already
    if (!CHECK_AFB(adif,AFB_MY_GRIDSQUARE)) {
        ll2maidenhead (spot.rx_grid, spot.rx_ll);
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: line %d: add grid %s for %s from ll\n", adif.line_n, spot.rx_grid,
                                            spot.rx_call);
    }

    // check rx and tx dxcc
    if (!CHECK_AFB(adif,AFB_MY_DXCC)) {
        if (!call2DXCC (spot.rx_call, spot.rx_dxcc)) {
            if (debugLevel (DEBUG_ADIF, 2))
                Serial.printf ("ADIF: line %d no DXCC for %s\n", adif.line_n, spot.rx_call);
            return (false);
        }
    }
    if (!CHECK_AFB(adif,AFB_DXCC)) {
        if (!call2DXCC (spot.tx_call, spot.tx_dxcc)) {
            if (debugLevel (DEBUG_ADIF, 2))
                Serial.printf ("ADIF: line %d no DXCC for %s\n", adif.line_n, spot.tx_call);
            return (false);
        }
    }

    // all good, just tidy up a bit
    strtoupper (spot.tx_call);
    normalizeLL (spot.tx_ll);
    strtoupper (spot.rx_call);
    normalizeLL (spot.rx_ll);

    return (true);
}

/* parse the next character of an ADIF file, updating parser state and filling in spot as we go along.
 * set adis.ps to ADIFPS_STARTFILE on first call then just leave ps alone.
 * returns true when a candidate spot has been assembled.
 */
static bool parseADIF (char c, ADIFParser &adif, DXSpot &spot)
{
    // update running line count
    if (c == '\n')
        adif.line_n++;

    // next action depends on current state

    switch (adif.ps) {

    case ADIFPS_STARTFILE:
        // full init
        memset (&adif, 0, sizeof(adif));
        adif.line_n = 1;
        memset (&spot, 0, sizeof(spot));

        // fallthru

    case ADIFPS_FINISHED:
        // putting FINISHED here allows caller to not have to change ps to look for the next spot

        // fallthru

    case ADIFPS_STARTSPOT:
        // init spot
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: starting new spot scan\n");
        memset (&spot, 0, sizeof(spot));

        // init per-spot fields in parser
        adif.qso_date[0] = '\0';
        adif.time_on[0] = '\0';
        adif.fields = 0;

        // fallthru

    case ADIFPS_STARTFIELD:
        // init per-field fields in parser
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
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: found EOH\n");
                adif.ps = ADIFPS_STARTSPOT;
            } else if (!strcasecmp (adif.name, "EOR")) {
                // yah!
                adif.ps = ADIFPS_FINISHED;
            } else {
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d no length with field %s", adif.line_n, adif.name);
                adif.ps = ADIFPS_SKIPTOEOR;
            }
        } else if (adif.name_seen > sizeof(adif.name)-1) {
            // too long for name[] but none of the field names we care about will overflow so just skip it
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: ignoring long name %.*s %d > %d\n", adif.line_n,
                                adif.name_seen, adif.name, adif.name_seen, (int)(sizeof(adif.name)-1));
            adif.ps = ADIFPS_STARTFIELD;
        } else {
            // append next character to field name, maintaining EOS
            adif.name[adif.name_seen] = c;
            adif.name[++adif.name_seen] = '\0';
        }
        break;

    case ADIFPS_INLENGTH:
        if (c == ':') {
            // finish value length, start skipping optional data type. TODO?
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: in type for %s\n", adif.line_n, adif.name);
            adif.ps = ADIFPS_INTYPE;
        } else if (c == '>') {
            // finish value length, start collecting value_len chars for field value unless 0
            adif.value[0] = '\0';
            adif.value_seen = 0;
            if (adif.value_len == 0) {
                adif.ps = ADIFPS_STARTFIELD;
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d: 0 length data field %s\n", adif.line_n, adif.name);
            } else
                adif.ps = ADIFPS_INVALUE;
        } else if (isdigit(c)) {
            // fold c as int into value_len
            adif.value_len = 10*adif.value_len + (c - '0');
            if (debugLevel (DEBUG_ADIF, 3) && adif.value_len == 0)
                Serial.printf ("ADIF: line %d: 0 in length field %s now %d\n", adif.line_n, adif.name,
                                        adif.value_len);
        } else {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF line %d: non-digit %c in field %s length\n", adif.line_n, c, adif.name);
            adif.ps = ADIFPS_SKIPTOEOR;
        }
        break;

    case ADIFPS_INTYPE:
        // just skip until see >
        if (c == '>') {
            // finish optional type length, start collecting value_len chars for field value
            adif.value[0] = '\0';
            adif.value_seen = 0;
            if (adif.value_len == 0)
                adif.ps = ADIFPS_STARTFIELD;
            else
                adif.ps = ADIFPS_INVALUE;
        }
        break;

    case ADIFPS_INVALUE:
        // append next character + EOS to field value if room, but always keep counting
        if (adif.value_seen < sizeof(adif.value)-1) {
            adif.value[adif.value_seen] = c;
            adif.value[adif.value_seen+1] = '\0';
        }
        adif.value_seen += 1;

        // finished when found entire field
        if (adif.value_seen == adif.value_len) {
            // install if we had room to store it al
            if (adif.value_seen < sizeof(adif.value)) {
                (void) addADIFFIeld (adif, spot);       // rely on spotLooksGood() for final qualification
            } else if (debugLevel (DEBUG_ADIF, 3)) {
                Serial.printf ("ADIF: ignoring long value <%s:%d>%.*s %d > %d\n",
                                adif.name, adif.value_len, adif.value_seen, adif.value,
                                adif.value_len, (int)(sizeof(adif.value)-1));
            }
            // start next field
            adif.ps = ADIFPS_STARTFIELD;
        }
        break;

    case ADIFPS_SKIPTOEOR:
        // just keep looking for <eor> in adif.name, start fresh spot when find it
        if (c == '>') {
            if (adif.name_seen < sizeof(adif.name)) {
                adif.name[adif.name_seen] = '\0';
                if (strcasecmp (adif.name, "EOR") == 0)
                    adif.ps = ADIFPS_STARTSPOT;
            }
            adif.name_seen = 0;
        } else if (c == '<') {
            adif.name_seen = 0;
        } else if (adif.name_seen < sizeof(adif.name)-1)
            adif.name[adif.name_seen++] = c;
        break;
    }

    // return whether finished
    return (adif.ps == ADIFPS_FINISHED);
}






/***********************************************************************************************************
 *
 * ADIF pane and mapping control
 *
 ***********************************************************************************************************/

/* save sort and file name
 */
static void saveADIFSettings (const char *fn)
{
    NVWriteUInt8 (NV_ADIFSORT, (uint8_t) adif_sort);
    setADIFFilename (fn);                               // changes persistent value managed by setup
}

/* load sort
 */
static void loadADIFSettings (void)
{
    uint8_t sort;
    if (!NVReadUInt8 (NV_ADIFSORT, &sort))
        adif_sort = ADS_AGE;                            // default sort by age
    else
        adif_sort = (ADIFSorts) sort;
}

/* draw all currently visible spots then update scroll markers
 */
static void drawAllVisADIFSpots (const SBox &box)
{
    drawVisibleSpots (WLID_ADIF, adif_spots, adif_ss, box, ADIF_COLOR);
}

/* shift the visible list up, if appropriate
 */
static void scrollADIFUp (const SBox &box)
{
    if (adif_ss.okToScrollUp ()) {
        adif_ss.scrollUp ();
        drawAllVisADIFSpots (box);
    }
}

/* shift the visible list down, if appropriate
 */
static void scrollADIFDown (const SBox &box)
{
    if (adif_ss.okToScrollDown()) {
        adif_ss.scrollDown ();
        drawAllVisADIFSpots (box);
    }
}

static void resetADIFMem(void)
{
    free (adif_spots);
    adif_spots = NULL;
    adif_ss.n_data = 0;
}

/* draw complete ADIF pane in the given box.
 * also indicate if any were removed from the list based on n_adif_bad.
 * if only want to show new spots, such as when scrolling, just call drawAllVisADIFSpots()
 */
void drawADIFPane (const SBox &box, const char *filename)
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

    // show fn with counts if they fit 
    int max_chw = (box.w-4)/6;                  // font is 6 pixels wide
    char info[200];
    int info_l;
    if (n_adif_bad > 0)
        info_l = snprintf (info, sizeof(info), "%s %d-%d", fn_basename, adif_ss.n_data+n_adif_bad,n_adif_bad);
    else
        info_l = snprintf (info, sizeof(info), "%s %d", fn_basename, adif_ss.n_data);
    if (info_l > max_chw)
        info_l = snprintf (info, max_chw, "%s", fn_basename);

    // center
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    uint16_t iw = getTextWidth(info);
    tft.setCursor (box.x + (box.w-iw)/2, box.y + SUBTITLE_Y0);
    tft.print (info);

    // draw spots
    drawAllVisADIFSpots (box);
}

/* add another spot to adif_spots[]
 */
static void addADIFSpot (DXSpot &spot)
{
    // more likely to make a separate location on the map
    ditherLL (spot.tx_ll);

    // insure room
    adif_spots = (DXSpot *) realloc (adif_spots, (adif_ss.n_data + 1) * sizeof(DXSpot));
    if (!adif_spots)
        fatalError ("ADIF: no memory for %d spots\n", adif_ss.n_data + 1);

    // place new spot at end
    adif_spots[adif_ss.n_data++] = spot;
}

/* test whether the given file name is suitable for ADIF, else return brief reason.
 */
bool checkADIFFilename (const char *fn, char *ynot, size_t n_ynot)
{
    // at all?
    if (!fn) {
        snprintf (ynot, n_ynot, "no file");
        return (false);
    }
    // edge tests
    if (strchr (fn, ' ')) {
        snprintf (ynot, n_ynot, "no blanks");
        return (false);
    }
    size_t fn_l = strlen (fn);
    if (fn_l == 0) {
        snprintf (ynot, n_ynot, "empty");
        return (false);
    }
    if (fn_l > NV_ADIFFN_LEN-1) {
        snprintf (ynot, n_ynot, "too long");
        return (false);
    }

    // worth trying for real
    char *fn_exp = expandENV (fn);
    FILE *fp = fopen (fn_exp, "r");
    bool ok = fp != NULL;
    if (ok)
        fclose (fp);
    else {
        snprintf (ynot, n_ynot, "open failed");
        Serial.printf ("ADIF: %s %s\n", fn_exp, strerror(errno));
    }
    free (fn_exp);
    return (ok);
}

/* callback for testing adif file name in the given MenuText->text
 */
static bool testADIFFilename (struct _menu_text *tfp, char ynot[], size_t n_ynot)
{
    return (checkADIFFilename (tfp->text, ynot, n_ynot));
}


/* run the ADIF menu
 */
static void runADIFMenu (const SBox &box)
{
    // set up the MENU_TEXT watch list field -- N.B. must free wl_mt.text
    MenuText wl_mt;                                             // watch list field
    char wl_state[WLA_MAXLEN];                                  // wl state, menu may change
    setupWLMenuText (WLID_ADIF, wl_mt, box, wl_state);

    // set up the MENU_TEXT file name field -- N.B. must free fn_mt.text
    MenuText fn_mt;                                             // file name field
    memset (&fn_mt, 0, sizeof(fn_mt));
    fn_mt.text = (char *) calloc (NV_ADIFFN_LEN, 1);            // full length text - N.B. must free!
    fn_mt.t_mem = NV_ADIFFN_LEN;                                // total text memory available
    snprintf (fn_mt.text, NV_ADIFFN_LEN,"%s",getADIFilename()); // init displayed file name
    fn_mt.label = strdup("File:");                              // field label -- N.B. must free!
    fn_mt.l_mem = strlen(fn_mt.label) + 1;                      // max label len
    fn_mt.text_fp = testADIFFilename;                           // file name test
    fn_mt.c_pos = fn_mt.w_pos = 0;                              // start at left

    // fill column-wise

    #define AM_INDENT  1

    // fill column-wise
    MenuItem mitems[] = {
        {MENU_LABEL,                false, 0, AM_INDENT, "Sort:"},                      // 0
        {MENU_BLANK,                false, 0, AM_INDENT, NULL },                        // 1
        {MENU_1OFN, adif_sort == ADS_AGE,  1, AM_INDENT, "Age"},                        // 2
        {MENU_1OFN, adif_sort == ADS_BAND, 1, AM_INDENT, "Band"},                       // 3
        {MENU_1OFN, adif_sort == ADS_CALL, 1, AM_INDENT, "Call"},                       // 4
        {MENU_1OFN, adif_sort == ADS_DIST, 1, AM_INDENT, "Dist"},                       // 5
        {MENU_TEXT,                 false, 3, AM_INDENT, wl_state, &wl_mt},             // 6
        {MENU_TEXT,                 false, 4, AM_INDENT, fn_mt.label, &fn_mt},          // 7
    };
    #define MI_N NARRAY(mitems)

    SBox menu_b;
    menu_b.x = box.x + 3;
    menu_b.y = box.y + 40;
    menu_b.w = box.w - 6;

    SBox ok_b;
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_CANCELOK, 3, MI_N, mitems};
    if (runMenu (menu)) {

        // new sort
        if (mitems[2].set)
            adif_sort = ADS_AGE;
        else if (mitems[5].set)
            adif_sort = ADS_DIST;
        else if (mitems[4].set)
            adif_sort = ADS_CALL;
        else if (mitems[3].set)
            adif_sort = ADS_BAND;
        else
            fatalError ("runADIFMenu no sort set");

        // must recompile to update WL but runMenu already insured wl compiles ok
        char ynot[100];
        if (lookupWatchListState (wl_mt.label) != WLA_OFF
                                        && !compileWatchList (WLID_ADIF, wl_mt.text, ynot, sizeof(ynot)))
            fatalError ("ADIF failed recompling wl %s: %s", wl_mt.text, ynot);
        setWatchList (WLID_ADIF, wl_mt.label, wl_mt.text);
        Serial.printf ("ADIF: set WL to %s %s\n", wl_mt.label, wl_mt.text);

        // save
        Serial.printf ("ADIF: new file name from menu: %s\n", fn_mt.text);
        saveADIFSettings (fn_mt.text);

        // refresh pane to engage choices
        scheduleNewPlot (PLOT_CH_ADIF);
    } else {
        // cancelled so maintain current display
        drawADIFPane (box, getADIFilename());
    }

    // clean up 
    free (wl_mt.text);
    free (fn_mt.text);
    free (fn_mt.label);

}

/* freshen the ADIF file if used and necessary then update pane if in use.
 * io errors are shown in pane else map
 * leave with newfile_pending set if file changed but not in a position to show new entries.
 */
void freshenADIFFile (void)
{
    // web files just sit until menu is run again
    if (showing_set_adif)
        return;

    // check for new/changed file
    const char *fn = getADIFilename();          // file name -- can't be NULL see plotChoiceIsAvailable()
    if (!fn)
        return;
    char *fn_exp = expandENV (fn);              // malloced expanded file name -- N.B. mustfree
    if (fsig.fileChanged (fn_exp))              // latch if flag file changes
        newfile_pending = true;

    // read file is newer and not scrolled away
    if (newfile_pending && adif_ss.atNewest()) {
        PlotPane pp = findPaneChoiceNow (PLOT_CH_ADIF);
        FILE *fp = fopen (fn_exp, "r");
        if (!fp) {
            char errmsg[100];
            snprintf (errmsg, sizeof(errmsg), "%s: %s", fn_exp, strerror(errno));
            if (pp == PANE_NONE)
                mapMsg (4000, "%s", errmsg);
            else
                plotMessage (plot_b[pp], RA8875_RED, errmsg);
        } else {
            GenReader gr(fp);
            int n_good;
            loadADIFFile (gr, 0, n_good, n_adif_bad);
            fclose (fp);

            // update list if showing
            if (pp != PANE_NONE)
                drawADIFPane (plot_b[pp], getADIFilename());

            // caught up
            newfile_pending = false;
        }
        showing_set_adif = false;
    }

    // clean up our malloc
    free (fn_exp);
}

/* replace adif_spots with those found in the given GenReader.
 * read gr_len bytes else until EOF if 0.
 * pass back number of qualifying spots and bad spots found.
 * N.B. caller must close gr
 * N.B. we set n_adif_bad for drawADIFPane()
 * N.B. unless loading directly from the web, you probably want freshenADIFFile().
 */
void loadADIFFile (GenReader &gr, long gr_len, int &n_good, int &n_bad)
{
    // restart list and insure settings are laoded
    resetADIFMem();
    loadADIFSettings();

    // init counts
    n_good = n_bad = 0;

    // curious how long this takes
    struct timeval t0;
    gettimeofday (&t0, NULL);

    // crack file
    DXSpot spot;
    ADIFParser adif;
    adif.ps = ADIFPS_STARTFILE;
    char c;
    long bytes_read = 0;
    int n_tot = 0;
    if (debugLevel (DEBUG_ADIF, 1))
        Serial.printf ("ADIF: WL DE_Call   Grid   DXCC  DX_Call   Grid   DXCC    Lat   Long Mode      kHz\n");
    while ((!gr_len || bytes_read++ < gr_len) && gr.getChar(&c)) {
        if (parseADIF (c, adif, spot)) {
            // spot parsing complete
            if (spotLooksGood (adif, spot)) {
                // at this point all spot fields are complete but may not qualify WL
                bool wl_ok = checkWatchListSpot(WLID_ADIF, spot) != WLS_NO;
                if (wl_ok)
                    addADIFSpot (spot);
                if ((wl_ok && debugLevel (DEBUG_ADIF, 1)) || (!wl_ok && debugLevel (DEBUG_ADIF, 2))) {
                    Serial.printf("ADIF: %s %-9.9s %-6.6s %4d  %-9.9s %-6.6s %4d  %5.1f %6.1f %4.4s %8.1f\n", 
                        wl_ok ? "OK" : "NO",
                        spot.rx_call, spot.rx_grid, spot.rx_dxcc,
                        spot.tx_call, spot.tx_grid, spot.tx_dxcc,
                        spot.tx_ll.lat_d, spot.tx_ll.lng_d, spot.mode, spot.kHz);
                }
            } else
                n_bad++;        // count actual broken spots, not ones that just aren't selected by WL

            // count and look alive
            if ((++n_tot%100) == 0)
                updateClocks(false);
        }
    }

    if (debugLevel (DEBUG_ADIF, 1)) {
        struct timeval t1;
        gettimeofday (&t1, NULL);
        Serial.printf ("ADIF: read %d spots in %ld usec\n", n_tot, TVDELUS (t0, t1));
    }

    // report
    n_good = adif_ss.n_data;
    n_adif_bad = n_bad;
    Serial.printf ("ADIF: %s contained %d total %d qualifying %d busted spots\n",
                                    gr.isFile() ? getADIFilename() : "set_adif", n_tot, n_good, n_bad);

    // sort and prep for display
    qsort (adif_spots, adif_ss.n_data, sizeof(DXSpot), adif_pqsf[adif_sort]);
    adif_ss.scrollToNewest();

    // note new source type ready
    showing_set_adif = !gr.isFile();
}

/* called frequently to check for new ADIF records.
 * refresh pane if requested or file changes.
 */
void updateADIF (const SBox &box, bool refresh)
{
    // restart if fresh
    if (refresh) {
        resetADIFMem();
        adif_ss.init ((box.h - LISTING_Y0)/LISTING_DY, 0, 0);
        adif_ss.initNewSpotsSymbol (box, ADIF_COLOR);
        adif_ss.scrollToNewest();
        showing_set_adif = false;
        newfile_pending = true;
    }

    // check for changed file
    freshenADIFFile();

    // update symbol and hold
    adif_ss.drawNewSpotsSymbol (box, newfile_pending, false);
    if (adif_ss.atNewest())
        ROTHOLD_CLR(PLOT_CH_ADIF);
    else
        ROTHOLD_SET(PLOT_CH_ADIF);
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
        drawSpotLabelOnMap (si, LOME_TXEND, LOMD_ALL);
        drawSpotLabelOnMap (si, LOME_RXEND, LOMD_ALL);
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

        // New spots symbol?
        if (adif_ss.checkNewSpotsTouch (s, box)) {
            if (!adif_ss.atNewest() && newfile_pending) {
                // scroll to newest, let updateADIF do the rest
                adif_ss.drawNewSpotsSymbol (box, true, true);   // immediate feedback
                adif_ss.scrollToNewest();
            }
            return (true);                                      // claim our even if not showing
        }

        // on hold?
        if (ROTHOLD_TST(PLOT_CH_ADIF))
            return (true);

        // else in title
        return (false);
    }

    else if (s.y < box.y + SUBTITLE_Y0 + 10) {

        runADIFMenu(box);
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
    return (adif_spots && findPaneForChoice(PLOT_CH_ADIF) != PANE_NONE
                && getClosestSpot (adif_spots, adif_ss.n_data, LOME_BOTH, ll, sp, llp));
}


/* return spot in our pane if under ms 
 */
bool getADIFPaneSpot (const SCoord &ms, DXSpot *dxs, LatLong *ll)
{
    // done if ms not showing our pane or not in our box
    PlotPane pp = findPaneChoiceNow (PLOT_CH_ADIF);
    if (pp == PANE_NONE)
        return (false);
    if (!inBox (ms, plot_b[pp]))
        return (false);

    // create box that will be placed over each listing entry
    SBox listrow_b;
    listrow_b.x = plot_b[pp].x;
    listrow_b.w = plot_b[pp].w;
    listrow_b.h = LISTING_DY;

    // scan listed spots for one located at ms
    uint16_t y0 = plot_b[pp].y + LISTING_Y0;
    int min_i, max_i;
    if (adif_ss.getVisIndices (min_i, max_i) > 0) {
        for (int i = min_i; i <= max_i; i++) {
            listrow_b.y = y0 + adif_ss.getDisplayRow(i) * LISTING_DY;
            if (inBox (ms, listrow_b)) {
                // ms is over this spot
                *dxs = adif_spots[i];
                *ll = dxs->tx_ll;
                return (true);
            }
        }
    }

    // none
    return (false);
}

/* return whether the given spot matches all the given tests for any ADIF spot.
 * N.B. check is limited to spots on ADIF watchlist if any.
 * N.B. if no tests are specified we always return true.
 * N.B. do NOT call freshenADIFFile() here -- it causes an inf loop
 */
bool onADIFList (const DXSpot &spot, bool chk_dxcc, bool chk_grid, bool chk_pref, bool chk_band)
{
    // prep spot
    HamBandSetting spot_band = findHamBand (spot.kHz);
    char spot_pref[MAX_PREF_LEN];
    findCallPrefix (spot.tx_call, spot_pref);

    // compare with each 
    for (int i = 0; i < adif_ss.n_data; i++) {
        DXSpot &adif_spot = adif_spots[i];
        bool pref_match = false;
        if (chk_pref) {
            char adif_pref[MAX_PREF_LEN];
            findCallPrefix (adif_spot.tx_call, adif_pref);
            pref_match = strcasecmp (spot_pref, adif_pref) == 0;
        }
        if (       (!chk_pref || pref_match)
                && (!chk_dxcc || spot.tx_dxcc == adif_spot.tx_dxcc)
                && (!chk_band || spot_band == findHamBand (adif_spot.kHz))
                && (!chk_grid || strncasecmp (spot.tx_grid, adif_spot.tx_grid, 4) == 0))
            return (true);
    }

    return (false);
}
