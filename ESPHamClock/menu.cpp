/* generic modal dialog
 */

#include "HamClock.h"

/* MENU_TEXT editing is a bear. These sketches help:
 *
 *   w_len = 8
 *                          l  c  w  action                                 l  c  w
 *     -----------------                               -----------------
 *   |a|b|c|d|e|f|g|h| |    8  8  1  CHAR_LEFT         |a|b|c|d|e|f|g|h|    8  7  0
 *     -----------------                               -----------------
 *                    _                                               _
 *
 *
 *     -----------------                               -----------------
 *   |a|b|c|d|e|f|g|h| |    8  8  1  CHAR_DEL          |a|b|c|d|e|f|g| |    7  7  0
 *     -----------------                               -----------------
 *                    _                                               _
 *  
 *     -----------------                               -----------------
 *   |a|b|c|d|e|f|g|h| |    8  8  1  type x        |a|b|c|d|e|f|g|h|x| |  9  9  2
 *     -----------------                               -----------------
 *                    _                                               _
 *  
 *     -----------------                               -----------------
 *   |a|b|c|d|e|f|g|h| |    8  7  1  type y          |a|b|c|d|e|f|g|y|h|  9  8  2
 *     -----------------                               -----------------
 *                  _                                                 _
 *
 */


// #define _SHOW_BB                      // RBF
#if defined(_SHOW_BB)
#warning _SHOW_BB is set
#endif 

// basic parameters
// allow setting some/all these in Menu?
#define MENU_TBM        2               // top and bottom margin
#define MENU_RM         2               // right margin
#define MENU_RH         11              // row height, includes room for MENU_TEXT cursor
#define MENU_IS         6               // indicator size
#define MENU_BB         5               // ok/cancel button horizontal border
#define MENU_BDX        2               // ok/cancel button text horizontal offset
#define MENU_BDY        2               // ok/cancel button text vertical offset
#define MENU_FW         6               // MENU_TEXT font width
#define MENU_CD         2               // cursor rows below text baseline
#define MENU_TIMEOUT    MENU_TO         // timeout, millis
#define MENU_FGC        RA8875_WHITE    // normal foreground color
#define MENU_BGC        RA8875_BLACK    // normal background color
#define MENU_ERRC       RA8875_RED      // error color
#define MENU_BSYC       RA8875_YELLOW   // busy color
#define MENU_FOCC       RA8875_GREEN    // focus color

static const char ok_label[] = "Ok";
static const char cancel_label[] = "Cancel";

/* draw text and optionally the label and/or cursor for the given MenuItem in the given box.
 * N.B. mi is assumed to be of type MENU_TEXT.
 */
static void drawMenuText (const MenuItem &mi, const SBox &pb, bool draw_label, bool draw_cursor)
{
    // require
    if (mi.type != MENU_TEXT)
        fatalError ("drawMenuText not %d: %d", (int)MENU_TEXT, (int)mi.type);

    // handy
    MenuText *tfp = mi.textf;

    // beware clock can change these!
    selectFontStyle (LIGHT_FONT, FAST_FONT);

    // start of visible text
    uint16_t text_x = pb.x + mi.indent + MENU_FW*(tfp->l_mem-1);        // not EOS

    // draw fresh label if desired
    if (draw_label) {
        tft.fillRect (pb.x, pb.y, text_x - pb.x, pb.h, MENU_BGC);

        #if defined(_SHOW_BB)
            tft.drawRect (pb.x, pb.y, text_x - pb.x, pb.h, RA8875_GREEN);
        #endif // _SHOW_BB

        tft.setCursor (pb.x + mi.indent, pb.y+2);
        tft.print (tfp->label);
    }

    // erase all text, including cursor
    tft.fillRect (text_x, pb.y, pb.w - (text_x - pb.x), pb.h, MENU_BGC);

    #if defined(_SHOW_BB)
        drawSBox (pb, RA8875_GREEN);
        tft.drawRect (text_x, pb.y, pb.w - (text_x - pb.x), pb.h, RA8875_RED);
    #endif // _SHOW_BB

    // check left end
    size_t t_len = strlen(tfp->text);
    if (tfp->c_pos > t_len)
        tfp->c_pos = t_len;
    if (tfp->c_pos < tfp->w_pos)
        tfp->w_pos = tfp->c_pos;

    // check right end: insure cursor is both within the string and within the text window
    if (tfp->c_pos > t_len)
        tfp->c_pos = t_len;
    if (tfp->c_pos > tfp->w_pos + tfp->w_len - 1)
        tfp->w_pos = tfp->c_pos - tfp->w_len + 1;

    // print starting at w_pos
    tft.setCursor (text_x, pb.y+2);
    tft.printf ("%.*s", tfp->w_len, tfp->text + tfp->w_pos);

    // draw cursor if desired (already erased above)
    if (draw_cursor) {
        uint16_t c_x = text_x + MENU_FW*(tfp->c_pos-tfp->w_pos);
        uint16_t c_y = pb.y + pb.h - MENU_CD;
        tft.drawLine (c_x, c_y, c_x + MENU_FW, c_y, MENU_FOCC);
    }
}

/* briefly show the given message over the given MENU_TEXT
 */
static void drawMenuTextMsg (const MenuItem &mi, const SBox &pb, const char *msg)
{
    // overlay the given msg
    uint16_t text_x = pb.x + mi.indent + MENU_FW*(mi.textf->l_mem-1);        // not EOS
    tft.fillRect (text_x, pb.y, pb.w - (text_x - pb.x), pb.h, MENU_BGC);
    tft.setCursor (text_x, pb.y+2);
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor(MENU_ERRC);
    tft.print (msg);

    // delay then restore
    wdDelay (3000);
    tft.setTextColor (MENU_FGC);
    drawMenuText (mi, pb, false, true);

    // dont keep doing this
    drainTouch();
}

/* draw selector symbol and label for the given menu item in the given pick box.
 * kb_focus indicates this item has the keyboard focus.
 */
static void menuDrawItem (const MenuItem &mi, const SBox &pb, bool draw_label, bool kb_focus)
{
    // N.B. updateClocks can change font!
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor (MENU_FGC);

    // prepare a copy of the label without underscores if drawing
    char *no__copy = NULL;
    if (draw_label && mi.label) {       // label will be NULL for IGNORE and BLANK
        no__copy = strdup (mi.label);
        strncpySubChar (no__copy, mi.label, ' ', '_', strlen(mi.label));
    }

    // draw depending on type

    switch (mi.type) {

    case MENU_BLANK:    // fallthru
    case MENU_IGNORE:
        break;

    case MENU_LABEL:
        if (draw_label) {
            tft.setCursor (pb.x + mi.indent, pb.y+2);
            tft.print (no__copy);
        }
        drawSBox (pb, kb_focus ? MENU_FOCC : MENU_BGC);
        break;

    case MENU_01OFN:    // fallthru
    case MENU_1OFN:
        if (mi.set)
            tft.fillCircle (pb.x + mi.indent + MENU_IS/2, pb.y + MENU_RH/2, MENU_IS/2, MENU_FGC);
        else {
            tft.fillCircle (pb.x + mi.indent + MENU_IS/2, pb.y + MENU_RH/2, MENU_IS/2, MENU_BGC);
            tft.drawCircle (pb.x + mi.indent + MENU_IS/2, pb.y + MENU_RH/2, MENU_IS/2, MENU_FGC);
        }
        if (draw_label) {
            tft.setCursor (pb.x + mi.indent + MENU_IS + MENU_IS/2, pb.y+2);
            tft.print (no__copy);
        }
        drawSBox (pb, kb_focus ? MENU_FOCC : MENU_BGC);
        break;

    case MENU_AL1OFN:   // fallthru
    case MENU_TOGGLE:
        if (mi.set)
            tft.fillRect (pb.x + mi.indent, pb.y + (MENU_RH-MENU_IS)/2, MENU_IS, MENU_IS, MENU_FGC);
        else {
            tft.fillRect (pb.x + mi.indent, pb.y + (MENU_RH-MENU_IS)/2, MENU_IS, MENU_IS, MENU_BGC);
            tft.drawRect (pb.x + mi.indent, pb.y + (MENU_RH-MENU_IS)/2, MENU_IS, MENU_IS, MENU_FGC);
        }
        if (draw_label) {
            tft.setCursor (pb.x + mi.indent + MENU_IS + MENU_IS/2, pb.y+2);
            tft.print (no__copy);
        }
        drawSBox (pb, kb_focus ? MENU_FOCC : MENU_BGC);
        break;

    case MENU_TEXT:
        // this type is complex enough deserves its own drawing code
        drawMenuText (mi, pb, draw_label, kb_focus);
        break;
    }

    // clean up
    free ((void*)no__copy);

    #if defined(_SHOW_BB)
        drawSBox (pb, RA8875_RED);
    #endif

    // draw now if menu is over map_b
    if (!draw_label && boxesOverlap (pb, map_b))
        tft.drawPR();
}

/* count how many items in the same group and type as ii are set
 */
static int menuCountItemsSet (MenuInfo &menu, int ii)
{
    MenuItem &menu_ii = menu.items[ii];
    int n_set = 0;

    for (int i = 0; i < menu.n_items; i++) {
        if (menu.items[i].type == MENU_IGNORE)
            continue;
        if (menu.items[i].type != menu_ii.type)
            continue;
        if (menu.items[i].group != menu_ii.group)
            continue;
        if (menu.items[i].set)
            n_set++;
    }

    return (n_set);
}

/* turn off MENU_TEXT and all items that are in the same group and type as item ii.
 */
static void menuItemsAllOff (MenuInfo &menu, SBox *pick_boxes, int ii)
{
    MenuItem &menu_ii = menu.items[ii];

    for (int i = 0; i < menu.n_items; i++) {
        MenuItem &mi = menu.items[i];
        if (mi.type != MENU_IGNORE && mi.set
                && (mi.type == MENU_TEXT || (mi.type == menu_ii.type && mi.group == menu_ii.group))) {
            mi.set = false;
            menuDrawItem (mi, pick_boxes[i], false, false);
        }
    }
}



/* engage an action at the specified pick index.
 * kb_focus indicates whether to highlight the new focus item for keyboard navigation.
 */
static void updateMenu (MenuInfo &menu, SBox *pick_boxes, int pick_i, bool kb_focus)
{
    SBox &pb = pick_boxes[pick_i];
    MenuItem &mi = menu.items[pick_i];

    switch (mi.type) {
    case MENU_LABEL:        // fallthru
    case MENU_BLANK:        // fallthru
    case MENU_IGNORE:
        break;

    case MENU_1OFN:
        if (!mi.set) {
            menuItemsAllOff (menu, pick_boxes, pick_i);
            mi.set = true;
            menuDrawItem (mi, pb, false, kb_focus);
        }
        break;

    case MENU_01OFN:
        // turn off if set, else turn this one on and all others in this group off
        if (mi.set) {
            mi.set = false;
            menuDrawItem (mi, pb, false, kb_focus);
        } else {
            menuItemsAllOff (menu, pick_boxes, pick_i);
            mi.set = true;
            menuDrawItem (mi, pb, false, kb_focus);
        }
        break;

    case MENU_AL1OFN:
        // turn on unconditionally, but turn off only if not the last one
        if (!mi.set) {
            mi.set = true;
            menuDrawItem (mi, pb, false, kb_focus);
        } else {
            if (menuCountItemsSet (menu, pick_i) > 1) {
                mi.set = false;
                menuDrawItem (mi, pb, false, kb_focus);
            }
        }
        break;

    case MENU_TOGGLE:
        // uncondition change
        mi.set = !mi.set;
        menuDrawItem (mi, pb, false, kb_focus);
        break;

    case MENU_TEXT:
        menuDrawItem (mi, pb, true, kb_focus);
        break;
    }
}


/* move to a different field depending on arrow keyin kb_char.
 * m_index is index of current menu item, return new item.
 */
static int kbNavigation (MenuInfo &menu, SBox *pick_boxes, int m_index, const char kb_char)
{

    // prep search state
    SBox &pm = pick_boxes[m_index];
    int mind = 100000;
    int candidate = -1;

    // search in desired direction
    switch (kb_char) {

    case CHAR_LEFT:
        // next field left closest in y, if any
        for (int i = 1; i <= menu.n_items; i++) {
            int ii = (m_index + i) % menu.n_items;
            MenuItem &mii = menu.items[ii];
            SBox &pii = pick_boxes[ii];
            int d = abs ((int)pii.x - (int)pm.x) + abs ((int)pii.y - (int)pm.y);
            if (MENU_ACTIVE(mii.type) && pii.x < pm.x && d < mind) {
                candidate = ii;
                mind = d;
            }
        }
        if (candidate >= 0) {
            // erase current, draw new
            menuDrawItem (menu.items[m_index], pick_boxes[m_index], false, false);
            menuDrawItem (menu.items[candidate], pick_boxes[candidate], false, true);
            m_index = candidate;
        }
        break;

    case CHAR_DOWN:
        // next field down closest to x, if any
        for (int i = 1; i <= menu.n_items; i++) {
            int ii = (m_index + i) % menu.n_items;
            MenuItem &mii = menu.items[ii];
            SBox &pii = pick_boxes[ii];
            int d = abs ((int)pii.x - (int)pm.x) + abs ((int)pii.y - (int)pm.y);
            if (MENU_ACTIVE(mii.type) && pii.y > pm.y && d < mind) {
                candidate = ii;
                mind = d;
            }
        }
        if (candidate >= 0) {
            // erase current, draw new
            menuDrawItem (menu.items[m_index], pick_boxes[m_index], false, false);
            menuDrawItem (menu.items[candidate], pick_boxes[candidate], false, true);
            m_index = candidate;
        }
        break;

    case CHAR_UP:
        // next field up closest to x, if any
        for (int i = 1; i <= menu.n_items; i++) {
            int ii = (m_index + i) % menu.n_items;
            MenuItem &mii = menu.items[ii];
            SBox &pii = pick_boxes[ii];
            uint16_t d = abs ((int)pii.x - (int)pm.x) + abs ((int)pii.y - (int)pm.y);
            if (MENU_ACTIVE(mii.type) && pii.y < pm.y && d < mind) {
                candidate = ii;
                mind = d;
            }
        }
        if (candidate >= 0) {
            // erase current, draw new
            menuDrawItem (menu.items[m_index], pick_boxes[m_index], false, false);
            menuDrawItem (menu.items[candidate], pick_boxes[candidate], false, true);
            m_index = candidate;
        }
        break;

    case CHAR_RIGHT:
        // next field right closest in y, if any
        for (int i = 1; i <= menu.n_items; i++) {
            int ii = (m_index + i) % menu.n_items;
            MenuItem &mii = menu.items[ii];
            SBox &pii = pick_boxes[ii];
            int d = abs ((int)pii.x - (int)pm.x) + abs ((int)pii.y - (int)pm.y);
            if (MENU_ACTIVE(mii.type) && pii.x > pm.x && d < mind) {
                candidate = ii;
                mind = d;
            }
        }
        if (candidate >= 0) {
            // erase current, draw new
            menuDrawItem (menu.items[m_index], pick_boxes[m_index], false, false);
            menuDrawItem (menu.items[candidate], pick_boxes[candidate], false, true);
            m_index = candidate;
        }
        break;

    case CHAR_SPACE:
        // activate this location
        updateMenu (menu, pick_boxes, m_index, true);
        break;

    default:
        break;
    }

    return (m_index);
}

/* given a MENU_TEXT text field index textf_i perform the editing actions of the given character.
 * if char is up or down arrow, defer to kbNavigation and return its index, else return the same index.
 */
static int textFieldEdit (MenuInfo &menu, SBox *pick_boxes, int textf_i, const char kb_char)
{
    // handy
    MenuItem &mi = menu.items[textf_i];
    if (mi.type != MENU_TEXT)
        fatalError ("textFieldEdit called with type %d", (int)mi.type);
    SBox &pb = pick_boxes[textf_i];
    MenuText *tfp = mi.textf;
    size_t t_len = strlen (tfp->text);

    bool debug = false;                                 // RBD
    if (debug)
        printf ("TFE l= %2d c= %2d w= %2d %2d='%-*s' ... ",  tfp->w_len, tfp->c_pos, tfp->w_pos,
                                (int)t_len, (int)tfp->t_mem, tfp->text);

    switch (kb_char) {
    case CHAR_UP:
    case CHAR_DOWN:
        return (kbNavigation (menu, pick_boxes, textf_i, kb_char));

    case CHAR_LEFT:
        if (debug)
            printf (" LEFT ");
        if (tfp->c_pos > 0) {
            tfp->c_pos -= 1;
            menuDrawItem (mi, pb, false, true);
        }
        break;

    case CHAR_RIGHT:
        if (debug)
            printf (" RIGT ");
        tfp->c_pos += 1;
        menuDrawItem (mi, pb, false, true);
        break;

    case CHAR_BS:
    case CHAR_DEL:
        if (debug)
            printf (" DEL  ");
        // delete char left of cursor, if any
        if (tfp->c_pos > 0) {
            memmove (&tfp->text[tfp->c_pos-1], &tfp->text[tfp->c_pos], t_len - tfp->c_pos + 1);  // w/EOS
            tfp->c_pos -= 1;
            menuDrawItem (mi, pb, false, true);
        }
        break;

    default:
        // insert kb_char at cursor pos if printable and more room left
        if (isprint(kb_char)) {
            if (t_len < tfp->t_mem-1) {
                if (debug)
                    printf (" IN %c ", kb_char);
                memmove (&tfp->text[tfp->c_pos+1], &tfp->text[tfp->c_pos], t_len - tfp->c_pos + 1); // w/EOS
                tfp->text[tfp->c_pos] = tfp->to_upper ? toupper(kb_char) : kb_char;
                tfp->c_pos += 1;
                menuDrawItem (mi, pb, false, true);
            } else {
                if (debug)
                    printf (" FULL ");
                drawMenuTextMsg (mi, pb, "full");
            }
        }
        break;
    }

    // final check
    t_len = strlen (tfp->text);
    if (t_len > tfp->t_mem - 1)
        fatalError ("textFieldEdit t_len %d > t_mem-1 %d", (int)t_len, (int)tfp->t_mem-1);

    if (debug)
        printf ("  %2d %2d %2d='%-*s'\n", tfp->c_pos, tfp->w_pos, (int)strlen(tfp->text), 
                                (int)tfp->t_mem, tfp->text);

    return (textf_i);
}

/* update menu from the given tap.
 * m_index is index of current menu item, return new item or same if tap not in any item.
 */
static int tapNavigation (MenuInfo &menu, SBox *pick_boxes, int m_index, const SCoord &tap)
{
    for (int i = 0; i < menu.n_items; i++) {

        MenuItem &mi = menu.items[i];
        SBox &pb = pick_boxes[i];

        if (mi.type != MENU_IGNORE && inBox (tap, pb)) {

            if (mi.type == MENU_TEXT) {

                // tap could be in either the label or text areas
                MenuText *tfp = mi.textf;

                // get start of text area -- N.B. must match menuDrawItem() position
                uint16_t text_x = pb.x + mi.indent + MENU_FW*(tfp->l_mem-1);    // not EOS


                if (tap.x < text_x) {
                    // call label updater, if any
                    if (tfp->label_fp)
                        (*tfp->label_fp) (tfp);
                } else {
                    // set cursor at tap location
                    unsigned c_pos = tfp->w_pos + (tap.x - text_x)/MENU_FW;
                    if (c_pos < tfp->w_pos)
                        c_pos = tfp->w_pos;
                    if (c_pos > strlen(tfp->text))
                        c_pos = strlen(tfp->text);
                    if (tfp->w_pos - c_pos >= tfp->w_len)
                    if (c_pos - tfp->w_pos > tfp->w_len)
                        c_pos = tfp->w_len + tfp->w_pos;
                    tfp->c_pos = c_pos;
                }
            }

            // erase current location
            menuDrawItem (menu.items[m_index], pick_boxes[m_index], false, false);

            // implement each type of behavior
            updateMenu (menu, pick_boxes, i, mi.type == MENU_TEXT);

            // tap found
            return (i);
        }
    }

    return (m_index);
}

/* operate the given menu until ok, cancel or timeout.
 * caller passes a box we use for ok so they can use it later with menuRedrawOk if needed.
 * return true if op clicked ok or CR/NL else false for all other cases.
 * N.B. menu.menu_b.x/y are required but we may adjust to prevent edge spill.
 * N.B. incoming menu.menu_b.w is only grown to fit; thus calling with 0 will shrink wrap.
 * N.B. incoming menu.menu_b.h is ignored, we always shrink wrap h.
 * N.B. menu box is erased before returning.
 * N.B. all MENU_TEXT, if any, must be last
 */
bool runMenu (MenuInfo &menu)
{
    // font
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor (MENU_FGC);

    // find number of non-ignore items and find widest item
    int n_table = 0;                                    // n menu.items not including MENU_TEXT
    int n_mt = 0;                                       // n MENU_TEXT
    int widest = 0;                                     // widest item, not including MENU_TEXT
    int widest_mt = 0;                                  // widest MENU_TEXT item
    for (int i = 0; i < menu.n_items; i++) {
        MenuItem &mi = menu.items[i];
        if (mi.type == MENU_IGNORE)
            continue;
        if (mi.type == MENU_TEXT) {
            if (i < menu.n_items-1) {
                MenuFieldType next_mft = menu.items[i+1].type;
                if (next_mft != MENU_TEXT && next_mft != MENU_IGNORE)
                fatalError ("all MENU_TEXT must be last");
            }
            MenuText *mtp = mi.textf;
            int w = mi.indent + MENU_FW * (mtp->l_mem-1 + mtp->w_len) + MENU_RM;
            if (w > widest_mt)
                widest_mt = w;
            n_mt++;
        } else {
            int w = mi.label ? getTextWidth(mi.label) + mi.indent + MENU_IS + MENU_IS/2 : 0;
            if (w > widest)
                widest = w;
            n_table++;                                  // MENU_TEXT is not part of table
        }
    }

    // number of rows in each table column
    int n_tblrows = (n_table + menu.n_cols - 1)/menu.n_cols;

    // set menu height, +1 for each MENU_TEXT, +1 for ok/cancel
    menu.menu_b.h = MENU_TBM + (n_tblrows + n_mt + 1)*MENU_RH + MENU_TBM;

    // set ok button size, don't know position yet
    menu.ok_b.w = getTextWidth (ok_label) + MENU_BDX*2;
    menu.ok_b.h = MENU_RH;

    // create cancel button, set size but don't know position yet
    SBox cancel_b;
    cancel_b.w = getTextWidth (cancel_label) + MENU_BDX*2;
    cancel_b.h = MENU_RH;

    // width is duplicated for each column plus add a bit of right margin
    if (menu.menu_b.w < widest * menu.n_cols + MENU_RM)
        menu.menu_b.w = widest * menu.n_cols + MENU_RM;

    // insure menu width accommodates ok and/or cancel buttons
    if (menu.cancel == M_NOCANCEL) {
        if (menu.menu_b.w < MENU_BB + menu.ok_b.w + MENU_BB)
            menu.menu_b.w = MENU_BB + menu.ok_b.w + MENU_BB;
    } else {
        if (menu.menu_b.w < MENU_BB + menu.ok_b.w + MENU_BB + cancel_b.w + MENU_BB)
            menu.menu_b.w = MENU_BB + menu.ok_b.w + MENU_BB + cancel_b.w + MENU_BB;
    }

    // insure menu width accommodates all MENU_TEXT, if any
    if (menu.menu_b.w < widest_mt + 2)
        menu.menu_b.w = widest_mt + 2;


    // now we know the size of menu_b


    // reposition box if needed to avoid spillage
    if (menu.menu_b.x + menu.menu_b.w >= tft.width())
        menu.menu_b.x = tft.width() - menu.menu_b.w - 1;
    if (menu.menu_b.y + menu.menu_b.h >= tft.height())
        menu.menu_b.y = tft.height() - menu.menu_b.h - 1;

    // now we can set ok and cancel button positions within the menu box
    if (menu.cancel == M_NOCANCEL) {
        menu.ok_b.x = menu.menu_b.x + (menu.menu_b.w - menu.ok_b.w)/2;  // center
        menu.ok_b.y = menu.menu_b.y + menu.menu_b.h - MENU_TBM - menu.ok_b.h;
        cancel_b.x = 0;
        cancel_b.y = 0;
    } else {
        menu.ok_b.x = menu.menu_b.x + MENU_BB;
        menu.ok_b.y = menu.menu_b.y + menu.menu_b.h - MENU_TBM - menu.ok_b.h;
        cancel_b.x = menu.menu_b.x + menu.menu_b.w - cancel_b.w - MENU_BB;
        cancel_b.y = menu.menu_b.y + menu.menu_b.h - MENU_TBM - cancel_b.h;
    }

    // can now prepare new menu box
    fillSBox (menu.menu_b, MENU_BGC);
    drawSBox (menu.menu_b, MENU_FGC);

    // display ok/cancel buttons
    fillSBox (menu.ok_b, MENU_BGC);
    drawSBox (menu.ok_b, MENU_FGC);
    tft.setCursor (menu.ok_b.x+MENU_BDX, menu.ok_b.y+MENU_BDY);
    tft.print (ok_label);
    if (menu.cancel == M_CANCELOK) {
        fillSBox (cancel_b, MENU_BGC);
        drawSBox (cancel_b, MENU_FGC);
        tft.setCursor (cancel_b.x+MENU_BDX, cancel_b.y+MENU_BDY);
        tft.print (cancel_label);
    }

    // display each table item in its own pick box, all MENU_TEXT will be last and not within table proper
    StackMalloc pbox_mem(menu.n_items*sizeof(SBox));
    SBox *pick_boxes = (SBox *) pbox_mem.getMem();
    uint16_t col_w = (menu.menu_b.w - MENU_RM)/menu.n_cols;
    int tblcell_i = 0;                  // table cell, only incremented for non-IGNORE/MENU_TEXT items
    int mt_i = 0;                       // separate index for MENU_TEXTs
    for (int i = 0; i < menu.n_items; i++) {

        MenuItem &mi = menu.items[i];
        SBox &pb = pick_boxes[i];

        // assign item next location and draw unless to be ignored
        if (mi.type == MENU_TEXT) {
            // N.B. this assumes all MENU_TEXT are below the table
            MenuText *mtp = mi.textf;
            pb.x = menu.menu_b.x + 1;
            pb.y = menu.menu_b.y + MENU_TBM + (n_tblrows + mt_i)*MENU_RH;
            pb.w = mi.indent + MENU_FW * (mtp->l_mem-1 + mtp->w_len) + MENU_RM;
            pb.h = MENU_RH;
            menuDrawItem (mi, pb, true, false);
            mt_i++;
        } else if (mi.type != MENU_IGNORE) {
            pb.x = menu.menu_b.x + 1 + (tblcell_i/n_tblrows)*col_w;
            pb.y = menu.menu_b.y + MENU_TBM + (tblcell_i%n_tblrows)*MENU_RH;
            pb.w = col_w;
            pb.h = MENU_RH;
            menuDrawItem (mi, pb, true, false);
            tblcell_i++;
        }
    }

    // immediate draw if menu is over map
    if (boxesOverlap (menu.menu_b, map_b))
        tft.drawPR();

    // set kb focus to first member of first active group, if any
    int focus_idx = -1;
    for (int i = 0; i < menu.n_items; i++) {
        if (MENU_ACTIVE(menu.items[i].type)) {
            focus_idx = i;
            break;
        }
    }

    // operations are severely restricted if there are no active menu items
    // N.B. code below will CRASH if focus < 0.
    bool is_active_menu = (focus_idx >= 0);

    SCoord tap;
    char kb_char;
    UserInput ui = {
        menu.menu_b,
        UI_UFuncNone,
        UF_UNUSED,
        MENU_TIMEOUT,
        menu.update_clocks,
        tap,
        kb_char,
        false,
        false
    };

    // run
    bool ok = false;
    while (waitForUser (ui)) {

        // OK if check for Enter or tap in ok
        if (kb_char == CHAR_CR || kb_char == CHAR_NL || inBox (tap, menu.ok_b)) {

            // done unless a text field reports an error
            ok = true;

            // must also check MenuText field test function if used
            for (int i = 0; ok && i < menu.n_items; i++) {
                if (menu.items[i].type == MENU_TEXT) {
                    MenuItem &mi = menu.items[i];
                    MenuText *tfp = mi.textf;
                    char ynot[50];
                    if (tfp->text_fp && !(tfp->text_fp)(tfp, ynot, sizeof(ynot))) {
                        SBox &pb = pick_boxes[i];
                        drawMenuTextMsg (mi, pb, ynot);
                        ok = false;
                    }
                }
            }

            // finished if still ok
            if (ok)
                break;
        }

        // finished w/o ok if type ESC or tap cancel box or tap anywhere outside the menu box
        if (kb_char == CHAR_ESC                                                         // type ESC
                        || (menu.cancel == M_CANCELOK && inBox (tap, cancel_b))         // tap cancel
                        || (kb_char == CHAR_NONE && !inBox (tap, menu.menu_b))          // tap outside
                    ) {
            break;
        }

        // check for kb or tap control
        if (is_active_menu) {
            if (kb_char) {
                if (menu.items[focus_idx].type == MENU_TEXT)
                    focus_idx = textFieldEdit (menu, pick_boxes, focus_idx, kb_char);
                else
                    focus_idx = kbNavigation (menu, pick_boxes, focus_idx, kb_char);
            } else
                focus_idx = tapNavigation (menu, pick_boxes, focus_idx, tap);
        }
    }

    // done
    drainTouch();

    // erase in prep for caller to restore covered content
    fillSBox (menu.menu_b, RA8875_BLACK);

    // record settings
    Serial.printf (_FX("Menu result after %s:\n"), ok ? "Ok" : "Cancel");
    for (int i = 0; i < menu.n_items; i++) {
        MenuItem &mi = menu.items[i];
        if (MENU_ACTIVE(mi.type))
            Serial.printf (_FX("  %-15s g%d s%d\n"), mi.label ? mi.label : "", mi.group, mi.set);
    }

    return (ok);
}

/* show a brief message in the given color in the given box, generally a box that had been used for a menu
 */
void menuMsg (const SBox &box, uint16_t color, const char *msg)
{
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setCursor (box.x + (box.w - getTextWidth(msg))/2, box.y + box.h/3);
    tft.setTextColor (color);
    drawSBox (box, RA8875_WHITE);
    tft.print (msg);
    wdDelay(2000);
    fillSBox (box, RA8875_BLACK);
}


/* redraw the given ok box in the given visual state.
 * used to allow caller to provide busy or error feedback.
 * N.B. we assume ok_b is same as passed to runMenu and remains unchanged since its return.
 */
void menuRedrawOk (SBox &ok_b, MenuOkState oks)
{
    switch (oks) {
    case MENU_OK_OK:
        tft.setTextColor (MENU_FGC);
        fillSBox (ok_b, MENU_BGC);
        drawSBox (ok_b, MENU_FGC);
        break;
    case MENU_OK_BUSY:
        tft.setTextColor (MENU_BGC);
        fillSBox (ok_b, MENU_BSYC);
        drawSBox (ok_b, MENU_FGC);
        break;
    case MENU_OK_ERR:
        tft.setTextColor (MENU_BGC);
        fillSBox (ok_b, MENU_ERRC);
        drawSBox (ok_b, MENU_FGC);
        break;
    }

    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setCursor (ok_b.x+MENU_BDX, ok_b.y+MENU_BDY);
    tft.print (ok_label);

    // immediate draw if over map
    if (boxesOverlap (ok_b, map_b))
        tft.drawPR();
}

/* wait until:
 *   a tap occurs:                                   set tap location and return true.
 *   a char is typed:                                set kb_char/ctrl/shift and return true.
 *   (*fp)() (IFF fp != NULL) returns true:          set fp_true to true and return false.
 *   to_ms is > 0 and nothing happens for that long: return false.
 * while waiting we optionally update clocks and allow some web server commands.
 */
bool waitForUser (UserInput &ui)
{
    // initial timeout
    uint32_t t0 = millis();

    // reset both actions until they happen here
    ui.kb_char = CHAR_NONE;
    ui.tap = {0, 0};

    for(;;) {

        if (readCalTouchWS(ui.tap) != TT_NONE)
            return(true);

        ui.kb_char = tft.getChar (&ui.kb_ctrl, &ui.kb_shift);
        if (ui.kb_char != CHAR_NONE)
            return (true);

        if (ui.to_ms != UI_NOTIMEOUT && timesUp (&t0, ui.to_ms))
            return (false);

        if (ui.fp != UI_UFuncNone && (*ui.fp)()) {
            ui.fp_true = UF_TRUE;
            return (false);
        }

        if (ui.update_clocks == UF_CLOCKSOK)
            updateClocks(false);

        wdDelay (10);

        // refresh protected region in case X11 window is moved
        if (boxesOverlap (ui.inbox, map_b))
            tft.drawPR();
    }
}
