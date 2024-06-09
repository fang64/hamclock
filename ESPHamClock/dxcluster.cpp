/* handle the DX Cluster display. Only active when visible on a Pane.
 *
 * Clusters:
 *   [ ] support Spider and AR
 *   [ ] as of 3.03 replace show/header with cty_wt_mod-ll.txt -- too big for ESP
 *
 * WSJT-X:
 *   [ ] packet definition: https://sourceforge.net/p/wsjt/wsjtx/ci/master/tree/Network/NetworkMessage.hpp
 *   [ ] We don't actually enforce the Status ID to be WSJT-X so this may also work for, say, JTCluster.
 */

#include "HamClock.h"



// config 
#define DXC_COLOR       RA8875_GREEN
#define KEEPALIVE_DT    (10*60*1000U)           // send something if idle this long, millis
#define CLRBOX_DX       10                      // dx clear control box center from left
#define CLRBOX_DY       15                      // dy " down from top
#define CLRBOX_R        4                       // clear box radius
#define SPOTMRNOP       (tft.SCALESZ+4)         // raw spot marker radius when no path
#define SUBTITLE_Y0     32                      // sub title y down from box top
#define MAX_AGE         300000                  // max age to restore spot in list, millis

// connection info
static WiFiClient dx_client;                    // persistent TCP connection while displayed ...
static WiFiUDP wsjtx_server;                    // or persistent UDP "connection" to WSJT-X client program
static uint32_t last_action;                    // time of most recent spot or user activity, millis()
static bool multi_cntn;                         // set when cluster has noticed multiple connections
#define MAX_LCN        10                       // max lost connections per MAX_LCDT
#define MAX_LCDT       3600                     // max lost connections period, seconds

// spots. only kept while open.
static DXSpot *dx_spots;                        // malloced list, oldest at [0]
static int max_spots;                           // max dx_spots allowed
static ScrollState dxc_ss;                      // scrolling info



// type
typedef enum {
    CT_UNKNOWN,
    CT_ARCLUSTER,
    CT_DXSPIDER,
    CT_WSJTX,
} DXClusterType;
static DXClusterType cl_type;

#if defined(__GNUC__)
static void dxcLog (const char *fmt, ...) __attribute__ ((format (__printf__, 1, 2)));
static void showDXClusterErr (const SBox &box, const char *fmt, ...) __attribute__ ((format (__printf__, 2, 3)));
#else
static void dxcLog (const char *fmt, ...);
static void showDXClusterErr (const SBox &box, const char *fmt, ...);
#endif


/* draw, else erase, the clear spots control
 */
static void drawClearListBtn (const SBox &box, bool draw)
{
    uint16_t color = draw ? DXC_COLOR : RA8875_BLACK;

    tft.drawRect (box.x + CLRBOX_DX - CLRBOX_R, box.y + CLRBOX_DY - CLRBOX_R, 2*CLRBOX_R+1, 2*CLRBOX_R+1, color);
    tft.drawLine (box.x + CLRBOX_DX - CLRBOX_R, box.y + CLRBOX_DY - CLRBOX_R,
                  box.x + CLRBOX_DX + CLRBOX_R, box.y + CLRBOX_DY + CLRBOX_R, color);
    tft.drawLine (box.x + CLRBOX_DX + CLRBOX_R, box.y + CLRBOX_DY - CLRBOX_R,
                  box.x + CLRBOX_DX - CLRBOX_R, box.y + CLRBOX_DY + CLRBOX_R, color);
}


/* draw all currently visible spots in the pane then update scroll markers if more
 */
static void drawAllVisDXCSpots (const SBox &box)
{
    int min_i, max_i;
    if (dxc_ss.getVisIndices (min_i, max_i) > 0) {
        for (int i = min_i; i <= max_i; i++) {
            DXSpot &spot = dx_spots[i];
            uint16_t bg_col = onDXWatchList (spot.tx_call) ? RA8875_RED : RA8875_BLACK;
            drawSpotOnList (box, spot, dxc_ss.getDisplayRow(i), bg_col);
        }
    }

    dxc_ss.drawScrollDownControl (box, DXC_COLOR);
    dxc_ss.drawScrollUpControl (box, DXC_COLOR);
    drawClearListBtn (box, dxc_ss.n_data > 0);
}


/* shift the visible list to show newer spots, if appropriate
 */
static void scrollDXCUp (const SBox &box)
{
    if (dxc_ss.okToScrollUp()) {
        dxc_ss.scrollUp();
        drawAllVisDXCSpots(box);
    }
}

/* shift the visible list to show older spots, if appropriate
 */
static void scrollDXCDown (const SBox &box)
{
    if (dxc_ss.okToScrollDown()) {
        dxc_ss.scrollDown ();
        drawAllVisDXCSpots (box);
    }
}

/* log the given message
 * N.B. we assume trailing \n and remove any \a characters
 */
static void dxcLog (const char *fmt, ...)
{
    // format
    char msg[400];
    va_list ap;
    va_start (ap, fmt);
    (void) vsnprintf (msg, sizeof(msg)-1, fmt, ap); // allow for adding \n
    va_end (ap);

    // remove all \a
    char *bell;
    while ((bell = strchr (msg, '\a')) != NULL)
        *bell = ' ';

    // insure trailing \n just to be kind
    if (!strchr (msg, '\n'))
        strcat (msg, "\n");

    // print with prefix
    Serial.printf ("DXC: %s", msg);
}

/* set radio and DX from given row, known to be defined
 */
static void engageDXCRow (DXSpot &s)
{
    setRadioSpot(s.kHz);
    newDX (s.tx_ll, NULL, s.tx_call);
}



/* add a new spot both on map and in list, scrolling list if already full.
 */
static void addDXClusterSpot (const SBox &box, DXSpot &new_spot)
{
    // skip if looks to be same as any previous
    for (int i = 0; i < dxc_ss.n_data; i++) {
        DXSpot &spot = dx_spots[i];
        if (fabsf(new_spot.kHz-spot.kHz) < 0.1F && strcmp (new_spot.tx_call, spot.tx_call) == 0) {
            dxcLog ("DXC: %s dup\n", new_spot.tx_call);
            return;
        }
    }

    // nice to insure calls are upper case
    strtoupper (new_spot.rx_call);
    strtoupper (new_spot.tx_call);

    // grow or slide down over oldest if full
    if (dxc_ss.n_data == max_spots) {
        memmove (dx_spots, dx_spots+1, (max_spots-1) * sizeof(*dx_spots));
        dxc_ss.n_data = max_spots - 1;
    } else {
        // grow dx_spots
        dx_spots = (DXSpot *) realloc (dx_spots, (dxc_ss.n_data+1) * sizeof(DXSpot));
        if (!dx_spots)
            fatalError ("No memory for %d spots", dxc_ss.n_data);
    }

    // append
    DXSpot &list_spot = dx_spots[dxc_ss.n_data++];
    list_spot = new_spot;

    // update list
    dxc_ss.scrollToNewest();
    drawAllVisDXCSpots(box);

    // show on map, only label tx end
    drawSpotPathOnMap (list_spot);
    drawSpotLabelOnMap (list_spot, LOM_TXEND, LOM_ALL);
    drawSpotLabelOnMap (list_spot, LOM_RXEND, LOM_JUSTDOT);
}

/* given address of pointer into a WSJT-X message, extract bool and advance pointer to next field.
 */
static bool wsjtx_bool (uint8_t **bpp)
{
    bool x = **bpp > 0;
    *bpp += 1;
    return (x);
}

/* given address of pointer into a WSJT-X message, extract uint32_t and advance pointer to next field.
 * bytes are big-endian order.
 */
static uint32_t wsjtx_quint32 (uint8_t **bpp)
{
    uint32_t x = ((*bpp)[0] << 24) | ((*bpp)[1] << 16) | ((*bpp)[2] << 8) | (*bpp)[3];
    *bpp += 4;
    return (x);
}

/* given address of pointer into a WSJT-X message, extract utf8 string and advance pointer to next field.
 * N.B. returned string points into message so will only be valid as long as message memory is valid.
 */
static char *wsjtx_utf8 (uint8_t **bpp)
{
    // save begining of this packet entry
    uint8_t *bp0 = *bpp;

    // decode length
    uint32_t len = wsjtx_quint32 (bpp);

    // check for flag meaning null length string same as 0 for our purposes
    if (len == 0xffffffff)
        len = 0;

    // advance packet pointer over contents
    *bpp += len;

    // copy contents to front, overlaying length, to make room to add EOS
    memmove (bp0, bp0+4, len);
    bp0[len] = '\0';

    // dxcLog ("utf8 %d '%s'\n", len, (char*)bp0);

    // return address of content now within packet
    return ((char *)bp0);
}

/* given address of pointer into a WSJT-X message, extract double and advance pointer to next field.
 */
static uint64_t wsjtx_quint64 (uint8_t **bpp)
{
    uint64_t x;

    x = ((uint64_t)(wsjtx_quint32(bpp))) << 32;
    x |= wsjtx_quint32 (bpp);

    return (x);
}

/* return whether the given packet contains a WSJT-X Status packet.
 * if true, leave *bpp positioned just after ID.
 */
static bool wsjtxIsStatusMsg (uint8_t **bpp)
{

    // crack magic header
    uint32_t magic = wsjtx_quint32 (bpp);
    // dxcLog ("magic 0x%x\n", magic);
    if (magic != 0xADBCCBDA) {
        dxcLog ("packet received but wrong magic\n");
        return (false);
    }

    // crack and ignore the max schema value
    (void) wsjtx_quint32 (bpp);                         // skip past max schema

    // crack message type. we only care about Status messages which are type 1
    uint32_t msgtype = wsjtx_quint32 (bpp);
    // dxcLog ("type %d\n", msgtype);
    if (msgtype != 1)
        return (false);

    // if we get this far assume packet is what we want.
    // crack ID but ignore to allow compatibility with clones.
    volatile char *id = wsjtx_utf8 (bpp);
    (void)id;           // lint
    // dxcLog ("id '%s'\n", id);
    // if (strcmp ("WSJT-X", id) != 0)
        // return (false);

    // ok!
    return (true);
}

/* parse and process WSJT-X message known to be Status.
 * *bpp is positioned just after ID field.
 * draw on screen in box.
 * return whether good.
 */
static bool wsjtxParseStatusMsg (const SBox &box, uint8_t **bpp)
{
    // dxcLog ("Parsing status\n");

    // crack remaining fields down to grid
    uint32_t hz = wsjtx_quint64 (bpp);                      // capture freq
    (void) wsjtx_utf8 (bpp);                                // skip over mode
    char *dx_call = wsjtx_utf8 (bpp);                       // capture DX call 
    (void) wsjtx_utf8 (bpp);                                // skip over report
    (void) wsjtx_utf8 (bpp);                                // skip over Tx mode
    (void) wsjtx_bool (bpp);                                // skip over Tx enabled flag
    (void) wsjtx_bool (bpp);                                // skip over transmitting flag
    (void) wsjtx_bool (bpp);                                // skip over decoding flag
    (void) wsjtx_quint32 (bpp);                             // skip over Rx DF -- not always correct
    (void) wsjtx_quint32 (bpp);                             // skip over Tx DF
    char *de_call = wsjtx_utf8 (bpp);                       // capture DE call 
    char *de_grid = wsjtx_utf8 (bpp);                       // capture DE grid
    char *dx_grid = wsjtx_utf8 (bpp);                       // capture DX grid

    // dxcLog ("WSJT: %7d %s %s %s %s\n", hz, de_call, de_grid, dx_call, dx_grid);

    // ignore if frequency is clearly bogus (which I have seen)
    if (hz == 0)
        return (false);

    // get each ll from grids
    LatLong ll_de, ll_dx;
    if (!maidenhead2ll (ll_de, de_grid)) {
        dxcLog ("%s invalid or missing DE grid: %s\n", de_call, de_grid);
        return (false);
    }
    if (!maidenhead2ll (ll_dx, dx_grid)) {
        dxcLog ("%s invalid or missing DX grid: %s\n", dx_call, dx_grid);
        return (false);
    }

    // looks good, create new record
    DXSpot new_spot;
    memset (&new_spot, 0, sizeof(new_spot));
    strncpy (new_spot.tx_call, dx_call, sizeof(new_spot.tx_call)-1);        // preserve EOS
    strncpy (new_spot.rx_call, de_call, sizeof(new_spot.rx_call)-1);        // preserve EOS
    strncpy (new_spot.tx_grid, dx_grid, sizeof(new_spot.tx_grid)-1);        // preserve EOS
    strncpy (new_spot.rx_grid, de_grid, sizeof(new_spot.rx_grid)-1);        // preserve EOS
    new_spot.kHz = hz*1e-3F;
    new_spot.rx_ll = ll_de;
    new_spot.tx_ll = ll_dx;

    // time is now
    new_spot.spotted = myNow();

    // add to list
    addDXClusterSpot (box, new_spot);

    // ok
    return (true);
}

/* display the given error message and shut down the connection.
 */
static void showDXClusterErr (const SBox &box, const char *fmt, ...)
{
    char buf[500];
    va_list ap;
    va_start (ap, fmt);
    size_t ml = snprintf (buf, sizeof(buf), "DX Cluster error: ");
    vsnprintf (buf+ml, sizeof(buf)-ml, fmt, ap);
    va_end (ap);

    plotMessage (box, RA8875_RED, buf);

    // log
    dxcLog ("%s\n", buf);

    // shut down connection
    closeDXCluster();
}


/* increment NV_DXMAX_N
 */
static void incLostConn(void)
{
    uint8_t n_lostconn;
    if (!NVReadUInt8 (NV_DXMAX_N, &n_lostconn))
        n_lostconn = 0;
    n_lostconn += 1;
    NVWriteUInt8 (NV_DXMAX_N, n_lostconn);
    dxcLog ("lost connection: now %u\n", n_lostconn);
}

/* return whether max lost connection rate has been reached
 */
static bool checkLostConnRate()
{
    uint32_t t0 = myNow();                  // time now
    uint32_t t_maxconn;                     // time when the limit was last reached
    uint8_t n_lostconn;                     // n connections lost so far since t_maxconn

    // get current state
    if (!NVReadUInt32 (NV_DXMAX_T, &t_maxconn)) {
        t_maxconn = t0;
        NVWriteUInt32 (NV_DXMAX_T, t_maxconn);
    }
    if (!NVReadUInt8 (NV_DXMAX_N, &n_lostconn)) {
        n_lostconn = 0;
        NVWriteUInt8 (NV_DXMAX_N, n_lostconn);
    }
    dxcLog ("%u lost connections since %u\n", n_lostconn, t_maxconn);

    // check if max lost connections have been hit
    bool hit_max = false;
    if (n_lostconn > MAX_LCN) {
        if (t0 < t_maxconn + MAX_LCDT) {
            // hit the max during the last MAX_LCDT 
            hit_max = true;
        } else {
            // record the time and start a new count
            NVWriteUInt32 (NV_DXMAX_T, t0);
            n_lostconn = 0;
            NVWriteUInt8 (NV_DXMAX_N, n_lostconn);
        }
    }

    return (hit_max);
}

/* given a cluster line, set multi_cntn if it seems to be telling us it has detected multiple connections
 *   from the same call-ssid.
 * not at all sure this works everywhere.
 * Only Spiders seem to care enough to dicsonnect, AR clusters report but otherwise don't care.
 */
static void detectMultiConnection (const char *line)
{
    // first seems typical for spiders, second for AR
    if (strstr (line, "econnected") != NULL || strstr (line, "Dupe call") != NULL)
        multi_cntn = true;
}

/* try to connect to the cluster.
 * if success: dx_client or wsjtx_server is live and return true,
 * else: both are closed, display error msg in box, return false.
 * N.B. inforce MAX_LCN
 */
static bool connectDXCluster (const SBox &box)
{
    // check max lost connection rate
    if (checkLostConnRate()) {
        showDXClusterErr (box, "Hit max %d lost connections/hr limit", MAX_LCN);
        return (false);
    }

    // reset connection flag
    multi_cntn = false;

    // get cluster connection info
    const char *dxhost = getDXClusterHost();
    int dxport = getDXClusterPort();

    dxcLog ("Connecting to %s:%d\n", dxhost, dxport);

    if (useWSJTX()) {

        // create fresh UDP for WSJT-X
        wsjtx_server.stop();

        // open normal or multicast depending on first octet
        bool ok;
        int first_octet = atoi (dxhost);
        if (first_octet >= 224 && first_octet <= 239) {

            // reformat as IPAddress
            unsigned o1, o2, o3, o4;
            if (sscanf (dxhost, "%u.%u.%u.%u", &o1, &o2, &o3, &o4) != 4) {
                showDXClusterErr (box, "Multicast address must be formatted as a.b.c.d: %s",
                                                dxhost);
                return (false);
            }
            IPAddress ifIP(0,0,0,0);                        // ignored
            IPAddress mcIP(o1,o2,o3,o4);

            ok = wsjtx_server.beginMulticast (ifIP, mcIP, dxport);
            if (ok)
                dxcLog ("multicast %s:%d ok\n", dxhost, dxport);

        } else {

            ok = wsjtx_server.begin(dxport);

        }

        if (ok) {

            // record and claim ok so far
            cl_type = CT_WSJTX;
            return (true);
        }

    } else {

        // open fresh socket
        dx_client.stop();
        if (wifiOk() && dx_client.connect(dxhost, dxport)) {

            // valid connection -- keep an eye out for lost connection

            // look alive
            updateClocks(false);
            dxcLog ("connect ok\n");

            // assume first question is asking for call
            wdDelay(100);
            const char *login = getDXClusterLogin();
            dxcLog ("logging in as %s\n", login);
            dx_client.println (login);

            // look for clue about type of cluster along the way
            uint16_t bl;
            char buf[200];
            size_t bufl = sizeof(buf);
            cl_type = CT_UNKNOWN;
            while (getTCPLine (dx_client, buf, bufl, &bl)) {
                dxcLog ("< %s\n", buf);
                detectMultiConnection (buf);
                strtolower(buf);
                if (strstr (buf, "dx") && strstr (buf, "spider"))
                    cl_type = CT_DXSPIDER;
                else if (strstr (buf, "ar-cluster"))
                    cl_type = CT_ARCLUSTER;

                // could just wait for timeout but usually ok to stop if find what looks like a prompt
                if (buf[bl-1] == '>')
                    break;
            }

            // what is it?
            if (cl_type == CT_UNKNOWN) {
                incLostConn();
                showDXClusterErr (box, "Type unknown or Login rejected");
                dx_client.stop();
                return (false);
            }
            if (cl_type == CT_DXSPIDER)
                dxcLog ("Cluster is Spider\n");
            if (cl_type == CT_ARCLUSTER)
                dxcLog ("Cluster is AR\n");

            // send our location
            if (!sendDXClusterDELLGrid()) {
                incLostConn();
                showDXClusterErr (box, "Error sending DE grid");
                dx_client.stop();
                return (false);
            }

            // send user commands
            const char *dx_cmds[N_DXCLCMDS];
            bool dx_on[N_DXCLCMDS];
            getDXClCommands (dx_cmds, dx_on);
            for (int i = 0; i < N_DXCLCMDS; i++) {
                if (dx_on[i] && strlen(dx_cmds[i]) > 0) {
                    dx_client.println(dx_cmds[i]);
                    dxcLog ("> %s\n", dx_cmds[i]);
                }
            }

            // confirm still ok
            if (!dx_client) {
                incLostConn();
                if (multi_cntn)
                    showDXClusterErr (box, "Multiple logins");
                else
                    showDXClusterErr (box, "Login failed");
                return (false);
            }

            // restore known spots if not too old else reset list
            if (millis() - last_action < MAX_AGE) {
                dxc_ss.scrollToNewest();
                drawAllVisDXCSpots(box);
            } else {
                dxc_ss.n_data = 0;
                dxc_ss.top_vis = 0;
            }

            // all ok so far
            return (true);
        }
    }

    // sorry
    showDXClusterErr (box, "%s:%d Connection failed", dxhost, dxport);    // calls dx_client.stop()
    return (false);
}

/* display the current cluster host in the given color
 */
static void showHost (const SBox &box, uint16_t c)
{
    const char *dxhost = getDXClusterHost();

    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor(c);
    uint16_t nw = getTextWidth (dxhost);
    tft.setCursor (box.x + (box.w-nw)/2, box.y + SUBTITLE_Y0);
    tft.print (dxhost);
}

/* send something passive just to keep the connection alive
 */
static bool sendDXClusterHeartbeat()
{
    if (!useDXCluster() || !dx_client)
        return (true);

    const char *hbcmd = "ping\n";
    dxcLog ("feeding %s", hbcmd);
    dx_client.print (hbcmd);

    return (true);
}

/* send our lat/long and grid to dx_client, depending on cluster type.
 * return whether successful.
 * N.B. can be called any time so be prepared to do nothing fast if not appropriate.
 */
bool sendDXClusterDELLGrid()
{
    // easy check
    if (!useDXCluster() || !dx_client)
        return (false);

    // handy DE grid as string
    char maid[MAID_CHARLEN];
    getNVMaidenhead (NV_DE_GRID, maid);

    // handy DE lat/lon in common format
    char llstr[30];
    snprintf (llstr, sizeof(llstr), "%.0f %.0f %c %.0f %.0f %c",
            floorf(fabsf(de_ll.lat_d)), floorf(fmodf(60*fabsf(de_ll.lat_d), 60)), de_ll.lat_d<0?'S':'N',
            floorf(fabsf(de_ll.lng_d)), floorf(fmodf(60*fabsf(de_ll.lng_d), 60)), de_ll.lng_d<0?'W':'E');

    if (cl_type == CT_DXSPIDER) {

        char buf[100];

        // set grid
        snprintf (buf, sizeof(buf), "set/qra %s\n", maid);
        dx_client.print(buf);
        dxcLog ("> %s", buf);

        // set DE ll
        snprintf (buf, sizeof(buf), "set/location %s\n", llstr);
        dx_client.print(buf);
        dxcLog ("> %s", buf);

        // ok!
        return (true);

    } else if (cl_type == CT_ARCLUSTER) {

        char buf[100];

        // friendly turn off skimmer just avoid getting swamped
        strcpy (buf, "set dx filter not skimmer\n");
        dx_client.print(buf);
        dxcLog ("> %s", buf);

        // set grid
        snprintf (buf, sizeof(buf), "set station grid %s\n", maid);
        dx_client.print(buf);
        dxcLog ("> %s", buf);

        // set ll
        snprintf (buf, sizeof(buf), "set station latlon %s\n", llstr);
        dx_client.print(buf);
        dxcLog ("> %s", buf);


        // ok!
        return (true);

    }

    // fail
    return (false);
}

/* prepare a fresh box but preserve any existing spots
 */
static void initDXGUI (const SBox &box)
{
    // prep
    prepPlotBox (box);

    // title
    const char *title = BOX_IS_PANE_0(box) ? "Cluster" : "DX Cluster";
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor(DXC_COLOR);
    uint16_t tw = getTextWidth(title);
    tft.setCursor (box.x + (box.w-tw)/2, box.y + PANETITLE_H);
    tft.print (title);

    // init scroller for this box size
    // N.B. leave n_data in order to preserve data across closes up to MAX_AGE
    dxc_ss.max_vis = (box.h - LISTING_Y0)/LISTING_DY;
    dxc_ss.top_vis = 0;

    // set max spots and trim n_data if already larger
    max_spots = dxc_ss.max_vis + nMoreScrollRows();
    if (dxc_ss.n_data > max_spots) {
        // shrink and keep newest
        int n_discard = dxc_ss.n_data - max_spots;
        memmove (dx_spots, dx_spots+n_discard, max_spots * sizeof(DXSpot));
        dxc_ss.n_data = max_spots;
    }
}

/* prep the given box and connect dx_client to a dx cluster or wsjtx_server.
 * return whether successful.
 */
static bool initDXCluster(const SBox &box)
{
    // skip if not configured
    if (!useDXCluster())
        return (true);              // feign success to avoid retries

    // prep a fresh GUI
    initDXGUI (box);

    // show cluster host busy
    showHost (box, RA8875_YELLOW);

    // connect to dx cluster
    if (connectDXCluster(box)) {

        // ok: show host in green
        showHost (box, RA8875_GREEN);

        // reinit time
        last_action = millis();

        // ok
        return (true);

    } // else already displayed error message

    // sorry
    return (false);
}


/* parse the given line into a new spot record.
 * return whether successful
 */
static bool crackClusterSpot (char line[], DXSpot &news)
{
    // fresh
    memset (&news, 0, sizeof(news));

    if (sscanf (line, "DX de %11[^ :]: %f %11s", news.rx_call, &news.kHz, news.tx_call) != 3) {
        // already logged
        return (false);
    }

    // looks good so far, reach over and extract time
    tmElements_t tm;
    breakTime (myNow(), tm);
    tm.Hour = 10*(line[70]-'0') + (line[71]-'0');
    tm.Minute = 10*(line[72]-'0') + (line[73]-'0');
    news.spotted = makeTime (tm);

    // find locations and grids
    bool ok = call2LL (news.rx_call, news.rx_ll) && call2LL (news.tx_call, news.tx_ll);
    if (ok) {
        ll2maidenhead (news.rx_grid, news.rx_ll);
        ll2maidenhead (news.tx_grid, news.tx_ll);
    }

    // return whether we had any luck
    return (ok);
}

/* called frequently to drain and process cluster connection, open if not already running.
 * return whether connection is ok.
 */
bool updateDXCluster(const SBox &box)
{
    // redraw occasionally if for no other reason than to update ages
    static uint32_t last_draw;
    bool any_new = false;

    // open if not already
    if (!isDXClusterConnected() && !initDXCluster(box)) {
        // error already shown
        return(false);
    }

    if ((cl_type == CT_DXSPIDER || cl_type == CT_ARCLUSTER) && dx_client) {

        // this works for both types of cluster

        // roll all pending new spots into list as fast as possible
        char line[120];
        while (dx_client.available() && getTCPLine (dx_client, line, sizeof(line), NULL)) {
            // DX de KD0AA:     18100.0  JR1FYS       FT8 LOUD in FL!                2156Z EL98
            dxcLog ("< %s\n", line);
            detectMultiConnection (line);

            // look alive
            updateClocks(false);

            // crack
            DXSpot new_spot;
            if (crackClusterSpot (line, new_spot)) {
                last_action = millis();

                // add and display unless not on exclusive watch list
                if (showOnlyOnDXWatchList() && !onDXWatchList(new_spot.tx_call)) {
                    dxcLog ("%s not on watch list\n", new_spot.tx_call);
                } else {
                    addDXClusterSpot (box, new_spot);
                    any_new = true;
                }
            }
        }

        // check for lost connection
        if (!dx_client) {
            incLostConn();
            if (multi_cntn)
                showDXClusterErr (box, "Multiple logins");
            else
                showDXClusterErr (box, "Lost connection");
            return(false);
        }

        // send something if quiet for too long
        if (millis() - last_action > KEEPALIVE_DT) {
            last_action = millis();        // avoid banging
            if (!sendDXClusterHeartbeat()) {
                showDXClusterErr (box, "Heartbeat lost connection");
                return(false);
            }
        }

    } else if (cl_type == CT_WSJTX && wsjtx_server) {


        // drain ALL pending packets, retain most recent Status message if any

        uint8_t *any_msg = NULL;        // malloced if get a new packet of any type
        uint8_t *sts_msg = NULL;        // malloced if find Status msg

        int packet_size;
        while ((packet_size = wsjtx_server.parsePacket()) > 0) {
            // dxcLog ("WSJT-X size= %d heap= %d\n", packet_size, ESP.getFreeHeap());
            any_msg = (uint8_t *) realloc (any_msg, packet_size);
            if (!any_msg)
                fatalError ("wsjt packet alloc %d", packet_size);
            if (wsjtx_server.read (any_msg, packet_size) > 0) {
                uint8_t *bp = any_msg;
                if (wsjtxIsStatusMsg (&bp)) {
                    // save from bp to the end in prep for wsjtxParseStatusMsg()
                    int n_skip = bp - any_msg;
                    int n_keep = packet_size - n_skip;
                    // dxcLog ("skip= %d packet_size= %d\n", n_skip, packet_size);
                    if (n_keep > 0) {
                        sts_msg = (uint8_t *) realloc (sts_msg, n_keep);
                        if (!sts_msg)
                            fatalError ("wsjt alloc fail %d", n_keep);
                        memcpy (sts_msg, any_msg + n_skip, n_keep);
// #define _WSJT_TRACE
#if defined(_WSJT_TRACE)
                        Serial.printf ("*************** %d\n", n_keep);
                        for (int i = 0; i < n_keep; i += 10) {
                            for (int j = 0; j < 10; j++) {
                                int n = 10*i+j;
                                if (n == n_keep)
                                    break;
                                uint8_t c = sts_msg[n];
                                Serial.printf ("  %02X %c\n", c, isprint(c) ? c : '?');
                            }
                        }
#endif
                    }
                }
            }
        }

        // process then free newest Status message if received
        if (sts_msg) {
            uint8_t *bp = sts_msg;
            if (wsjtxParseStatusMsg (box, &bp))
                any_new = true;
            free (sts_msg);
        }

        // clean up
        if (any_msg)
            free (any_msg);
    }

    // just update ages occasionally if nothing new
    if (any_new)
        last_draw = millis();
    else if (timesUp (&last_draw, 1000))
        drawAllVisDXCSpots (box);

    // didn't break
    return (true);
}

/* insure cluster connection is closed
 */
void closeDXCluster()
{
    // make sure either/both connection is/are closed
    if (dx_client) {
        dx_client.stop();
        dxcLog ("disconnect %s\n", dx_client ? "failed" : "ok");
    }
    if (wsjtx_server) {
        wsjtx_server.stop();
        dxcLog ("WSTJ-X disconnect %s\n", wsjtx_server ?"failed":"ok");
    }

    // reset multi flag
    multi_cntn = false;
}

/* determine and engage a dx cluster pane touch.
 * return true if looks like user is interacting with the cluster pane, false if wants to change pane.
 * N.B. we assume s is within box
 */
bool checkDXClusterTouch (const SCoord &s, const SBox &box)
{
    if (s.y < box.y + PANETITLE_H) {

        // somewhere in the title bar

        // scroll up?
        if (dxc_ss.checkScrollUpTouch (s, box)) {
            scrollDXCUp (box);
            return (true);
        }

        // scroll down?
        if (dxc_ss.checkScrollDownTouch (s, box)) {
            scrollDXCDown (box);
            return (true);
        }

        // clear control?
        if (s.x < box.x + CLRBOX_DX+2*CLRBOX_R) {
            dxc_ss.n_data = 0;
            initDXGUI(box);
            showHost (box, RA8875_GREEN);
            return (true);
        }

        // none of those, so we shut down and return indicating user can choose another pane
        closeDXCluster();             // insure disconnected
        last_action = millis();       // in case op wants to come back soon
        return (false);

    }

    // not in title so engage a tapped row, if defined
    int vis_row = (s.y - (box.y + LISTING_Y0)) / LISTING_DY;
    int spot_row;
    if (dxc_ss.findDataIndex (vis_row, spot_row) && dx_spots[spot_row].tx_call[0] != '\0'
                                                            && isDXClusterConnected())
        engageDXCRow (dx_spots[spot_row]);

    // ours 
    return (true);
}

/* pass back current spots list, and return whether enabled at all.
 * ok to pass back if not displayed because spot list is still intact.
 * N.B. caller should not modify the list
 */
bool getDXClusterSpots (DXSpot **spp, uint8_t *nspotsp)
{
    if (useDXCluster()) {
        *spp = dx_spots;
        *nspotsp = dxc_ss.n_data;
        return (true);
    }

    return (false);
}

/* draw all path and spots on map, as desired
 */
void drawDXClusterSpotsOnMap ()
{
    // skip if we are neither configured nor up.
    if (!useDXCluster() || findPaneForChoice(PLOT_CH_DXCLUSTER) == PANE_NONE)
        return;

    // draw all paths then labels
    for (int i = 0; i < dxc_ss.n_data; i++) {
        DXSpot &si = dx_spots[i];
        drawSpotPathOnMap (si);
    }
    for (int i = 0; i < dxc_ss.n_data; i++) {
        DXSpot &si = dx_spots[i];
        drawSpotLabelOnMap (si, LOM_TXEND, LOM_ALL);
        drawSpotLabelOnMap (si, LOM_RXEND, LOM_JUSTDOT);
    }
}

/* return whether cluster is currently connected
 */
bool isDXClusterConnected()
{
    return (useDXCluster() && (dx_client || wsjtx_server));
}

/* find closest spot and location on either end to given ll, if any.
 */
bool getClosestSpot (const LatLong &ll, DXSpot *sp, LatLong *llp)
{
    return (getClosestSpot (dx_spots, dxc_ss.n_data, ll, sp, llp));
}

/* find closest spot and location on either end to given ll, if any.
 */
bool getClosestDXCluster (const LatLong &ll, DXSpot *sp, LatLong *llp)
{
    return (getClosestSpot (dx_spots, dxc_ss.n_data, ll, sp, llp));
}
