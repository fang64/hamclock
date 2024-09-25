/* manage PSKReporter, WSPR and RBN records and drawing.
 */

#include "HamClock.h"



// global state for webserver
uint8_t psk_mask;                               // one of PSKModeBits
uint32_t psk_bands;                             // bitmask of HamBandSetting
uint16_t psk_maxage_mins;                       // query period, minutes
uint8_t psk_showdist;                           // show max distances, else count

// query urls
static const char psk_page[] PROGMEM = "/fetchPSKReporter.pl";
static const char wspr_page[] PROGMEM = "/fetchWSPR.pl";
static const char rbn_page[] PROGMEM = "/fetchRBN.pl";

// color config
#define LIVE_COLOR      RGB565(80,80,255)       // title color

// private state
static DXSpot *reports;                         // malloced list of all reports, not just TST_PSKBAND
static int n_reports;                           // count of reports used in psk_bands, might be < n_malloced
static int n_malloced;                          // total n malloced in reports[]
static int spot_maxrpt[HAMBAND_N];              // indices into reports[] for the farthest spot per band
static PSKBandStats bstats[HAMBAND_N];          // band stats


// handy test and set whether a band is in use
#define SET_PSKBAND(b)  (psk_bands |= (1 << (b)))               // record that band b paths are displayed
#define TST_PSKBAND(b)  ((b) != HAMBAND_NONE && (psk_bands & (1 << (b))) != 0)  // test if band b displayed


/* draw a distance target marker at Raw s with the given fill color.
 */
static void drawDistanceTarget (const SCoord &s, uint16_t fill_color)
{
    // ignore if not enabled
    uint16_t szRaw = getSpotDotRadius();
    if (szRaw == 0)
        return;

    uint16_t cross_color = getGoodTextColor (fill_color);

    // raw looks nicer

    tft.fillCircleRaw (s.x, s.y, szRaw, fill_color);
    tft.drawCircleRaw (s.x, s.y, szRaw, cross_color);
    tft.drawLineRaw (s.x-szRaw, s.y, s.x+szRaw, s.y, 1, cross_color);
    tft.drawLineRaw (s.x, s.y-szRaw, s.x, s.y+szRaw, 1, cross_color);
}

/* return whether the given age, in minutes, is allowed.
 */
bool maxPSKageOk (int m)
{
    return (m==15 || m==30 || m==60 || m==360 || m==1440);
}

/* get NV settings related to PSK
 */
void initPSKState()
{
    if (!NVReadUInt8 (NV_PSK_MODEBITS, &psk_mask)) {
        // default PSK of grid
        psk_mask = PSKMB_PSK | PSKMB_OFDE;
        NVWriteUInt8 (NV_PSK_MODEBITS, psk_mask);
    }
    if (!NVReadUInt32 (NV_PSK_BANDS, &psk_bands)) {
        // default all ham_bands
        psk_bands = 0;
        for (int i = 0; i < HAMBAND_N; i++)
            SET_PSKBAND(i);
        NVWriteUInt32 (NV_PSK_BANDS, psk_bands);
    }
    if (!NVReadUInt16 (NV_PSK_MAXAGE, &psk_maxage_mins) || !maxPSKageOk(psk_maxage_mins)) {
        // default 30 minutes
        psk_maxage_mins = 30;
        NVWriteUInt16 (NV_PSK_MAXAGE, psk_maxage_mins);
    }
    if (!NVReadUInt8 (NV_PSK_SHOWDIST, &psk_showdist)) {
        psk_showdist = 0;
        NVWriteUInt8 (NV_PSK_SHOWDIST, psk_showdist);
    }
}

/* save NV settings related to PSK
 */
void savePSKState()
{
    NVWriteUInt8 (NV_PSK_MODEBITS, psk_mask);
    NVWriteUInt32 (NV_PSK_BANDS, psk_bands);
    NVWriteUInt16 (NV_PSK_MAXAGE, psk_maxage_mins);
    NVWriteUInt8 (NV_PSK_SHOWDIST, psk_showdist);
}

/* draw a target at the farthest spot in each active band as needed.
 */
void drawFarthestPSKSpots ()
{
    // or not
    int tw = getSpotDotRadius();
    if (tw == 0)
        return;

    // proceed unless not in use
    if (findPaneForChoice(PLOT_CH_PSK) == PANE_NONE)
        return;

    for (int i = 0; i < HAMBAND_N; i++) {
        PSKBandStats &pbs = bstats[i];
        if (pbs.maxkm > 0 && TST_PSKBAND(i)) {
            SCoord s;
            ll2s (pbs.maxll, s, tw);
            if (overMap(s)) {
                ll2sRaw (pbs.maxll, s, tw);
                drawDistanceTarget (s, getMapColor(ham_bands[i].cid));
            }
        }
    }
}

/* draw the PSK pane in the given box
 */
static void drawPSKPane (const SBox &box)
{
    // clear
    prepPlotBox (box);

    // handy
    bool use_call = (psk_mask & PSKMB_CALL) != 0;
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;
    bool ispsk = (psk_mask & PSKMB_SRCMASK) == PSKMB_PSK;
    bool iswspr = (psk_mask & PSKMB_SRCMASK) == PSKMB_WSPR;

    // title
    static const char *title = "Live Spots";
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    uint16_t tw = getTextWidth(title);
    tft.setTextColor (LIVE_COLOR);
    tft.setCursor (box.x + (box.w - tw)/2, box.y + PANETITLE_H);
    tft.print (title);

    // set name to call or 4x grid
    char name[20];
    if (use_call) {
        strcpy (name, getCallsign());
    } else {
        char de_maid[MAID_CHARLEN];
        getNVMaidenhead (NV_DE_GRID, de_maid);
        snprintf (name, sizeof(name), "%.4s", de_maid);
    }

    // show how and when
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor (RA8875_WHITE);
    char where_how[100];
    snprintf (where_how, sizeof(where_how), "%s %s - %s %d %s",
                of_de ? "of" : "by", name,
                (ispsk ? "PSK" : (iswspr ? "WSPR" : "RBN")),
                psk_maxage_mins < 60 ? psk_maxage_mins : psk_maxage_mins/60,
                psk_maxage_mins < 60 ? "mins" : (psk_maxage_mins == 60 ? "hour" : "hrs"));
    uint16_t whw = getTextWidth(where_how);
    tft.setCursor (box.x + (box.w-whw)/2, box.y + SUBTITLE_Y0);
    tft.print (where_how);

    // table
    #define TBLHGAP (box.w/20)
    #define TBCOLW (43*box.w/100)
    #define TBLRH (PLOTBOX123_H/HAMBAND_N)      // really each /2 but that loses too much precision
    for (int i = 0; i < HAMBAND_N; i++) {
        int row = i % (HAMBAND_N/2);
        int col = i / (HAMBAND_N/2);
        uint16_t x = box.x + TBLHGAP + col*(TBCOLW+TBLHGAP);
        uint16_t y = box.y + 3*box.h/8 + row*TBLRH;
        char report[30];
        if (psk_showdist) {
            float d = bstats[i].maxkm;
            if (!useMetricUnits())
                d *= MI_PER_KM;
            snprintf (report, sizeof(report), "%3sm %5.0f", ham_bands[i].name, d);
        } else
            snprintf (report, sizeof(report), "%3sm %5d", ham_bands[i].name, bstats[i].count);
        if (TST_PSKBAND(i)) {
            uint16_t map_col = getMapColor(ham_bands[i].cid);
            uint16_t txt_col = getGoodTextColor(map_col);
            tft.fillRect (x, y-LISTING_OS+1, TBCOLW, box.h/14, map_col);
            tft.setTextColor (txt_col);
            tft.setCursor (x+2, y);
            tft.print (report);
        } else {
            // disabled, always show but diminished
            tft.fillRect (x, y-LISTING_OS+1, TBCOLW, box.h/14, RA8875_BLACK);
            tft.setTextColor (GRAY);
            tft.setCursor (x+2, y);
            tft.print (report);
        }
    }

    // caption
    const char *label = psk_showdist ? (useMetricUnits() ? "Max distance (km)" : "Max distance (mi)")
                                     : "Counts";
    uint16_t lw = getTextWidth (label);
    uint16_t tr = getSpotDotRadius();
    uint16_t x = box.x + (box.w-lw)/2;
    if (psk_showdist && tr)
        x -= 2*tr;
    uint16_t y = box.y + box.h - 15;
    tft.setTextColor (RA8875_WHITE);
    tft.setCursor (x, y);
    tft.print (label);

    // show a target example if showing distance
    if (psk_showdist && tr) {
        SCoord s;
        s.x = tft.SCALESZ*(tft.getCursorX() + 3 + tr);
        s.y = tft.SCALESZ*(y + 3);
        drawDistanceTarget (s, RA8875_BLACK);
    }
}

/* retrieve spots into reports[] according to current settings.
 * return whether io ok.
 */
static bool retrievePSK (void)
{
    // get fresh
    WiFiClient psk_client;
    bool ok = false;

    // query type
    bool ispsk = (psk_mask & PSKMB_SRCMASK) == PSKMB_PSK;
    bool iswspr = (psk_mask & PSKMB_SRCMASK) == PSKMB_WSPR;
    bool isrbn = (psk_mask & PSKMB_SRCMASK) == PSKMB_RBN;
    bool use_call = (psk_mask & PSKMB_CALL) != 0;
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;

    // handy 4x DE maid if needed
    char de_maid[MAID_CHARLEN];
    getNVMaidenhead (NV_DE_GRID, de_maid);
    de_maid[4] = '\0';

    // build query
    char query[100];
    if (ispsk)
        strcpy_P (query, psk_page);
    else if (iswspr)
        strcpy_P (query, wspr_page);
    else
        strcpy_P (query, rbn_page);
    int qlen = strlen (query);
    snprintf (query+qlen, sizeof(query)-qlen, "?%s%s=%s&maxage=%d",
                                        of_de ? "of" : "by",
                                        use_call ? "call" : "grid",
                                        use_call ? getCallsign() : de_maid,
                                        psk_maxage_mins*60 /* wants seconds */);
    Serial.printf (_FX("PSK: query: %s\n"), query);

    // fetch and fill reports[]
    resetWatchdog();
    if (wifiOk() && psk_client.connect(backend_host, backend_port)) {
        updateClocks(false);
        resetWatchdog();

        // query web page
        httpHCGET (psk_client, backend_host, query);

        // skip header
        if (!httpSkipHeader (psk_client)) {
            Serial.print (F("PSK: no header\n"));
            goto out;
        }

        // consider io ok
        ok = true;

        // reset lists
        n_reports = 0;
        memset (bstats, 0, sizeof(bstats));

        // read lines -- anything unexpected is considered an error message
        char line[100];
        while (getTCPLine (psk_client, line, sizeof(line), NULL)) {

            // Serial.printf (_FX("PSK: fetched %s\n"), line);

            // parse.
            // N.B. match sscanf sizes with array sizes
            // N.B. first grid/call pair is always TX, second always RX; which is DE depends on PSKMB_OFDE
            DXSpot new_r;
            memset (&new_r, 0, sizeof(new_r));
            long posting_temp;
            long Hz_temp;
            if (sscanf (line, "%ld,%6[^,],%11[^,],%6[^,],%11[^,],%7[^,],%ld,%f", &posting_temp,
                            new_r.tx_grid, new_r.tx_call, new_r.rx_grid, new_r.rx_call,
                            new_r.mode, &Hz_temp, &new_r.snr) != 8) {
                Serial.printf (_FX("PSK: %s\n"), line);
                goto out;
            }
            new_r.spotted = posting_temp;
            new_r.kHz = Hz_temp * 1e-3F;

            // RBN does not provide tx_grid but it must be us. N.B. this will be blank from rbndaemon
            if (isrbn)
                strcpy (new_r.tx_grid, de_maid);

            // convert grids to ll
            if (!maidenhead2ll (new_r.tx_ll, new_r.tx_grid)) {
                Serial.printf (_FX("PSK: RX grid? %s\n"), line);
                continue;
            }
            if (!maidenhead2ll (new_r.rx_ll, new_r.rx_grid)) {
                Serial.printf (_FX("PSK: RX grid? %s\n"), line);
                continue;
            }

            // check for unknown or unsupported band
            const HamBandSetting band = findHamBand (1000*new_r.kHz);
            if (band == HAMBAND_NONE) {
                Serial.printf (_FX("PSK: band? %s\n"), line);
                continue;
            }

            // update stats for this band
            PSKBandStats &pbs = bstats[band];

            // update count of this band
            pbs.count++;

            // dither ll for unique selection
            ditherLL (new_r.tx_ll);
            ditherLL (new_r.rx_ll);

            // finally! save new report, grow array if out of room
            if ( !(n_reports < n_malloced) ) {
                reports = (DXSpot *) realloc (reports, (n_malloced += 100) * sizeof(DXSpot));
                if (!reports)
                    fatalError (_FX("Live Spots: no mem %d"), n_malloced);
            }
            reports[n_reports] = new_r;         // N.B. do not inc yet, used last

            // check each end for farthest from DE
            float tx_dist, rx_dist, bearing;        
            propDEPath (false, new_r.tx_ll, &tx_dist, &bearing);
            propDEPath (false, new_r.rx_ll, &rx_dist, &bearing);
            tx_dist *= KM_PER_MI * ERAD_M;                         // convert core angle to surface km
            rx_dist *= KM_PER_MI * ERAD_M;                         // convert core angle to surface km
            bool tx_gt_rx = (tx_dist > rx_dist);
            float max_dist = tx_gt_rx ? tx_dist : rx_dist;
            if (max_dist > pbs.maxkm) {

                // update pbs for this band with farther spot
                LatLong max_ll = tx_gt_rx ? new_r.tx_ll : new_r.rx_ll;
                const char *call = tx_gt_rx ? new_r.tx_call : new_r.rx_call;
                pbs.maxkm = max_dist;
                pbs.maxll = max_ll;
                if (getSpotLabelType() == LBL_PREFIX)
                    findCallPrefix (call, pbs.maxcall);
                else
                    strcpy (pbs.maxcall, call);

                // newest spot is now farthest for this band
                spot_maxrpt[band] = n_reports;
            }

            // ok, another report
            n_reports++;
        }

    } else
        Serial.print (F("PSK: Spots connection failed\n"));

out:
    // reset counts if trouble
    if (!ok) {
        n_reports = 0;
        for (int i = 0; i < HAMBAND_N; i++) {
            bstats[i].count = -1;
            bstats[i].maxkm = -1;
        }
    }

    // finish up
    psk_client.stop();
    Serial.printf (_FX("PSK: found %d %s reports %s %s\n"),
                        n_reports,
                        (ispsk ? "PSK" : (iswspr ? "WSPR" : "RBN")),
                        of_de ? "of" : "by",
                        use_call ? getCallsign() : de_maid);

    // already logged any problems
    return (ok);
}

/* query PSK reporter etc for new reports, draw results and return whether all ok
 */
bool updatePSKReporter (const SBox &box, bool force)
{
    // save last retrieval settings to know whether reports[] can be reused
    static time_t next_update;                          // don't update faster than PSK_INTERVAL
    static uint8_t my_psk_mask;                         // setting used for reports[]
    static uint32_t my_psk_bands;                       // setting used for reports[]
    static uint16_t my_psk_maxage_mins;                 // setting used for reports[]
    static bool last_ok;                                // used to force retry

    // just use cache if settings all match and not too old
    if (!force && last_ok && reports && n_malloced > 0 && myNow() < next_update
                            && my_psk_mask == psk_mask && my_psk_maxage_mins == psk_maxage_mins
                            && my_psk_bands == psk_bands) {
        drawPSKPane (box);
        return (true);
    }

    // save settings
    my_psk_mask = psk_mask;
    my_psk_maxage_mins = psk_maxage_mins;
    my_psk_bands = psk_bands;
    next_update = myNow() + PSK_INTERVAL;;

    // get fresh
    last_ok = retrievePSK();

    // display whatever we got regardless
    drawPSKPane (box);

    // reply
    return (last_ok);
}

/* check for tap at s known to be within a PLOT_CH_PSK box.
 * return whether it was ours.
 */
bool checkPSKTouch (const SCoord &s, const SBox &box)
{
    // done if tap title
    if (s.y < box.y + PANETITLE_H)
        return (false);

    // handy current state
    bool ispsk = (psk_mask & PSKMB_SRCMASK) == PSKMB_PSK;
    bool iswspr = (psk_mask & PSKMB_SRCMASK) == PSKMB_WSPR;
    bool isrbn = (psk_mask & PSKMB_SRCMASK) == PSKMB_RBN;
    bool use_call = (psk_mask & PSKMB_CALL) != 0;
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;
    bool show_dist = psk_showdist != 0;

    // menu
    #define PRI_INDENT 2
    #define SEC_INDENT 12
    #define MI_N (HAMBAND_N + 18)                                // ham_bands + controls
    MenuItem mitems[MI_N];

    // runMenu() expects column-major entries

    mitems[0] = {MENU_1OFN,  isrbn, 1, PRI_INDENT, "RBN"};
    mitems[1] = {MENU_LABEL, false, 0, PRI_INDENT, "Spot:"};
    mitems[2] = {MENU_LABEL, false, 0, PRI_INDENT, "What:"};
    mitems[3] = {MENU_LABEL, false, 0, PRI_INDENT, "Show:"};
    mitems[4] = {MENU_LABEL, false, 5, PRI_INDENT, "Age:"};
    mitems[5] = {MENU_1OFN,  false, 6, 5, "1 hr"};
    mitems[6] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_160M), 4, SEC_INDENT, ham_bands[HAMBAND_160M].name};
    mitems[7] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_80M),  4, SEC_INDENT, ham_bands[HAMBAND_80M].name};
    mitems[8] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_60M),  4, SEC_INDENT, ham_bands[HAMBAND_60M].name};
    mitems[9] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_40M),  4, SEC_INDENT, ham_bands[HAMBAND_40M].name};

    mitems[10] = {MENU_1OFN, ispsk,     1, PRI_INDENT, "PSK"};
    mitems[11] = {MENU_1OFN, of_de,     2, PRI_INDENT, "of DE"};
    mitems[12] = {MENU_1OFN, use_call,  3, PRI_INDENT, "Call"};
    mitems[13] = {MENU_1OFN, show_dist, 7, PRI_INDENT, "MaxDst"};
    mitems[14] = {MENU_1OFN, false,     6, PRI_INDENT, "15 min"};
    mitems[15] = {MENU_1OFN, false,     6, PRI_INDENT, "6 hrs"};
    mitems[16] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_30M),  4, SEC_INDENT, ham_bands[HAMBAND_30M].name};
    mitems[17] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_20M),  4, SEC_INDENT, ham_bands[HAMBAND_20M].name};
    mitems[18] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_17M),  4, SEC_INDENT, ham_bands[HAMBAND_17M].name};
    mitems[19] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_15M),  4, SEC_INDENT, ham_bands[HAMBAND_15M].name};

    mitems[20] = {MENU_1OFN, iswspr,    1, PRI_INDENT, "WSPR"};
    mitems[21] = {MENU_1OFN, !of_de,    2, PRI_INDENT, "by DE"};
    mitems[22] = {MENU_1OFN, !use_call, 3, PRI_INDENT, "Grid"};
    mitems[23] = {MENU_1OFN, !show_dist,7, PRI_INDENT, "Count"};
    mitems[24] = {MENU_1OFN, false,     6, PRI_INDENT, "30 min"};
    mitems[25] = {MENU_1OFN, false,     6, PRI_INDENT, "24 hrs"};
    mitems[26] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_12M),  4, SEC_INDENT, ham_bands[HAMBAND_12M].name};
    mitems[27] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_10M),  4, SEC_INDENT, ham_bands[HAMBAND_10M].name};
    mitems[28] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_6M),   4, SEC_INDENT, ham_bands[HAMBAND_6M].name};
    mitems[29] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_2M),   4, SEC_INDENT, ham_bands[HAMBAND_2M].name};

    // set age
    switch (psk_maxage_mins) {
    case 15:   mitems[14].set = true; break;
    case 30:   mitems[24].set = true; break;
    case 60:   mitems[5].set  = true; break;
    case 360:  mitems[15].set = true; break;
    case 1440: mitems[25].set = true; break;
    default:   fatalError (_FX("Bad psk_maxage_mins: %d"), psk_maxage_mins);
    }

    // create a box for the menu
    SBox menu_b;
    menu_b.x = box.x+9;
    menu_b.y = box.y + 5;
    menu_b.w = 0;               // shrink to fit

    // run
    SBox ok_b;
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_CANCELOK, 3, MI_N, mitems};
    if (runMenu (menu)) {

        // handy
        bool psk_set = mitems[10].set;
        bool wspr_set = mitems[20].set;
        bool rbn_set = mitems[0].set;
        bool ofDE_set = mitems[11].set;
        bool call_set = mitems[12].set;

        // RBN only works with ofcall
        if (rbn_set && (!ofDE_set || !call_set)) {

            // show error briefly then restore existing settings
            plotMessage (box, RA8875_RED, _FX("RBN requires \"of DE\" and \"Call\""));
            wdDelay (5000);
            drawPSKPane(box);

        } else {

            // set new mode mask;
            psk_mask = psk_set ? PSKMB_PSK : (wspr_set ? PSKMB_WSPR : PSKMB_RBN);
            if (ofDE_set)
                psk_mask |= PSKMB_OFDE;
            if (call_set)
                psk_mask |= PSKMB_CALL;

            // set new ham_bands
            psk_bands = 0;
            if (mitems[6].set)  SET_PSKBAND(HAMBAND_160M);
            if (mitems[7].set)  SET_PSKBAND(HAMBAND_80M);
            if (mitems[8].set)  SET_PSKBAND(HAMBAND_60M);
            if (mitems[9].set)  SET_PSKBAND(HAMBAND_40M);
            if (mitems[16].set) SET_PSKBAND(HAMBAND_30M);
            if (mitems[17].set) SET_PSKBAND(HAMBAND_20M);
            if (mitems[18].set) SET_PSKBAND(HAMBAND_17M);
            if (mitems[19].set) SET_PSKBAND(HAMBAND_15M);
            if (mitems[26].set) SET_PSKBAND(HAMBAND_12M);
            if (mitems[27].set) SET_PSKBAND(HAMBAND_10M);
            if (mitems[28].set) SET_PSKBAND(HAMBAND_6M);
            if (mitems[29].set) SET_PSKBAND(HAMBAND_2M);

            // get new age
            if (mitems[14].set)
                psk_maxage_mins = 15;
            else if (mitems[24].set)
                psk_maxage_mins = 30;
            else if (mitems[5].set)
                psk_maxage_mins = 60;
            else if (mitems[15].set)
                psk_maxage_mins = 360;
            else if (mitems[25].set)
                psk_maxage_mins = 1440;
            else
                fatalError (_FX("PSK: No menu age"));

            // get how to show
            psk_showdist = mitems[13].set;

            // persist
            savePSKState();

            // refresh with new criteria
            updatePSKReporter (box, true);
        }

    } else  {

        // just restore current settings
        drawPSKPane(box);
    }

    // ours alright
    return (true);
}

/* return current stats, if active
 */
bool getPSKBandStats (PSKBandStats stats[HAMBAND_N], const char *names[HAMBAND_N])
{
    if (findPaneForChoice(PLOT_CH_PSK) == PANE_NONE)
        return (false);

    // copy but zero out entries with 0 count
    memcpy (stats, bstats, sizeof(PSKBandStats) * HAMBAND_N);
    for (int i = 0; i < HAMBAND_N; i++) {
        if (bstats[i].count == 0) {
            stats[i].maxkm = 0;
            memset (&stats[i].maxll, 0, sizeof(stats[i].maxll));
        }
        names[i] = ham_bands[i].name;
    }

    return (true);
}



/* draw the current set of spot paths in reports[] if enabled
 */
void drawPSKPaths ()
{
    // ignore if not in any rotation set
    if (findPaneForChoice(PLOT_CH_PSK) == PANE_NONE)
        return;

    // which end to mark
    LabelOnMapEnd lom = (psk_mask & PSKMB_OFDE) ? LOME_RXEND : LOME_TXEND;

    if (psk_showdist) {

        // just show the longest path in each band
        for (int i = 0; i < HAMBAND_N; i++) {
            if (bstats[i].maxkm > 0 && TST_PSKBAND(i)) {
                drawSpotPathOnMap (reports[spot_maxrpt[i]]);
                drawSpotLabelOnMap (reports[spot_maxrpt[i]], lom, LOMD_ALL);
            }
        }

    } else {

        // show all paths first
        for (int i = 0; i < n_reports; i++) {
            DXSpot &s = reports[i];
            if (TST_PSKBAND(findHamBand(1000*s.kHz)))
                drawSpotPathOnMap (s);
        }

        // then label all without text
        for (int i = 0; i < n_reports; i++) {
            // N.B. we know band in all reports[] are ok
            DXSpot &s = reports[i];
            if (TST_PSKBAND(findHamBand(1000*s.kHz)))
                drawSpotLabelOnMap (s, LOME_BOTH, LOMD_JUSTDOT);
        }

        // then finally label only the farthest with text
        for (int i = 0; i < HAMBAND_N; i++)
            if (bstats[i].maxkm > 0 && TST_PSKBAND(i))
                drawSpotLabelOnMap (reports[spot_maxrpt[i]], lom, LOMD_ALL);
    }
}

/* return spot closest to ll if appropriate.
 * also indicate whether these are of_de spots
 */
bool getClosestPSK (const LatLong &ll, DXSpot *sp, bool &of_de)
{
    // ignore if not in any rotation set
    if (findPaneForChoice(PLOT_CH_PSK) == PANE_NONE)
        return (false);

    // report which way
    of_de = (psk_mask & PSKMB_OFDE) != 0;

    // find closest spot -- kd3tree would consider all spots even if not being shown, linear is fine.
    DXSpot *return_sp = NULL;

    if (psk_showdist) {

        // just check bstats if only showing farthest spots

        float min_d = 0;
        int min_i = -1;
        for (int i = 0; i < HAMBAND_N; i++) {
            if (TST_PSKBAND(i)) {
                float d = simpleSphereDist (ll, bstats[i].maxll);
                if (min_i < 0 || d < min_d) {
                    min_d = d;
                    min_i = i;
                }
            }
        }

        if (min_i >= 0 && min_d*ERAD_M < MAX_CSR_DIST)
            return_sp = &reports[spot_maxrpt[min_i]];
    
    } else {

        // check all spots in displayed ham_bands.
        // N.B. can't use getClosestSpot() because of TST_PSKBAND

        float min_d = 0;
        int min_i = -1;
        for (int i = 0; i < n_reports; i++) {
            DXSpot &s = reports[i];
            if (TST_PSKBAND(findHamBand(1000*s.kHz))) {
                float d = simpleSphereDist (ll, s.rx_ll);
                if (min_i < 0 || d < min_d) {
                    min_d = d;
                    min_i = i;
                }
                d = simpleSphereDist (ll, s.tx_ll);
                if (min_i < 0 || d < min_d) {
                    min_d = d;
                    min_i = i;
                }
            }
        }

        if (min_i >= 0 && min_d*ERAD_M < MAX_CSR_DIST)
            return_sp = &reports[min_i];
    }

    // well??
    if (return_sp) {
        *sp = *return_sp;
        return (true);
    }
    return (false);
}

/* return PSKReports list
 */
void getPSKSpots (const DXSpot* &rp, int &n_rep)
{
    rp = reports;
    n_rep = n_reports;
}

/* return drawing color for the given frequency, or black if not found.
 */
uint16_t getBandColor (long Hz)
{
    HamBandSetting b = findHamBand (Hz);
    return (b != HAMBAND_NONE ? getMapColor(ham_bands[(int)b].cid) : RA8875_BLACK);
}

/* return whether the path for the given freq should be drawn dashed
 */
bool getBandDashed (long Hz)
{
    HamBandSetting b = findHamBand (Hz);
    return (b != HAMBAND_NONE ? getColorDashed(ham_bands[(int)b].cid) : false);
}
