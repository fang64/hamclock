/* handle remote firmware updating
 */

#include <ESP8266httpUpdate.h>
#include <WiFiUdp.h>


#include "HamClock.h"

// server path to script that returns the newest version available
static const char v_page[] = "/version.pl";

// query layout
#define ASK_TO          60                              // ask timeout, secs
#define Q_Y             40                              // question y
#define C_Y             80                              // controls y
#define LH              30                              // line height
#define FD              7                               // font descent
#define LINDENT         10                              // list indent
#define INFO_Y          150                             // first list y
#define YNBOX_W         120                             // Y/N box width
#define YNBOX_H         40                              // Y/N box height
#define YNBOX_GAP       200                             // Y/N boxs gap
#define NBOX_X          50                              // no box x
#define NBOX_Y          C_Y                             // no box y
#define YBOX_X          (800-NBOX_X-YNBOX_W)            // yes box x
#define YBOX_Y          C_Y                             // yes box y
#define SCR_W           17                              // scroll arrow width
#define SCR_H           30                              // scroll arrow height
#define SCR_BGAP        5                               // scroll arrows box border gap
#define SCR_UPX         (800-SCR_BGAP-SCR_W-5)          // scroll up x
#define SCR_UPY         INFO_Y                          // scroll up top y
#define SCR_DWX         SCR_UPX                         // scroll down x
#define SCR_DWY         (480-SCR_H-10)                  // scroll down top y

// install layout
#define PROG_Y0         100                             // progress text y
#define PROG_DY         45                              // progress text line spacing
#define PBAR_INDENT     30                              // left and right progress bar indent
#define PBAR_Y0         200                             // progress bar top
#define PBAR_H          30                              // progress bar height
#define PBAR_W          (800-2*PBAR_INDENT)             // progress bar width

/* called by ESPhttpUpdate during download with bytes so far and total.
 */
static void onProgressCB (int sofar, int total)
{
    tft.drawRect (PBAR_INDENT, PBAR_Y0, PBAR_W, PBAR_H, RA8875_WHITE);
    tft.fillRect (PBAR_INDENT, PBAR_Y0, sofar*PBAR_W/total, PBAR_H, RA8875_WHITE);
    checkWebServer (true);
}



/* return whether a new version is available.
 * if so pass back the name in new_ver[new_verl]
 * default no if error.
 */
bool newVersionIsAvailable (char *new_ver, uint16_t new_verl)
{
    WiFiClient v_client;
    char line[100];
    bool found_newer = false;

    Serial.printf ("%s/%s\n", backend_host, v_page);
    if (wifiOk() && v_client.connect (backend_host, backend_port)) {
        resetWatchdog();

        // query page
        httpHCGET (v_client, backend_host, v_page);

        // skip header
        if (!httpSkipHeader (v_client)) {
            Serial.println ("Version query header is short");
            goto out;
        }

        // next line is new version number
        if (!getTCPLine (v_client, line, sizeof(line), NULL)) {
            Serial.println ("Version query timed out");
            goto out;
        }

        // non-rc accepts newer non-rc; rc accepts newer or any rc
        Serial.printf ("found version %s\n", line);
        float this_v = atof(hc_version);
        float new_v = atof(line);
        bool this_rc = strstr (hc_version, "rc");
        bool new_rc = strstr (line, "rc");
        // Serial.printf ("V %g >? %g\n", new_v, this_v);
        if ((!this_rc && !new_rc && new_v > this_v) || (this_rc && (new_rc || new_v >= this_v))) {
            found_newer = true;
            strncpy (new_ver, line, new_verl);
        }

        // just log next few lines for debug
        // for (int i = 0; i < 2 && getTCPLine (v_client, line, sizeof(line), NULL); i++)
            // Serial.printf ("  %s\n", line);
    }

out:

    // finished with connection
    v_client.stop();

    return (found_newer);
}

/* draw as many of the given lines starting with top_line as will fit
 */
static void drawChangeList (char **line, int top_line, int n_lines)
{
    uint16_t line_y = INFO_Y;

    // erase over to scroll bar
    tft.fillRect (0, line_y, SCR_UPX-SCR_BGAP-1, tft.height() - line_y, RA8875_BLACK);

    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    for (int i = top_line; i < n_lines && (line_y += LH) < tft.height() - FD; i++) {
        tft.setCursor (LINDENT, line_y);
        tft.print (line[i]);
    }
}


/* draw or erase an up or down arrow in the given box
 */
typedef enum {
    DA_UP,
    DA_DOWN
} DrawArrowDir;
typedef enum {
    DA_ON,
    DA_OFF
} DrawArrowOnOff;
static void drawArrow (const SBox &b, DrawArrowOnOff oo, DrawArrowDir ud)
{
    // stay within b
    uint16_t b_y = b.y + b.h - 1;
    uint16_t r_x = b.x + b.w - 1;
    uint16_t tip_x = b.x + b.w/2;

    if (oo == DA_ON) {
        if (ud == DA_UP)
            tft.fillTriangle (tip_x, b.y,   b.x, b_y,   r_x, b_y,    RA8875_WHITE);
        else
            tft.fillTriangle (tip_x, b_y,   b.x, b.y,   r_x, b.y,    RA8875_WHITE);
    } else {
        fillSBox (b, RA8875_BLACK);
        if (ud == DA_UP)
            tft.drawTriangle (tip_x, b.y,   b.x, b_y,   r_x, b_y,    RA8875_WHITE);
        else
            tft.drawTriangle (tip_x, b_y,   b.x, b.y,   r_x, b.y,    RA8875_WHITE);
    }
}

/* draw the scale trough
 */
static void drawTrough (int max_lines, int n_lines, int top_line)
{
    // erase trough the border and arrows
    uint16_t in_x = SCR_UPX-SCR_BGAP+2;
    uint16_t in_y = SCR_UPY+SCR_H+2;
    uint16_t in_w = SCR_W+2*SCR_BGAP-4;
    uint16_t in_h = SCR_DWY-2 - in_y;
    tft.fillRect (in_x, in_y, in_w, in_h, RA8875_BLACK);

    // draw thumb
    uint16_t tr_x = in_x;
    uint16_t tr_y = in_y + (int)top_line * in_h / n_lines;
    uint16_t tr_w = in_w;
    uint16_t tr_h = (int)max_lines * in_h / n_lines;
    tft.fillRect (tr_x, tr_y, tr_w, tr_h, RA8875_WHITE);
}

/* ask and return whether to install the given (presumably newer) version.
 * default no if trouble of no user response.
 */
bool askOTAupdate(char *new_ver)
{
    // prep
    eraseScreen();
    hideClocks();
    selectFontStyle (BOLD_FONT, SMALL_FONT);
    tft.setTextColor (RA8875_WHITE);
    char line[128];

    // ask whether to install
    tft.setCursor (LINDENT, Q_Y);
    snprintf (line, sizeof(line), "New version %s is available. Update now?  ... ", new_ver);
    tft.print (line);
    uint16_t count_x = tft.getCursorX();
    uint16_t count_y = tft.getCursorY();
    int count_s = ASK_TO;
    tft.print(count_s);

    // draw yes/no boxes
    SBox no_b =  {NBOX_X, NBOX_Y, YNBOX_W, YNBOX_H};
    SBox yes_b = {YBOX_X, YBOX_Y, YNBOX_W, YNBOX_H};
    bool active_yes = false;
    drawStringInBox ("No", no_b, !active_yes, RA8875_WHITE);
    drawStringInBox ("Yes", yes_b, active_yes, RA8875_WHITE);

    // prep for potentially long wait
    closeGimbal();

    // read list of changes
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    WiFiClient v_client;
    char **lines = NULL;                        // malloced list of malloced strings -- N.B. free!
    int n_lines = 0;
    if (wifiOk() && v_client.connect (backend_host, backend_port)) {
        resetWatchdog();

        // query page
        httpHCGET (v_client, backend_host, v_page);

        // skip header
        if (!httpSkipHeader (v_client)) {
            Serial.println ("Info header is short");
            goto out;
        }

        // skip next line which is new version number
        if (!getTCPLine (v_client, line, sizeof(line), NULL)) {
            Serial.println ("Info timed out");
            goto out;
        }

        // remaining lines are changes, add to lines[]
        while (getTCPLine (v_client, line, sizeof(line), NULL)) {
            maxStringW (line, SCR_UPX-SCR_BGAP-LINDENT-1);       // insure fit
            lines = (char **) realloc (lines, (n_lines+1) * sizeof(char*));
            lines[n_lines++] = strdup (line);
        }
    }
  out:
    v_client.stop();

    // how many will fit
    const int max_lines = (tft.height() - FD - INFO_Y)/LH;

    // prep first display of changes
    int top_line = 0;
    drawChangeList (lines, top_line, n_lines);

    // scrolling tests
    #define NEED_SCR    (n_lines > max_lines)
    #define MORE_UP     (NEED_SCR && top_line > 0)
    #define MORE_DOWN   (NEED_SCR && top_line < n_lines - max_lines)

    // add scroll controls if needed

    SBox sup_b;
    sup_b.x = SCR_UPX;
    sup_b.y = SCR_UPY;
    sup_b.w = SCR_W;
    sup_b.h = SCR_H;
    if (NEED_SCR)
        drawArrow (sup_b, MORE_UP ? DA_ON : DA_OFF, DA_UP);

    SBox sdw_b;
    sdw_b.x = SCR_DWX;
    sdw_b.y = SCR_DWY;
    sdw_b.w = SCR_W;
    sdw_b.h = SCR_H;
    if (NEED_SCR)
        drawArrow (sdw_b, MORE_DOWN ? DA_ON : DA_OFF, DA_DOWN);

    // draw arrows border if needed
    if (NEED_SCR) {
        uint16_t brd_x = SCR_UPX-SCR_BGAP;
        uint16_t brd_y = SCR_UPY-SCR_BGAP;
        uint16_t brd_w = SCR_W+2*SCR_BGAP;
        uint16_t brd_h = SCR_DWY+SCR_H+SCR_BGAP - brd_y;
        tft.drawRect (brd_x, brd_y, brd_w, brd_h, RA8875_WHITE);
    }

    // draw scale if needed
    if (NEED_SCR)
        drawTrough (max_lines, n_lines, top_line);

    // wait for response or time out
    drainTouch();
    uint32_t t0 = millis();
    Serial.println ("Waiting for update y/n ...");
    bool finished = false;
    bool result = false;
    while (!finished && count_s > 0) {

        // check for scroll
        int prev_topl = top_line;

        // update countdown
        wdDelay(10);
        if (timesUp(&t0,1000)) {
            selectFontStyle (BOLD_FONT, SMALL_FONT);
            tft.fillRect (count_x, count_y-30, 60, 40, RA8875_BLACK);
            tft.setCursor (count_x, count_y);
            tft.print(--count_s);
        }

        // handle typed chars
        switch (tft.getChar(NULL,NULL)) {
        case CHAR_TAB:
        case CHAR_LEFT:
        case CHAR_RIGHT:
            active_yes = !active_yes;
            drawStringInBox ("Yes", yes_b, active_yes, RA8875_WHITE);
            drawStringInBox ("No", no_b, !active_yes, RA8875_WHITE);
            count_s = ASK_TO;
            break;
        case CHAR_ESC:
            finished = true;
            result = false;
            break;
        case CHAR_CR:
        case CHAR_NL:
            finished = true;
            result = active_yes;
            break;
        case CHAR_UP:
            if (MORE_UP)
                drawChangeList (lines, --top_line, n_lines);
            count_s = ASK_TO;
            break;
        case CHAR_DOWN:
            if (MORE_DOWN)
                drawChangeList (lines, ++top_line, n_lines);
            count_s = ASK_TO;
            break;
        }

        // handle clicks
        SCoord s;
        if (readCalTouchWS(s) != TT_NONE) {
            if (inBox (s, yes_b)) {
                drawStringInBox ("Yes", yes_b, true, RA8875_WHITE);
                finished = true;
                result = true;
            }
            if (inBox (s, no_b)) {
                drawStringInBox ("No", no_b, false, RA8875_WHITE);
                finished = true;
                result = false;
            }
            if (inBox (s, sup_b)) {
                if (MORE_UP)
                    drawChangeList (lines, --top_line, n_lines);
                count_s = ASK_TO;
            }
            if (inBox (s, sdw_b)) {
                if (MORE_DOWN)
                    drawChangeList (lines, ++top_line, n_lines);
                count_s = ASK_TO;
            }
        }

        // manage scroll arrows
        if (prev_topl != top_line && NEED_SCR) {
            drawArrow (sup_b, MORE_UP ? DA_ON : DA_OFF, DA_UP);
            drawArrow (sdw_b, MORE_DOWN ? DA_ON : DA_OFF, DA_DOWN);
            drawTrough (max_lines, n_lines, top_line);
        }
    }

    // clean up
    while (--n_lines >= 0)
        free (lines[n_lines]);
    free (lines);

    // return result
    return (result);
}

/* reload HamClock with the given version.
 * we never return regardless of success or fail.
 */
void doOTAupdate(const char *newver)
{
    Serial.println ("Begin download");

    // inform user
    eraseScreen();
    selectFontStyle (BOLD_FONT, SMALL_FONT);
    tft.setTextColor (RA8875_WHITE);
    tft.setCursor (0, PROG_Y0);
    tft.printf ("  Performing remote update to V%s...", newver);
    tft.setCursor (0, PROG_Y0 + PROG_DY);
    tft.print ("  Do not interrupt power or network during this process.");

    // connect progress callback
    ESPhttpUpdate.onProgress (onProgressCB);

    // build url
    resetWatchdog();
    WiFiClient client;
    char url[200];
    if (strstr(hc_version, "rc") && strstr (newver, "rc"))
        snprintf (url, sizeof(url), "http://%s/ham/HamClock/ESPHamClock-V%s.zip", backend_host, newver);
    else
        snprintf (url, sizeof(url), "https://%s/ham/HamClock/ESPHamClock.zip", backend_host);

    // go
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);
    resetWatchdog();

    // show error message and exit
    switch (ret) {
    case HTTP_UPDATE_FAILED:
        fatalError ("Update failed: Error %d\n%s\n",
                        ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
        break;

    case HTTP_UPDATE_NO_UPDATES:
        fatalError ("No updates found");
        break;

    case HTTP_UPDATE_OK:
        fatalError ("Update Ok??");
        break;

    default:
        fatalError ("Unknown failure code: ");
        tft.println (ret);
        break;
    }
}
