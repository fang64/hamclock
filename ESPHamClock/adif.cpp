/* support for the ADIF file pane and mapping.
 * based on https://www.adif.org/314/ADIF_314.htm
 * pane layout and operations are the same as dxcluster.
 */

#include "HamClock.h"

bool from_set_adif;                                     // set when spots are loaded via RESTful set_adif

static bool verbose = false;                            // debug info -- RBF

#define ADIF_COLOR      RGB565 (255,228,225)            // misty rose

static DXSpot *adif_spots;                              // malloced
static ScrollState adif_ss;                             // scroll controller, n_data is count
static int n_read_bad;                                  // n_bad across invocations of readADIFFile()


// tables of sort enum ids and corresponding qsort compare functions -- use X trick to insure in sync

#define _ADIF_SORTS             \
    X(ADS_AGE, qsDXCSpotted)    \
    X(ADS_DIST, qsDXCDist)      \

#define X(a,b) a,                                       // expands X to each enum value and comma
typedef enum {
    _ADIF_SORTS
} ADIFSorts;                                            // enum of each sort function
#undef X

#define X(a,b) b,                                       // expands X to each function pointer and comma
static PQSF adif_pqsf[] = {                             // sort functions, in order of ADIFSorts
    _ADIF_SORTS
};
#undef X

static ADIFSorts adif_sort;                             // current sort code index into adif_pqsf


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
            } else
                reset();
            return (changed);
        }

    private:

        // current state
        time_t mtime;                                   // modification time
        long len;                                       // file length

};

static FileSignature fsig;                              // used to decide whether to read file again



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
    AFB_CONTACTED_OP,
    AFB_FREQ,
    AFB_GRIDSQUARE,
    AFB_LAT,
    AFB_LON,
    AFB_MODE,
    AFB_MY_GRIDSQUARE,
    AFB_MY_LAT,
    AFB_MY_LON,
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
        if (verbose)
            Serial.printf ("ADIF trace: parseDT2UNIX(%s, %s) failed\n", date, tim);
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

    if (verbose)
        Serial.printf ("ADIF trace: spotted %s %s -> %ld\n", date, tim, (long)unix);

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

/* strncpy that insures "to" has EOS (and avoids the g++ fussing)
 */
static void quietStrncpy (char *to, const char *from, int len)
{
    snprintf (to, len, "%.*s", len-1, from);
}

/* add a completed ADIF name/value pair to spot and update fields mask if recognized.
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
        ADD_AFB (adif, AFB_MY_GRIDSQUARE);

        if (!maidenhead2ll (spot.rx_ll, adif.value)) {
            Serial.printf ("ADIF: line %d bogus MY_GRIDSQUARE %s\n", adif.line_n, adif.value);
            return (false);
        }

        quietStrncpy (spot.rx_grid, adif.value, sizeof(spot.rx_grid));


    } else if (!strcasecmp (adif.name, "MY_LAT")) {
        ADD_AFB (adif, AFB_MY_LAT);

        if (!parseADIFLocation (adif.value, spot.rx_ll.lat_d)
                                        || spot.rx_ll.lat_d < -90 || spot.rx_ll.lat_d > 90) {
            Serial.printf ("ADIF: line %d bogus MY_LAT %s\n", adif.line_n, adif.value);
            return (false);
        }

    } else if (!strcasecmp (adif.name, "MY_LON")) {
        ADD_AFB (adif, AFB_MY_LON);

        if (!parseADIFLocation (adif.value, spot.rx_ll.lng_d)
                                        || spot.rx_ll.lng_d < -180 || spot.rx_ll.lng_d > 180) {
            Serial.printf ("ADIF: line %d bogus MY_LON %s\n", adif.line_n, adif.value);
            return (false);
        }


    } else if (!strcasecmp (adif.name, "CALL")) {
        ADD_AFB (adif, AFB_CALL);
        quietStrncpy (spot.tx_call, adif.value, sizeof(spot.tx_call));


    } else if (!strcasecmp (adif.name, "CONTACTED_OP")) {
        ADD_AFB (adif, AFB_CONTACTED_OP);
        quietStrncpy (spot.tx_call, adif.value, sizeof(spot.tx_call));


    } else if (!strcasecmp (adif.name, "QSO_DATE")) {
        ADD_AFB (adif, AFB_QSO_DATE);

        if (CHECK_AFB (adif, AFB_TIME_ON)) {
            if (!parseDT2UNIX (adif.value, adif.time_on, spot.spotted)) {
                Serial.printf ("ADIF: line %d bogus QSO_DATE %s or TIME_ON %s\n", adif.line_n,
                                                adif.value, adif.time_on);
                return (false);
            }
        }
        quietStrncpy (adif.qso_date, adif.value, sizeof(adif.qso_date));


    } else if (!strcasecmp (adif.name, "TIME_ON")) {
        ADD_AFB (adif, AFB_TIME_ON);

        if (CHECK_AFB (adif, AFB_QSO_DATE)) {
            if (!parseDT2UNIX (adif.qso_date, adif.value, spot.spotted)) {
                Serial.printf ("ADIF: line %d bogus TIME_ON %s or QSO_DATE %s\n", adif.line_n,
                                                adif.value, adif.qso_date);
                return (false);
            }
        }
        quietStrncpy (adif.time_on, adif.value, sizeof(adif.time_on));


    } else if (!strcasecmp (adif.name, "BAND")) {
        ADD_AFB (adif, AFB_BAND);

        // don't use BAND if FREQ already set
        if (!CHECK_AFB (adif, AFB_FREQ) && !parseADIFBand (adif.value, spot.kHz)) {
            Serial.printf ("ADIF: line %d unknown band %s\n", adif.line_n, adif.value);
            return (false);
        }


    } else if (!strcasecmp (adif.name, "FREQ")) {
        ADD_AFB (adif, AFB_FREQ);

        spot.kHz = 1e3 * atof(adif.value); // ADIF stores MHz
        if (spot.kHz <= 0) {
            Serial.printf ("ADIF: line %d bogus FREQ %s\n", adif.line_n, adif.value);
            return (false);
        }


    } else if (!strcasecmp (adif.name, "MODE")) {
        ADD_AFB (adif, AFB_MODE);
        quietStrncpy (spot.mode, adif.value, sizeof(spot.mode));


    } else if (!strcasecmp (adif.name, "GRIDSQUARE")) {
        ADD_AFB (adif, AFB_GRIDSQUARE);

        if (!maidenhead2ll (spot.tx_ll, adif.value)) {
            Serial.printf ("ADIF: line %d bogus GRIDSQUARE %s\n", adif.line_n, adif.value);
            return (false);
        }

        quietStrncpy (spot.tx_grid, adif.value, sizeof(spot.tx_grid));


    } else if (!strcasecmp (adif.name, "LAT")) {
        ADD_AFB (adif, AFB_LAT);

        if (!parseADIFLocation (adif.value, spot.tx_ll.lat_d)
                                        || spot.tx_ll.lat_d < -90 || spot.tx_ll.lat_d > 90) {
            Serial.printf ("ADIF: line %d bogus LAT %s\n", adif.line_n, adif.value);
            return (false);
        }


    } else if (!strcasecmp (adif.name, "LON")) {
        ADD_AFB (adif, AFB_LON);

        if (!parseADIFLocation (adif.value, spot.tx_ll.lng_d)
                                        || spot.tx_ll.lng_d < -180 || spot.tx_ll.lng_d > 180) {
            Serial.printf ("ADIF: line %d bogus LON %s\n", adif.line_n, adif.value);
            return (false);
        }

    } else 
        useful_field = false;

    if (verbose) {
        if (useful_field)
            Serial.printf ("ADIF trace: added <%s:%d>%s\n", adif.name, adif.value_seen, adif.value);
        else
            Serial.printf ("ADIF trace: unused field <%s:%d>%s\n", adif.name, adif.value_seen, adif.value);
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
        Serial.printf ("ADIF: line %d No CALL or CONTACTED_OP\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_FREQ) || CHECK_AFB(adif, AFB_BAND)) ) {
        Serial.printf ("ADIF: line %d No FREQ or BAND\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_QSO_DATE)) ) {
        Serial.printf ("ADIF: line %d No QSO_DATE\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_TIME_ON)) ) {
        Serial.printf ("ADIF: line %d No TIME_ON\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif,AFB_GRIDSQUARE) || (CHECK_AFB(adif,AFB_LAT) && CHECK_AFB(adif,AFB_LON))) ) {
        if (!call2LL (spot.tx_call, spot.tx_ll)) {
            Serial.printf ("ADIF: line %d No GRIDSQUARE or LAT and LON and cty lookup for %s failed\n",
                                adif.line_n, spot.tx_call);
            return (false);
        }
        if (verbose)
            Serial.printf ("ADIF trace line %d: cty for %s found %g %g\n", adif.line_n, spot.tx_call,
                        spot.tx_ll.lat_d, spot.tx_ll.lng_d);
    }

    if (!CHECK_AFB(adif, AFB_MODE)) {
        Serial.printf ("ADIF: line %d No MODE\n", adif.line_n);
        return (false);
    }


    if (! (CHECK_AFB(adif, AFB_OPERATOR) || CHECK_AFB(adif, AFB_STATION_CALLSIGN)) )
        quietStrncpy (spot.rx_call, getCallsign(), sizeof(spot.rx_call));


    if (! (CHECK_AFB(adif,AFB_MY_GRIDSQUARE) || (CHECK_AFB(adif,AFB_MY_LAT) && CHECK_AFB(adif,AFB_MY_LON))) ){
        if (!call2LL (spot.rx_call, spot.rx_ll)) {
            Serial.printf ("ADIF: line %d No MY_GRIDSQUARE or MY_LAT and MY_LON and cty lookup for %s failed\n",
                                                                adif.line_n, spot.rx_call);
            return (false);
        }
        if (verbose)
            Serial.printf ("ADIF trace line %d: cty for %s found %g %g\n", adif.line_n, spot.rx_call,
                        spot.rx_ll.lat_d, spot.rx_ll.lng_d);
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
        if (verbose)
            Serial.printf ("ADIF trace: starting new spot search\n");
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
                if (verbose)
                    Serial.printf ("ADIF trace: found EOH\n");
                adif.ps = ADIFPS_STARTSPOT;
            } else if (!strcasecmp (adif.name, "EOR")) {
                // yah!
                adif.ps = ADIFPS_FINISHED;
            } else {
                Serial.printf ("ADIF: line %d no length with field %s", adif.line_n, adif.name);
                adif.ps = ADIFPS_SKIPTOEOR;
            }
        } else if (adif.name_seen > sizeof(adif.name)-1) {
            // too long for name[] but none of the field names we care about will overflow so just skip it
            if (verbose)
                Serial.printf ("ADIF trace line %d: ignoring long name %.*s %d > %d\n", adif.line_n,
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
            if (verbose)
                Serial.printf ("ADIF trace line %d: in type for %s\n", adif.line_n, adif.name);
            adif.ps = ADIFPS_INTYPE;
        } else if (c == '>') {
            // finish value length, start collecting value_len chars for field value unless 0
            adif.value[0] = '\0';
            adif.value_seen = 0;
            if (adif.value_len == 0) {
                adif.ps = ADIFPS_STARTFIELD;
                if (verbose)
                    Serial.printf ("ADIF trace line %d: 0 length data field %s\n", adif.line_n, adif.name);
            } else
                adif.ps = ADIFPS_INVALUE;
        } else if (isdigit(c)) {
            // fold c as int into value_len
            adif.value_len = 10*adif.value_len + (c - '0');
            if (verbose && adif.value_len == 0)
                Serial.printf ("ADIF trace line %d: 0 in length field %s now %d\n", adif.line_n, adif.name,
                                        adif.value_len);
        } else {
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
            } else if (verbose) {
                Serial.printf ("ADIF trace: ignoring long value <%s:%d>%.*s %d > %d\n",
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
    setADIFFilename (fn);
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

static void resetADIFMem(void)
{
    free (adif_spots);
    adif_spots = NULL;
    adif_ss.n_data = 0;
}

/* draw complete ADIF pane in the given box.
 * also indicate if any were removed from the list.
 */
static void drawADIFPane (const SBox &box, const char *filename, int n_bad)
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
    if (n_bad > 0)
        info_l = snprintf (info, sizeof(info), "%s %d-%d", fn_basename, adif_ss.n_data+n_bad, n_bad);
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
    // more likely to make a seprate location on the map
    ditherLL (spot.tx_ll);

    if (verbose)
        Serial.printf ("ADIF trace: new spot: %s %s %s %s %g %g %s %g %ld\n", 
            spot.rx_call, spot.rx_grid, spot.tx_call, spot.tx_grid, spot.tx_ll.lat_d,
            spot.tx_ll.lng_d, spot.mode, spot.kHz, (long)spot.spotted);

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
    else
        snprintf (ynot, n_ynot, "bad file");
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
    fn_mt.w_len = box.w/6 - WLA_MAXLEN - 2;                     // n chars to display, use most of the box

    // fill column-wise

    #define AM_INDENT  4

    MenuItem mitems[5] = {
        {MENU_LABEL,                false, 0, AM_INDENT, "Sort:"},                      // 0
        {MENU_1OFN, adif_sort == ADS_AGE,  1, AM_INDENT, "Age"},                        // 1
        {MENU_1OFN, adif_sort == ADS_DIST, 1, AM_INDENT, "Dist"},                       // 2
        {MENU_TEXT,   false,               3, AM_INDENT, wl_state, &wl_mt},             // 3
        {MENU_TEXT,   false,               4, AM_INDENT, fn_mt.label, &fn_mt},          // 4
    };
    #define MI_N NARRAY(mitems)

    SBox menu_b;
    menu_b.x = box.x + 5;
    menu_b.y = box.y + 40;
    menu_b.w = 0;

    SBox ok_b;
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_CANCELOK, 3, MI_N, mitems};
    if (runMenu (menu)) {

        // new sort
        if (mitems[1].set)
            adif_sort = ADS_AGE;
        else if (mitems[2].set)
            adif_sort = ADS_DIST;
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
        Serial.printf ("ADIF: file name %s\n", fn_mt.text);
        saveADIFSettings (fn_mt.text);
    }

    // refresh pane to engage choices or just to erase menu
    scheduleNewPlot (PLOT_CH_ADIF);

    // clean up 
    free (wl_mt.text);
    free (fn_mt.text);
    free (fn_mt.label);

}

/* replace adif_spots with those found in the given GenReader.
 * read gr_len bytes else until EOF if 0.
 * pass back number of qualifying spots and bad spots found.
 * N.B. caller must close gr
 * N.B. keep a record of n_bad in n_read_bad
 */
void readADIFFile (GenReader &gr, long gr_len, int &n_good, int &n_bad)
{
    // init
    n_good = n_read_bad = 0;

    // restart list
    if (adif_spots)
        free (adif_spots);
    adif_spots = NULL;
    adif_ss.n_data = 0;

    // curious how long this takes
    struct timeval t0, t1;
    gettimeofday (&t0, NULL);

    // crack file
    DXSpot spot;
    ADIFParser adif;
    adif.ps = ADIFPS_STARTFILE;
    char c;
    long bytes_read = 0;
    while ((!gr_len || bytes_read++ < gr_len) && gr.getChar(&c)) {
        if (parseADIF (c, adif, spot)) {
            // spot parsing complete
            if (spotLooksGood (adif, spot)) {
                // spot fields are complete
                if (checkWatchListSpot(WLID_ADIF, spot) != WLS_NO)
                    addADIFSpot (spot);
                else if (verbose)
                    // spot does not qualify
                    Serial.printf ("ADIF trace line %d: no watch: %s %s %s %s %g %g %s %g %ld\n", 
                            adif.line_n, spot.rx_call, spot.rx_grid, spot.tx_call, spot.tx_grid,
                            spot.tx_ll.lat_d, spot.tx_ll.lng_d, spot.mode, spot.kHz, (long)spot.spotted);
            } else
                n_read_bad++;
            updateClocks(false);
        }
    }

    gettimeofday (&t1, NULL);
    if (verbose)
        Serial.printf ("ADIF: file update took %ld usec\n", TVDELUS (t0, t1));

    // report
    n_good = adif_ss.n_data;
    n_bad = n_read_bad;
}

/* called occasionally to show ADIF records.
 * refresh pane if requested or file changes.
 * N.B. support set_adif even if no getADIFilename()
 */
void updateADIF (const SBox &box, bool refresh)
{
    // insure settings are loaded
    loadADIFSettings();

    // get full file name, or show web hint
    const char *fn = getADIFilename();          // file name from Setup
    char *fn_exp;                               // malloced name with any ENV expanded
    if (!fn || from_set_adif) {
        fn = fn_exp = strdup ("set_adif");      // tell user spots are from web command
        from_set_adif = true;                   // user can use set_adif even if no Setup name
    } else {
        fn_exp = expandENV (fn);                // malloced expanded file name
    }

    // error message, [0] set if used
    char errmsg[100] = "";

    // read file unless spots are from_set_adif or unchanged
    bool new_file = false;
    if (from_set_adif) {
        // insure any subsequent file will be seen as new
        fsig.reset();

    } else {

        // read fn_exp unless seems to be unchanged
        new_file = refresh || fsig.fileChanged (fn_exp);
        if (new_file) {
            Serial.printf ("ADIF: reading %s new or changed\n", fn_exp);

            FILE *fp = fopen (fn_exp, "r");
            if (!fp) {
                snprintf (errmsg, sizeof(errmsg), "%s: %s", fn_exp, strerror(errno));
            } else {
                GenReader gr(fp);
                int n_good, n_bad;
                readADIFFile (gr, 0, n_good, n_bad);
                Serial.printf ("ADIF: %s contained %d qualifying %d busted spots\n", fn_exp, n_good, n_bad);
                fclose (fp);
            }
        }
    }

    // show errmsg or update pane if required
    if (errmsg[0])
        plotMessage (box, RA8875_RED, errmsg);
    else if (new_file || refresh) {
        // accommodate possible change in box size
        adif_ss.max_vis = (box.h - LISTING_Y0)/LISTING_DY;
        adif_ss.scrollToNewest();

        // perform desired sort
        qsort (adif_spots, adif_ss.n_data, sizeof(DXSpot), adif_pqsf[adif_sort]);

        // display
        drawADIFPane (box, fn_exp, n_read_bad);
    }

    // clean up our malloc
    free (fn_exp);
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
    return (getClosestSpot (adif_spots, adif_ss.n_data, ll, sp, llp));
}

/* call to clean up memory if not in use, get out fast if nothing to do.
 */
void cleanADIF()
{
    if (adif_spots && findPaneForChoice(PLOT_CH_ADIF) == PANE_NONE)
        resetADIFMem();
}
