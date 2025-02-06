/* handy tools for Spots
 */

#include "HamClock.h"


/* find closest location from ll to the given end(s) of paths defined in the given list of spots.
 * return whether found one within MAX_CSR_DIST.
 */
bool getClosestSpot (const DXSpot *list, int n_list, LabelOnMapEnd which_end, const LatLong &from_ll,
    DXSpot *closest_sp, LatLong *closest_llp)
{
    // linear search -- not worth kdtree etc
    const DXSpot *min_sp = NULL;   
    float min_d = 1e10;
    bool min_is_de = false;         
    for (int i = 0; i < n_list; i++) {

        const DXSpot *sp = &list[i];
        float d;                    

        if (which_end == LOME_RXEND || which_end == LOME_BOTH) {
            d = simpleSphereDist (sp->rx_ll, from_ll);
            if (d < min_d) {
                min_d = d;
                min_sp = sp;
                min_is_de = true;
            }
        }

        if (which_end == LOME_TXEND || which_end == LOME_BOTH) {
            d = simpleSphereDist (sp->tx_ll, from_ll);
            if (d < min_d) {
                min_d = d;
                min_sp = sp;
                min_is_de = false;
            }
        }
    }

    if (min_sp && min_d*ERAD_M < MAX_CSR_DIST) {

        // return ll depending on end
        *closest_llp = min_is_de ? min_sp->rx_ll : min_sp->tx_ll;

        // return spot
        *closest_sp = *min_sp;

        // ok
        return (true);
    }

    // none within MAX_CSR_DIST
    return (false);
}


/* draw a dot and/or label at the given end of a spot path, as per setup options.
 * N.B. this only handles LOME_RXEND or LOME_TXEND, not LOME_BOTH.
 */
static void drawSpotTXRXOnMap (const DXSpot &spot, LabelOnMapEnd txrx, LabelOnMapDot dot)
{
    // always draw at least the dot unless no label at all
    LabelType lblt = getSpotLabelType();
    if (lblt == LBL_NONE)
        return;

    // handy bools
    bool tx_end = txrx == LOME_TXEND;
    bool just_dot = dot == LOMD_JUSTDOT;

    // get dot size
    int dot_r = getRawBandSpotRadius (spot.kHz);

    // handy ll of desired end
    const LatLong &ll = tx_end ? spot.tx_ll : spot.rx_ll;

    // color depends on band
    uint16_t b_color = getBandColor (spot.kHz);

    // get screen coord, insure over map
    SCoord s;
    ll2s (ll, s, dot_r);                                                // overkill since raw >= canonical
    if (!overMap(s))
        return;

    // rx "dot" end is square, tx is a circle
    SCoord s_raw;
    ll2sRaw (ll, s_raw, dot_r);
    drawSpotDot (s_raw.x, s_raw.y, dot_r, txrx, b_color);

    // done if no text label
    if (lblt == LBL_DOT || just_dot)
        return;

    // decide text: whole call or just prefix
    char prefix[MAX_PREF_LEN];
    const char *call = tx_end ? spot.tx_call : spot.rx_call;
    const char *tag = NULL;
    if (lblt == LBL_PREFIX) {
        findCallPrefix (call, prefix);
        tag = prefix;
    } else if (lblt == LBL_CALL) {
        tag = call;
    } else
        fatalError ("Bogus label type: %d\n", (int)lblt);


    // position and draw
    SBox b;
    setMapTagBox (tag, s, dot_r/tft.SCALESZ+1, b);                        // wants canonical size
    uint16_t txt_color = getGoodTextColor (b_color);
    drawMapTag (tag, b, txt_color, b_color);
}

/* draw a dot and/or label at the given end/ends of a spot path, as per setup options.
 * N.B. we don't draw the path, use drawSpotPathOnMap() for that.
 */
void drawSpotLabelOnMap (const DXSpot &spot, LabelOnMapEnd txrx, LabelOnMapDot dot)
{
    if (txrx == LOME_TXEND || txrx == LOME_BOTH)
        drawSpotTXRXOnMap (spot, LOME_TXEND, dot);
    if (txrx == LOME_RXEND || txrx == LOME_BOTH)
        drawSpotTXRXOnMap (spot, LOME_RXEND, dot);
}

/* draw path if enabled as per setup options.
 * N.B. we don't draw ends or labels; use drawSpotLabelOnMap() for those.
 */
void drawSpotPathOnMap (const DXSpot &spot)
{
    // raw line size, unless none
    int raw_pw = getRawBandPathWidth(spot.kHz);
    if (raw_pw == 0)
        return;

    // printf ("******** pw %d\n", raw_pw);        // RBF

    const uint16_t color = getBandColor(spot.kHz);

    // draw from rx to tx
    float slat = sinf (spot.rx_ll.lat);
    float clat = cosf (spot.rx_ll.lat);
    float dist, bear;
    propPath (false, spot.rx_ll, slat, clat, spot.tx_ll, &dist, &bear);
    const int n_step = ((int)ceilf(dist/deg2rad(PATH_SEGLEN))) | 1;     // always odd so both ends are drawn
    const float step = dist/n_step;
    const bool dashed = getBandPathDashed (spot.kHz);
    SCoord prev_s = {0, 0};                                             // .x == 0 means don't show


    for (int i = 0; i <= n_step; i++) {                                 // fence posts
        float r = i*step;
        float ca, B;
        SCoord s;
        solveSphere (bear, r, slat, clat, &ca, &B);
        ll2sRaw (asinf(ca), fmodf(spot.rx_ll.lng+B+5*M_PIF,2*M_PIF)-M_PIF, s, raw_pw);
        if (prev_s.x > 0) {
            if (segmentSpanOkRaw(prev_s, s, raw_pw)) {
                if (!dashed || n_step < 7 || (i & 1))
                    tft.drawLineRaw (prev_s.x, prev_s.y, s.x, s.y, raw_pw, color);
            } else
               s.x = 0;
        }
        prev_s = s;
    }
}

/* draw the given spot in the given pane row with given bg color, known to be visible.
 */
void drawSpotOnList (const SBox &box, const DXSpot &spot, int row, uint16_t bg_col)
{
    if (debugLevel (DEBUG_SCROLL, 1))
        Serial.printf ("row %2d: drawing\n", row);

    selectFontStyle (LIGHT_FONT, FAST_FONT);
    char line[50];

    // set entire row to bg_col
    const uint16_t x = box.x+1;
    const uint16_t y = box.y + LISTING_Y0 + row*LISTING_DY;
    const uint16_t h = LISTING_DY - 2;
    tft.fillRect (x, y-LISTING_OS, box.w-2, h, bg_col);

    // pretty freq, fixed 8 chars, bg matching band color assignment
    const char *f_fmt = spot.kHz < 1e6F ? "%8.1f" : "%8.0f";
    snprintf (line, sizeof(line), f_fmt, spot.kHz);
    const uint16_t fbg_col = getBandColor (spot.kHz);
    const uint16_t ffg_col = getGoodTextColor(fbg_col);
    tft.setTextColor(ffg_col);
    tft.fillRect (x, y-LISTING_OS, 50, h, fbg_col);
    tft.setCursor (x, y);
    tft.print (line);

    // add call
    const int max_call = BOX_IS_PANE_0(box) ? MAX_SPOTCALL_LEN-3 : MAX_SPOTCALL_LEN-1;
    tft.setTextColor(RA8875_WHITE);
    snprintf (line, sizeof(line), " %-*.*s ", max_call, max_call, spot.tx_call);
    tft.print (line);

    // and finally age, width depending on pane
    time_t age = myNow() - spot.spotted;
    tft.print (formatAge (age, line, sizeof(line), BOX_IS_PANE_0(box) ? 3 : 4));
}

/* shift ll slightly so it's more likely to have a separate pick position
 */
void ditherLL (LatLong &ll)
{
    // move around within roughly 1 pixel
    const float deg_per_pix = (360.0F/BUILD_W)/pan_zoom.zoom;
    ll.lat_d += deg_per_pix * (0.5F - random(100)/100.0F);
    ll.lng_d += deg_per_pix * (0.5F - random(100)/100.0F);
    normalizeLL (ll);
}


/* draw the visible spots and scroll controls
 */
void drawVisibleSpots (WatchListId wl_id, const DXSpot *spots, const ScrollState &ss, const SBox &box,
int16_t app_color)
{
    // show vis spots and note if any would be red above and below
    bool any_older = false;
    bool any_newer = false;
    int min_i, max_i;
    if (ss.getVisIndices (min_i, max_i) > 0) {
        for (int i = 0; i < ss.n_data; i++) {
            const DXSpot &spot = spots[i];
            if (i < min_i) {
                if (!any_older)
                    any_older = checkWatchListSpot (wl_id, spot) == WLS_HILITE;
            } else if (i > max_i) {
                if (!any_newer)
                    any_newer = checkWatchListSpot (wl_id, spot) == WLS_HILITE;
            } else {
                uint16_t bg_col = checkWatchListSpot (wl_id, spot) == WLS_HILITE ? RA8875_RED:RA8875_BLACK;
                drawSpotOnList (box, spot, ss.getDisplayRow(i), bg_col);
            }
        }
    }

    // erase unused rows below min_i
    for (int row = ss.getDisplayRow(--min_i); row >= 0 && row < ss.max_vis; row = ss.getDisplayRow(--min_i)) {
        if (debugLevel (DEBUG_SCROLL, 1))
            Serial.printf ("row %2d: erasing\n", row);
        const uint16_t y = box.y + LISTING_Y0 + row*LISTING_DY - LISTING_OS;
        tft.fillRect (box.x+1, y, box.w-2, LISTING_DY - 2, RA8875_BLACK);
    }

    // scroll controls red if any more red spots in their directions
    uint16_t up_color = app_color;
    uint16_t dw_color = app_color;
    if (ss.okToScrollDown() &&
                ((scrollTopToBottom() && any_older) || (!scrollTopToBottom() && any_newer)))
        dw_color = RA8875_RED;
    if (ss.okToScrollUp() &&
                ((scrollTopToBottom() && any_newer) || (!scrollTopToBottom() && any_older)))
        up_color = RA8875_RED;

    ss.drawScrollUpControl (box, up_color, app_color);
    ss.drawScrollDownControl (box, dw_color, app_color);
}

/* qsort-style function to compare two DXSpot by freq
 */
int qsDXCFreq (const void *v1, const void *v2)
{
    DXSpot *s1 = (DXSpot *)v1;
    DXSpot *s2 = (DXSpot *)v2;
    return (roundf(s2->kHz - s1->kHz));
}

/* qsort-style function to compare two DXSpot by rx_grid
 * N.B. actually used by OnAir to sort by org
 */
int qsDXCRXGrid (const void *v1, const void *v2)
{
    DXSpot *s1 = (DXSpot *)v1;
    DXSpot *s2 = (DXSpot *)v2;
    return (strcmp (s2->rx_grid, s1->rx_grid));
}

/* qsort-style function to compare two DXSpot by tx_call
 */
int qsDXCTXCall (const void *v1, const void *v2)
{
    DXSpot *s1 = (DXSpot *)v1;
    DXSpot *s2 = (DXSpot *)v2;
    return (strcmp (s2->tx_call, s1->tx_call));
}

/* qsort-style function to compare two DXSpot by time spotted
 */
int qsDXCSpotted (const void *v1, const void *v2)
{
    DXSpot *s1 = (DXSpot *)v1;
    DXSpot *s2 = (DXSpot *)v2;
    return (s1->spotted - s2->spotted);         // newest times (larger numbers) first
}

/* qsort-style function to compare two DXSpot by separation distance
 */
int qsDXCDist (const void *v1, const void *v2)
{
    DXSpot *s1 = (DXSpot *)v1;
    DXSpot *s2 = (DXSpot *)v2;
    float d1 = simpleSphereDist (s1->rx_ll, s1->tx_ll);
    float d2 = simpleSphereDist (s2->rx_ll, s2->tx_ll);
    return (roundf(1000*(d1 - d2)));            // want farthest (largest) first
}

/* actual drawing of a DXSpot dot
 */
void drawSpotDot (int16_t raw_x, int16_t raw_y, uint16_t radius, LabelOnMapEnd txrx, uint16_t color)
{
    if (txrx == LOME_TXEND) {
        // circle suggesting expanding wave
        tft.fillCircleRaw (raw_x, raw_y, radius, color);
        tft.drawCircleRaw (raw_x, raw_y, radius+1, RA8875_BLACK);
    } else {
        // square suggesting receiving array ??
        tft.fillRectRaw (raw_x-radius, raw_y-radius, 2*radius, 2*radius, color);
        tft.drawRectRaw (raw_x-radius-1, raw_y-radius-1, 2*radius+1, 2*radius+1, RA8875_BLACK);
    }
}
