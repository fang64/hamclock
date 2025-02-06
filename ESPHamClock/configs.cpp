/* manage configuration files.
 */

#include "HamClock.h"

// directpry and suffix for more eeprom files
static const char cfg_dir[] = "configurations";
static const char cfg_suffix[] = ".eeprom";

// handy config file name
typedef const char *CfgName;

// layout
#define TB_SET_COL  RA8875_GREEN                // most toggle button selected color
#define TB_DEL_COL  RA8875_RED                  // Delete toggle button selected color
#define TB_OFF_COL  RA8875_BLACK                // toggle button idle color
#define TB_B_COL    RA8875_WHITE                // toggle button border color
#define TL_COL      RGB565(255,200,100)         // titles color
#define TX_COL      RA8875_WHITE                // text color
#define TITLE_Y     50                          // title y
#define TB_SZ       20                          // toggle button size
#define ROW_DY      30                          // table row spacing
#define TBL_HY      130                         // restore/delete table headings y
#define TBL_Y       150                         // " top y
#define TBL_T1X      50                         // " title 1 x
#define TBL_C1X      80                         // " TB column 1 x
#define TBL_T2X     140                         // " title 2 x
#define TBL_C2X     170                         // " TB column 2 x
#define TBL_T3X     240                         // " title 3 x
#define TBL_C3X     250                         // " column 3 x
#define TBL_T4X     320                         // " title 4 x
#define TBL_C4X     320                         // " column 4 x
#define COK_Y       410                         // Cancel and Ok button top y
#define COK_W       100                         // " width
#define COK_H       38                          // " height
#define SAVE_CW     10                          // " cursor width
#define SAVE_TX     TBL_T1X                     // " TB x
#define SAVE_LX     (SAVE_TX+45)                // " label x
#define SAVE_NX     (SAVE_LX+70)                // " name field x
#define SAVE_Y      90                          // Save baseline Y
#define FONT_DY     26                          // " distance from top to baseline
#define SAVE_CDY    10                          // " cursor drop below baseline
#define SAVE_NH     (FONT_DY+SAVE_CDY)          // " name field height
#define SAVE_NW     480                         // " name field width
#define SAVE_CCOL   RA8875_GREEN                // " cursor color
#define SAVE_MAXN   40                          // max chars in save name, including EOS
#define MAX_CFG     ((COK_Y-TBL_Y)/ROW_DY)      // " max rows

// toggle button controller
typedef struct {
    SBox box;                                   // where
    bool on;                                    // state
} TBControl;

// table
enum {
    USE_CIDX,                                   // use column index
    UPD_CIDX,                                   // update column index
    DEL_CIDX,                                   // delete column index
    N_CIDX,
};
typedef struct {
    TBControl sav_tb;                           // Save TB
    char sav_name[SAVE_MAXN];                   // Save name string, always with EOS
    unsigned sav_cursor;                        // cursor index into name
    uint16_t sav_cy;                            // cursor y
    SBox sav_b;                                 // Save name display box
    TBControl tb[MAX_CFG][N_CIDX];              // main table
} CfgTable;


/* draw the given control in its current state
 */
static void drawTBControl (const CfgTable &ctbl, const TBControl &tb)
{
    if (tb.on) {
        // color depends on column
        uint16_t color = TB_SET_COL;
        for (int r = 0; r < MAX_CFG; r++) {
            if (&ctbl.tb[r][DEL_CIDX] == &tb) {
                color = TB_DEL_COL;
                break;
            }
        }
        fillSBox (tb.box, color);
        drawSBox (tb.box, TB_B_COL);
    } else {
        fillSBox (tb.box, TB_OFF_COL);
        drawSBox (tb.box, TB_B_COL);
    }
}


/* given pointer to one of the TB within ctbl.tb that has just toggled on or off:
 * go through the other TB and turn off any that conflict.
 */
static void checkCfgChoices (CfgTable &ctbl, int n_names, TBControl *tbp)
{
    // nothing conflicts with save or turning off
    if (tbp == &ctbl.sav_tb || !tbp->on)
        return;

    // find row and column
    int row, col;
    bool found = false;
    for (row = 0; row < n_names && row < MAX_CFG; row++) {
        for (col = 0; col < N_CIDX; col++) {
            if (tbp == &ctbl.tb[row][col]) {
                found = true;
                goto out;       // two-level break
            }
        }
    }
 out:
    if (!found)
        fatalError ("Bogus checkCfgChoices() tbp %p", tbp);

    // all rows require only one col per row
    for (int c = 0; c < N_CIDX; c++) {
        if (c != col) {
            TBControl &tb = ctbl.tb[row][c];
            if (tb.on) {
                tb.on = false;
                drawTBControl (ctbl, tb);
            }
        }
    }

    // USE_CIDX and UPD_CIDX require only one row per col
    if (col == USE_CIDX || col == UPD_CIDX) {
        for (int r = 0; r < n_names && r < MAX_CFG; r++) {
            if (r != row) {
                TBControl &tb = ctbl.tb[r][col];
                if (tb.on) {
                    tb.on = false;
                    drawTBControl (ctbl, tb);
                }
            }
        }
    }
}


/* toggle the given table control
 */
static void toggleTBControl (CfgTable &ctbl, int n_names, TBControl &tb)
{
    tb.on = !tb.on;
    drawTBControl (ctbl, tb);

    checkCfgChoices (ctbl, n_names, &tb);
}

/* convert the given config name to an actual full file name
 */
static void cfg2file (const char *cfg_name, char fn[], size_t fn_l)
{
    int n_pref = snprintf (fn, fn_l, "%s/%s/", our_dir.c_str(), cfg_dir);
    strncpySubChar (fn+n_pref, cfg_name, '_', ' ', fn_l - n_pref);
    n_pref += strlen(cfg_name);
    snprintf (fn+n_pref, fn_l-n_pref, "%s", cfg_suffix);
}

/* convert the given file name to a config name.
 * return whether file was properly converted.
 */
static bool file2cfg (const char *fn, char cfg[], size_t cfg_l)
{
    const char *suffix = strstr (fn, cfg_suffix);
    size_t name_len;
    if (suffix && (name_len = (suffix - fn)) < cfg_l) {
        strncpySubChar (cfg, fn, ' ', '_', name_len+1);
        return (true);
    }
    return (false);
}

/* pass back malloced array of all existing configuration names.
 * N.B. caller must always call freeCfgFiles()
 */
static int getCfgFiles (const char *dir, CfgName **names)
{
    DIR *dirp = opendir (dir);
    if (dirp == NULL)
        fatalError ("%s: %s", dir, strerror(errno));

    CfgName *&nm_list = *names;
    nm_list = NULL;
    struct dirent *dp;
    int n_names = 0;
    while ((dp = readdir(dirp)) != NULL) {
        nm_list = (CfgName *) realloc (nm_list, (n_names + 1) * sizeof(CfgName));
        if (nm_list == NULL)
            fatalError ("No memory for %d config files", n_names+1);
        char cfg_name[SAVE_MAXN];
        if (file2cfg (dp->d_name, cfg_name, SAVE_MAXN))
            nm_list[n_names++] = strdup (cfg_name);
    }

    closedir (dirp);

    qsort (nm_list, n_names, sizeof(char*), qsString);

    return (n_names);
}

/* free a list of CfgName
 */
static void freeCfgFiles (CfgName *names, int n_names)
{
    for (int i = 0; i < n_names; i++)
        free ((void*)names[i]);
    free ((void*)names);
}

/* copy the given config name as the new default eeprom
 */
static void engageCfgFile (const char *cfg_name)
{
    // open for reading
    char buf[2000];
    cfg2file (cfg_name, buf, sizeof(buf));
    FILE *from_fp = fopen (buf, "r");
    if (!from_fp)
        fatalError ("%s: %s", buf, strerror(errno));

    // overwrite existing
    const char *eeprom = EEPROM.getFilename();
    FILE *to_fp = fopen (eeprom, "w");
    if (!to_fp)
        fatalError ("%s: %s", eeprom, strerror(errno));

    // set real owner, not fatal if can't
    if (fchown (fileno(to_fp), getuid(), getgid()) < 0)
        Serial.printf ("CFG: chown(%s,%d,%d): %s\n", eeprom, getuid(), getgid(), strerror(errno));

    // copy
    size_t n_r;
    while ((n_r = fread (buf, 1, sizeof(buf), from_fp)) > 0) {
        if (fwrite (buf, 1, n_r, to_fp) != n_r)
            fatalError ("%s: %s\n", eeprom, strerror(errno));
    }

    // finished
    fclose (to_fp);
    fclose (from_fp);

    Serial.printf ("CFG: engage '%s'\n", cfg_name);
}

/* save the current configuration to a file based on the given name.
 */
static void saveCfgFile (const char *cfg_name)
{
    // create new
    char buf[2000];
    cfg2file (cfg_name, buf, sizeof(buf));
    FILE *to_fp = fopen (buf, "w");
    if (!to_fp)
        fatalError ("%s: %s", buf, strerror(errno));

    // set real owner, not fatal if can't
    if (fchown (fileno(to_fp), getuid(), getgid()) < 0)
        Serial.printf ("CFG: chown(%s,%d,%d): %s\n", buf, getuid(), getgid(), strerror(errno));

    // read existing
    const char *eeprom = EEPROM.getFilename();
    FILE *from_fp = fopen (eeprom, "r");
    if (!from_fp)
        fatalError ("%s: %s", eeprom, strerror(errno));

    // copy
    size_t n_r;
    while ((n_r = fread (buf, 1, sizeof(buf), from_fp)) > 0) {
        if (fwrite (buf, 1, n_r, to_fp) != n_r)
            fatalError ("%s: %s\n", eeprom, strerror(errno));
    }

    // finished
    fclose (to_fp);
    fclose (from_fp);

    Serial.printf ("CFG: save '%s'\n", cfg_name);
}

/* delete the config file based on the given name
 */
static void deleteCfgFile (const char *cfg_name)
{
    char buf[2000];
    cfg2file (cfg_name, buf, sizeof(buf));
    if (unlink (buf) < 0)
        fatalError ("unlink(%s): %s", buf, strerror(errno));
    Serial.printf ("CFG: delete '%s'\n", cfg_name);
}

/* draw, else erase, the save cursor
 */
static void drawCfgCursor (const CfgTable &ctbl, bool draw)
{
    // find x position by finding width of string shopped at cursor
    char copy[sizeof(ctbl.sav_name)];
    strcpy (copy, ctbl.sav_name);
    copy[ctbl.sav_cursor] = '\0';
    uint16_t cx = ctbl.sav_b.x + getTextWidth (copy);
    tft.drawLine (cx, ctbl.sav_cy, cx+SAVE_CW, ctbl.sav_cy, draw ? SAVE_CCOL : RA8875_BLACK);
}

/* given a tap and/or keyboard char known to be within the save box, update
 */
static void updateSaveText (const SCoord &s, const char kbc, CfgTable &ctbl)
{
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (TX_COL);

    if (inBox (s, ctbl.sav_b)) {
        // set cursor based on tap position, if within string
        uint16_t nw = getTextWidth (ctbl.sav_name);
        uint16_t sdx = s.x - ctbl.sav_b.x;
        if (sdx < nw) {
            char copy[sizeof(ctbl.sav_name)];
            strcpy (copy, ctbl.sav_name);
            (void) maxStringW (copy, sdx);
            drawCfgCursor (ctbl, false);
            ctbl.sav_cursor = strlen (copy);
        }
        drawCfgCursor (ctbl, true);

    } else if (kbc == CHAR_RIGHT) {
        if (ctbl.sav_cursor < strlen(ctbl.sav_name)) {
            drawCfgCursor (ctbl, false);
            ctbl.sav_cursor += 1;
            drawCfgCursor (ctbl, true);
        }

    } else if (kbc == CHAR_LEFT) {
        if (ctbl.sav_cursor > 0) {
            drawCfgCursor (ctbl, false);
            ctbl.sav_cursor -= 1;
            drawCfgCursor (ctbl, true);
        }

    } else if (kbc == CHAR_BS || kbc == CHAR_DEL) {
        if (ctbl.sav_cursor > 0) {
            drawCfgCursor (ctbl, false);
            memmove (ctbl.sav_name+ctbl.sav_cursor-1, ctbl.sav_name+ctbl.sav_cursor,
                                                            SAVE_MAXN-ctbl.sav_cursor+1);       // w/EOS
            fillSBox (ctbl.sav_b, RA8875_BLACK);
            tft.setCursor (ctbl.sav_b.x, ctbl.sav_b.y + FONT_DY);
            tft.print (ctbl.sav_name);
            ctbl.sav_cursor -= 1;
            drawCfgCursor (ctbl, true);
        }

    } else if (isprint (kbc)) {
        size_t sl = strlen(ctbl.sav_name);
        if (sl < SAVE_MAXN-1 && getTextWidth(ctbl.sav_name) < ctbl.sav_b.w - SAVE_CW - 15) { // for added char
            drawCfgCursor (ctbl, false);
            memmove (ctbl.sav_name+ctbl.sav_cursor+1, ctbl.sav_name+ctbl.sav_cursor,
                                                            SAVE_MAXN-ctbl.sav_cursor+1);       // w/EOS
            ctbl.sav_name[ctbl.sav_cursor] = kbc;
            fillSBox (ctbl.sav_b, RA8875_BLACK);
            tft.setCursor (ctbl.sav_b.x, ctbl.sav_b.y + FONT_DY);
            tft.print (ctbl.sav_name);
            ctbl.sav_cursor += 1;
            drawCfgCursor (ctbl, true);
        }
    }
}


/* offer user the means to save, delete or restore from a set of existing configuration files.
 * return whether they have elected to change the current configuration.
 */
static bool runConfigMenu (const CfgName *names, int n_names)
{
    // position the ok and cancel buttons
    SBox ok_b = {200, COK_Y, COK_W, COK_H};
    SBox cancel_b = {500, COK_Y, COK_W, COK_H};

    // state
    CfgTable ctbl;
    memset (&ctbl, 0, sizeof(ctbl));

    // position the save name input area
    ctbl.sav_tb = {{SAVE_TX, SAVE_Y-TB_SZ, TB_SZ, TB_SZ}, false};
    ctbl.sav_b = {SAVE_NX, SAVE_Y-FONT_DY, SAVE_NW, SAVE_NH};
    ctbl.sav_cy = SAVE_Y-FONT_DY+SAVE_NH;

    // position and init table
    for (int i = 0; i < MAX_CFG; i++) {
        ctbl.tb[i][0] = {{TBL_C1X, (uint16_t)(TBL_Y + i*ROW_DY), TB_SZ, TB_SZ}, false};
        ctbl.tb[i][1] = {{TBL_C2X, (uint16_t)(TBL_Y + i*ROW_DY), TB_SZ, TB_SZ}, false};
        ctbl.tb[i][2] = {{TBL_C3X, (uint16_t)(TBL_Y + i*ROW_DY), TB_SZ, TB_SZ}, false};
    }

    // draw title
    static const char title[] = "Manage Configurations";
    selectFontStyle (BOLD_FONT, SMALL_FONT);
    tft.setTextColor (TL_COL);
    tft.setCursor ((800-getTextWidth(title))/2, TITLE_Y);
    tft.print (title);

    // draw column headings
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (TL_COL);
    tft.setCursor (TBL_T1X, TBL_HY);
    tft.print ("Restore");
    tft.setCursor (TBL_T2X, TBL_HY);
    tft.print ("Update");
    tft.setCursor (TBL_T3X, TBL_HY);
    tft.print ("Delete");
    tft.setCursor (TBL_T4X, TBL_HY);
    tft.print ("Name");

    // add restart reminder below Restore
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setCursor (TBL_T1X+10, TBL_HY+4);
    tft.print ("(restarts)");

    // draw cancel and ok
    selectFontStyle (BOLD_FONT, SMALL_FONT);
    drawStringInBox ("Ok", ok_b, false, RA8875_WHITE);
    drawStringInBox ("Cancel", cancel_b, false, RA8875_WHITE);

    // draw save
    drawTBControl (ctbl, ctbl.sav_tb);
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (TL_COL);
    tft.setCursor (SAVE_LX, SAVE_Y);
    tft.print ("Save:");
    updateSaveText ({0,0}, CHAR_NONE, ctbl);

    // draw each defined table control and name
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (TX_COL);
    for (int r = 0; r < n_names && r < MAX_CFG; r++) {
        for (int c = 0; c < N_CIDX; c++)
            drawTBControl (ctbl, ctbl.tb[r][c]);
        tft.setCursor (TBL_C4X, TBL_Y + r*ROW_DY + TB_SZ - 1);
        tft.print (names[r]);
    }

    // reduce n_names to MAX_CFG but at least show some are not being shown
    if (n_names > MAX_CFG) {
        Serial.printf ("CFG: reducing n_names from %d to %d\n", n_names, MAX_CFG);
        selectFontStyle (LIGHT_FONT, FAST_FONT);
        tft.setCursor (TBL_C1X, TBL_Y + MAX_CFG*ROW_DY);
        tft.printf ("%d more not shown", n_names - MAX_CFG);
        n_names = MAX_CFG;
    }

    // prep run
    SBox screen_b = {0, 0, tft.width(), tft.height()};
    drawSBox (screen_b, GRAY);
    SCoord s;
    char kbc;
    UserInput ui = {
        screen_b,
        UI_UFuncNone, 
        UF_UNUSED,
        UI_NOTIMEOUT,
        UF_NOCLOCKS,
        s,
        kbc,
        false,
        false
    }; 
    
    // run
    bool doit = false;
    bool in_save_text = false;
    while (waitForUser(ui)) {

        // done when ESC or Cancel
        if (kbc == CHAR_ESC || inBox (s, cancel_b))
            break;

        // engage when CR or Ok but beware blank save
        if (kbc == CHAR_NL || kbc == CHAR_CR || inBox (s, ok_b)) {
            bool save_err = false;
            if (ctbl.sav_tb.on) {
                strtrim (ctbl.sav_name);
                if (strlen (ctbl.sav_name) == 0) {
                    tft.setCursor (ctbl.sav_b.x, ctbl.sav_b.y + FONT_DY);
                    tft.setTextColor (RA8875_RED);
                    tft.print ("Err");
                    wdDelay(2000);
                    fillSBox (ctbl.sav_b, RA8875_BLACK);
                    toggleTBControl (ctbl, n_names, ctbl.sav_tb);
                    save_err = true;
                }
            }
            if (!save_err) {
                doit = true;
                break;
            }
        }

        // steer input
        if (inBox (s, ctbl.sav_b)) {
            in_save_text = true;
        } else if (inBox (s, ctbl.sav_tb.box)) {
            toggleTBControl (ctbl, n_names, ctbl.sav_tb);
            in_save_text = false;
            updateSaveText ({0,0}, CHAR_NONE, ctbl);
        } else {
            bool done = false;
            for (int i = 0; !done && i < n_names && i < MAX_CFG; i++) {
                for (int j = 0; !done && j < N_CIDX; j++) {
                    TBControl &tb = ctbl.tb[i][j];
                    if (inBox (s, tb.box)) {
                        toggleTBControl (ctbl, n_names, tb);
                        in_save_text = false;
                        updateSaveText ({0,0}, CHAR_NONE, ctbl);
                        done = true;
                    }
                }
            }
        }

        if (in_save_text)
            updateSaveText (s, kbc, ctbl);
    }

    // engage all specified changes, noting if any require restarting
    bool restart = false;
    if (doit) {
        if (ctbl.sav_tb.on)
            saveCfgFile (ctbl.sav_name);                // copy eeprom to sav_name
        for (int r = 0; r < n_names && r < MAX_CFG; r++) {
            if (ctbl.tb[r][USE_CIDX].on) {
                engageCfgFile (names[r]);               // copy names[r] to eeprom
                restart = true;                         // restart to engage
            }
            if (ctbl.tb[r][UPD_CIDX].on)
                saveCfgFile (names[r]);                 // copy eeprom to names[r]
            if (ctbl.tb[r][DEL_CIDX].on)
                deleteCfgFile (names[r]);               // delete names[r]
        }
    }

    return (restart);
}

/* take over the entire screen to offer saving and restoring config files.
 */
void runConfigManagement(void)
{
    // all ours
    eraseScreen();

    // handy dir and insure it exists
    char dir[1000];
    snprintf (dir, sizeof(dir), "%s/%s", our_dir.c_str(), cfg_dir);
    if (mkdir (dir, 0755) == 0) {
        if (chown (dir, getuid(), getgid()) < 0)        // set real owner, not fatal
            Serial.printf ("CFG: chown(%d,%d): %s\n", getuid(), getgid(), strerror(errno));
    } else if (errno != EEXIST) {
        // ok if already exists
        fatalError ("%s: %s", dir, strerror(errno));
    }

    // gather collection of existing saved files
    CfgName *cfg_names;
    int n_cfgs = getCfgFiles (dir, &cfg_names);

    // run menu and restart if needed
    if (runConfigMenu (cfg_names, n_cfgs))
        doReboot(true);

    // no change, just clean up and resume
    freeCfgFiles (cfg_names, n_cfgs);
    initScreen();
}
