/* handle contest retrieval and display.
 */

#include "HamClock.h"



#define CONTEST_COLOR   RGB565(205,91,69)       // X11 coral3
#define TD_COLOR        CONTEST_COLOR           // titles-with-dates background
#define NOW_COLOR       RGB565(40,140,40)       // background when contest is happening now
#define CREDITS_Y0      SUBTITLE_Y0             // dy of credits row
#define START_DY        LISTING_Y0              // dy of first contest row
#define CONTEST_DY      12                      // dy of each successive row -- a bit tighter than LISTING_DY

// NV_CONTESTS bits
#define NVBIT_SHOWDATE  0x1                     // showing dates
#define NVBIT_SHOWDETZ  0x2                     // showing DE time zone

// URL to access info
static const char contest_page[] = "/contests/contests311.txt";

static ContestEntry *contests;                  // malloced list of ContestEntry
static char *credit;                            // malloced credit line
static bool show_date;                          // whether to show 2nd line with date
static bool show_detz;                          // whether to show dates in DE timezone
static ScrollState cts_ss;                      // scrolling context, max_vis/2 if showing date

/* save NV_CONTESTS
 */
static void saveContestNV (void)
{
    uint8_t contest_mask = 0;

    contest_mask |= show_date ? NVBIT_SHOWDATE : 0;
    contest_mask |= show_detz ? NVBIT_SHOWDETZ : 0;

    NVWriteUInt8 (NV_CONTESTS, contest_mask);
}

/* load NV_CONTESTS
 */
static void loadContestNV (void)
{
    uint8_t contest_mask = 0;

    if (!NVReadUInt8 (NV_CONTESTS, &contest_mask)) {
        contest_mask = 0;
        NVWriteUInt8 (NV_CONTESTS, contest_mask);
    }

    show_date = (contest_mask & NVBIT_SHOWDATE) != 0;
    show_detz = (contest_mask & NVBIT_SHOWDETZ) != 0;
}

/* draw contests[] in the given pane box
 */
static void drawContestsPane (const SBox &box)
{
    // skip if no credit yet
    if (!credit)
        return;

    // erase
    prepPlotBox (box);

    // handy UTC
    time_t now = myNow();

    // title
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor(CONTEST_COLOR);
    static const char *title = "Contests";
    uint16_t tw = getTextWidth(title);
    tft.setCursor (box.x + (box.w-tw)/2, box.y + PANETITLE_H);
    tft.print (title);

    // credit
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor(CONTEST_COLOR);
    uint16_t cw = getTextWidth(credit);
    tft.setCursor (box.x + (box.w-cw)/2, box.y + CREDITS_Y0);
    tft.print (credit);

    // show each contest starting with top_vis, up to max visible.
    // N.B. scroller doesn't know show_data entries occupy two rows.
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor(RA8875_WHITE);
    uint16_t y0 = box.y + START_DY;
    int min_i, max_i;
    if (cts_ss.getVisIndices (min_i, max_i) > 0) {
        for (int i = min_i; i <= max_i; i++) {
            ContestEntry &ce = contests[i];
            int r = cts_ss.getDisplayRow(i);
            if (show_date) {
                uint16_t y = y0 + r*2*CONTEST_DY;
                if (now > ce.start_t && now < ce.end_t)
                    tft.fillRect (box.x+1, y-4, box.w-2, 2*CONTEST_DY+1, NOW_COLOR);
                else if (i < max_i)
                    tft.drawLine (box.x+1, y+2*CONTEST_DY-3, box.x+box.w-2, y+2*CONTEST_DY-3, 2, TD_COLOR);
                uint16_t w = getTextWidth (ce.title);
                tft.setCursor (box.x + (box.w-w)/2, y);
                tft.print (ce.title);
                y += CONTEST_DY;
                w = getTextWidth (ce.date_str);
                tft.setCursor (box.x + (box.w-w)/2, y);
                tft.print (ce.date_str);
            } else {
                uint16_t y = y0 + r*CONTEST_DY;
                if (now > ce.start_t && now < ce.end_t)
                    tft.fillRect (box.x+1, y-2, box.w-2, CONTEST_DY, NOW_COLOR);
                uint16_t w = getTextWidth (ce.title);
                tft.setCursor (box.x + (box.w-w)/2, y);
                tft.print (ce.title);
            }
        }
    }

    // draw scroll controls, if needed
    cts_ss.drawScrollDownControl (box, CONTEST_COLOR, CONTEST_COLOR);
    cts_ss.drawScrollUpControl (box, CONTEST_COLOR, CONTEST_COLOR);
}

/* scroll up, if appropriate to do so now.
 */
static void scrollContestUp (const SBox &box)
{
    if (cts_ss.okToScrollUp()) {
        cts_ss.scrollUp();
        drawContestsPane (box);
    }
}

/* scroll down, if appropriate to do so now.
 */
static void scrollContestDown (const SBox &box)
{
    if (cts_ss.okToScrollDown()) {
        cts_ss.scrollDown();
        drawContestsPane (box);
    }
}

/* format the two unix UTC contest start and end times as nice text that fits in the given box.
 * N.B. we assume desired font is already selected.
 */
static void formatTimeLine (const SBox &box, time_t t1, time_t t2, char str[], size_t str_l)
{
    if (show_detz) {

        // DE timezone uses AM PM notation

        // break out in DE timezone
        t1 += getTZ (de_tz);
        t2 += getTZ (de_tz);
        struct tm tm1 = *gmtime (&t1);
        struct tm tm2 = *gmtime (&t2);

        // convert to AM PM
        int h1_12 = tm1.tm_hour % 12;
        if (h1_12 == 0)
            h1_12 = 12;
        const char *m1 = tm1.tm_hour < 12 ? "AM" : "PM";
        int h2_12 = tm2.tm_hour % 12;
        if (h2_12 == 0)
            h2_12 = 12;
        const char *m2 = tm2.tm_hour < 12 ? "AM" : "PM";

        if (tm1.tm_wday == tm2.tm_wday) {
            // starts and ends on same day, just show once with time range, assume always fits in box
            if (strcmp (m1, m2) == 0) {
                // both AM or both PM
                snprintf (str, str_l, "%s %d:%02d - %d:%02d %s", dayShortStr(tm1.tm_wday+1),
                    h1_12, tm1.tm_min, h2_12, tm2.tm_min, m1);
            } else {
                // different AM PM
                snprintf (str, str_l, "%s %d:%02d %s - %d:%02d %s", dayShortStr(tm1.tm_wday+1),
                    h1_12, tm1.tm_min, m1, h2_12, tm2.tm_min, m2);
            }
        } else {
            // different days so must show each, take care to fit in box
            // N.B. dayShortStr() returns pointer to the same static array
            char wd1[10], wd2[10];
            strcpy (wd1, dayShortStr(tm1.tm_wday+1));
            strcpy (wd2, dayShortStr(tm2.tm_wday+1));
            snprintf (str, str_l, "%s %d:%02d %s - %s %d:%02d %s", wd1, h1_12, tm1.tm_min, m1,
                        wd2, h2_12, tm2.tm_min, m2);
            if (getTextWidth(str) >= box.w) {
                // printf ("************* box.w %d str_w %d %s -> ", box.w, getTextWidth(str), str);
                snprintf (str, str_l, "%s %d:%02d%s-%s %d:%02d%s", wd1, h1_12, tm1.tm_min, m1,
                        wd2, h2_12, tm2.tm_min, m2);
                // printf ("str_w %d %s\n", getTextWidth(str), str);
            }
        }

    } else {

        // UTC uses 24 hour notation

        struct tm tm1 = *gmtime (&t1);
        struct tm tm2 = *gmtime (&t2);

        if (tm1.tm_wday == tm2.tm_wday) {
            // starts and ends on same day, just show once with time range
            snprintf (str, str_l, "%s %02d:%02d - %02d:%02dZ", dayShortStr(tm1.tm_wday+1),
                            tm1.tm_hour, tm1.tm_min, tm2.tm_hour, tm2.tm_min);
        } else {
            // show each day.
            // N.B. dayShortStr() returns pointer to the same static array
            char wd1[10], wd2[10];
            strcpy (wd1, dayShortStr(tm1.tm_wday+1));
            strcpy (wd2, dayShortStr(tm2.tm_wday+1));
            snprintf (str, str_l, "%s %02d:%02d - %s %02d:%02dZ", wd1, tm1.tm_hour, tm1.tm_min,
                            wd2, tm2.tm_hour, tm2.tm_min);
        }
    }

    // be absolutely certain it fits within box
    for (uint16_t str_w = getTextWidth(str); str_w >= box.w; str_w = getTextWidth(str))
        str[strlen(str)-1] = '\0';
}

/* show the contest menu knowing s is within box.
 * return true if enough changed that a complete update is required, such as changing show_date,
 * else false if ok to just redraw pane without any changes.
 */
static bool runContestMenu (const SCoord &s, const SBox &box)
{
    // whether caller must redo everything
    bool full_redo = false;

    // decide which contest s is pointing at, if any
    ContestEntry *cep = NULL;
    if (s.y >= box.y + START_DY) {
        int display_i = (s.y - box.y - START_DY)/CONTEST_DY;
        if (show_date)
            display_i /= 2;
        int data_i;
        if (cts_ss.findDataIndex (display_i, data_i))
            cep = &contests[data_i];
        // printf ("****************** %d %d %d %d %s\n", s.y, box.y, display_i, data_i, cep ? cep->title : "NONE");
    }

    // prepare menu
    const int indent = 2;

    // get alarm status
    AlarmState a_s;
    time_t a_t;
    bool a_utc;
    getOneTimeAlarmState (a_s, a_t, a_utc);
    bool starts_in_future = cep && cep->start_t > nowWO();
    bool alarm_is_set = cep && a_s == ALMS_ARMED && a_t == cep->start_t && starts_in_future;

    // build a version of the contest name that fits well within box
    const uint16_t menu_gap = 20;
    char cname[50];
    quietStrncpy (cname, cep ? cep->title : "", sizeof(cname));
    for (uint16_t t_l = getTextWidth(cname); t_l > box.w-2*menu_gap; t_l = getTextWidth(cname)) {
        // try to chop at blank, else just be ruthless
        char *r_space = strrchr (cname, ' ');
        if (r_space)
            *r_space = '\0';
        else
            cname[strlen(cname)-1] = '\0';
    }

    // decide which menu items to show
    MenuFieldType title_mft = cep ? MENU_LABEL : MENU_IGNORE;
    MenuFieldType alarm_mft = cep && starts_in_future ? MENU_TOGGLE : MENU_IGNORE;
    MenuFieldType showdtz_mft = cep ? MENU_IGNORE : MENU_TOGGLE;

#if defined(_USE_FB0)
    MenuFieldType web_mft = MENU_IGNORE;
#else
    MenuFieldType web_mft = cep ? MENU_TOGGLE : MENU_IGNORE;
#endif


    // build menu
    MenuItem mitems[] = {
        {title_mft,    false,         0, indent, cname, 0},                        // 0
        {showdtz_mft,  show_date,     1, indent, "Show dates", 0},                 // 1
        {showdtz_mft,  show_detz,     2, indent, "Use DE TZ", 0},                  // 2
        {alarm_mft,    alarm_is_set,  3, indent, "Set alarm", 0},                  // 3
        {web_mft,      false,         4, indent, "Show web page", 0},              // 4
    };
    const int n_mi = NARRAY(mitems);


    // boxes
    const uint16_t menu_x = box.x + menu_gap;
    const uint16_t menu_h = 60;
    const uint16_t menu_max_y = box.y + box.h - menu_h - 5;
    const uint16_t menu_y = s.y < menu_max_y ? s.y : menu_max_y;
    SBox menu_b = {menu_x, menu_y, 0, 0};
    SBox ok_b;

    // run
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_CANCELOK, 1, n_mi, mitems};
    if (runMenu (menu)) {

        // check for show_date change
        if (mitems[1].set != show_date) {
            show_date = mitems[1].set;
            saveContestNV();
            full_redo = true;   // requires ScrollState reset
        }

        // check for TZ change
        if (mitems[2].set != show_detz) {
            show_detz = mitems[2].set;
            saveContestNV();
            full_redo = true;   // requires new formatTimeLine()
        }

        if (cep) {

            // check alarm state change
            if (mitems[3].set != alarm_is_set)
                setOneTimeAlarmState (mitems[3].set ? ALMS_ARMED : ALMS_OFF, !show_detz, cep->start_t);

            // open web page if desired
            if (mitems[4].set)
                openURL (cep->url);
        }
    }

    // return whether redo is required
    return (full_redo);
}

/* collect Contest info into the contests[] array.
 * return whether io ok.
 */
static bool retrieveContests (const SBox &box)
{
    WiFiClient ctst_client;
    bool ok = false;

    // get date state
    loadContestNV();

    // download and load contests[]
    Serial.println(contest_page);
    if (wifiOk() && ctst_client.connect(backend_host, backend_port)) {

        // look alive
        updateClocks(false);

        // handy UTC
        time_t now = myNow();

        // fetch page and skip header
        httpHCGET (ctst_client, backend_host, contest_page);
        if (!httpSkipHeader (ctst_client)) {
            Serial.printf ("CTS: %s failed\n", contest_page);
            goto out;
        }

        // reset contests and credit
        for (int i = 0; i < cts_ss.n_data; i++) {
            ContestEntry &ce = contests[i];
            free (ce.date_str);
            free (ce.title);
            free (ce.url);
        }
        free (contests);
        contests = NULL;
        free (credit);
        credit = NULL;

        // init scroller and max data size. max_vis is half the number of rows if showing date too.
        cts_ss.init ((box.h - START_DY)/CONTEST_DY, 0, 0);      // max_vis, top_vis, n_data
        if (show_date)
            cts_ss.max_vis /= 2;

        // contests consist of 2 lines each
        char line1[100], line2[100];

        // first line is credit
        if (!getTCPLine (ctst_client, line1, sizeof(line1), NULL)) {
            Serial.printf ("CTS: %s no credit line\n", contest_page);
            goto out;
        }
        credit = strdup (line1);

        // consider transaction is ok if get at least the credit message
        ok = true;

        // set font for scrubContestTitleLine() and formatTimeLine()
        selectFontStyle (LIGHT_FONT, FAST_FONT);

        // read 2 lines per contest: info and url
        while (getTCPLine (ctst_client, line1, sizeof(line1), NULL)
                                        && getTCPLine (ctst_client, line2, sizeof(line2), NULL)) {
            if (debugLevel (DEBUG_CONTESTS, 1))
                Serial.printf ("CTS line %d: %s\n%s\n", cts_ss.n_data, line1, line2);

            // split line1 into the two unix UTC times and the title
            char *ut1 = line1;
            char *ut2 = strchr (ut1, ' ');
            if (!ut2) {
                Serial.printf ("CTS: line has no ut2: %s\n", line1);
                continue;
            }
            char *title = strchr (++ut2, ' ');
            if (!title) {
                Serial.printf ("CTS: line has no title: %s\n", line1);
                continue;
            }

            // skip if already over
            time_t end_t = atol(ut2);
            if (now > end_t) {
                Serial.printf ("CTS %s is already passed %ld\n", title, (long)end_t);
                continue;
            }

            // clean up title to fix in box
            scrubContestTitleLine (++title, box);

            // looks good, add to contests[]
            contests = (ContestEntry*) realloc (contests, (cts_ss.n_data+1) * sizeof(ContestEntry));
            if (!contests)
                fatalError ("No memory for %d contests", cts_ss.n_data+1);
            ContestEntry &ce = contests[cts_ss.n_data++];

            // save times, title and url 
            if (debugLevel (DEBUG_CONTESTS, 2)) {
                // inject as recent times
                ce.start_t = myNow() - 180 + 60*cts_ss.n_data;
                ce.end_t = myNow() + 60*cts_ss.n_data;
            } else {
                ce.start_t = atol (ut1);
                ce.end_t = end_t;
            }
            ce.title = strdup (title);                  // N.B. must free()
            ce.url = strdup (line2);                    // N.B. must free()

            // format date string. N.B. we REUSE line2 (ut1 and ut2 are in line1)
            formatTimeLine (box, ce.start_t, ce.end_t, line2, sizeof(line2));
            ce.date_str = strdup (line2);               // N.B. must free()
        }
    }

out:

    Serial.printf ("CTS: Found %d\n", cts_ss.n_data);

    ctst_client.stop();

    return (ok);
}

/* remove contests that are over and reset display if any were removed.
 */
static bool rmPastContests (void)
{
    bool any_past = false;
    time_t now = myNow();

    for (int i = 0; i < cts_ss.n_data; i++) {
        ContestEntry *cp = &contests[i];
        if (cp->end_t <= now) {
            memmove (cp, cp+1, (--cts_ss.n_data - i) * sizeof(ContestEntry));
            any_past = true;
        }
    }

    if (any_past)
        cts_ss.scrollToNewest();

    return (any_past);
}



/* collect Contest info into the contests[] array and show in the given pane box
 */
bool updateContests (const SBox &box, bool fresh)
{
    // retrieve once an hour at a random minute
    static int retrieve_hour = -1;              // init with any impossible value
    static int retrieve_min = -1;               // init with any negative value

    // pick a random minute first time through
    if (retrieve_min < 0) {
        retrieve_min = random (60);
        Serial.printf ("CTS: updating at %d mins past the hour\n", retrieve_min);
    }

    bool ok = true;

    if (fresh || (retrieve_hour != hour() && retrieve_min == minute())) {

        retrieve_hour = hour();

        ok = retrieveContests (box);
        cts_ss.scrollToNewest();
    }

    if (ok) {

        if (rmPastContests() || fresh)
            drawContestsPane (box);

    } else {

        plotMessage (box, CONTEST_COLOR, "Contests error");
    }

    return (ok);
}

/* return true if user is interacting with the contest pane, false if wants to change pane.
 * N.B. we assume s is within box
 */
bool checkContestsTouch (const SCoord &s, const SBox &box)
{
    if (s.y < box.y + PANETITLE_H) {

        // scroll control?

        if (cts_ss.checkScrollUpTouch (s, box)) {
            scrollContestUp (box);
            return (true);
        }
        if (cts_ss.checkScrollDownTouch (s, box)) {
            scrollContestDown (box);
            return (true);
        }

    } else {

        // run the menu, then minimal update
        if (runContestMenu (s, box))
            scheduleNewPlot (PLOT_CH_CONTESTS);
        else
            drawContestsPane (box);

        // ours regardless of menu outcome
        return (true);
    }

    // not ours
    return (false);
}

/* scrub the given contest title IN PLACE to fit within the given box
 * N.B. we assume desired font is already selected.
 */
void scrubContestTitleLine (char *line, const SBox &box)
{
    // look for a few common phases
    char *phrase;
    if ((phrase = strstr (line, "Parks on the Air")) != NULL)
        strcpy (phrase, "POTA");

    // keep chopping off at successive right-most space until fits within box
    uint16_t lw;                                        // line width in pixels
    while ((lw = getTextWidth (line)) >= box.w) {
        char *right_space = strrchr (line, ' ');
        if (right_space)
            *right_space = '\0';                        // EOS now at right-most space
        else
            break;                                      // still too long but no more spaces to chop
    }

    // always chop off any trailing punct char
    size_t ll = strlen (line);                          // line length in chars
    if (ll > 0 && ispunct(line[ll-1]))
        line[--ll] = '\0';

    // well just hack off if still too long
    while (getTextWidth (line) >= box.w)
        line[--ll] = '\0';
}

/* return title of contest with matching time, if any.
 */
const char *getAlarmedContestTitle (time_t t)
{
    for (int i = 0; i < cts_ss.n_data; i++)
        if (contests[i].start_t == t)
            return (contests[i].title);
    return (NULL);
}

/* return contests to caller
 */
int getContests (const char **credp, const ContestEntry **cepp)
{
    *credp = credit;
    *cepp = contests;
    return (cts_ss.n_data);
}
