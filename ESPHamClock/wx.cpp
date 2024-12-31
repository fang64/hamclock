/* This file manages two types of caches for weather and time zone:
 *   exact: WXInfo for last known DE and DX locations
 *          uses separate timeouts for wx and tz
 *   fast:  collection of WXInfo in WXTable on a fixed grid of lat/lng for approx but fast info.
 */


#include "HamClock.h"

// downloads
static const char wx_ll[] = "/wx.pl";                   // URL for requesting specific wx/time at lat/lng
static const char ww_page[] = "/worldwx/wx.txt";        // URL for the gridded world weather table


// config
#define WWXTBL_INTERVAL (45*60)                         // "fast" world wx table update interval, secs
#define MAX_WXTZ_AGE    (30*60)                         // max age of info for same location, secs

// WXInfo and exactly where it applies and when it should be updated
typedef struct {
    WXInfo info;
    float lat_d, lng_d;
    time_t next_update;                                 // next routine update of same lat/lng
    time_t next_err_update;                             // next update if net trouble
} WXCache;
static WXCache de_cache, dx_cache;

/* table of world-wide info for fast general lookups by roaming cursor.
 * wwt.table is a 2d table n_cols x n_rows.
 *   width:  columns are latitude [-90,90] in steps of 180/(n_cols-1).
 *   height: rows are longitude [-180..180) in steps of 360/n_rows.
 */
typedef struct {
    WXInfo *table;                                      // malloced array of info, latitude-major order
    int n_rows, n_cols;                                 // table dimensions
    time_t next_update;                                 // next fresh
} WWTable;
static WWTable wwt;


/* convert wind direction in degs to name, return whether in range.
 */
static bool windDeg2Name (float deg, char dirname[4])
{
    const char *name;

    if (deg < 0)          name = "?";
    else if (deg < 22.5)  name = "N";
    else if (deg < 67.5)  name = "NE";
    else if (deg < 112.5) name = "E";
    else if (deg < 157.5) name = "SE";
    else if (deg < 202.5) name = "S";
    else if (deg < 247.5) name = "SW";
    else if (deg < 292.5) name = "W";
    else if (deg < 337.5) name = "NW";
    else if (deg <= 360)  name = "N";
    else                  name = "?";

    strcpy (dirname, name);

    return (dirname[0] != '?');
}

/* download world wx grid data into wwt.table every 
 */
static bool retrieveWorldWx(void)
{
    WiFiClient ww_client;
    bool ok = false;

    // reset table
    free (wwt.table);
    wwt.table = NULL;
    wwt.n_rows = wwt.n_cols = 0;

    Serial.printf ("WWX: %s\n", ww_page);

    // get
    if (wifiOk() && ww_client.connect(backend_host, backend_port)) {
        updateClocks(false);

        // query web page
        httpHCGET (ww_client, backend_host, ww_page);

        // prep for scanning (ahead of skipping header to avoid stupid g++ goto errors)
        int line_n = 0;                         // line number
        int n_wwtable = 0;                      // entries defined found so far
        int n_wwmalloc = 0;                     // malloced room so far
        int n_lngcols = 0;                      // build up n cols of constant lng so far this block
        float del_lat = 0, del_lng = 0;         // check constant step sizes
        float prev_lat = 0, prev_lng = 0;       // for checking step sizes

        // skip response header
        if (!httpSkipHeader (ww_client)) {
            Serial.printf ("WWX: header timeout");
            goto out;
        }

        /* read file and build table. confirm regular spacing each dimension.
         * file is one line per datum, increasing lat with same lng, then lng steps at each blank line.
         * file contains lng 180 for plotting but we don't use it.
         */
        char line[100];
        while (getTCPLine (ww_client, line, sizeof(line), NULL)) {

            // another line
            line_n++;

            // skip comment lines
            if (line[0] == '#')
                continue;

            // crack line:   lat     lng  temp,C     %hum    mps     dir    mmHg Wx
            float lat, lng, windir;
            WXInfo wx;
            memset (&wx, 0, sizeof(wx));
            int ns = sscanf (line, "%g %g %g %g %g %g %g %31s %d", &lat, &lng, &wx.temperature_c,
                        &wx.humidity_percent, &wx.wind_speed_mps, &windir, &wx.pressure_hPa, wx.conditions,
                        &wx.timezone);

            // skip lng 180
            if (lng == 180)
                break;

            // add and check
            if (ns == 9) {

                // confirm regular spacing
                if (n_lngcols > 0 && lng != prev_lng) {
                    Serial.printf ("WWX: irregular lng: %d x %d  lng %g != %g\n",
                                wwt.n_rows, n_lngcols, lng, prev_lng);
                    goto out;
                }
                if (n_lngcols > 1 && lat != prev_lat + del_lat) {
                    Serial.printf ("WWX: irregular lat: %d x %d    lat %g != %g + %g\n",
                                wwt.n_rows, n_lngcols,  lat, prev_lat, del_lat);
                    goto out;
                }

                // convert wind direction to name
                if (!windDeg2Name (windir, wx.wind_dir_name)) {
                    Serial.printf ("WWX: bogus wind direction: %g\n", windir);
                    goto out;
                }

                // add to wwt.table
                if (n_wwtable + 1 > n_wwmalloc)
                    wwt.table = (WXInfo *) realloc (wwt.table, (n_wwmalloc += 100) * sizeof(WXInfo));
                memcpy (&wwt.table[n_wwtable++], &wx, sizeof(WXInfo));

                // update walk
                if (n_lngcols == 0)
                    del_lng = lng - prev_lng;
                del_lat = lat - prev_lat;
                prev_lat = lat;
                prev_lng = lng;
                n_lngcols++;

            } else if (ns <= 0) {

                // blank line separates blocks of constant longitude

                // check consistency so far
                if (wwt.n_rows == 0) {
                    // we know n cols after completing the first lng block, all remaining must equal this 
                    wwt.n_cols = n_lngcols;
                } else if (n_lngcols != wwt.n_cols) {
                    Serial.printf ("WWX: inconsistent columns %d != %d after %d rows\n",
                                                n_lngcols, wwt.n_cols, wwt.n_rows);
                    goto out;
                }

                // one more wwt.table row
                wwt.n_rows++;

                // reset block stats
                n_lngcols = 0;

            } else {

                Serial.printf ("WWX: bogus line %d: %s\n", line_n, line);
                goto out;
            }
        }

        // final check
        if (wwt.n_rows != 360/del_lng || wwt.n_cols != 1 + 180/del_lat) {
            Serial.printf ("WWX: incomplete table: rows %d != 360/%g   cols %d != 1 + 180/%g\n",
                                        wwt.n_rows, del_lng,  wwt.n_cols, del_lat);
            goto out;
        }

        // yah!
        ok = true;
        Serial.printf ("WWX: fast table %d lat x %d lng\n", wwt.n_cols, wwt.n_rows);

    out:

        if (!ok) {
            // reset table
            free (wwt.table);
            wwt.table = NULL;
            wwt.n_rows = wwt.n_cols = 0;
        }

        ww_client.stop();
    }

    return (ok);
}

/* download current weather and time info for the given exact location.
 * if wip is filled ok return true, else return false with short reason in ynot[] if set
 */
static bool retrieveCurrentWX (const LatLong &ll, bool is_de, WXInfo *wip, char ynot[])
{
    WiFiClient wx_client;
    char line[100];

    bool ok = false;

    resetWatchdog();

    // get
    if (wifiOk() && wx_client.connect(backend_host, backend_port)) {
        updateClocks(false);
        resetWatchdog();

        // query web page
        snprintf (line, sizeof(line), "%s?is_de=%d&lat=%g&lng=%g", wx_ll, is_de, ll.lat_d, ll.lng_d);
        Serial.printf ("WX: %s\n", line);
        httpHCGET (wx_client, backend_host, line);

        // skip response header
        if (!httpSkipHeader (wx_client)) {
            strcpy (ynot, "WX timeout");
            goto out;
        }

        // init response 
        memset (wip, 0, sizeof(*wip));

        // crack response
        uint8_t n_found = 0;
        while (n_found < N_WXINFO_FIELDS && getTCPLine (wx_client, line, sizeof(line), NULL)) {
            // Serial.printf ("WX: %s\n", line);
            updateClocks(false);

            // check for error message in which case abandon further search
            if (sscanf (line, "error=%[^\n]", ynot) == 1)
                goto out;

            // find start of data value after =
            char *vstart = strchr (line, '=');
            if (!vstart)
                continue;
            *vstart++ = '\0';   // eos for name and move to value

            // check for content line
            if (strcmp (line, "city") == 0) {
                strncpy (wip->city, vstart, sizeof(wip->city)-1);
                n_found++;
            } else if (strcmp (line, "temperature_c") == 0) {
                wip->temperature_c = atof (vstart);
                n_found++;
            } else if (strcmp (line, "pressure_hPa") == 0) {
                wip->pressure_hPa = atof (vstart);
                n_found++;
            } else if (strcmp (line, "pressure_chg") == 0) {
                wip->pressure_chg = atof (vstart);
                n_found++;
            } else if (strcmp (line, "humidity_percent") == 0) {
                wip->humidity_percent = atof (vstart);
                n_found++;
            } else if (strcmp (line, "wind_speed_mps") == 0) {
                wip->wind_speed_mps = atof (vstart);
                n_found++;
            } else if (strcmp (line, "wind_dir_name") == 0) {
                strncpy (wip->wind_dir_name, vstart, sizeof(wip->wind_dir_name)-1);
                n_found++;
            } else if (strcmp (line, "clouds") == 0) {
                strncpy (wip->clouds, vstart, sizeof(wip->clouds)-1);
                n_found++;
            } else if (strcmp (line, "conditions") == 0) {
                strncpy (wip->conditions, vstart, sizeof(wip->conditions)-1);
                n_found++;
            } else if (strcmp (line, "attribution") == 0) {
                strncpy (wip->attribution, vstart, sizeof(wip->attribution)-1);
                n_found++;
            } else if (strcmp (line, "timezone") == 0) {
                wip->timezone = atoi (vstart);
                n_found++;
            }

            // Serial.printf ("WX %d: %s\n", n_found, line);
        }

        if (n_found < N_WXINFO_FIELDS) {
            strcpy (ynot, "Missing WX data");
            goto out;
        }

        // ok!
        ok = true;

    } else {

        strcpy (ynot, "WX connection failed");

    }



    // clean up
out:
    wx_client.stop();
    resetWatchdog();
    return (ok);
}



/* display the given location weather in NCDXF_b or err.
 */
static void drawNCDXFBoxWx (BRB_MODE m, const WXInfo &wi, bool ok)
{
    // init arrays for drawNCDXFStats() then replace values with real if ok
    uint16_t color = m == BRB_SHOW_DEWX ? DE_COLOR : DX_COLOR;
    char values[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN];
    uint16_t colors[NCDXF_B_NFIELDS];
    char titles[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN] = {
        "",                     // set later with DE/DX prefix
        "Humidity",
        "Wind Dir",
        "W Speed",
    };
    snprintf (titles[0], sizeof(titles[0]), "%s Temp", m == BRB_SHOW_DEWX ? "DE" : "DX");
    for (int i = 0; i < NCDXF_B_NFIELDS; i++) {
        strcpy (values[i], "Err");
        colors[i] = color;
    }

    if (ok) {
        float v = showTempC() ? wi.temperature_c : CEN2FAH(wi.temperature_c);
        snprintf (values[0], sizeof(values[0]), "%.1f", v);
        snprintf (values[1], sizeof(values[1]), "%.1f", wi.humidity_percent);
        snprintf (values[2], sizeof(values[2]), "%s", wi.wind_dir_name);
        v = (showDistKm() ? 3.6 : 2.237) * wi.wind_speed_mps;                       // kph or mph
        snprintf (values[3], sizeof(values[3]), "%.0f", v);
    }

    // show it
    drawNCDXFStats (RA8875_BLACK, titles, values, colors);
}

/* display current DE weather in the given box and in NCDXF_b if up.
 */
bool updateDEWX (const SBox &box)
{
    char ynot[100];
    WXInfo wi;

    bool ok = getCurrentWX (de_ll, true, &wi, ynot);
    if (ok)
        plotWX (box, DE_COLOR, wi);
    else
        plotMessage (box, DE_COLOR, ynot);

    if (brb_mode == BRB_SHOW_DEWX)
        drawNCDXFBoxWx (BRB_SHOW_DEWX, wi, ok);

    return (ok);
}

/* display current DX weather in the given box and in NCDXF_b if up.
 */
bool updateDXWX (const SBox &box)
{
    char ynot[100];
    WXInfo wi;

    bool ok = getCurrentWX (dx_ll, false, &wi, ynot);
    if (ok)
        plotWX (box, DX_COLOR, wi);
    else
        plotMessage (box, DX_COLOR, ynot);

    if (brb_mode == BRB_SHOW_DXWX)
        drawNCDXFBoxWx (BRB_SHOW_DXWX, wi, ok);

    return (ok);
}

/* display weather for the given mode in NCDXF_b.
 * return whether all ok.
 */
bool drawNCDXFWx (BRB_MODE m)
{
    // get weather
    char ynot[100];
    WXInfo wi;
    bool ok = false;
    if (m == BRB_SHOW_DEWX)
        ok = getCurrentWX (de_ll, true, &wi, ynot);
    else if (m == BRB_SHOW_DXWX)
        ok = getCurrentWX (dx_ll, false, &wi, ynot);
    else
        fatalError ("Bogus drawNCDXFWx mode: %d", m);
    if (!ok)
        Serial.printf ("WX: %s\n", ynot);

    // show it
    drawNCDXFBoxWx (m, wi, ok);

    // done
    return (ok);
}



/* return current WXInfo for the given de or dx, else NULL
 */
static const WXInfo *findWXTXCache (const LatLong &ll, bool is_de, char ynot[])
{
    // who are we?
    WXCache &wxc           = is_de ? de_cache : dx_cache;

    // new location?
    bool new_loc = ll.lat_d != wxc.lat_d || ll.lng_d != wxc.lng_d;

    // update depending same location and how well things are working
    if (myNow() > wxc.next_err_update && (new_loc || myNow() > wxc.next_update)) {

        // get fresh, schedule retry if fail
        if (!retrieveCurrentWX (ll, is_de, &wxc.info, ynot)) {
            char retry_msg[50];
            snprintf (retry_msg, sizeof(retry_msg), "%s WX/TZ", is_de ? "DE" : "DX");
            wxc.next_err_update = nextWiFiRetry (retry_msg);
            return (NULL);
        }

        // ok! update location and next routine expiration

        wxc.lat_d = ll.lat_d;
        wxc.lng_d = ll.lng_d;
        wxc.next_update = myNow() + MAX_WXTZ_AGE;

        // log
        int at = millis()/1000 + MAX_WXTZ_AGE;
        Serial.printf ("WXTZ: expires in %d sec at %d\n", MAX_WXTZ_AGE, at);
    }

    // return requested info
    return (&wxc.info);
}

/* return current WXInfo with weather at de or dx.
 */
static const WXInfo *findWXCache (const LatLong &ll, bool is_de, char ynot[])
{
    return (findWXTXCache (ll, is_de, ynot));
}

/* return current WXInfo with timezone at de or dx.
 */
const WXInfo *findTZCache (const LatLong &ll, bool is_de, char ynot[])
{
    return (findWXTXCache (ll, is_de, ynot));
}



/* return closest WXInfo to ll within grid, else NULL.
 */
const WXInfo *findWXFast (const LatLong &ll)
{
    // update wwt cache if stale
    if (myNow() > wwt.next_update) {
        static const char wwx_label[] = "FastWXTable";
        if (retrieveWorldWx()) {
            wwt.next_update = myNow() + WWXTBL_INTERVAL;
            int at = millis()/1000 + WWXTBL_INTERVAL;
            Serial.printf ("WWX: Next %s update in %d sec at %d\n", wwx_label, WWXTBL_INTERVAL, at);
        } else {
            wwt.next_update = nextWiFiRetry (wwx_label);
            return (NULL);
        }
    }

    // find closest indices
    int row = floorf (wwt.n_rows*(ll.lng_d+180)/360);
    int col = floorf (wwt.n_cols*(ll.lat_d+90)/180);

    // ok
    return (&wwt.table[row*wwt.n_cols + col]);
}


/* look up wx conditions from local cache for the approx location, if possible.
 * return whether wi has been filled
 */
bool getFastWx (const LatLong &ll, WXInfo &wi)
{
    const WXInfo *wip = findWXFast (ll);
    if (wip) {
        wi = *wip;
        return (true);
    }
    return (false);
}

/* fill wip with weather data for ll.
 * return whether ok, with short reason if not.
 */
bool getCurrentWX (const LatLong &ll, bool is_de, WXInfo *wip, char ynot[])
{
    const WXInfo *new_wip = findWXCache (ll, is_de, ynot);
    if (new_wip) {
        *wip = *new_wip;
        return (true);
    }
    return (false);
}
