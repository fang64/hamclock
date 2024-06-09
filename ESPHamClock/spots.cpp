/* handy tools for Spots
 */

#include "HamClock.h"


/* find closest location from ll to either end of paths defined in the given list of spots.
 * return whether found one within MAX_CSR_DIST.
 */
bool getClosestSpot (const DXSpot *list, int n_list, const LatLong &from_ll,
    DXSpot *closest_sp, LatLong *closest_llp)
{
    // linear search -- not worth kdtree etc
    const DXSpot *min_sp = NULL;   
    float min_d = 1e10;
    bool min_is_de = false;         
    for (int i = 0; i < n_list; i++) {

        const DXSpot *sp = &list[i];
        float d;                    

        d = simpleSphereDist (sp->rx_ll, from_ll);
        if (d < min_d) {
            min_d = d;
            min_sp = sp;
            min_is_de = true;
        }

        d = simpleSphereDist (sp->tx_ll, from_ll);
        if (d < min_d) {
            min_d = d;
            min_sp = sp;
            min_is_de = false;
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
 * N.B. we don't draw the path, use drawSpotPathOnMap() for that.
 */
void drawSpotLabelOnMap (const DXSpot &spot, LabelOnMapEnd txrx, LabelOnMapDot dot)
{
    // always draw at least the dot unless no label at all
    LabelType lblt = getSpotLabelType();
    if (lblt == LBL_NONE)
        return;
    int dot_r = getSpotDotRadius();
    // printf ("******** dot_r %d\n", dot_r);        // RBF

    // handy bools
    bool tx_end = txrx == LOM_TXEND;
    bool just_dot = dot == LOM_JUSTDOT;

    // handy ll of desired end
    const LatLong &ll = tx_end ? spot.tx_ll : spot.rx_ll;

    // color depends on band
    uint16_t b_color = getBandColor ((long)(spot.kHz*1000));            // wants Hz

    // get screen coord, insure over map
    SCoord s;
    ll2s (ll, s, dot_r);  // overkill since raw >= canonical
    if (!overMap(s))
        return;

    // rx "dot" end is square, tx is a circle
    SCoord s_raw;
    ll2sRaw (ll, s_raw, dot_r);
    if (tx_end) {
        tft.fillCircleRaw (s_raw.x, s_raw.y, dot_r, b_color);
        tft.drawCircleRaw (s_raw.x, s_raw.y, dot_r, RA8875_BLACK);
    } else {
        tft.fillRectRaw (s_raw.x-dot_r, s_raw.y-dot_r, 2*dot_r, 2*dot_r, b_color);
        tft.drawRectRaw (s_raw.x-dot_r, s_raw.y-dot_r, 2*dot_r, 2*dot_r, RA8875_BLACK);
    }

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

/* draw path if enabled as per setup options.
 * N.B. we don't draw ends or labels; use drawSpotLabelOnMap() for those.
 */
void drawSpotPathOnMap (const DXSpot &spot)
{
    // raw line size, unless none
    int raw_pw = getSpotPathWidth();
    if (raw_pw == 0)
        return;

    // printf ("******** pw %d\n", raw_pw);        // RBF

    const uint16_t color = getBandColor(spot.kHz * 1000);               // wants Hz

    // draw from rx to tx
    float slat = sinf (spot.rx_ll.lat);
    float clat = cosf (spot.rx_ll.lat);
    float dist, bear;
    propPath (false, spot.rx_ll, slat, clat, spot.tx_ll, &dist, &bear);
    const int n_step = (int)ceilf(dist/deg2rad(PATH_SEGLEN)) | 1;   // always odd for dashed ends
    const float step = dist/n_step;
    const bool dashed = getBandDashed (spot.kHz * 1000);
    SCoord prev_s = {0, 0};                                         // .x == 0 means don't show

    for (int i = 0; i <= n_step; i++) {                             // fence posts
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
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    char line[50];

    // set entire row to bg_col
    const uint16_t x = box.x+4;
    const uint16_t y = box.y + LISTING_Y0 + row*LISTING_DY;
    const uint16_t h = LISTING_DY - 2;
    tft.fillRect (x, y-2, box.w-6, h, bg_col);

    // pretty freq, fixed 8 chars, bg matching band color assignment
    const char *f_fmt = spot.kHz < 1e6F ? "%8.1f" : "%8.0f";
    snprintf (line, sizeof(line), f_fmt, spot.kHz);
    const uint16_t fbg_col = getBandColor ((long)(1000*spot.kHz)); // wants Hz
    const uint16_t ffg_col = getGoodTextColor(fbg_col);
    tft.setTextColor(ffg_col);
    tft.fillRect (x, y-2, 50, h, fbg_col);
    tft.setCursor (x, y);
    tft.print (line);

    // add call
    const int max_call = MAX_SPOTCALL_LEN-1;
    tft.setTextColor(RA8875_WHITE);
    snprintf (line, sizeof(line), " %-*.*s ", max_call, max_call, spot.tx_call);
    tft.print (line);

    // and finally age, width depending on pane
    time_t age = myNow() - spot.spotted;
    tft.print (formatAge (age, line, sizeof(line), BOX_IS_PANE_0(box) ? 1 : 4));
}
