/* manage most wifi uses, including pane and map updates.
 */

#include "HamClock.h"


// host name and port of backend server
const char *backend_host = "clearskyinstitute.com";
int backend_port = 80;

// IP where server thinks we came from
char remote_addr[16];                           // INET_ADDRSTRLEN

// user's date and time, UNIX only
time_t usr_datetime;

// world wx table update, new data every hour but N.B. make it longer than OTHER_MAPS_INTERVAL
#define WWX_INTERVAL    (2300)                  // polling interval, secs

// ADIF pane
#define ADIF_INTERVAL   30                      // polling interval, secs

// band conditions and voacap map, models change each hour
#define BC_INTERVAL     (2400)                  // polling interval, secs
#define VOACAP_INTERVAL (2500)                  // polling interval, secs
uint16_t bc_powers[] = {1, 5, 10, 50, 100, 500, 1000};
const int n_bc_powers = NARRAY(bc_powers);
static const char bc_page[] PROGMEM = "/fetchBandConditions.pl";
static time_t bc_time;                          // nowWO() when bc_matrix was loaded
static time_t map_time;                         // nowWO() when map was loaded
BandCdtnMatrix bc_matrix;                       // percentage reliability for each band
uint16_t bc_power;                              // VOACAP power setting
float bc_toa;                                   // VOACAP take off angle
uint8_t bc_utc_tl;                              // label band conditions timeline in utc else DE local

uint8_t bc_modevalue;                           // VOACAP sensitivity value
const BCModeSetting bc_modes[N_BCMODES] {
    {"CW",  19},
    {"SSB", 38},
    {"AM",  49},
    {"WSPR", 3},
    {"FT8", 13},
    {"FT4", 17}
};
uint8_t findBCModeValue (const char *name)      // find value give name, else 0
{
    for (int i = 0; i < N_BCMODES; i++)
        if (strcmp (name, bc_modes[i].name) == 0)
            return (bc_modes[i].value);
    return (0);
}
const char *findBCModeName (uint8_t value)      // find name given value, else NULL
{
    for (int i = 0; i < N_BCMODES; i++)
        if (bc_modes[i].value == value)
            return (bc_modes[i].name);
    return (NULL);
}

// geolocation web page
static const char locip_page[] PROGMEM = "/fetchIPGeoloc.pl";


// weather displays
#define DEWX_INTERVAL   (1700)                  // polling interval, secs
#define DXWX_INTERVAL   (1600)                  // polling interval, secs

// moon display
#define MOON_INTERVAL   50                      // annotation update interval, secs


// list of default NTP servers unless user has set their own
static NTPServer ntp_list[] = {                 // init times to 0 insures all get tried initially
    {"time.google.com", 0},
    {"time.apple.com", 0},
    {"pool.ntp.org", 0},
    {"europe.pool.ntp.org", 0},
    {"asia.pool.ntp.org", 0},
    {"time.nist.gov", 0},
};
#define N_NTP NARRAY(ntp_list)                  // number of possible servers


// web site retry interval, secs
#define WIFI_RETRY      (15)

// pane auto rotation period in seconds
#define ROTATION_INTERVAL       (getPaneRotationPeriod())

/* "reverting" refers to restoring PANE_1 after temporarily forced to show DE or DX weather.
 */
static time_t revert_time;                      // when to resume normal pane operation
static PlotPane revert_pane;                    // which pane is being temporarily reverted

/* time of next update attempts for each pane and the maps.
 * 0 will refresh immediately.
 * reset all in initWiFiRetry()
 */
time_t next_update[PANE_N];
static time_t next_map;
static time_t next_wwx;

/* indicate pane must perform a fresh retrieval
 */
static bool fresh_update[PLOT_CH_N];

// fwd local funcs
static bool updateDRAP(const SBox &box);
static bool updateKp(const SBox &box);
static bool updateXRay(const SBox &box);
static bool updateSunSpots(const SBox &box);
static bool updateSolarFlux(const SBox &box);
static bool updateBzBt(const SBox &box);
static bool updateBandConditions(const SBox &box, bool force);
static bool updateNOAASWx(const SBox &box);
static bool updateSolarWind(const SBox &box);
static bool updateAurora(const SBox &box);
static uint32_t crackBE32 (uint8_t bp[]);


/* return absolute difference in two time_t regardless of time_t implementation is signed or unsigned.
 */
static time_t tdiff (const time_t t1, const time_t t2)
{
    if (t1 > t2)
        return (t1 - t2);
    if (t2 > t1)
        return (t2 - t1);
    return (0);
}

/* return the next retry time_t.
 * retries are spaced out every WIFI_RETRY
 */
static time_t nextWiFiRetry (void)
{
    int interval = WIFI_RETRY;

    // set and save next retry time
    static time_t prev_try;
    time_t next_t0 = myNow() + interval;                        // interval after now
    time_t next_try = prev_try + interval;                      // interval after prev
    prev_try = next_t0 > next_try ? next_t0 : next_try;         // use whichever is later
    return (prev_try);
}

/* calls nextWiFiRetry() and logs the given string
 */
time_t nextWiFiRetry (const char *str)
{
    time_t next_try = nextWiFiRetry();
    int dt = next_try - myNow();
    Serial.printf (_FX("Next %s retry in %d sec at %ld\n"), str, dt, next_try);
    return (next_try);
}

/* calls nextWiFiRetry() and logs the given plot choice.
 */
time_t nextWiFiRetry (PlotChoice pc)
{
    time_t next_try = nextWiFiRetry();
    int dt = next_try - myNow();
    int nm = millis()/1000+dt;
    Serial.printf (_FX("Next %s retry in %d sec at %d\n"), plot_names[pc], dt, nm);
    return (next_try);
}

/* figure out when to next rotate the given pane.
 * rotations are spaced out to avoid swamping the backend.
 */
static time_t nextPaneRotationTime (PlotPane pp)
{
    // start with standard rotation interval
    int interval = ROTATION_INTERVAL;
    time_t rot_time = myNow() + interval;

    // then find soonest rot_time that is at least interval away from all other active panes
    for (int i = 0; i < PANE_N*PANE_N; i++) {           // all permutations
        PlotPane ppi = (PlotPane) (i % PANE_N);
        if (ppi == pp)
            continue;
        if (paneIsRotating(ppi)  || (plot_ch[ppi] == PLOT_CH_SDO && isSDORotating())) {
            if ((rot_time >= next_update[ppi] && rot_time - next_update[ppi] < interval)
                              || (rot_time <= next_update[ppi] && next_update[ppi] - rot_time < interval))
                rot_time = next_update[ppi] + interval;
        }
    }

    return (rot_time);
}

/* given a plot choice return time of its next update.
 * if choice is in play and rotating use pane rotation duration else the given interval.
 */
time_t nextPaneUpdate (PlotChoice pc, int interval)
{
    PlotPane pp = findPaneForChoice (pc);
    time_t t0 = myNow();
    time_t next;

    if (pp == PANE_NONE) {
        // pc not in play: use interval
        next = t0 + interval;
        int dt = next - t0;
        int at = millis()/1000+dt;
        Serial.printf (_FX("%s updates in %d sec at %d\n"), plot_names[pc], dt, at);
    } else if (paneIsRotating(pp) || (pc == PLOT_CH_SDO && isSDORotating())) {
        // pc is in rotation
        next = nextPaneRotationTime (pp);
        int dt = next - t0;
        int at = millis()/1000+dt;
        if (pc == plot_ch[pp])
            Serial.printf (_FX("Pane %d now showing %s updates in %d sec at %d\n"), pp,plot_names[pc],dt,at);
        else
            Serial.printf (_FX("Pane %d hiding %s updates in %d sec at %d\n"), pp, plot_names[pc], dt, at);
    } else {
        // pc is alone in a pane
        next = t0 + interval;
        int dt = next - t0;
        int at = millis()/1000+dt;
        Serial.printf (_FX("Pane %d now showing %s updates in %d sec at %d\n"), pp, plot_names[pc], dt, at);
    }

    return (next);
}

/* return the next time of routine download.
 */
time_t nextRetrieval (PlotChoice pc, int interval)
{
    time_t next_update = myNow() + interval;
    int nm = millis()/1000 + interval;
    Serial.printf (_FX("%s data now good for %d sec at %d\n"), plot_names[pc], interval, nm);
    return (next_update);
}


/* set de_ll.lat_d and de_ll.lng_d from the given ip else our public ip.
 * report status via tftMsg
 */
static void geolocateIP (const char *ip)
{
    WiFiClient iploc_client;                            // wifi client connection
    float lat, lng;
    char llline[80];
    char ipline[80];
    char credline[80];
    int nlines = 0;

    if (wifiOk() && iploc_client.connect(backend_host, backend_port)) {

        // create proper query
        strcpy_P (llline, locip_page);
        size_t l = sizeof(locip_page) - 1;              // not EOS
        if (ip)
            l += snprintf (llline+l, sizeof(llline)-l, _FX("?IP=%s"), ip);
        Serial.println(llline);

        // send
        httpHCGET (iploc_client, backend_host, llline);
        if (!httpSkipHeader (iploc_client)) {
            Serial.println (F("geoIP header short"));
            goto out;
        }

        // expect 4 lines: LAT=, LNG=, IP= and CREDIT=, anything else first line is error message
        if (!getTCPLine (iploc_client, llline, sizeof(llline), NULL))
            goto out;
        nlines++;
        lat = atof (llline+4);
        if (!getTCPLine (iploc_client, llline, sizeof(llline), NULL))
            goto out;
        nlines++;
        lng = atof (llline+4);
        if (!getTCPLine (iploc_client, ipline, sizeof(ipline), NULL))
            goto out;
        nlines++;
        if (!getTCPLine (iploc_client, credline, sizeof(credline), NULL))
            goto out;
        nlines++;
    }

out:

    if (nlines == 4) {
        // ok

        tftMsg (true, 0, _FX("IP %s geolocation"), ipline+3);
        tftMsg (true, 0, _FX("  by %s"), credline+7);
        tftMsg (true, 0, _FX("  %.2f%c %.2f%c"), fabsf(lat), lat < 0 ? 'S' : 'N',
                                fabsf(lng), lng < 0 ? 'W' : 'E');

        de_ll.lat_d = lat;
        de_ll.lng_d = lng;
        normalizeLL (de_ll);
        NVWriteFloat (NV_DE_LAT, de_ll.lat_d);
        NVWriteFloat (NV_DE_LNG, de_ll.lng_d);
        setNVMaidenhead(NV_DE_GRID, de_ll);
        de_tz.tz_secs = getTZ (de_ll);
        NVWriteInt32(NV_DE_TZ, de_tz.tz_secs);


    } else {
        // trouble, error message if 1 line

        if (nlines == 1) {
            tftMsg (true, 0, _FX("IP geolocation err:"));
            tftMsg (true, 1000, _FX("  %s"), llline);
        } else
            tftMsg (true, 1000, _FX("IP geolocation failed"));
    }

    iploc_client.stop();
    printFreeHeap (F("geolocateIP"));
}

/* search ntp_list for the fastest so far, or rotate if all bad.
 * N.B. always return one of ntp_list, never NULL
 */
static NTPServer *findBestNTP()
{
    static uint8_t prev_fixed;

    NTPServer *best_ntp = &ntp_list[0];
    int rsp_min = ntp_list[0].rsp_time;

    for (int i = 1; i < N_NTP; i++) {
        NTPServer *np = &ntp_list[i];
        if (np->rsp_time < rsp_min) {
            best_ntp = np;
            rsp_min = np->rsp_time;
        }
    }
    if (rsp_min == NTP_TOO_LONG) {
        prev_fixed = (prev_fixed+1) % N_NTP;
        best_ntp = &ntp_list[prev_fixed];
    }
    return (best_ntp);
}


/* init and connect, inform via tftMsg() if verbose.
 * non-verbose is used for automatic retries that should not clobber the display.
 */
static void initWiFi (bool verbose)
{
    // N.B. look at the usages and make sure this is "big enough"
    static const char dots[] = ".........................................";

    // probable mac when only localhost -- used to detect LAN but no WLAN
    const char *mac_lh = _FX("FF:FF:FF:FF:FF:FF");


    // begin
    // N.B. ESP seems to reconnect much faster if avoid begin() unless creds change
    // N.B. non-RPi UNIX systems return NULL from getWiFI*()
    WiFi.mode(WIFI_STA);
    const char *myssid = getWiFiSSID();
    const char *mypw = getWiFiPW();
    if (myssid && mypw && (strcmp (WiFi.SSID().c_str(), myssid) || strcmp (WiFi.psk().c_str(), mypw)))
        WiFi.begin ((char*)myssid, (char*)mypw);

    // prep
    uint32_t t0 = millis();
    uint32_t timeout = verbose ? 30000UL : 3000UL;      // dont wait nearly as long for a retry, millis
    uint16_t ndots = 0;                                 // progress counter
    char mac[30];
    strcpy (mac, WiFi.macAddress().c_str());
    tftMsg (verbose, 0, _FX("MAC addr: %s"), mac);

    // wait for connection
    if (myssid)
        tftMsg (verbose, 0, "\r");                      // init overwrite
    do {
        if (myssid)
            tftMsg (verbose, 0, _FX("Connecting to %s %.*s\r"), myssid, ndots, dots);
        Serial.printf (_FX("Trying network %d\n"), ndots);
        if (timesUp(&t0,timeout) || ndots == (sizeof(dots)-1)) {
            if (myssid)
                tftMsg (verbose, 1000, _FX("WiFi failed -- signal? credentials?"));
            else
                tftMsg (verbose, 1000, _FX("Network connection attempt failed"));
            return;
        }

        wdDelay(1000);
        ndots++;

        // WiFi.printDiag(Serial);

    } while (strcmp (mac, mac_lh) && (WiFi.status() != WL_CONNECTED));

    // init retry times
    initWiFiRetry();

    // report stats
    if (WiFi.status() == WL_CONNECTED) {

        // just to get remote_addr
        char line[50];
        (void)newVersionIsAvailable (line, sizeof(line));

        IPAddress ip = WiFi.localIP();
        tftMsg (verbose, 0, _FX("Local IP: %d.%d.%d.%d"), ip[0], ip[1], ip[2], ip[3]);
        tftMsg (verbose, 0, _FX("Public IP: %s"), remote_addr);
        ip = WiFi.subnetMask();
        tftMsg (verbose, 0, _FX("Mask: %d.%d.%d.%d"), ip[0], ip[1], ip[2], ip[3]);
        ip = WiFi.gatewayIP();
        tftMsg (verbose, 0, _FX("GW: %d.%d.%d.%d"), ip[0], ip[1], ip[2], ip[3]);
        ip = WiFi.dnsIP();
        tftMsg (verbose, 0, _FX("DNS: %d.%d.%d.%d"), ip[0], ip[1], ip[2], ip[3]);

        int rssi;
        if (readWiFiRSSI(rssi)) {
            tftMsg (verbose, 0, _FX("Signal strength: %d dBm"), rssi);
            tftMsg (verbose, 0, _FX("Channel: %d"), WiFi.channel());
        }

        tftMsg (verbose, 0, _FX("S/N: %u"), ESP.getChipId());
    }

    // retrieve cities
    readCities();

    // log server's idea of our IP
    Serial.printf (_FX("Remote_Addr: %s\n"), remote_addr);
}

/* call exactly once to init wifi, maps and maybe time and location.
 * report on initial startup screen with tftMsg.
 */
void initSys()
{
    // start/check WLAN
    initWiFi(true);

    // start web servers
    initWebServer();
#if defined (_IS_UNIX)
    initLiveWeb(true);
#endif

    // init location if desired
    if (useGeoIP() || init_iploc || init_locip) {
        if (WiFi.status() == WL_CONNECTED)
            geolocateIP (init_locip);
        else
            tftMsg (true, 0, _FX("no network for geo IP"));
    } else if (useGPSDLoc()) {
        LatLong ll;
        if (getGPSDLatLong(&ll)) {

            // good -- set de_ll
            de_ll = ll;
            normalizeLL (de_ll);
            NVWriteFloat (NV_DE_LAT, de_ll.lat_d);
            NVWriteFloat (NV_DE_LNG, de_ll.lng_d);
            setNVMaidenhead(NV_DE_GRID, de_ll);

            // leave user's tz offset
            // de_tz.tz_secs = getTZ (de_ll);
            // NVWriteInt32(NV_DE_TZ, de_tz.tz_secs);

            tftMsg (true, 0, _FX("GPSD: %.2f%c %.2f%c"),
                                fabsf(de_ll.lat_d), de_ll.lat_d < 0 ? 'S' : 'N',
                                fabsf(de_ll.lng_d), de_ll.lng_d < 0 ? 'W' : 'E');

        } else
            tftMsg (true, 1000, _FX("GPSD: no Lat/Long"));
    }


    // skip box
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    drawStringInBox (_FX("Skip"), skip_b, false, RA8875_WHITE);
    bool skipped_here = false;


    // init time service as desired
    if (useGPSDTime()) {
        if (getGPSDUTC(&gpsd_server)) {
            tftMsg (true, 0, _FX("GPSD: time ok"));
            initTime();
        } else
            tftMsg (true, 1000, _FX("GPSD: no time"));

    } else if (useOSTime()) {
        tftMsg (true, 0, _FX("Time from OS"));

    } else if (WiFi.status() == WL_CONNECTED) {

        if (useLocalNTPHost()) {

            // test user choice
            const char *local_ntp = getLocalNTPHost();
            tftMsg (true, 0, _FX("NTP test %s ...\r"), local_ntp);
            if (getNTPUTC(&ntp_server))
                tftMsg (true, 0, _FX("NTP %s: ok\r"), local_ntp);
            else
                tftMsg (true, 0, _FX("NTP %s: fail\r"), local_ntp);
        } else {

            // try all the NTP servers to find the fastest (with sneaky way out)
            SCoord s;
            drainTouch();
            tftMsg (true, 0, _FX("Finding best NTP ..."));
            NTPServer *best_ntp = NULL;
            for (int i = 0; i < N_NTP; i++) {
                NTPServer *np = &ntp_list[i];

                // measure the next. N.B. assumes we stay in sync
                if (getNTPUTC(&ntp_server) == 0)
                    tftMsg (true, 0, _FX("%s: err\r"), np->server);
                else {
                    tftMsg (true, 0, _FX("%s: %d ms\r"), np->server, np->rsp_time);
                    if (!best_ntp || np->rsp_time < best_ntp->rsp_time)
                        best_ntp = np;
                }

                // cancel scan if found at least one good and tapped or typed
                if (best_ntp && (skip_skip || tft.getChar(NULL,NULL)
                                   || (readCalTouchWS(s) != TT_NONE && inBox (s, skip_b)))) {
                    drawStringInBox (_FX("Skip"), skip_b, true, RA8875_WHITE);
                    Serial.printf (_FX("NTP search cancelled with %s\n"), best_ntp->server);
                    skipped_here = true;
                    break;
                }
            }
            if (!skip_skip)
                wdDelay(800); // linger to show last time
            if (best_ntp)
                tftMsg (true, 0, _FX("Best NTP: %s %d ms\r"), best_ntp->server, best_ntp->rsp_time);
            else
                tftMsg (true, 0, _FX("No NTP\r"));
            drainTouch();
        }
        tftMsg (true, 0, NULL);   // next row

        // go
        initTime();

    } else {

        tftMsg (true, 0, _FX("No time"));
    }

    // track from user's time if set
    if (usr_datetime > 0)
        setTime (usr_datetime);


    // init fs
    LittleFS.begin();
    LittleFS.setTimeCallback(now);

    // init bc_power, bc_toa, bc_utc_tl and bc_modevalue
    if (!NVReadUInt16 (NV_BCPOWER, &bc_power)) {
        bc_power = 100;
        NVWriteUInt16 (NV_BCPOWER, bc_power);
    }
    if (!NVReadFloat (NV_BCTOA, &bc_toa)) {
        bc_toa = 3;
        NVWriteFloat (NV_BCTOA, bc_toa);
    }
    if (!NVReadUInt8 (NV_BC_UTCTIMELINE, &bc_utc_tl)) {
        bc_utc_tl = 0;  // default to local time line
        NVWriteUInt8 (NV_BC_UTCTIMELINE, bc_utc_tl);
    }
    if (!NVReadUInt8 (NV_BCMODE, &bc_modevalue)) {
        bc_modevalue = findBCModeValue("CW");           // default to CW
        NVWriteUInt8 (NV_BCMODE, bc_modevalue);
    }

    // insure core_map is defined
    initCoreMaps();

    // offer time to peruse unless alreay opted to skip
    if (!skipped_here) {
        #define     TO_DS 50                                // timeout delay, decaseconds
        drawStringInBox (_FX("Skip"), skip_b, false, RA8875_WHITE);
        uint8_t s_left = TO_DS/10;                          // seconds remaining
        uint32_t t0 = millis();
        drainTouch();
        for (uint8_t ds_left = TO_DS; !skip_skip && ds_left > 0; --ds_left) {
            SCoord s;
            if (tft.getChar(NULL,NULL) || (readCalTouchWS(s) != TT_NONE && inBox(s, skip_b))) {
                drawStringInBox (_FX("Skip"), skip_b, true, RA8875_WHITE);
                break;
            }
            if ((TO_DS - (millis() - t0)/100)/10 < s_left) {
                // just printing every ds_left/10 is too slow due to overhead
                tftMsg (true, 0, _FX("Ready ... %d\r"), s_left--);
            }
            wdDelay(100);
        }
    }
}

/* check if time to update background map
 */
void checkBGMap(void)
{
    // local effective time
    int now_time = nowWO();

    // note whether BC is up
    PlotPane bc_pp = findPaneChoiceNow (PLOT_CH_BC);
    bool bc_up = bc_pp != PANE_NONE && bc_matrix.ok;

    // check VOACAP first
    if (prop_map.active) {

        // update if time or to stay in sync with BC or it's beee over an hour
        if (myNow()>next_map || (bc_up && tdiff(map_time,bc_time)>=3600) || tdiff(now_time,map_time)>=3600) {

            // show busy if BC up
            if (bc_up)
                plotBandConditions (plot_b[bc_pp], 1, NULL, NULL);

            // update prop map, schedule next
            bool ok = installFreshMaps();
            if (ok) {
                next_map = myNow() + VOACAP_INTERVAL;           // schedule normal refresh
                map_time = now_time;                            // map is now current
                initEarthMap();                                 // restart fresh

                // sync DRAP plot too if in use
                PlotPane drap_pp = findPaneChoiceNow(PLOT_CH_DRAP);
                if (drap_pp != PANE_NONE)
                    next_update[drap_pp] = myNow();

            } else {
                next_map = nextWiFiRetry("VOACAP");             // schedule retry
                map_time = bc_time;                             // match bc to avoid immediate retry
            }

            // show result of effort if BC up
            if (bc_up)
                plotBandConditions (plot_b[bc_pp], ok ? 0 : -1, NULL, NULL);

            time_t dt = next_map - myNow();
            Serial.printf (_FX("Next VOACAP map check in %ld s at %ld\n"), dt, millis()/1000+dt);
        }

    } else if (core_map != CM_NONE) {

        if (myNow() > next_map || tdiff(now_time,map_time)>=3600) {

            // update map, schedule next
            bool ok = installFreshMaps();
            if (ok) {
                // schedule next refresh
                if (core_map == CM_DRAP)
                    next_map = myNow() + DRAPMAP_INTERVAL;
                else if (core_map == CM_MUF_RT)
                    next_map = myNow() + MUF_RT_INTERVAL;
                else
                    next_map = myNow() + OTHER_MAPS_INTERVAL;

                // update corresponding world wx data
                if (core_map == CM_WX) {
                    fetchWorldWx();
                    next_wwx = myNow() + WWX_INTERVAL;
                }

                // note time of map
                map_time = now_time;                            // map is now current

                // start
                initEarthMap();

            } else
                next_map = nextWiFiRetry(coremap_names[core_map]); // schedule retry

            // insure BC band is off
            if (bc_up)
                plotBandConditions (plot_b[bc_pp], 0, NULL, NULL);

            time_t dt = next_map - myNow();
            Serial.printf (_FX("Next %s map check in %ld s at %ld\n"), coremap_names[core_map],
                                        dt, millis()/1000+dt);
        }

    } else {

        // eh??
        fatalError (_FX("no map"));

    }
}

/* given a GOES XRAY Flux value, return its event level designation in buf.
 */
char *xrayLevel (char *buf, const SpaceWeather_t &xray)
{
    if (!xray.value_ok)
        strcpy (buf, "Err");
    else if (xray.value < 1e-8)
        strcpy (buf, _FX("A0.0"));
    else {
        static const char levels[] = "ABCMX";
        int power = floorf(log10f(xray.value));
        if (power > -4)
            power = -4;
        float mantissa = xray.value*powf(10.0F,-power);
        char alevel = levels[8+power];
        snprintf (buf, 10, _FX("%c%.1f"), alevel, mantissa);
    }
    return (buf);
}


/* retrieve bc_matrix and optional config line underneath PLOT_CH_BC.
 * return whether at least config line was received (even if data was not)
 */
static bool retrieveBandConditions (char *config)
{
    char buf[100];
    WiFiClient bc_client;
    bool ok = false;

    // init data unknown
    bc_matrix.ok = false;

    // build query -- muck about because of PROGMEM
    const size_t bc_page_len = sizeof(bc_page) - 1;
    const size_t qsize = bc_page_len+200;
    StackMalloc query_mem (qsize);
    char *query = (char *) query_mem.getMem();
    time_t t = nowWO();
    strcpy_P (query, bc_page);
    snprintf (query+bc_page_len, qsize-bc_page_len,
        _FX("?YEAR=%d&MONTH=%d&RXLAT=%.3f&RXLNG=%.3f&TXLAT=%.3f&TXLNG=%.3f&UTC=%d&PATH=%d&POW=%d&MODE=%d&TOA=%.1f"),
        year(t), month(t), dx_ll.lat_d, dx_ll.lng_d, de_ll.lat_d, de_ll.lng_d,
        hour(t), show_lp, bc_power, bc_modevalue, bc_toa);

    Serial.println (query);
    if (wifiOk() && bc_client.connect(backend_host, backend_port)) {
        updateClocks(false);

        // query web page
        httpHCGET (bc_client, backend_host, query);

        // skip header
        if (!httpSkipHeader (bc_client)) {
            Serial.println (F("BC: no header"));
            goto out;
        }

        // next line is CSV path reliability for the requested time between DX and DE, 9 bands 80-10m
        if (!getTCPLine (bc_client, buf, sizeof(buf), NULL)) {
            Serial.println (F("BC: No response"));
            goto out;
        }
        // Serial.printf (_FX("BC response: %s\n"), buf);

        // next line is configuration summary, save if interested
        if (!getTCPLine (bc_client, buf, sizeof(buf), NULL)) {
            Serial.println (F("BC: No config"));
            goto out;
        }
        // Serial.printf (_FX("BC config: %s\n"), buf);
        if (config)
            strcpy (config, buf);

        // transaction for at least config is ok
        ok = true;

        // keep time fresh
        updateClocks(false);

        // next 24 lines are reliability matrix.
        // N.B. col 1 is UTC but runs from 1 .. 24, 24 is really 0
        // lines include data for 9 bands, 80-10, but we drop 60 for BandCdtnMatrix
        float rel[BMTRX_COLS];          // values are path reliability 0 .. 1
        for (int r = 0; r < BMTRX_ROWS; r++) {

            // read next row
            if (!getTCPLine (bc_client, buf, sizeof(buf), NULL)) {
                Serial.printf (_FX("BC: fail row %d"), r);
                goto out;
            }

            // crack row, skipping 60 m
            int utc_hr;
            if (sscanf(buf, _FX("%d %f,%*f,%f,%f,%f,%f,%f,%f,%f"), &utc_hr,
                        &rel[0], &rel[1], &rel[2], &rel[3], &rel[4], &rel[5], &rel[6], &rel[7])
                            != BMTRX_COLS + 1) {
                Serial.printf (_FX("BC: bad matrix line: %s\n"), buf);
                goto out;
            }

            // insure correct utc
            utc_hr %= 24;

            // add to bc_matrix as integer percent
            for (int c = 0; c < BMTRX_COLS; c++)
                bc_matrix.m[utc_hr][c] = (uint8_t)(100*rel[c]);
        }

        // #define _TEST_BAND_MATRIX
        #if defined(_TEST_BAND_MATRIX)
            for (int r = 0; r < BMTRX_ROWS; r++)                    // time 0 .. 23
                for (int c = 0; c < BMTRX_COLS; c++)                // band 80 .. 10
                    bc_matrix.m[r][c] = 100*r*c/BMTRX_ROWS/BMTRX_COLS;
        #endif

        // matrix ok
        bc_matrix.ok = true;

    } else {
        Serial.println (F("VOACAP connection failed"));
    }

out:

    // clean up
    bc_client.stop();
    return (ok);
}

/* check for tap at s known to be within BandConditions box b:
 *    tapping left-half band toggles REF map, right-half toggles TOA map
 *    tapping timeline toggles bc_utc_tl;
 *    tapping power offers power menu;
 *    tapping TOA offers take-off menu;
 *    tapping SP/LP toggles.
 * return whether tap was useful for us.
 * N.B. coordinate tap positions with plotBandConditions()
 */
bool checkBCTouch (const SCoord &s, const SBox &b)
{
    // not ours if not in our box or tapped titled or data not valid
    if (!inBox (s, b) || s.y < b.y+PANETITLE_H || !bc_matrix.ok)
        return (false);

    // tap area for power cycle
    SBox power_b;
    power_b.x = b.x + 5;
    power_b.y = b.y + 13*b.h/14;
    power_b.w = b.w/5;
    power_b.h = b.h/12;
    // drawSBox (power_b, RA8875_RED);     // RBF

    // tap area for mode choice
    SBox mode_b;
    mode_b.x = power_b.x + power_b.w + 1;
    mode_b.y = power_b.y;
    mode_b.w = b.w/6;
    mode_b.h = power_b.h;
    // drawSBox (mode_b, RA8875_RED);      // RBF

    // tap area for TOA
    SBox toa_b;
    toa_b.x = mode_b.x + mode_b.w + 1;
    toa_b.y = mode_b.y;
    toa_b.w = b.w/5;
    toa_b.h = mode_b.h;
    // drawSBox (toa_b, RA8875_RED);       // RBF

    // tap area for SP/LP
    SBox splp_b;
    splp_b.x = toa_b.x + toa_b.w + 1;
    splp_b.y = toa_b.y;
    splp_b.w = b.w/6;
    splp_b.h = toa_b.h;
    // drawSBox (splp_b, RA8875_RED);      // RBF

    // tap area for timeline strip
    SBox tl_b;
    tl_b.x = b.x + 1;
    tl_b.y = b.y + 12*b.h/14;
    tl_b.w = b.w - 2;
    tl_b.h = b.h/12;
    // drawSBox (tl_b, RA8875_WHITE);      // RBF

    if (inBox (s, power_b)) {

        // build menu of available power choices
        MenuItem mitems[n_bc_powers];
        char labels[n_bc_powers][20];
        for (int i = 0; i < n_bc_powers; i++) {
            MenuItem &mi = mitems[i];
            mi.type = MENU_1OFN;
            mi.set = bc_power == bc_powers[i];
            mi.group = 1;
            mi.indent = 5;
            mi.label = labels[i];
            snprintf (labels[i], sizeof(labels[i]), _FX("%d watt%s"), bc_powers[i],
                                bc_powers[i] > 1 ? "s" : ""); 
        };

        SBox menu_b;
        menu_b.x = power_b.x;
        menu_b.y = b.y + b.h/4;
        menu_b.w = 0;           // shrink to fit

        // run menu, find selection
        SBox ok_b;
        MenuInfo menu = {menu_b, ok_b, true, false, 1, NARRAY(mitems), mitems};
        uint16_t new_power = bc_power;
        if (runMenu (menu)) {
            for (int i = 0; i < n_bc_powers; i++) {
                if (menu.items[i].set) {
                    new_power = bc_powers[i];
                    break;
                }
            }
        }

        // always redo BC if nothing else to erase menu but only update voacap if power changed
        bool power_changed = new_power != bc_power;
        if (power_changed) {
            bc_power = new_power;
            NVWriteUInt16 (NV_BCPOWER, bc_power);
            scheduleNewVOACAPMap(prop_map);
        }
        (void) updateBandConditions (b, power_changed);

    } else if (inBox (s, mode_b)) {

        // show menu of available mode choices
        MenuItem mitems[N_BCMODES];
        for (int i = 0; i < N_BCMODES; i++)
            mitems[i] = {MENU_1OFN, bc_modevalue == bc_modes[i].value, 1, 5, bc_modes[i].name};

        SBox menu_b;
        menu_b.x = mode_b.x;
        menu_b.y = b.y + b.h/3;
        menu_b.w = 0;           // shrink to fit

        // run menu, find selection
        SBox ok_b;
        MenuInfo menu = {menu_b, ok_b, true, false, 1, N_BCMODES, mitems};
        uint16_t new_modevalue = bc_modevalue;
        if (runMenu (menu)) {
            for (int i = 0; i < N_BCMODES; i++) {
                if (menu.items[i].set) {
                    new_modevalue = bc_modes[i].value;
                    break;
                }
            }
        }

        // always redo BC if nothing else to erase menu but only update voacap if mode changed
        bool mode_changed = new_modevalue != bc_modevalue;
        if (mode_changed) {
            bc_modevalue = new_modevalue;
            NVWriteUInt8 (NV_BCMODE, bc_modevalue);
            scheduleNewVOACAPMap(prop_map);
        }
        (void) updateBandConditions (b, mode_changed);

    } else if (inBox (s, toa_b)) {

        // show menu of available TOA choices
        // N.B. line display width can only accommodate 1 character
        MenuItem mitems[3];
        mitems[0] = {MENU_1OFN, bc_toa <= 1,              1, 5, ">1 deg"};
        mitems[1] = {MENU_1OFN, bc_toa > 1 && bc_toa < 9, 1, 5, ">3 degs"};
        mitems[2] = {MENU_1OFN, bc_toa >= 9,              1, 5, ">9 degs"};

        SBox menu_b;
        menu_b.x = toa_b.x;
        menu_b.y = b.y + b.h/2;
        menu_b.w = 0;           // shrink to fit

        // run menu, find selection
        SBox ok_b;
        MenuInfo menu = {menu_b, ok_b, true, false, 1, NARRAY(mitems), mitems};
        float new_toa = bc_toa;
        if (runMenu (menu)) {
            for (int i = 0; i < NARRAY(mitems); i++) {
                if (menu.items[i].set) {
                    new_toa = atof (mitems[i].label+1);
                    break;
                }
            }
        }

        // always redo BC if nothing else to erase menu but only update voacap if mode changed
        bool toa_changed = new_toa != bc_toa;
        if (toa_changed) {
            bc_toa = new_toa;
            NVWriteFloat (NV_BCTOA, bc_toa);
            scheduleNewVOACAPMap(prop_map);
        }
        (void) updateBandConditions (b, toa_changed);

    } else if (inBox (s, splp_b)) {

        // toggle short/long path -- update DX info too
        show_lp = !show_lp;
        NVWriteUInt8 (NV_LP, show_lp);
        scheduleNewVOACAPMap(prop_map);
        drawDXInfo ();
        (void) updateBandConditions (b, true);
    
    } else if (inBox (s, tl_b)) {

        // toggle bc_utc_tl and redraw
        bc_utc_tl = !bc_utc_tl;
        NVWriteUInt8 (NV_BC_UTCTIMELINE, bc_utc_tl);
        plotBandConditions (b, 0, NULL, NULL);

    } else {

        // check tapping a row in the table. if so toggle band and type.

        PropMapSetting new_prop_map = prop_map;
        PropMapBand tap_band = (PropMapBand) ((b.y + b.h - 20 - s.y) / ((b.h - 47)/BMTRX_COLS));
        PropMapType tap_type = s.x < b.x + b.w/2 ? PROPTYPE_REL : PROPTYPE_TOA;
        if (prop_map.active && tap_band == prop_map.band && tap_type == prop_map.type) {
            // tapped same prop map, turn off active VOACAP selection
            new_prop_map.active = false;
        } else if (tap_band >= 0 && tap_band < PROPBAND_N) {
            // tapped a different VOACAP selection
            new_prop_map.active = true;
            new_prop_map.band = tap_band;
            new_prop_map.type = tap_type;
        }

        // update
        scheduleNewVOACAPMap(new_prop_map);
        if (!new_prop_map.active) {
            scheduleNewCoreMap (core_map);
            plotBandConditions (b, 0, NULL, NULL);  // indicate no longer active
        }

    }

    // ours just because tap was below title
    return (true);
}

/* keep the NCDXF_b up to date.
 * N.B. this is called often so do minimal work.
 */
static void checkBRB (time_t t)
{
    // routine update of NCFDX map beacons
    updateBeacons (false);

    // see if it's time to rotate
    if (t > brb_updateT) {

        // move brb_mode to next rotset if rotating
        if (BRBIsRotating()) {
            for (int i = 1; i < BRB_N; i++) {
                int next_mode = (brb_mode + i) % BRB_N;
                if (brb_rotset & (1 << next_mode)) {
                    brb_mode = next_mode;
                    Serial.printf (_FX("BRB: rotating to mode \"%s\"\n"), brb_names[brb_mode]);
                    break;
                }
            }
        }

        // update brb_mode
        if (!drawNCDXFBox()) {

            // trouble: retry
            brb_updateT = nextWiFiRetry("BRB");

        } else {

            // ok: sync rotation with next soonest rotating pane if any, else use standard pane rotation
            time_t next_rotT = 0;
            for (int i = 0; i < PANE_N; i++) {
                if (paneIsRotating((PlotPane)i)) {
                    if (!next_rotT || next_update[i] < next_rotT)
                        next_rotT = next_update[i];
                }
            }
            if (next_rotT)
                brb_updateT = next_rotT;
            else
                brb_updateT = t + ROTATION_INTERVAL;
        }

        int dt = brb_updateT - myNow();
        int nm = millis()/1000+dt;
        Serial.printf (_FX("BRB: Next pane update in %d sec at %d\n"), dt, nm);

    } else {

        // check a few that need spontaneous updating

        switch (brb_mode) {

        case BRB_SHOW_BME76:
        case BRB_SHOW_BME77:
            // only if new BME
            if (newBME280data())
                (void) drawNCDXFBox();
            break;

        case BRB_SHOW_SWSTATS:
            // only if new space stats
            if (checkForNewSpaceWx())
                (void) drawNCDXFBox();
            break;

        case BRB_SHOW_DEWX:
        case BRB_SHOW_DXWX:
            // routine drawNCDXFBox() are enough
            break;

        case BRB_SHOW_PHOT:
        case BRB_SHOW_BR:
            // these are updated from followBrightness() in main loop()
            break;
        }

    }
}

/* set the given pane to the given plot choice now.
 * return whether successful.
 * N.B. we might change plot_ch but we NEVER change plot_rotset here
 * N.B. it's harmless to set pane to same choice again.
 */
bool setPlotChoice (PlotPane pp, PlotChoice pc)
{
    // ignore if new choice is already in some other pane
    PlotPane pp_now = findPaneForChoice (pc);
    if (pp_now != PANE_NONE && pp_now != pp)
        return (false);

    // display box
    const SBox &box = plot_b[pp];

    // first check a few plot types that require extra tests or processing.
    switch (pc) {
    case PLOT_CH_DXCLUSTER:
        if (!useDXCluster())
            return (false);
        break;
    case PLOT_CH_GIMBAL:
        if (!haveGimbal())
            return (false);
        break;
    case PLOT_CH_TEMPERATURE:
        if (getNBMEConnected() == 0)
            return (false);
        drawOneBME280Pane (box, pc);
        break;
    case PLOT_CH_PRESSURE:
        if (getNBMEConnected() == 0)
            return (false);
        drawOneBME280Pane (box, pc);
        break;
    case PLOT_CH_HUMIDITY:
        if (getNBMEConnected() == 0)
            return (false);
        drawOneBME280Pane (box, pc);
        break;
    case PLOT_CH_DEWPOINT:
        if (getNBMEConnected() == 0)
            return (false);
        drawOneBME280Pane (box, pc);
        break;
    case PLOT_CH_COUNTDOWN:
        if (getSWEngineState(NULL,NULL) != SWE_COUNTDOWN)
            return (false);
        if (getSWDisplayState() == SWD_NONE) {
            plot_ch[pp] = pc;           // must set here so force arg ripples through to drawCDTimeRemaining()
            drawMainPageStopwatch(true);
        }
        break;
    default:
        break;          // lint
    }

    // ok, commit choice to the given pane with immediate refresh
    plot_ch[pp] = pc;
    next_update[pp] = 0;
    fresh_update[pc] = true;

    // insure DX and gimbal are off if no longer selected for display
    if (findPaneChoiceNow (PLOT_CH_DXCLUSTER) == PANE_NONE)
        closeDXCluster();
    if (findPaneChoiceNow (PLOT_CH_GIMBAL) == PANE_NONE)
        closeGimbal();

    // persist
    savePlotOps();

    // ok!
    return (true);
}

/* check if it is time to update any info via wifi.
 * proceed even if no wifi to allow subsystems to update.
 */
void updateWiFi(void)
{

    // time now
    time_t t0 = myNow();

    // update each pane
    for (int i = PANE_0; i < PANE_N; i++) {

        // too bad you can't iterate an enum
        PlotPane pp = (PlotPane)i;

        // handy
        const SBox &box = plot_b[pp];
        PlotChoice pc = plot_ch[pp];
        bool new_rot_ch = false;

        // rotate if this pane is rotating and it's time but not if being forced
        if (paneIsRotating(pp) && next_update[pp] > 0 && t0 >= next_update[pp]) {
            pc = plot_ch[pp] = getNextRotationChoice(pp, plot_ch[pp]);
            new_rot_ch = true;

            // a few panes must do a complete refresh when they are freshly exposed
            switch (pc) {
            case PLOT_CH_MOON:
            case PLOT_CH_SDO:
                fresh_update[pc] = true;
                break;
            default:
                break;
            }

        }

        switch (pc) {

        case PLOT_CH_BC:
            if (t0 >= next_update[pp]) {
                if (updateBandConditions (box, fresh_update[pc])) {
                    next_update[pp] = nextPaneUpdate (pc, BC_INTERVAL);
                    fresh_update[pc] = false;
                } else
                    next_update[pp] = nextWiFiRetry (PLOT_CH_BC);
            }
            break;

        case PLOT_CH_DEWX:
            if (t0 >= next_update[pp]) {
                if (updateDEWX(box))
                    next_update[pp] = nextPaneUpdate (pc, DEWX_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_DXCLUSTER:
            if (t0 >= next_update[pp]) {
                if (updateDXCluster(box))
                    next_update[pp] = 0;   // constant poll
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_DXWX:
            if (t0 >= next_update[pp]) {
                if (updateDXWX(box))
                    next_update[pp] = nextPaneUpdate (pc, DXWX_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_FLUX:
            if (t0 >= next_update[pp]) {
                if (updateSolarFlux(box))
                    next_update[pp] = nextPaneUpdate (pc, SFLUX_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_KP:
            if (t0 >= next_update[pp]) {
                if (updateKp(box))
                    next_update[pp] = nextPaneUpdate (pc, KP_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_MOON:
            if (t0 >= next_update[pp]) {
                updateMoonPane (box, fresh_update[pc]);
                fresh_update[pc] = false;
                next_update[pp] = nextPaneUpdate (pc, MOON_INTERVAL);
            }
            break;

        case PLOT_CH_NOAASPW:
            if (t0 >= next_update[pp]) {
                if (updateNOAASWx(box))
                    next_update[pp] = nextPaneUpdate (pc, NOAASPW_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_SSN:
            if (t0 >= next_update[pp]) {
                if (updateSunSpots(box))
                    next_update[pp] = nextPaneUpdate (pc, SSN_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_XRAY:
            if (t0 >= next_update[pp]) {
                if (updateXRay(box))
                    next_update[pp] = nextPaneUpdate (pc, XRAY_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_GIMBAL:
            if (t0 >= next_update[pp]) {
                updateGimbal(box);
                next_update[pp] = 0;                // constant poll
            }
            break;

        case PLOT_CH_TEMPERATURE:               // fallthru
        case PLOT_CH_PRESSURE:                  // fallthru
        case PLOT_CH_HUMIDITY:                  // fallthru
        case PLOT_CH_DEWPOINT:
            if (t0 >= next_update[pp]) {
                drawOneBME280Pane (box, pc);
                next_update[pp] = nextPaneUpdate (pc, ROTATION_INTERVAL);
            } else if (newBME280data()) {
                drawOneBME280Pane (box, pc);
            }
            break;

        case PLOT_CH_SDO:
            if (t0 >= next_update[pp]) {
                if (updateSDOPane (box, fresh_update[pc])) {
                    next_update[pp] = nextPaneUpdate (pc, isSDORotating() ? ROTATION_INTERVAL : SDO_INTERVAL);
                    fresh_update[pc] = false;
                } else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_SOLWIND:
            if (t0 >= next_update[pp]) {
                if (updateSolarWind(box))
                    next_update[pp] = nextPaneUpdate (pc, SWIND_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_DRAP:
            if (t0 >= next_update[pp]) {
                if (updateDRAP(box))
                    next_update[pp] = nextPaneUpdate (pc, DRAPPLOT_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_COUNTDOWN:
            // handled by stopwatch system
            break;

        case PLOT_CH_CONTESTS:
            if (t0 >= next_update[pp]) {
                if (updateContests(box))
                    next_update[pp] = nextPaneUpdate (pc, CONTESTS_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_PSK:
            if (t0 >= next_update[pp]) { 
                if (updatePSKReporter(box, fresh_update[pc])) {
                    next_update[pp] = nextPaneUpdate (pc, PSK_INTERVAL);
                    fresh_update[pc] = false;
                } else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_BZBT:
            if (t0 >= next_update[pp]) { 
                if (updateBzBt(box))
                    next_update[pp] = nextPaneUpdate (pc, BZBT_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_POTA:
            if (t0 >= next_update[pp]) { 
                if (updateOnTheAir(box, ONTA_POTA, fresh_update[pc])) {
                    next_update[pp] = nextPaneUpdate (pc, ONTA_INTERVAL);
                    fresh_update[pc] = false;
                } else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_SOTA:
            if (t0 >= next_update[pp]) { 
                if (updateOnTheAir(box, ONTA_SOTA, fresh_update[pc])) {
                    next_update[pp] = nextPaneUpdate (pc, ONTA_INTERVAL);
                    fresh_update[pc] = false;
                } else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_ADIF:
            if (t0 >= next_update[pp]) {
                updateADIF (box);
                next_update[pp] = nextPaneUpdate (pc, ADIF_INTERVAL);
            }
            break;

        case PLOT_CH_AURORA:
            if (t0 >= next_update[pp]) { 
                if (updateAurora(box))
                    next_update[pp] = nextPaneUpdate (pc, AURORA_INTERVAL);
                else
                    next_update[pp] = nextWiFiRetry(pc);
            }
            break;

        case PLOT_CH_N:
            break;              // lint
        }

        // show immediately this is a new rotating pane
        if (new_rot_ch)
            showRotatingBorder ();
    }

    // freshen ADIF memory usage
    checkADIF();

    // freshen NCDXF_b
    checkBRB(t0);

    // freshen RSS
    checkRSS();

    // freshen world weather table unless wx map is doing it
    if (t0 >= next_wwx) {
        if (!prop_map.active && core_map == CM_WX)
            next_map = 0;                       // this causes checkMap() to call fetchWorldWx()
        else
            fetchWorldWx();
        next_wwx = myNow() + WWX_INTERVAL;
    }

    // maps are checked after each full earth draw -- see drawMoreEarth()

    // check for server commands
    checkWebServer(false);
}


/* NTP time server query.
 * returns UNIX time and server used if ok, or 0 if trouble.
 * for good NTP packet description try
 *   http://www.cisco.com
 *      /c/en/us/about/press/internet-protocol-journal/back-issues/table-contents-58/154-ntp.html
 */
time_t getNTPUTC(const char **server)
{
    // NTP contents packet
    static const uint8_t timeReqA[] = { 0xE3, 0x00, 0x06, 0xEC };
    static const uint8_t timeReqB[] = { 0x31, 0x4E, 0x31, 0x34 };

    // N.B. do not call wifiOk: now() -> us -> wifiOk -> initWiFi -> initWiFiRetry which forces all 
    // if (!wifiOk())
    //    return (0);

    // create udp endpoint
    WiFiUDP ntp_udp;
    if (!ntp_udp.begin(1000+random(50000))) {                   // any local port
        Serial.println (F("NTP: UDP startup failed"));
        return (0);
    }

    // decide on server: user's else fastest 
    NTPServer *ntp_use = &ntp_list[0];                          // a place for rsp_time if useLocal
    const char *ntp_server;
    if (useLocalNTPHost()) {
        ntp_server = getLocalNTPHost();
    } else {
        ntp_use = findBestNTP();
        ntp_server = ntp_use->server;
    }

    // NTP buffer and timers
    uint8_t  buf[48];
    uint32_t tx_ms, rx_ms;

    // Assemble request packet
    memset(buf, 0, sizeof(buf));
    memcpy(buf, timeReqA, sizeof(timeReqA));
    memcpy(&buf[12], timeReqB, sizeof(timeReqB));

    // send
    Serial.printf(_FX("NTP: Issuing request to %s\n"), ntp_server);
    ntp_udp.beginPacket (ntp_server, 123);                      // NTP uses port 123
    ntp_udp.write(buf, sizeof(buf));
    tx_ms = millis();                                           // record when packet sent
    if (!ntp_udp.endPacket()) {
        Serial.println (F("NTP: UDP write failed"));
        ntp_use->rsp_time = NTP_TOO_LONG;                       // force different choice next time
        ntp_udp.stop();
        return (0UL);
    }
    // Serial.print (F("NTP: Sent 48 ... "));

    // receive response
    // Serial.print(F("NTP: Awaiting response ... "));
    memset(buf, 0, sizeof(buf));
    uint32_t t0 = millis();
    while (!ntp_udp.parsePacket()) {
        if (timesUp (&t0, NTP_TOO_LONG)) {
            Serial.println(F("NTP: UDP timed out"));
            ntp_use->rsp_time = NTP_TOO_LONG;                   // force different choice next time
            ntp_udp.stop();
            return (0UL);
        }
        wdDelay(10);
    }
    rx_ms = millis();                                           // record when packet arrived

    // record response time
    ntp_use->rsp_time = rx_ms - tx_ms;
    Serial.printf (_FX("NTP: %s replied after %d ms\n"), ntp_server, ntp_use->rsp_time);

    // read response
    if (ntp_udp.read (buf, sizeof(buf)) != sizeof(buf)) {
        Serial.println (F("NTP: UDP read failed"));
        ntp_use->rsp_time = NTP_TOO_LONG;                       // force different choice next time
        ntp_udp.stop();
        return (0UL);
    }
    Serial.printf (_FX("NTP: received 48 from %s\n"), ntp_udp.remoteIP().toString().c_str());

    // only accept server responses which are mode 4
    uint8_t mode = buf[0] & 0x7;
    if (mode != 4) {                                            // insure server packet
        Serial.printf (_FX("NTP: RX mode must be 4 but it is %d\n"), mode);
        ntp_udp.stop();
        return (0UL);
    }

    // crack and advance to next whole second
    time_t unix_s = crackBE32 (&buf[40]) - 2208988800UL;        // packet transmit time - (1970 - 1900)
    if ((uint32_t)unix_s > 0x7FFFFFFFUL) {                      // sanity check beyond unsigned value
        Serial.printf (_FX("NTP: crazy large UNIX time: %ld\n"), unix_s);
        ntp_udp.stop();
        return (0UL);
    }
    uint32_t fraction_more = crackBE32 (&buf[44]);              // x / 10^32 additional second
    uint16_t ms_more = 1000UL*(fraction_more>>22)/1024UL;       // 10 MSB to ms
    uint16_t transit_time = (rx_ms - tx_ms)/2;                  // transit = half the round-trip time
    ms_more += transit_time;                                    // with transit now = unix_s + ms_more
    uint16_t sec_more = ms_more/1000U+1U;                       // whole seconds behind rounded up
    wdDelay (sec_more*1000U - ms_more);                         // wait to next whole second
    unix_s += sec_more;                                         // account for delay
    // Serial.print (F("NTP: Fraction ")); Serial.print(ms_more);
    // Serial.print (F(", transit ")); Serial.print(transit_time);
    // Serial.print (F(", seconds ")); Serial.print(sec_more);
    // Serial.print (F(", UNIX ")); Serial.print (unix_s); Serial.println();

    // one more sanity check
    if (unix_s < 1577836800L) {          // Jan 1 2020
        Serial.printf (_FX("NTP: crazy small UNIX time: %ld\n"), unix_s);
        ntp_udp.stop();
        return (0UL);
    }

    ntp_udp.stop();
    *server = ntp_server;
    printFreeHeap (F("NTP"));
    return (unix_s);
}

/* read next char from client.
 * return whether another character was in fact available.
 */
bool getTCPChar (WiFiClient &client, char *cp)
{
    // wait for char, avoid calling millis() if more data are already ready
    if (!client.available()) {
        uint32_t t0 = millis();
        while (!client.available()) {
            if (!client.connected()) {
                // Serial.print (F("getTCPChar disconnect\n"));
                return (false);
            }
            if (timesUp(&t0,10000)) {
                Serial.print (F("getTCPChar timeout\n"));
                return (false);
            }

            // N.B. do not call wdDelay -- it calls checkWebServer() most of whose handlers
            // call back here via getTCPLine()
            delay(2);
        }
    }

    // read, which offers yet another way to indicate failure
    int c = client.read();
    if (c < 0) {
        Serial.print (F("bad getTCPChar read\n"));
        return (false);
    }

    // got one
    *cp = (char)c;
    return (true);
}

/* send User-Agent to client
 */
void sendUserAgent (WiFiClient &client)
{
    char ua[400];

    if (logUsageOk()) {

        // display mode: 0=X11 1=fb0 2=X11full 3=X11+live 4=X11full+live 5=noX
        int dpy_mode = 0;
        #if defined(_USE_FB0)
            dpy_mode = 1;
        #else
            bool fs = getX11FullScreen() || getWebFullScreen();
            bool live = n_rwweb > 0;
            if (live)
                dpy_mode = fs ? 4 : 3;
            else if (fs)
                dpy_mode = 2;
        #endif
        #if defined(_IS_UNIX)
            #if defined(_WEB_ONLY)
                dpy_mode = 5;
            #endif
        #endif

        // encode stopwatch if on else as per map_proj
        int main_page = 0;
        switch (getSWDisplayState()) {
        default:
        case SWD_NONE:
            // < V2.81: main_page = azm_on ? 1: 0;
            // >= 2.96: add MAPP_MOLL
            // >= 3.05: change MAP_MOLL to MAPP_ROB
            switch ((MapProjection)map_proj) {
            case MAPP_MERCATOR:  main_page = 0; break;
            case MAPP_AZIMUTHAL: main_page = 1; break;
            case MAPP_AZIM1:     main_page = 5; break;
            case MAPP_ROB:       main_page = 6; break;
            default: fatalError(_FX("sendUserAgent() map_proj %d"), map_proj);
            }
            break;
        case SWD_MAIN:
            main_page = 2;
            break;
        case SWD_BCDIGITAL:
            main_page = 4;
            break;
        case SWD_BCANALOG:
            main_page = 3;
            break;
        }

        // alarm clocks
        AlarmState a_ds, a_os;
        time_t a_utct;
        bool a_utc;
        uint16_t a_hr, a_mn;
        getDailyAlarmState (a_ds, a_hr, a_mn);
        char a_str[100];
        getOneTimeAlarmState (a_os, a_utct, a_utc, a_str, sizeof(a_str));
        int alarms = 0;
        if (a_ds != ALMS_OFF)
            alarms += 1;
        if (a_os != ALMS_OFF)
            alarms += 2;


        // encode plot options
        // prior to V2.67: value was either plot_ch or 99
        // since V2.67:    value is 100 + plot_ch
        // V3.07: added PANE_0
        int plotops[PANE_N];
        for (int i = 0; i < PANE_N; i++)
            plotops[i] = paneIsRotating((PlotPane)i) ? 100+(int)plot_ch[i] : (int)plot_ch[i];

        // prefix map style with N if not showing night
        char map_style[NV_COREMAPSTYLE_LEN+1];
        (void) getMapStyle(map_style);
        if (!night_on) {
            memmove (map_style+1, map_style, sizeof(map_style)-1);
            map_style[0] = 'N';
        }

        // kx3 baud else gpio on/off
        int gpio = getKX3Baud();
        if (gpio == 0) {
            if (GPIOOk())
                gpio = 1;
            else if (found_mcp)
                gpio = 2;
        }

        // which phot, if any
        int io = 0;
        if (found_phot) io |= 1;
        if (found_ltr) io |= 2;
        if (found_mcp) io |= 4;
        if (getI2CFilename()) io |= 8;

        // combine rss_on and rss_local
        int rss_code = rss_on + 2*rss_local;

        // gimbal and rig bit mask: 4 = rig, 2 = azel  1 = az only
        bool gconn, vis_now, has_el, gstop, gauto;
        float az, el;
        bool gbl_on = getGimbalState (gconn, vis_now, has_el, gstop, gauto, az, el);
        bool rig_on = getRigctld (NULL, NULL);
        bool flrig = getFlrig (NULL, NULL);
        int rr_score = (gbl_on ? (has_el ? 2 : 1) : 0) | (rig_on ? 4 : 0) | (flrig ? 8 : 0);

        // brb_mode plus 100 to indicate rotation code
        int brb = brb_mode;
        if (BRBIsRotating())
            brb += 100;

        // GPSD
        int gpsd = 0;
        if (useGPSDTime())
            gpsd |= 1;
        if (useGPSDLoc())
            gpsd |= 2;

        // date formatting
        int dayf = (int)getDateFormat();
        if (weekStartsOnMonday())                       // added in 2.86
            dayf |= 4;

        // number of dashed colors                      // added to first LV6 in 2.90
        int n_dashed = 0;
        for (int i = 0; i < N_CSPR; i++)
            if (getColorDashed((ColorSelection)i))
                n_dashed++;

        // path size: 0 none, 1 thin, 2 wide
        int path = getSpotPathWidth();                  // returns 0, THINPATHSZ or WIDEPATHSZ
        if (path)
            path = (path == THINPATHSZ ? 1 : 2);

        // label spots: 0 no, 1 prefix, 2 call, 3 dot
        int spots = 0;
        switch (getSpotLabelType()) {
        case LBL_NONE:   spots = 0; break;
        case LBL_DOT:    spots = 3; break;
        case LBL_PREFIX: spots = 1; break;
        case LBL_CALL:   spots = 2; break;
        case LBL_N:      fatalError ("Bogus log spots\n");
        }

        // crc code
        int crc = flash_crc_ok;
        if (want_kbcursor)
            crc |= (1<<15);                             // max old _debug_ was 1<<14

        // callsign colors
        uint16_t call_fg, call_bg;
        uint8_t call_rb;
        NVReadUInt16 (NV_CALL_FG_COLOR, &call_fg);
        NVReadUInt8 (NV_CALL_BG_RAINBOW, &call_rb);
        if (call_rb)
            call_bg = 1;                                // unlikely color to mean rainbow
        else
            NVReadUInt16 (NV_CALL_BG_COLOR, &call_bg);

        // ntp source
        int ntp = 0;
        if (useLocalNTPHost())
            ntp = 1;
        else if (useOSTime())
            ntp = 2;                                    // added in 3.05

        // panzoom
        bool pz = pan_zoom.zoom != MIN_ZOOM || pan_zoom.pan_x != 0 || pan_zoom.pan_y != 0;

        snprintf (ua, sizeof(ua),
            "User-Agent: %s/%s (id %u up %lld) crc %d "
                "LV7 %s %d %d %d %d %d %d %d %d %d %d %d %d %d %.2f %.2f %d %d %d %d "
                "%d %d %d %d %d %d %d %d %d %d %d %d "
                "%d %d %d %d %d %u %u %d "                              // LV6 starts here
                "LV7 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "     // LV7 starts here
                "\r\n",
            platform, hc_version, ESP.getChipId(), (long long)getUptime(NULL,NULL,NULL,NULL), crc,
            map_style, main_page, mapgrid_choice, plotops[PANE_1], plotops[PANE_2], plotops[PANE_3],
            de_time_fmt, brb, dx_info_for_sat, rss_code, useMetricUnits(),
            getNBMEConnected(), gpio, io, getBMETempCorr(BME_76), getBMEPresCorr(BME_76),
            desrss, dxsrss, BUILD_W, dpy_mode,

            // new for LV5:
            alarms, getCenterLng(), (int)auxtime /* getDoy() before 2.80 */, names_on, getDemoMode(),
            (int)getSWEngineState(NULL,NULL), (int)getBigClockBits(), utcOffset(), gpsd,
            rss_interval, dayf, rr_score,

            // new for LV6:
            useMagBearing(), n_dashed, ntp, path, spots,
            call_fg, call_bg, !clockTimeOk(), // default clock 0 == ok

            // new for LV7:
            scrollTopToBottom(), nMoreScrollRows(), rankSpaceWx(), showNewDXDEWx(), getPaneRotationPeriod(),
            pw_file != NULL, n_roweb>0, pz, plotops[PANE_0], screenIsLocked(), showPIP(),
            0, 0, 0, 0);

    } else {
        snprintf (ua, sizeof(ua), _FX("User-Agent: %s/%s (id %u up %lld) crc %d\r\n"),
            platform, hc_version, ESP.getChipId(), (long long)getUptime(NULL,NULL,NULL,NULL), flash_crc_ok);
    }

    // send
    client.print(ua);

}

/* issue an HTTP Get for an arbitary page
 */
static void httpGET (WiFiClient &client, const char *server, const char *page)
{

    FWIFIPR (client, F("GET ")); client.print(page); FWIFIPRLN (client, F(" HTTP/1.0"));
    FWIFIPR (client, F("Host: ")); client.println (server);
    sendUserAgent (client);
    FWIFIPRLN (client, F("Connection: close\r\n"));

}

/* issue an HTTP Get to a /ham/HamClock page named in ram
 */
void httpHCGET (WiFiClient &client, const char *server, const char *hc_page)
{
    static const char hc[] PROGMEM = "/ham/HamClock";
    StackMalloc full_mem(strlen(hc_page) + sizeof(hc));         // sizeof includes the EOS
    char *full_hc_page = (char *) full_mem.getMem();
    snprintf (full_hc_page, full_mem.getSize(), "%s%s", _FX_helper(hc), hc_page);
    httpGET (client, server, full_hc_page);
}

/* issue an HTTP Get to a /ham/HamClock page named in PROGMEM
 */
void httpHCPGET (WiFiClient &client, const char *server, const char *hc_page_progmem)
{
    httpHCGET (client, server, _FX_helper(hc_page_progmem));
}

/* skip the given wifi client stream ahead to just after the first blank line, return whether ok.
 * this is often used so subsequent stop() on client doesn't slam door in client's face with RST.
 * Along the way, if find a header field with the given name (unless NULL) return value in the given string.
 * if header is not found, we still return true but value[0] will be '\0'.
 */
bool httpSkipHeader (WiFiClient &client, const char *header, char *value, int value_len)
{
    char line[200];

    // prep
    int hdr_len = header ? strlen(header) : 0;
    if (value)
        value[0] = '\0';
    char *hdr;

    // read until find a blank line
    do {
        if (!getTCPLine (client, line, sizeof(line), NULL))
            return (false);
        // Serial.println (line);

        if (header && value && (hdr = strstr (line, header)) != NULL)
            snprintf (value, value_len, "%s", hdr + hdr_len);

    } while (line[0] != '\0');  // getTCPLine absorbs \r\n so this tests for a blank line

    return (true);
}

/* same but when we don't care about any header field;
 * so we pick up Remote_Addr for postDiags()
 */
bool httpSkipHeader (WiFiClient &client)
{
    return (httpSkipHeader (client, _FX("Remote_Addr: "), remote_addr, sizeof(remote_addr)));
}

/* retrieve and plot latest and predicted DRAP indices, return whether io ok
 */
static bool updateDRAP (const SBox &box)
{
    DRAPData drap;
    bool ok = retrieveDRAP (drap);

    updateClocks(false);

    if (ok) {

        if (!drap.data_ok) {
            plotMessage (box, DRAPPLOT_COLOR, _FX("DRAP data invalid"));
        } else {
            // plot
            plotXY (box, drap.x, drap.y, DRAPDATA_NPTS, _FX("Hours"), _FX("DRAP, max MHz"), DRAPPLOT_COLOR,
                                                            0, 0, drap.y[DRAPDATA_NPTS-1]);
        }

        // update NCDXF box too either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {
        plotMessage (box, DRAPPLOT_COLOR, _FX("DRAP connection failed"));
    }

    // done
    return (ok);
}

/* retrieve and plot latest and predicted kp indices, return whether io ok
 */
static bool updateKp(const SBox &box)
{
    // data are provided every 3 hours == 8/day. collect 7 days of history + 2 days of predictions
    KpData kp;

    bool ok = retrieveKp (kp);

    updateClocks(false);

    if (ok) {

        if (!kp.data_ok) {
            plotMessage (box, KP_COLOR, _FX("Kp data invalid"));
        } else {
            // plot
            plotXY (box, kp.x, kp.p, KP_NV, "Days", "Planetary Kp", KP_COLOR, 0, 9, space_wx[SPCWX_KP].value);
        }

        // update NCDXF box too either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {
        plotMessage (box, KP_COLOR, _FX("Kp connection failed"));
    }

    // done
    return (ok);
}

/* retrieve and plot latest xray indices, return whether io ok
 */
static bool updateXRay(const SBox &box)
{
    XRayData xray;

    bool ok = retrieveXRay (xray);

    updateClocks(false);

    if (ok) {

        if (!xray.data_ok) {
            plotMessage (box, XRAY_LCOLOR, "XRay data invalid");
        } else {
            // overlay short over long with fixed y axis
            char level_str[10];
            plotXYstr (box, xray.x, xray.l, XRAY_NV, "Hours", "GOES 16 X-Ray", XRAY_LCOLOR, -9, -2, NULL)
                 && plotXYstr (box, xray.x, xray.s, XRAY_NV, NULL, NULL, XRAY_SCOLOR, -9, -2,     
                                xrayLevel(level_str, space_wx[SPCWX_XRAY]));
        }

        // update NCDXF box too either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {
        plotMessage (box, XRAY_LCOLOR, _FX("X-Ray connection failed"));
    }

    // done
    return (ok);
}

/* retrieve and plot fresh sun spot indices, return whether io ok
 */
static bool updateSunSpots (const SBox &box)
{
    SunSpotData ssn;

    bool ok = retrieveSunSpots (ssn);

    updateClocks(false);

    if (ok) {

        if (!ssn.data_ok) {
            plotMessage (box, SSN_COLOR, "SSN data invalid");
        } else {
            // plot, showing value as traditional whole number
            char label[20];
            snprintf (label, sizeof(label), "%.0f", ssn.ssn[SSN_NV-1]);
            plotXYstr (box, ssn.x, ssn.ssn, SSN_NV, "Days", "Sunspot Number", SSN_COLOR, 0, 0, label);
        }

        // update NCDXF box too either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {
        plotMessage (box, SSN_COLOR, _FX("SSN connection failed"));
    }

    // done
    return (ok);
}

/* retrieve and plot latest and predicted solar flux indices, return whether io ok.
 */
static bool updateSolarFlux(const SBox &box)
{
    SolarFluxData sf;

    bool ok = retrievSolarFlux(sf);

    updateClocks(false);

    if (ok) {
        if (!sf.data_ok) {
            plotMessage (box, SFLUX_COLOR, "Solar Flux data invalid");
        } else {
            plotXY (box, sf.x, sf.sflux, SFLUX_NV, "Days", "10.7 cm Solar flux",
                                                SFLUX_COLOR, 0, 0, sf.sflux[SFLUX_NV-10]);
        }

        // update NCDXF box too either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {
        plotMessage (box, SFLUX_COLOR, _FX("Flux connection failed"));
    }

    // done
    return (ok);
}

/* retrieve and plot latest and predicted solar wind indices, return whether io ok.
 */
static bool updateSolarWind(const SBox &box)
{
    SolarWindData sw;

    bool ok = retrieveSolarWind (sw);

    if (ok) {
        if (!sw.data_ok) {
            plotMessage (box, SWIND_COLOR, "Solar wind data invalid");
        } else {
            plotXY (box, sw.x, sw.y, sw.n_values, "Hours", "Solar wind", SWIND_COLOR,0,0,sw.y[sw.n_values-1]);
        }

        // update NCDXF box too either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {
        plotMessage (box, SWIND_COLOR, _FX("Sol Wind connection failed"));
    }

    // done
    return (ok);
}

/* retrieve and plot latest BZBT indices, return whether io ok
 */
static bool updateBzBt (const SBox &box)
{
    BzBtData bzbt;

    bool ok = retrieveBzBt (bzbt);

    if (ok) {

        if (!bzbt.data_ok) {
            plotMessage (box, BZBT_BZCOLOR, "BzBt data invalid");
        } else {

            // find first within 25 hours thence min/max over both
            float min_bzbt = 1e10, max_bzbt = -1e10;
            int f25 = -1;
            for (int i = 0; i < BZBT_NV; i++) {
                if (f25 < 0 && bzbt.x[i] >= -25)
                    f25 = i;
                if (f25 >= 0) {
                    if (bzbt.bz[i] < min_bzbt)
                        min_bzbt = bzbt.bz[i];
                    else if (bzbt.bz[i] > max_bzbt)
                        max_bzbt = bzbt.bz[i];
                    if (bzbt.bt[i] < min_bzbt)
                        min_bzbt = bzbt.bt[i];
                    else if (bzbt.bt[i] > max_bzbt)
                        max_bzbt = bzbt.bt[i];
                }
            }

            // plot
            char bz_label[30];
            snprintf (bz_label, sizeof(bz_label), "%.1f", bzbt.bz[BZBT_NV-1]);         // newest Bz
            plotXYstr (box, bzbt.x+f25, bzbt.bz+f25, BZBT_NV-f25, "Hours", "Solar Bz and Bt, nT",
                                        BZBT_BZCOLOR, min_bzbt, max_bzbt, NULL)
                     && plotXYstr (box, bzbt.x+f25, bzbt.bt+f25, BZBT_NV-f25, NULL, NULL,
                                        BZBT_BTCOLOR, min_bzbt, max_bzbt, bz_label);
        }

        // update NCDXF box too either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {

        plotMessage (box, BZBT_BZCOLOR, _FX("BzBt update error"));
    }

    // done
    return (ok);
}

/* retrieve and draw latest band conditions in box b if needed or requested.
 * return whether io ok.
 */
static bool updateBandConditions(const SBox &box, bool force)
{
    // update if asked to or out of sync with prop map or it's just been a while
    bool update_bc = force || (prop_map.active && tdiff(bc_time,map_time) >= 3600)
                           || (tdiff (nowWO(), bc_time) >= 3600)
                           || myNow() >= bc_matrix.next_update;

    static char config[100];    // retain value after each retrieval

    // download if so and note whether io ok
    bool io_ok = true;
    if (update_bc) {

        // fresh download
        if (retrieveBandConditions (config))
            bc_matrix.next_update = nextRetrieval (PLOT_CH_BC, BC_INTERVAL);
        else {
            bc_matrix.next_update = nextWiFiRetry(PLOT_CH_BC);
            io_ok = false;
        }

        // note time of attemp to coordinate with maps
        bc_time = nowWO();
    }

    // plot
    if (bc_matrix.ok) {

        plotBandConditions (box, 0, &bc_matrix, config);

    } else {

        plotMessage (box, RA8875_RED, _FX("No VOACAP data"));

        // if problem persists more than an hour, this prevents the tdiff's above from being true every time
        map_time = bc_time = nowWO() - 1000;
    }

    return (io_ok);
}

/* display the RSG NOAA solar environment scale values in the given box.
 * return whether data transaction was valid, even if data are bad.
 */
static bool updateNOAASWx(const SBox &box)
{
    updateClocks(false);
    return (plotNOAASWx (box));
}

/* retrieve and plot latest aurora indices, return whether io ok
 */
static bool updateAurora (const SBox &box)
{
    AuroraData a;

    bool ok = retrieveAurora (a);

    if (ok) {

        if (!a.data_ok) {
            plotMessage (box, AURORA_COLOR, "Aurora data invalid");
        } else {

            // plot
            char aurora_label[30];
            snprintf (aurora_label, sizeof(aurora_label), "%.0f", a.percent[a.n_points-1]);
            plotXYstr (box, a.age_hrs, a.percent, a.n_points, "Hours", "Aurora Chances, max %",
                                        AURORA_COLOR, 0, 100, aurora_label);
        }

        // update NCDXF box either way
        if (brb_mode == BRB_SHOW_SWSTATS)
            drawSpaceStats(RA8875_BLACK);

    } else {

        plotMessage (box, AURORA_COLOR, _FX("Aurora error"));
    }

    // done
    return (ok);
}

/* get next line from client in line[] then return true, else nothing and return false.
 * line[] will have \r and \n removed and end with \0, optional line length in *ll will not include \0.
 * if line is longer than line_len it will be silently truncated.
 */
bool getTCPLine (WiFiClient &client, char line[], uint16_t line_len, uint16_t *ll)
{
    // update network stack
    yield();

    // decrement available length so there's always room to add '\0'
    line_len -= 1;

    // read until find \n or time out.
    uint16_t i = 0;
    while (true) {
        char c;
        if (!getTCPChar (client, &c))
            return (false);
        if (c == '\r')
            continue;
        if (c == '\n') {
            line[i] = '\0';
            if (ll)
                *ll = i;
            // Serial.println(line);
            return (true);
        } else if (i < line_len)
            line[i++] = c;
    }
}

/* convert an array of 4 big-endian network-order bytes into a uint32_t
 */
static uint32_t crackBE32 (uint8_t bp[])
{
    union {
        uint32_t be;
        uint8_t ba[4];
    } be4;

    be4.ba[3] = bp[0];
    be4.ba[2] = bp[1];
    be4.ba[1] = bp[2];
    be4.ba[0] = bp[3];

    return (be4.be);
}

/* it is MUCH faster to print F() strings in a String than using them directly.
 * see esp8266/2.3.0/cores/esp8266/Print.cpp::print(const __FlashStringHelper *ifsh) to see why.
 */
void FWIFIPR (WiFiClient &client, const __FlashStringHelper *str)
{
    String _sp(str);
    client.print(_sp);
}

void FWIFIPRLN (WiFiClient &client, const __FlashStringHelper *str)
{
    String _sp(str);
    client.println(_sp);
}

// handy wifi health check
bool wifiOk()
{
    if (WiFi.status() == WL_CONNECTED)
        return (true);

    // retry occasionally
    static uint32_t last_wifi;
    if (timesUp (&last_wifi, WIFI_RETRY*1000)) {
        initWiFi(false);
        return (WiFi.status() == WL_CONNECTED);
    } else
        return (false);
}

/* arrange for everything to update immediately
 */
void initWiFiRetry()
{
    // this addresses the panes
    memset (next_update, 0, sizeof(next_update));

    // a few more misc
    next_wwx = 0;
    next_map = 0;
    brb_updateT = 0;
    scheduleRSSNow();
}

/* show PLOT_CH_DEWX or PLOT_CH_DXWX in pp immediately then arrange to resume normal pp operation
 * after DXPATH_LINGER using revert_time and revert_pp.
 */
static void revertWXPane (PlotPane pp, PlotChoice wxpc)
{
    // easier to just show weather immediately without using the pane rotation system
    if (wxpc == PLOT_CH_DEWX) {
        (void) updateDEWX (plot_b[pp]);
    } else if (wxpc == PLOT_CH_DXWX) {
        (void) updateDXWX (plot_b[pp]);
    } else {
        fatalError ("revertWXPane with pc %d\n", (int)wxpc);
        return;                 // lint
    }

    // record where a revert is in progress and when it will be over.
    revert_time = next_update[pp] = myNow() + DXPATH_LINGER/1000;
    revert_pane = pp;

    // best to do a fresh update of original content when revert is over
    fresh_update[plot_ch[pp]] = true;

    // a few plot types require extra processing when shut down
    switch (plot_ch[pp]) {
    case PLOT_CH_DXCLUSTER:
        closeDXCluster();       // will reopen after revert
        break;
    case PLOT_CH_GIMBAL:
        closeGimbal();          // will reopen after revert
        break;
    default:
        break;                  // lint
    }
}

/* request that the given pane use fresh data, if and when it next becomes visible.
 * for most panes:
 *   if the pane is currently visible: fresh update immediately;
 *   if in rotation but not visible:   it is marked for fresh update when its turn comes;
 *   if not selected anywhere:         we do nothing.
 * for PLOT_CH_DEWX or PLOT_CH_DXWX which are only for temporary display called reverting:
 *   if the pane is currently visible: it will refresh immediately;
 *   if in rotation but not visible:   immediately displayed in its pane, normal rotation after DXPATH_LINGER
 *   if not selected anywhere:         immediately displayed in PANE_1, normal rotation after DXPATH_LINGER
 */
void scheduleNewPlot (PlotChoice pc)
{
    PlotPane pp = findPaneChoiceNow (pc);
    if (pp == PANE_NONE) {
        // not currently visible ...
        pp = findPaneForChoice (pc);
        if (pp == PANE_NONE) {
            // ... and not in any rotation set either
            if (pc == PLOT_CH_DEWX || pc == PLOT_CH_DXWX) {
                if (showNewDXDEWx()) {
                    // force immediate WX in PANE_1, then revert after DXPATH_LINGER
                    revertWXPane (PANE_1, pc);
                }
            }
            // ignore all others
        } else {
            // ... but is in rotation
            if (pc == PLOT_CH_DEWX || pc == PLOT_CH_DXWX) {
                if (showNewDXDEWx()) {
                    // force immediate WX in pane pp, then revert after DXPATH_LINGER
                    revertWXPane (pp, pc);
                }
            } else {
                // just mark for fresh update when it's turn comes
                fresh_update[pc] = true;
            }
        } 
    } else {
        // currently visible: force fresh update now
        next_update[pp] = 0;
        fresh_update[pc] = true;
    }
}

/* called to schedule an immediate update of the given VOACAP map.
 * leave core_map unchanged to use later if VOACAP turned off.
 */
void scheduleNewVOACAPMap(PropMapSetting &pm)
{
    bool active_changed = prop_map.active != pm.active;
    prop_map = pm;
    if (prop_map.active || active_changed)
        next_map = 0;
}

/* called to schedule an immediate update of the give core map, unless being turned off
 * turns off any VOACAP map.
 */
void scheduleNewCoreMap(CoreMaps cm)
{
    prop_map.active = false;
    core_map = cm;
    if (cm != CM_NONE)
        next_map = 0;
}

/* schedule a refresh of the current map
 */
void scheduleFreshMap (void)
{
    next_map = 0;
}

/* return current NTP response time list.
 * N.B. this is the real data, caller must not modify.
 */
int getNTPServers (const NTPServer **listp)
{
    *listp = ntp_list;
    return (N_NTP);
}

/* return when the given pane will next update.
 */
time_t nextPaneRotation(PlotPane pp)
{
    return (next_update[pp]);
}

/* return pane for which taps are to be ignored because a revert is in progress, if any
 */
PlotPane ignorePaneTouch()
{
    if (myNow() < revert_time)
        return (revert_pane);
    return (PANE_NONE);
}
