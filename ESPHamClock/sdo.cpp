/* manage the Solar Dynamics Observatory pane
 */

#include "HamClock.h"

#define SDO_IMG_INTERVAL        1800            // image update interval, seconds
#define SDO_COLOR               RA8875_MAGENTA  // just for error messages

typedef enum {
    SDOT_COMP,
    SDOT_HMIB,
    SDOT_HMIIC,
    SDOT_131,
    SDOT_193,
    SDOT_211,
    SDOT_304,
    SDOT_N
} SDOImgType;

static const char *sdo_menu[SDOT_N] = {
    "Composite",
    "Magnetogram",
    "6173A",
    "131A",
    "193A",
    "211A",
    "304A",
};

static const char *sdo_url[SDOT_N] = {
    "https://sdo.gsfc.nasa.gov/assets/img/latest/mpeg/latest_1024_211193171.mp4",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/mpeg/latest_1024_HMIB.mp4",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/mpeg/latest_1024_HMIIC.mp4",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/mpeg/latest_1024_0131.mp4",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/mpeg/latest_1024_0193.mp4",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/mpeg/latest_1024_0211.mp4",
    "https://sdo.gsfc.nasa.gov/assets/img/latest/mpeg/latest_1024_0304.mp4",
};

static const char *sdo_file[SDOT_N] = {
    #if defined(_CLOCK_1600x960) 
        "f_211_193_171_340.bmp",
        "latest_340_HMIB.bmp",
        "latest_340_HMIIC.bmp",
        "f_131_340.bmp",
        "f_193_340.bmp",
        "f_211_340.bmp",
        "f_304_340.bmp",
    #elif defined(_CLOCK_2400x1440)
        "f_211_193_171_510.bmp",
        "latest_510_HMIB.bmp",
        "latest_510_HMIIC.bmp",
        "f_131_510.bmp",
        "f_193_510.bmp",
        "f_211_510.bmp",
        "f_304_510.bmp",
    #elif defined(_CLOCK_3200x1920)
        "f_211_193_171_680.bmp",
        "latest_680_HMIB.bmp",
        "latest_680_HMIIC.bmp",
        "f_131_680.bmp",
        "f_193_680.bmp",
        "f_211_680.bmp",
        "f_304_680.bmp",
    #else
        "f_211_193_171_170.bmp",
        "latest_170_HMIB.bmp",
        "latest_170_HMIIC.bmp",
        "f_131_170.bmp",
        "f_193_170.bmp",
        "f_211_170.bmp",
        "f_304_170.bmp",
    #endif
};

// state
#define _SDO_ROT_INIT   10
static uint8_t sdo_choice, sdo_rotating = _SDO_ROT_INIT; // rot is always 0 or 1, init with anything else

/* save image choice and whether rotating to nvram
 */
static void saveSDOChoice (void)
{
    NVWriteUInt8 (NV_SDO, sdo_choice);
    NVWriteUInt8 (NV_SDOROT, sdo_rotating);
}

/* insure sdo_choice and sdo_rotating are loaded from nvram
 */
static void loadSDOChoice (void)
{
    if (sdo_rotating == _SDO_ROT_INIT) {
        if (!NVReadUInt8 (NV_SDOROT, &sdo_rotating)) {
            sdo_rotating = 0;
            NVWriteUInt8 (NV_SDOROT, sdo_rotating);
        }
        if (!NVReadUInt8 (NV_SDO, &sdo_choice) || sdo_choice >= SDOT_N) {
            sdo_choice = SDOT_HMIIC;
            NVWriteUInt8 (NV_SDO, sdo_choice);
        }
    }
}

/* render sdo_choice, downloading fresh if not found or stale.
 * use plotMessage if error.
 * return whether ok.
 */
static bool drawSDOImage (const SBox &box)
{
    // get corresponding file name
    const char *fn = sdo_file[sdo_choice];

    // check local file first
    bool need_fresh = true;             // assume true until proven otherwise
    char local_path[1000];
    snprintf (local_path, sizeof(local_path), "%s/%s", our_dir.c_str(), fn);
    struct stat sbuf;
    if (stat (local_path, &sbuf) == 0)
        need_fresh = myNow() > sbuf.st_mtime + SDO_IMG_INTERVAL;

    // assume download bad until proven otherwise
    bool ok = !need_fresh;

    // download if time to refresh
    if (need_fresh) {
        char url[1000];
        snprintf (url, sizeof(url), "/SDO/%s", fn);

        WiFiClient client;
        Serial.println (url);
        if (wifiOk() && client.connect(backend_host, backend_port)) {
            updateClocks(false);
        
            // query web page
            httpHCGET (client, backend_host, url);
        
            // skip response header
            if (!httpSkipHeader (client)) {
                plotMessage (box, SDO_COLOR, "SDO header short");
                goto out;
            }

            // create local
            FILE *fp = fopen (local_path, "w");
            if (!fp) {
                plotMessage (box, SDO_COLOR, "can not create local SDO image");
                goto out;
            }
            if (chown (local_path, getuid(), getgid()) < 0)     // friendly
                Serial.printf ("chown(%s,%d,%d): %s\n", local_path, getuid(), getgid(), strerror(errno));

            // copy to local
            char c;
            while (getTCPChar (client, &c)) {
                if (fputc (c, fp) == EOF) {
                    plotMessage (box, SDO_COLOR, "can not save SDO image");
                    goto out;
                }
            }

            // finished with fp
            fclose (fp);

            // got something
            Serial.printf ("saved local %s\n", fn);
            ok = true;

        } else {
            plotMessage (box, SDO_COLOR, "SDO connection failed");
        }

      out:
        client.stop();
    }

    // display local file
    if (ok) {
        Serial.printf ("reading local %s\n", fn);
        FILE *fp = fopen (local_path, "r");
        if (!fp) {
            plotMessage (box, SDO_COLOR, "local SDO file is missing");
            ok = false;
        } else {
            GenReader gr(fp);
            char ynot[256];
            if (!install24BMP (gr, box, ynot, sizeof(ynot))) {
                plotMessage (box, SDO_COLOR, ynot);
                ok = false;
            }
            fclose (fp);
        }
    }

    return (ok);
}

/* return whether sdo image is rotating
 */
bool isSDORotating(void)
{
    loadSDOChoice();
    return (sdo_rotating != 0);
}


/* update SDO pane
 */
bool updateSDOPane (const SBox &box)
{
    resetWatchdog();

    // advance choice if rotating
    if (isSDORotating()) {
        sdo_choice = (sdo_choice + 1) % SDOT_N;
        saveSDOChoice();
    }


    // draw image
    bool ok = drawSDOImage(box);

    // draw info if ok, layout similar to moon
    if (ok) {

        // current user's time
        time_t t0 = nowWO();

        // fresh info at user's effective time
        getSolarCir (t0, de_ll, solar_cir);

        // draw corners, similar to moon

        char str[128];
        selectFontStyle (LIGHT_FONT, FAST_FONT);
        tft.setTextColor (DE_COLOR);

        snprintf (str, sizeof(str), "Az:%.0f", rad2deg(solar_cir.az));
        tft.setCursor (box.x+1, box.y+2);
        tft.print (str);

        snprintf (str, sizeof(str), "El:%.0f", rad2deg(solar_cir.el));
        tft.setCursor (box.x+box.w-getTextWidth(str)-1, box.y+2);
        tft.print (str);

        // show which ever rise or set event comes next
        time_t rise, set;
        int detz = getTZ (de_tz);
        getSolarRS (t0, de_ll, &rise, &set);
        if (rise > t0 && (set < t0 || rise - t0 < set - t0))
            snprintf (str, sizeof(str), "R@%02d:%02d", hour(rise+detz), minute (rise+detz));
        else if (set > t0 && (rise < t0 || set - t0 < rise - t0))
            snprintf (str, sizeof(str), "S@%02d:%02d", hour(set+detz), minute (set+detz));
        else
            strcpy (str, "No R/S");
        tft.setCursor (box.x+1, box.y+box.h-10);
        tft.print (str);

        snprintf (str, sizeof(str), "%.0fm/s", solar_cir.vel);;
        tft.setCursor (box.x+box.w-getTextWidth(str)-1, box.y+box.h-10);
        tft.print (str);
    }

    return (ok);
}

/* attempt to show the movie for sdo_choice
 */
static void showSDOmovie (void)
{
    openURL (sdo_url[sdo_choice]);
}

/* check for our touch in the given pane box.
 * return whether we should stay on this pane, else give user choice of new panes.
 * N.B. we assume s is within box
 */
bool checkSDOTouch (const SCoord &s, const SBox &box)
{
    // not ours when in title
    if (s.y < box.y + PANETITLE_H)
        return (false);

    // insure current values
    loadSDOChoice();

    // show menu of SDOT_N images options + rotate option + show grayline + show movie

    #define SM_INDENT 5

    // handy indices to extra menu items
    enum {
        SDOM_ROTATE = SDOT_N,
        SDOM_GAP,
        SDOM_GRAYLINE,
        SDOM_SHOWWEB,
        SDOM_NMENU
    };

    MenuItem mitems[SDOM_NMENU];

    // set first SDOT_N to name collection
    for (int i = 0; i < SDOT_N; i++) {
        mitems[i] = {MENU_1OFN, !sdo_rotating && i == sdo_choice, 1, SM_INDENT, sdo_menu[i]};
    }

    // set whether rotating
    mitems[SDOM_ROTATE] = {MENU_1OFN, (bool)sdo_rotating, 1, SM_INDENT, "Rotate"};

    // nice gap
    mitems[SDOM_GAP] = {MENU_BLANK, false, 0, 0, NULL};

    // set grayline option
    mitems[SDOM_GRAYLINE] = {MENU_TOGGLE, false, 2, SM_INDENT, "Grayline tool"};

    // set show web page option, but not on fb0
#if defined(_USE_FB0)
    mitems[SDOM_SHOWWEB] = {MENU_IGNORE, false, 0, 0, NULL};
#else
    mitems[SDOM_SHOWWEB] = {MENU_TOGGLE, false, 3, SM_INDENT, "Show movie"};
#endif

    SBox menu_b = box;          // copy, not ref
    menu_b.x += box.w/4;
    menu_b.y += 5;
    menu_b.w = 0;               // shrink wrap
    SBox ok_b;

    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_CANCELOK, 1, SDOM_NMENU, mitems};
    bool ok = runMenu (menu);

    // change to new option unless cancelled
    bool refresh_pane = true;
    if (ok) {

        // set new selection unless rotating
        sdo_rotating = mitems[SDOM_ROTATE].set;
        if (!sdo_rotating) {
            for (int i = 0; i < SDOT_N; i++) {
                if (mitems[i].set) {
                    sdo_choice = i;
                    break;
                }
            }
        }

        // save
        saveSDOChoice();

        // check for movie
        if (mitems[SDOM_SHOWWEB].set)
            showSDOmovie ();

        // gray line must 1) fix hole in pane 2) show grayline then 3) fix map on return
        if (mitems[SDOM_GRAYLINE].set) {
            updateSDOPane (box);
            plotGrayline();
            initEarthMap();
            refresh_pane = false;
        }

    } else if (sdo_rotating) {

        // cancelled but this kludge effectively causes updateSDO to show the same image
        sdo_choice = (sdo_choice + SDOT_N - 1) % SDOT_N;
        saveSDOChoice();
    }

    // show image unless already done so, even if cancelled just to erase the menu
    if (refresh_pane)
        scheduleNewPlot(PLOT_CH_SDO);

    // ours
    return (true);
}
