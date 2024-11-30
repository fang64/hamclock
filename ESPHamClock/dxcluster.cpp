/* handle the DX Cluster display. Connects when first displayed in a pane, disconnects if none.
 *
 * Clusters:
 *   [ ] support Spider, AR and CC
 *   [ ] as of 3.03 replace show/header with cty_wt_mod-ll.txt -- too big for ESP
 *
 * WSJT-X:
 *   [ ] packet definition: https://sourceforge.net/p/wsjt/wsjtx/ci/master/tree/Network/NetworkMessage.hpp
 *   [ ] We don't actually enforce the Status ID to be WSJT-X so this may also work for, say, JTCluster.
 *
 * We actually keep two lists:
 *   dxc_spots: the complete raw list, not filtered nor sorted; length in n_dxspots.
 *   dxwl_spots: watchlist-filterd and time-sorted for display; length in dxc_ss.n_data.
 * 
 * can inject spots from local file for debugging, see getNextDXCLine()
 */

#include "HamClock.h"



// layout 
#define DXC_COLOR       RA8875_GREEN
#define CLRBOX_DX       10                      // dx clear control box center from left
#define CLRBOX_DY       10                      // dy " center down from top
#define CLRBOX_R        4                       // clear box radius
#define SPOTMRNOP       (tft.SCALESZ+4)         // raw spot marker radius when no path

// times
#define KEEPALIVE_DT    600                     // send something if last_heard idle this long, secs
#define BGCHECK_DT      5000                    // background checkDXCluster period, millis
#define MAXUSE_DT       (5*60)                  // remove all spots memory if pane not used this long, secs
#define MAXKEEP_DT      3600                    // max age on dxc_spots list, secs
#define DXCMSG_DT       500                     // delay before sending each cluster message, millis
static time_t last_heard;                       // last time anything arrived from the cluster

// connection info
static WiFiClient dxc_client;                   // persistent TCP connection while displayed ...
static WiFiUDP wsjtx_server;                    // or persistent UDP "connection" to WSJT-X client program
static bool multi_cntn;                         // set when cluster has noticed multiple connections
#define MAX_LCN         10                      // max lost connections per MAX_LCDT
#define MAX_LCDT        3600                    // max lost connections period, seconds

// ages
static uint8_t dxc_ages[] = {10, 20, 40, 60};   // minutes
static uint8_t dxc_age;                         // one of above, once set
#define N_DXCAGES NARRAY(dxc_ages)              // handy count

// state
static DXSpot *dxc_spots;                       // malloced list, complete
static int n_dxspots;                           // n spots in dxc_spots
static DXSpot *dxwl_spots;                      // malloced list, filtered for display, count in dxc_ss.n_data
static ScrollState dxc_ss;                      // scrolling info
static bool dxc_showbio;                        // whether click shows bio
static time_t scrolledaway_tm;                  // time when user scrolled away from top of list
static time_t rebuild_tm;                       // last time rebuildDXWatchList() was called


// type
typedef enum {
    CT_UNKNOWN,
    CT_ARCLUSTER,
    CT_DXSPIDER,
    CT_VE7CC,
    CT_WSJTX,
} DXClusterType;
static DXClusterType cl_type;

#if defined(__GNUC__)
static void dxcLog (const char *fmt, ...) __attribute__ ((format (__printf__, 1, 2)));
static void showDXClusterErr (const SBox &box, const char *fmt, ...) __attribute__ ((format (__printf__, 2, 3)));
static void dxcSendMsg (const char *fmt, ...) __attribute__ ((format (__printf__, 1, 2)));
#else
static void dxcLog (const char *fmt, ...);
static void showDXClusterErr (const SBox &box, const char *fmt, ...);
static void dxcSendMsg (const char *fmt, ...);
#endif


/* draw, else erase, the clear spots control
 */
static void drawClearListBtn (const SBox &box, bool draw)
{
    uint16_t color = draw ? DXC_COLOR : RA8875_BLACK;

    tft.drawRect (box.x + CLRBOX_DX - CLRBOX_R, box.y + CLRBOX_DY - CLRBOX_R,
                  2*CLRBOX_R+1, 2*CLRBOX_R+1, color);
    tft.drawLine (box.x + CLRBOX_DX - CLRBOX_R, box.y + CLRBOX_DY - CLRBOX_R,
                  box.x + CLRBOX_DX + CLRBOX_R, box.y + CLRBOX_DY + CLRBOX_R, color);
    tft.drawLine (box.x + CLRBOX_DX + CLRBOX_R, box.y + CLRBOX_DY - CLRBOX_R,
                  box.x + CLRBOX_DX - CLRBOX_R, box.y + CLRBOX_DY + CLRBOX_R, color);
}

/* handy check whether we are, or should, show the New spots symbol
 */
static bool showingNewSpot(void)
{
    return (scrolledaway_tm > 0 && n_dxspots > 0 && dxc_spots[n_dxspots-1].spotted > scrolledaway_tm);
}

/* rebuild dxwl_spots from dxc_spots
 */
static void rebuildDXWatchList(void)
{
    // extract qualifying spots
    time_t oldest = myNow() - 60*dxc_age;               // minutes to seconds
    dxc_ss.n_data = 0;                                  // reset count, don't bother to resize dxwl_spots
    for (int i = 0; i < n_dxspots; i++) {
        DXSpot &spot = dxc_spots[i];
        if (spot.spotted >= oldest && checkWatchListSpot (WLID_DX, spot) != WLS_NO) {
            dxwl_spots = (DXSpot *) realloc (dxwl_spots, (dxc_ss.n_data+1) * sizeof(DXSpot));
            if (!dxwl_spots)
                fatalError ("No mem for %d watch list spots", dxc_ss.n_data+1);
            dxwl_spots[dxc_ss.n_data++] = spot;
        }
    }

    // resort and scroll to newest
    qsort (dxwl_spots, dxc_ss.n_data, sizeof(DXSpot), qsDXCSpotted);
    dxc_ss.scrollToNewest();

    // note time
    rebuild_tm = myNow();
}

/* draw all currently visible spots in the pane then update scroll markers if more
 */
static void drawAllVisDXCSpots (const SBox &box)
{
    drawVisibleSpots (WLID_DX, dxwl_spots, dxc_ss, box, DXC_COLOR);
    drawClearListBtn (box, dxc_ss.n_data > 0);
}

/* handy check whether New Spot symbol needs changing on/off
 */
static void checkNewSpotSymbol (const SBox &box, bool was_at_newest)
{
    if (was_at_newest && !dxc_ss.atNewest()) {
        scrolledaway_tm = myNow();                              // record when moved off top
        ROTHOLD_SET(PLOT_CH_DXCLUSTER);                         // disable rotation
    } else if (!was_at_newest && dxc_ss.atNewest()) {
        dxc_ss.drawNewSpotsSymbol (box, false, false);          // turn off entirely
        rebuildDXWatchList ();
        scrolledaway_tm = 0;
        ROTHOLD_CLR(PLOT_CH_DXCLUSTER);                         // resume rotation
    }
}

/* shift the visible list up, if possible.
 * if reach the end with the newest entry, turn off New spots and update dxwl_spots
 */
static void scrollDXCUp (const SBox &box)
{
    bool was_at_newest = dxc_ss.atNewest();
    if (dxc_ss.okToScrollUp()) {
        dxc_ss.scrollUp();
        drawAllVisDXCSpots(box);
    }
    checkNewSpotSymbol (box, was_at_newest);
}

/* shift the visible list down, if possible.
 * set scrolledaway_tm if scrolling away from newest entry
 */
static void scrollDXCDown (const SBox &box)
{
    bool was_at_newest = dxc_ss.atNewest();
    if (dxc_ss.okToScrollDown()) {
        dxc_ss.scrollDown ();
        drawAllVisDXCSpots (box);
    }
    checkNewSpotSymbol (box, was_at_newest);
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

/* set bio, radio and DX from given row, known to be defined
 */
static void engageDXCRow (DXSpot &s)
{
    if (dxc_showbio)
        openQRZBio (s);
    setRadioSpot(s.kHz);
    newDX (s.tx_ll, NULL, s.tx_call);
}

/* add a new spot to dxc_spots[] then update dxwl_spots
 */
static void addDXClusterSpot (DXSpot &new_spot)
{
    // nice to insure calls are upper case
    strtoupper (new_spot.rx_call);
    strtoupper (new_spot.tx_call);

    // mark if looks to be dup and remove any ancient spots along the way
    time_t ancient = myNow() - MAXKEEP_DT;
    int n_ancient = 0;
    bool dup = false;
    for (int i = 0; i < n_dxspots; i++) {
        DXSpot &spot = dxc_spots[i];
        if (!dup && checkDXDup (new_spot, spot)) {
            dup = true;
        } else if (spot.spotted < ancient) {
            memmove (&dxc_spots[i], &dxc_spots[i+1], (n_dxspots - i - 1) * sizeof(DXSpot));
            n_ancient += 1;
        }
    }

    // update n_dxspots if shrunk, don't bother to realloc.
    // N.B. if do realloc beware non-portable return from realloc if n_dxspots becomes 0
    if (n_ancient > 0) {
        dxcLog ("%d spots aged out older than %d mins\n", n_ancient, MAXKEEP_DT/60);
        n_dxspots -= n_ancient;
    }

    // do not add if too old or dup
    if (new_spot.spotted < ancient) {
        dxcLog ("DXC: %s already more than %d mins old\n", new_spot.tx_call, MAXKEEP_DT/60);
        return;
    }
    if (dup) {
        dxcLog ("DXC: %s dup within last %d mins\n", new_spot.tx_call, MAXDUP_DT/60);
        return;
    }

    // tweak map location for unique picking
    ditherLL (new_spot.tx_ll);

    // bump dxc_spots for one more
    dxc_spots = (DXSpot *) realloc (dxc_spots, (n_dxspots+1) * sizeof(DXSpot));
    if (!dxc_spots)
        fatalError ("No memory for %d DX spots", n_dxspots+1);

    // append to dxc_spots
    dxc_spots[n_dxspots++] = new_spot;
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

/* parse and process WSJT-X message known to be of type Status.
 */
static void wsjtxParseStatusMsg (uint8_t *msg)
{
    // dxcLog ("Parsing status\n");
    uint8_t **bpp = &msg;                               // walk along msg

    // crack remaining fields down to grid
    uint32_t hz = wsjtx_quint64 (bpp);                  // capture freq
    (void) wsjtx_utf8 (bpp);                            // skip over mode
    char *dx_call = wsjtx_utf8 (bpp);                   // capture DX call 
    (void) wsjtx_utf8 (bpp);                            // skip over report
    (void) wsjtx_utf8 (bpp);                            // skip over Tx mode
    (void) wsjtx_bool (bpp);                            // skip over Tx enabled flag
    (void) wsjtx_bool (bpp);                            // skip over transmitting flag
    (void) wsjtx_bool (bpp);                            // skip over decoding flag
    (void) wsjtx_quint32 (bpp);                         // skip over Rx DF -- not always correct
    (void) wsjtx_quint32 (bpp);                         // skip over Tx DF
    char *de_call = wsjtx_utf8 (bpp);                   // capture DE call 
    char *de_grid = wsjtx_utf8 (bpp);                   // capture DE grid
    char *dx_grid = wsjtx_utf8 (bpp);                   // capture DX grid

    // dxcLog ("WSJT: %7d %s %s %s %s\n", hz, de_call, de_grid, dx_call, dx_grid);

    // ignore if frequency is clearly bogus (which I have seen)
    if (hz == 0) {
        dxcLog ("%s invalid frequency: %u\n", de_call, hz);
        return;
    }

    // get each ll from grids
    LatLong ll_de, ll_dx;
    if (!maidenhead2ll (ll_de, de_grid)) {
        dxcLog ("%s invalid or missing DE grid: %s\n", de_call, de_grid);
        return;
    }
    if (!maidenhead2ll (ll_dx, dx_grid)) {
        dxcLog ("%s invalid or missing DX grid: %s\n", dx_call, dx_grid);
        return;
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
    addDXClusterSpot (new_spot);
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
 * Only Spiders seem to care enough to dicsonnect, AR and CC clusters report but otherwise don't care.
 */
static void detectMultiConnection (const char *line)
{
    // first seems typical for spiders, second for AR
    if (strstr (line, "econnected") != NULL || strstr (line, "Dupe call") != NULL)
        multi_cntn = true;
}

/* send a message to dxc_client.
 * here for convenience of stdarg, logging and delay.
 */
static void dxcSendMsg (const char *fmt, ...)
{
    // format
    char msg[400];
    va_list ap;
    va_start (ap, fmt);
    size_t msg_l = vsnprintf (msg, sizeof(msg)-1, fmt, ap);     // allow room for \n if not already in msg
    va_end (ap);

    // add nl if not already
    if (msg_l == 0 || msg[msg_l-1] != '\n') {
        msg[msg_l] = '\n';
        msg[++msg_l] = '\0';
    }

    // delay
    wdDelay (DXCMSG_DT);

    // send and log
    dxc_client.print (msg);
    dxcLog ("> %s", msg);
}

/* send a request for recent spots such that they will arrive the same as normal
 */
static void requestRecentSpots (void)
{
    const char *msg = NULL;

    if (cl_type == CT_DXSPIDER)
        msg = "sh/dx filter real 30";
    else if (cl_type == CT_ARCLUSTER)
        msg = "show/dx/30 @";
    else if (cl_type == CT_VE7CC)
        msg = "show/myfdx";   // always 30, does not accept a count

    if (msg)
        dxcSendMsg ("%s\n", msg);
}

/* free both spots lists memory
 */
static void resetDXMem()
{
    free (dxc_spots);
    dxc_spots = NULL;
    n_dxspots = 0;

    free (dxwl_spots);
    dxwl_spots = NULL;
    dxc_ss.n_data = 0;
}

/* try to connect to the cluster.
 * if success: dxc_client or wsjtx_server is live and return true,
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
        dxc_client.stop();
        if (wifiOk() && dxc_client.connect(dxhost, dxport)) {

            // valid connection -- keep an eye out for lost connection

            // look alive
            updateClocks(false);
            dxcLog ("connect ok\n");

            // assume first question is asking for call.
            // don't try to read with getTCPLine because first line is "login: " without trailing nl
            const char *login = getDXClusterLogin();
            dxcLog ("logging in as %s\n", login);
            dxcSendMsg ("%s\n", login);

            // look for clue about type of cluster along the way
            uint16_t bl;
            char buf[200];
            const size_t bufl = sizeof(buf);
            cl_type = CT_UNKNOWN;
            while (getTCPLine (dxc_client, buf, bufl, &bl)) {
                dxcLog ("< %s\n", buf);
                detectMultiConnection (buf);
                strtolower(buf);
                if (strstr (buf, "dx") && strstr (buf, "spider"))
                    cl_type = CT_DXSPIDER;
                else if (strstr (buf, " cc "))
                    cl_type = CT_VE7CC;
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
                dxc_client.stop();
                return (false);
            }
            if (cl_type == CT_DXSPIDER)
                dxcLog ("Cluster is Spider\n");
            if (cl_type == CT_ARCLUSTER)
                dxcLog ("Cluster is AR\n");
            if (cl_type == CT_VE7CC)
                dxcLog ("Cluster is CC\n");

            // send our location
            if (!sendDXClusterDELLGrid()) {
                incLostConn();
                showDXClusterErr (box, "Error sending DE grid");
                dxc_client.stop();
                return (false);
            }

            // send user commands
            const char *dx_cmds[N_DXCLCMDS];
            bool dx_on[N_DXCLCMDS];
            getDXClCommands (dx_cmds, dx_on);
            for (int i = 0; i < N_DXCLCMDS; i++) {
                if (dx_on[i] && strlen(dx_cmds[i]) > 0)
                    dxcSendMsg("%s\n", dx_cmds[i]);
            }

            // reset list and view
            resetDXMem();

            // request recent spots
            requestRecentSpots();

            // confirm still ok
            if (!dxc_client) {
                incLostConn();
                if (multi_cntn)
                    showDXClusterErr (box, "Multiple logins");
                else
                    showDXClusterErr (box, "Login or init failed");
                return (false);
            }

            // all ok so far
            return (true);
        }
    }

    // sorry
    showDXClusterErr (box, "%s:%d Connection failed", dxhost, dxport);    // calls dxc_client.stop()
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
static void sendDXClusterHeartbeat()
{
    const char *hbcmd = "ping";
    dxcSendMsg ("%s\n", hbcmd);
}

/* send our lat/long and grid to dxc_client, depending on cluster type.
 * return whether successful.
 * N.B. can be called any time so be prepared to do nothing fast if not appropriate.
 */
bool sendDXClusterDELLGrid()
{
    // easy check
    if (!isDXClusterConnected())
        return (false);

    // handy DE grid as string
    char maid[MAID_CHARLEN];
    getNVMaidenhead (NV_DE_GRID, maid);

    // handy DE lat/lon in common format
    char llstr[30];
    snprintf (llstr, sizeof(llstr), "%.0f %.0f %c %.0f %.0f %c",
            floorf(fabsf(de_ll.lat_d)), floorf(fmodf(60*fabsf(de_ll.lat_d), 60)), de_ll.lat_d<0?'S':'N',
            floorf(fabsf(de_ll.lng_d)), floorf(fmodf(60*fabsf(de_ll.lng_d), 60)), de_ll.lng_d<0?'W':'E');

    if (cl_type == CT_DXSPIDER || cl_type == CT_VE7CC) {

        // set grid and DE ll
        dxcSendMsg ("set/qra %s\n", maid);
        dxcSendMsg ("set/location %s\n", llstr);

        // ok!
        return (true);

    } else if (cl_type == CT_ARCLUSTER) {

        // friendly turn off skimmer just avoid getting swamped
        dxcSendMsg ("set dx filter not skimmer\n");

        // set grid
        dxcSendMsg ("set station grid %s\n", maid);

        // set ll
        dxcSendMsg ("set station latlon %s\n", llstr);

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
    // prep box
    prepPlotBox (box);

    // title
    const char *title = BOX_IS_PANE_0(box) ? "Cluster" : "DX Cluster";
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor(DXC_COLOR);
    uint16_t tw = getTextWidth(title);
    tft.setCursor (box.x + (box.w-tw)/2, box.y + PANETITLE_H);
    tft.print (title);

    // init scroller for this box size but leave n_data
    dxc_ss.max_vis = (box.h - LISTING_Y0)/LISTING_DY;
    dxc_ss.initNewSpotsSymbol (box, DXC_COLOR);
}

/* connect dxc_client to a dx cluster or wsjtx_server.
 * return whether successful.
 */
static bool initDXCluster(const SBox &box)
{
    // prep box
    initDXGUI (box);
    dxc_ss.scrollToNewest();

    // show cluster host busy
    showHost (box, RA8875_YELLOW);

    // connect to dx cluster
    if (connectDXCluster(box)) {

        // ok: show host in green
        showHost (box, RA8875_GREEN);

        // reinit times
        last_heard = myNow();

        // max age
        if (!NVReadUInt8 (NV_DXCAGE, &dxc_age)) {
            dxc_age = dxc_ages[1];
            NVWriteUInt8 (NV_DXCAGE, dxc_age);
        }

        // determine dxc_showbio
        uint8_t bio = 0;
        if (getQRZId() != QRZ_NONE) {
            if (!NVReadUInt8 (NV_DXCBIO, &bio))
                bio = 0;
        }
        dxc_showbio = (bio != 0);

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

    // DX de KD0AA:     18100.0  JR1FYS       FT8 LOUD in FL!                2156Z EL98
    if (sscanf (line, "DX de %11[^ :]: %f %11s", news.rx_call, &news.kHz, news.tx_call) != 3) {
        // already logged
        return (false);
    }

    // looks good so far, reach over to extract time and perform strict sanity check
    int hr = 10*(line[70]-'0') + (line[71]-'0');
    int mn = 10*(line[72]-'0') + (line[73]-'0');
    if (!isdigit(line[70]) || !isdigit(line[71]) || !isdigit(line[72]) || !isdigit(line[73])
                        || hr < 0 || hr > 59 || mn < 0 || mn > 60) {
        dxcLog ("bogus time in spot: '%4.4s'\n", &line[70]);
        return (false);
    }

    // assign time augmented with current date
    tmElements_t tm;
    time_t n0 = myNow();
    breakTime (n0, tm);
    tm.Hour = hr;
    tm.Minute = mn;
    news.spotted = makeTime (tm);

    // the spot does not indicate the date so assume future times are from yesterday
    if (news.spotted > n0)
        news.spotted -= SECSPERDAY;

    // find locations and grids
    bool ok = call2LL (news.rx_call, news.rx_ll) && call2LL (news.tx_call, news.tx_ll);
    if (ok) {
        ll2maidenhead (news.rx_grid, news.rx_ll);
        ll2maidenhead (news.tx_grid, news.tx_ll);
    }

    // return whether we had any luck
    return (ok);
}

/* run menu to allow editing watch list
 */
static void runDXClusterMenu (const SBox &box)
{
    // set up the MENU_TEXT field 
    MenuText mtext;                                             // menu text prompt context
    char wl_state[WLA_MAXLEN];                                  // wl state, menu may change
    setupWLMenuText (WLID_DX, mtext, box, wl_state);

    // build the possible age labels
    char dxages_str[N_DXCAGES][10];
    for (int i = 0; i < N_DXCAGES; i++)
        snprintf (dxages_str[i], sizeof(dxages_str[i]), "%d m", dxc_ages[i]);

    // whether to show bio on click, only show in menu at all if bio source has been set in Setup
    bool show_bio_enabled = getQRZId() != QRZ_NONE;
    MenuFieldType bio_lbl_mft = show_bio_enabled ? MENU_LABEL : MENU_IGNORE;
    MenuFieldType bio_yes_mft = show_bio_enabled ? MENU_1OFN : MENU_IGNORE;
    MenuFieldType bio_no_mft = show_bio_enabled ? MENU_1OFN : MENU_IGNORE;

    // optional bio and watch list
    #define MI_AGE_GRP  3                                       // MenuItem.group for the age items
    MenuItem mitems[10] = {
        // column 1
        {bio_lbl_mft, false,                 0, 2, "Bio:", NULL},                       // 0
        {MENU_LABEL, false,                  1, 2, "Age:", NULL},                       // 1
        {MENU_BLANK, false,                  2, 0, NULL, NULL},                         // 2

        // column 2
        {bio_yes_mft, dxc_showbio,           5, 2, "Yes", NULL},                        // 3
        {MENU_1OFN,  dxc_age == dxc_ages[0], MI_AGE_GRP, 2, dxages_str[0], NULL},       // 4
        {MENU_1OFN,  dxc_age == dxc_ages[2], MI_AGE_GRP, 2, dxages_str[2], NULL},       // 5

        // column 3
        {bio_no_mft, !dxc_showbio,           5, 2, "No", NULL},                         // 6
        {MENU_1OFN,  dxc_age == dxc_ages[1], MI_AGE_GRP, 2, dxages_str[1], NULL},       // 7
        {MENU_1OFN,  dxc_age == dxc_ages[3], MI_AGE_GRP, 2, dxages_str[3], NULL},       // 8

        // watch list
        {MENU_TEXT,  false,                  4, 2, wl_state, &mtext},                   // 9
    };


    SBox menu_b = box;                                  // copy, not ref!
    menu_b.x = box.x + 5;
    menu_b.y = box.y + SUBTITLE_Y0;
    menu_b.w = 0;
    SBox ok_b;
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_CANCELOK, 3, NARRAY(mitems), mitems};
    if (runMenu (menu)) {

        // check bio
        if (show_bio_enabled) {
            dxc_showbio = mitems[3].set;
            NVWriteUInt8 (NV_DXCBIO, dxc_showbio);
        }

        // set desired age
        for (int i = 0; i < NARRAY(mitems); i++) {
            MenuItem &mi = mitems[i];
            if (mi.group == MI_AGE_GRP && mi.set) {
                dxc_age = atoi (mi.label);
                NVWriteUInt8 (NV_DXCAGE, dxc_age);
                break;
            }
        }

        // must recompile to update wl but runMenu already insured wl compiles ok
        char ynot[100];
        if (lookupWatchListState (mtext.label) != WLA_OFF
                                && !compileWatchList (WLID_DX, mtext.text, ynot, sizeof(ynot)))
            fatalError ("dxc failed recompling wl %s: %s", mtext.text, ynot);
        setWatchList (WLID_DX, mtext.label, mtext.text);
        dxcLog ("set WL to %s %s\n", mtext.label, mtext.text);

        // rebuild with new options
        rebuildDXWatchList();

        // full update to capture any/all changes
        scheduleNewPlot (PLOT_CH_DXCLUSTER);

    } else {

        // cancelled so just restore 
        initDXGUI (box);
        showHost (box, RA8875_GREEN);
        drawAllVisDXCSpots (box);

    }


    // always free the working watch list text
    free (mtext.text);
}

/* get next line from dxc_client, or inject from local debug file
 */
static bool getNextDXCLine (char line[], size_t ll)
{
    // inject local test file if tracing
    if (gimbal_trace_level > 1) {
        static const char inject_fn[] = "x.dxc-injection";
        static FILE *fp;
        if (!fp) {
            fp = fopen (inject_fn, "r");
            if (!fp) {
                fatalError ("%s: %s", inject_fn, strerror (errno));
                return (false);     // lint
            }
        }
        if (fgets (line, ll, fp)) {
            line[strlen(line)-1] = '\0';                // rm nl like getTCPLine()
            return (true);
        } else {
            return (false);
        }

    } else {

        // normal
        return (dxc_client.available() && getTCPLine (dxc_client, line, ll, NULL));
    }
}

/* insure cluster connection is closed
 */
void closeDXCluster()
{
    // make sure either/both connection is/are closed
    if (dxc_client) {
        dxc_client.stop();
        dxcLog ("disconnect %s\n", dxc_client ? "failed" : "ok");
    }
    if (wsjtx_server) {
        wsjtx_server.stop();
        dxcLog ("WSTJ-X disconnect %s\n", wsjtx_server ?"failed":"ok");
    }

    // reset multi flag
    multi_cntn = false;
}

/* called often while pane is visible, fresh is set when newly so.
 * connect if not already then show list.
 * return whether connection is open.
 * N.B. we never read new spots, that is done by checkDXCluster()
 */
bool updateDXCluster (const SBox &box, bool fresh)
{
    // insure connected
    if (!isDXClusterConnected() && !initDXCluster(box)) // shows error message in box
        return (false);

    // fresh prep
    if (fresh) {
        initDXGUI (box);
        dxc_ss.scrollToNewest();
        showHost (box, RA8875_GREEN);
    }

    if (dxc_ss.atNewest()) {
        // rebuild displayed list if new spot since last rebuild
        if (n_dxspots > 0 && dxc_spots[n_dxspots-1].spotted > rebuild_tm) {
            rebuildDXWatchList();
            dxc_ss.drawNewSpotsSymbol (box, false, false);      // insure off
            scrolledaway_tm = 0;
        }
        ROTHOLD_CLR(PLOT_CH_DXCLUSTER);                         // resume rotation
    } else {
        // show "new spots" symbol if added more spots since scrolled away
        if (showingNewSpot())
            dxc_ss.drawNewSpotsSymbol (box, true, false);       // show passively
        ROTHOLD_SET(PLOT_CH_DXCLUSTER);                         // disable rotation
    }

    // update list, if only to show aging
    drawAllVisDXCSpots (box);

    // ok
    return (true);

}

/* called often to add any new spots to list IFF connection is already open.
 * N.B. this is not a thread but can be thought of as a "background" function, no GUI.
 * N.B. we never open the cluster connection, that is done by updateDXCluster() but we will close it
 * if there have been no activity
 */
void checkDXCluster()
{
    // out fast if no connection
    if (!isDXClusterConnected())
        return;

    // not crazy fast
    static uint32_t prev_check;
    if (!timesUp (&prev_check, BGCHECK_DT))
        return;

    // close if not selected in any pane
    if (findPaneForChoice(PLOT_CH_DXCLUSTER) == PANE_NONE) {
        dxcLog ("closing because no longer in any pane\n");
        closeDXCluster();
        return;
    }

    if ((cl_type == CT_DXSPIDER || cl_type == CT_ARCLUSTER || cl_type == CT_VE7CC) && dxc_client) {

        // roll all pending new spots into list as fast as possible

        // check status first.. fairly expensive
        if (!wifiOk()) {

            dxcLog ("bg has no network\n");
            closeDXCluster();

        } else {

            // don't block if nothing waiting
            char line[120];
            while (getNextDXCLine (line, sizeof(line))) {

                dxcLog ("< %s\n", line);
                detectMultiConnection (line);

                // look alive
                updateClocks(false);

                // any new data counts even if not valid
                last_heard = myNow();

                // crack
                DXSpot new_spot;
                if (crackClusterSpot (line, new_spot)) {

                    // add if qualifies watch list requirements
                    if (checkWatchListSpot (WLID_DX, new_spot) == WLS_NO)
                        dxcLog ("%s not on watch list\n", new_spot.tx_call);
                    else
                        addDXClusterSpot (new_spot);
                }
            }

            // check for no network or server closing the connection
            if (!dxc_client) {
                dxcLog ("bg lost connection\n");
                incLostConn();
                closeDXCluster();
            }

            // send something if spider quiet for too long
            if (cl_type == CT_DXSPIDER && myNow() - last_heard > KEEPALIVE_DT) {
                last_heard = myNow();        // avoid banging
                sendDXClusterHeartbeat();
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
            wsjtxParseStatusMsg (sts_msg);
            free (sts_msg);

            // any new data counts even if invalid
            last_heard = myNow();
        }

        // clean up
        if (any_msg)
            free (any_msg);
    }
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
        if (s.y < box.y + CLRBOX_DY+CLRBOX_R && s.x < box.x + CLRBOX_DX+CLRBOX_R) {
            dxcLog ("User erased list of %d spots, %d qualified\n", n_dxspots, dxc_ss.n_data);
            resetDXMem();
            initDXGUI(box);
            dxc_ss.scrollToNewest();
            showHost (box, RA8875_GREEN);
            return (true);
        }

        // New spots?
        if (dxc_ss.checkNewSpotsTouch (s, box)) {
            if (!dxc_ss.atNewest() && showingNewSpot()) {
                // scroll to newest, let updateDXCluster() do the rest
                dxc_ss.scrollToNewest();
            }
            return (true);                      // claim our even if not showing
        }

        // on hold?
        if (ROTHOLD_TST(PLOT_CH_DXCLUSTER))
            return (true);

        // none of those, so we return indicating user can choose another pane
        return (false);

    }

    // check tapping host to edit watch list
    if (s.y < box.y + LISTING_Y0) {
        runDXClusterMenu (box);
        return (true);
    }

    // everything else below may be a tapped spot
    int vis_row = (s.y - (box.y + LISTING_Y0)) / LISTING_DY;
    int spot_row;
    if (dxc_ss.findDataIndex (vis_row, spot_row)
                        && dxwl_spots[spot_row].tx_call[0] != '\0' && isDXClusterConnected())
        engageDXCRow (dxwl_spots[spot_row]);

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
        *spp = dxc_spots;
        *nspotsp = n_dxspots;
        return (true);
    }

    return (false);
}

/* draw all qualiying paths and spots on map, as desired
 */
void drawDXClusterSpotsOnMap ()
{
    // skip if we are not running
    if (!isDXClusterConnected())
        return;

    // draw all paths then labels
    for (int i = 0; i < dxc_ss.n_data; i++) {
        DXSpot &si = dxwl_spots[i];
        drawSpotPathOnMap (si);
    }
    for (int i = 0; i < dxc_ss.n_data; i++) {
        DXSpot &si = dxwl_spots[i];
        drawSpotLabelOnMap (si, LOME_TXEND, LOMD_ALL);
        drawSpotLabelOnMap (si, LOME_RXEND, LOMD_JUSTDOT);
    }
}

/* return whether cluster is currently connected
 */
bool isDXClusterConnected()
{
    return (useDXCluster() && (dxc_client || wsjtx_server));
}

/* find closest spot and location on either end to given ll, if any.
 */
bool getClosestDXCluster (const LatLong &ll, DXSpot *sp, LatLong *llp)
{
    return (isDXClusterConnected() && getClosestSpot (dxwl_spots, dxc_ss.n_data, LOME_BOTH, ll, sp, llp));
}

/* remove all spots memory if unused for some time
 */
void cleanDXCluster()
{
    if (dxc_spots || dxwl_spots) {
        static time_t last_used;
        if (findPaneForChoice(PLOT_CH_DXCLUSTER) != PANE_NONE)
            last_used = myNow();
        else if (myNow() - last_used > MAXUSE_DT)
            resetDXMem();
    }
}

/* return spot in our pane if under ms
 */
bool getDXCPaneSpot (const SCoord &ms, DXSpot *dxs, LatLong *ll)
{
    // done if ms not showing our pane or not in our box
    PlotPane pp = findPaneChoiceNow (PLOT_CH_DXCLUSTER);
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
    if (dxc_ss.getVisIndices (min_i, max_i) > 0) {
        for (int i = min_i; i <= max_i; i++) {
            listrow_b.y = y0 + dxc_ss.getDisplayRow(i) * LISTING_DY;
            if (inBox (ms, listrow_b)) {
                // ms is over this spot
                *dxs = dxwl_spots[i];
                *ll = dxs->tx_ll;
                return (true);
            }
        }
    }

    // none
    return (false);
}
