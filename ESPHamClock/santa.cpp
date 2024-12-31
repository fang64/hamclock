/* code to support a few easter eggs.
 */

#include "HamClock.h"

#define SANTA_C         RA8875_RED

#define SANTA_W 17
#define SANTA_H 50
static const uint8_t santa[SANTA_W*SANTA_H] PROGMEM = {
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x07, 0x00, 0x20, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x01, 0x00, 0xf0, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0xe0, 0x00, 0x00, 0xe0, 0x03, 0xa0, 0x00,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0xe0, 0x01, 0x00, 0xc0, 0x07, 0xc0, 0x01,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0xf0, 0x01, 0x00, 0xc0, 0x01, 0xc0, 0x03,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0xff, 0x01, 0x00, 0xe0, 0x00, 0x80, 0x1f,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0xd0, 0xff, 0xff, 0x09, 0xf8, 0x00, 0x80, 0x0f,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xf0, 0xff, 0x03, 0xfc, 0xff, 0x00, 0x00, 0x07,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xf0, 0xff, 0x0f, 0xf8, 0xff, 0x00, 0x00, 0x07,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0xe6, 0xff, 0x0c, 0xf8, 0xff, 0x00, 0x00, 0x0f,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xde, 0xe1, 0x1f, 0x04, 0xf8, 0xff, 0x00, 0x80, 0x0f,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xfc, 0x3f, 0xe0, 0x01, 0x04, 0xf8, 0xff, 0x01, 0xc0, 0x0f,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xff, 0x1f, 0xe0, 0x00, 0x06, 0x78, 0xfe, 0x07, 0xf8, 0x1f,
 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0xff, 0x1f, 0xe0, 0x00, 0x02, 0x3c, 0x00, 0x99, 0xfe, 0x1f,
 0xe0, 0x01, 0x3e, 0x00, 0x00, 0xfc, 0x81, 0xff, 0x1f, 0x70, 0x00, 0x00, 0x0e, 0x00, 0xe1, 0xff, 0x7f,
 0xe0, 0xe1, 0x7f, 0x00, 0xc0, 0x01, 0x80, 0xff, 0x0f, 0x38, 0x00, 0x00, 0x07, 0x80, 0x80, 0xff, 0x6f,
 0xc0, 0x07, 0x7c, 0x00, 0x18, 0x00, 0x80, 0xff, 0x1f, 0x08, 0x00, 0x80, 0x01, 0x40, 0x00, 0xff, 0x43,
 0x80, 0x1f, 0x78, 0x00, 0x03, 0x00, 0x80, 0x87, 0x31, 0x08, 0x00, 0x80, 0x00, 0x00, 0x00, 0x3f, 0x20,
 0x80, 0x7f, 0xf8, 0xe0, 0x00, 0x06, 0xc0, 0x03, 0x30, 0x08, 0x00, 0x60, 0x00, 0x00, 0x00, 0x0e, 0x20,
 0xc0, 0xff, 0xf8, 0x70, 0x00, 0x0f, 0xe0, 0x01, 0x10, 0x06, 0x00, 0x20, 0x00, 0x00, 0x00, 0x0e, 0x20,
 0xc0, 0xff, 0xf1, 0x3f, 0x00, 0x07, 0x70, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
 0xc0, 0xff, 0xf1, 0x0f, 0x80, 0x03, 0x10, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00,
 0xe0, 0xff, 0xff, 0x03, 0x80, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x03, 0x00,
 0xe0, 0xff, 0xff, 0x03, 0xc0, 0x03, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
 0xfc, 0xff, 0xff, 0x07, 0xe0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
 0xfe, 0xff, 0xff, 0x1f, 0xf0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
 0xf9, 0xff, 0xff, 0x3f, 0xf8, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
 0xc0, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
 0x80, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0xff, 0xff, 0xff, 0xc7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0xff, 0xff, 0x3f, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0xff, 0x7f, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0xff, 0x07, 0x81, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0xff, 0x10, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0x0f, 0xc0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0xff, 0x06, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x1c, 0x90, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define SANTA_WPIX      (SANTA_W*8)     // width in pixels
#define SANTA_HPIX      (SANTA_H)       // height in pixels


// boundary box
SBox santa_b = {0, 0, SANTA_WPIX, SANTA_HPIX};

static void drawSantaBox(void)
{
    // Serial.printf ("Painting santa at %d x %d\n", santa_b.x, santa_b.y);
    for (uint16_t sr = 0; sr < SANTA_HPIX; sr++) {
        for (uint16_t sc = 0; sc < SANTA_W; sc++) {
            uint16_t mask = pgm_read_byte(&santa[sr*SANTA_W+sc]);
            for (uint16_t bc = 0; bc < 8; bc++) {
                uint16_t sx = tft.SCALESZ*santa_b.x + 8*sc + bc;
                uint16_t sy = tft.SCALESZ*santa_b.y + sr;
                if (mask & (1<<bc))
                    tft.drawPixelRaw (sx, sy, SANTA_C);
            }
        }
    }
}

static void eraseSantaBox(const SBox &s)
{
#if defined(_IS_ESP8266)
    // only ESP erases, UNIX just redraws the whole scene every update cycle.
    // Serial.printf ("Erasing santa from %d x %d\n", s.x, s.y);
    for (uint16_t sr = 0; sr < SANTA_HPIX; sr++) {
        for (uint16_t sc = 0; sc < SANTA_W; sc++) {
            for (uint16_t bc = 0; bc < 8; bc++) {
                uint16_t sx = s.x + 8*sc + bc;
                uint16_t sy = s.y + sr;
                SCoord ss = {sx, sy};
                drawMapCoord(ss);
            }
        }
    }
#else
    (void) s;
#endif // _IS_ESP8266
}


/* draw santa on christmas eve
 */
void drawSanta()
{
    // skip unless DE Christmas eve day
    time_t t = nowWO() + getTZ (de_tz);
    if (month(t) != 12 || day(t) != 24) {
        santa_b.x = santa_b.y = 0;
        return;
    }

    // place so he moves over the globe througout the day.
    // left-right each hour, top-bottom throughout the day

    float hr_frac = hour(t)/23.0F;
    float mn_frac = minute(t)/59.0F;

    // location now before next iteration
    SBox s0 = santa_b;

    // handy
    const uint16_t hw = map_b.w/2;
    const uint16_t hh = map_b.h/2;
    const uint16_t xc = map_b.x + hw;
    const uint16_t yc = map_b.y + hh;

    // position onto globe
    switch ((MapProjection)map_proj) {
    case MAPP_MERCATOR: {
        // fit within full map_b
        uint16_t y_t = map_b.y + 1;
        uint16_t y_b = map_b.y + map_b.h - SANTA_HPIX - 1;
        santa_b.y = y_t + hr_frac*(y_b-y_t);
        uint16_t x_l = map_b.x + 1;
        uint16_t x_r = map_b.x + map_b.w - SANTA_WPIX - 1;
        santa_b.x = x_l + mn_frac*(x_r-x_l);
        }
        break;
    
    case MAPP_ROB: {
        uint16_t tb_border = SANTA_HPIX/2;                              // top/bot border
        uint16_t y_t = map_b.y + tb_border;                             // santa_b.y at hr 0
        uint16_t y_b = map_b.y + map_b.h - tb_border - SANTA_HPIX;      // santa_b.y at hr 23
        santa_b.y = y_t + hr_frac*(y_b-y_t);                            // santa_b.y now
        uint16_t y_hw;                                                  // globe half-width 
        if (santa_b.y < yc-SANTA_HPIX/2) {                              // test using top or bottom of box
            y_hw = hw * RobLat2G (90*(yc - santa_b.y)/hh);              // globe half-width at santa top
        } else {
            y_hw = hw * RobLat2G (90*(yc - santa_b.y - SANTA_HPIX)/hh); // globe half-width at santa bottom
        }
        uint16_t x_l = xc - y_hw;
        uint16_t x_r = xc + y_hw - SANTA_WPIX;
        santa_b.x = x_l + mn_frac*(x_r-x_l);                            // spread x along [x_l,x_r]
        }
        break;
    
    case MAPP_AZIMUTHAL: {
        // fit within one of two circles
        uint16_t tb_border = SANTA_HPIX/2;                              // top/bot border
        uint16_t y_t = map_b.y + tb_border;                             // santa_b.y at hr 0
        uint16_t y_b = map_b.y + map_b.h - tb_border - SANTA_HPIX;      // santa_b.y at hr 23
        santa_b.y = y_t + hr_frac*(y_b-y_t);                            // santa_b.y now
        float y_frac;
        if (santa_b.y < yc-SANTA_HPIX/2)                                // test using top or bottom
            y_frac = (yc - santa_b.y)/(float)hh;                        // overall y fraction on santa top
        else
            y_frac = (santa_b.y + SANTA_HPIX - yc)/(float)hh;           // overall y fraction on santa bottom
        uint16_t ew = hh * sqrtf(1 - y_frac*y_frac);                    // half-width to each edge
        uint16_t x_l, x_r;
        if (mn_frac < 0.5F) {                                           // test left or right hemisphere
            x_l = xc - hh - ew;                                         // left edge left hemi
            x_r = xc - hh + ew - SANTA_WPIX;                            // right edge left hemi
            santa_b.x = x_l + 2*mn_frac*(x_r-x_l);                      // spread x along [x_l,x_r] first half
        } else {
            x_l = xc + hh - ew;                                         // left edge right hemi
            x_r = xc + hh + ew - SANTA_WPIX;                            // right edge right hemi
            santa_b.x = x_l + 2*(mn_frac-0.5F)*(x_r-x_l);               // spread x along [x_l,x_r] secnd half
        }
        }
        break;
    
    case MAPP_AZIM1: {
        // fit within central circle
        uint16_t tb_border = SANTA_HPIX/2;                              // top/bot border
        uint16_t y_t = map_b.y + tb_border;                             // santa_b.y at hr 0
        uint16_t y_b = map_b.y + map_b.h - tb_border - SANTA_HPIX;      // santa_b.y at hr 23
        santa_b.y = y_t + hr_frac*(y_b-y_t);                            // santa_b.y now
        float y_frac;
        if (santa_b.y < yc-SANTA_HPIX/2)                                // test using top or bottom
            y_frac = (yc - santa_b.y)/(float)hh;                        // overall y fraction on santa top
        else
            y_frac = (santa_b.y + SANTA_HPIX - yc)/(float)hh;           // overall y fraction on santa bottom
        uint16_t ew = hw/2 * sqrtf(1 - y_frac*y_frac);                  // half-width to each edge
        uint16_t x_l = xc - ew;                                         // left edge
        uint16_t x_r = xc + ew - SANTA_WPIX;                            // right edge
        santa_b.x = x_l + mn_frac*(x_r-x_l);                            // spread x along [x_l,x_r] by minutes
        }
        break;

    default:
        fatalError (_FX("drawSanta() map_proj %d"), map_proj);
    }


    // erase if previously drawn and has now moved 
    if (s0.x && (santa_b.x != s0.x || santa_b.y != s0.y))
        eraseSantaBox (s0);

    // draw if over map else mark as not drawn
    if (overMap (santa_b))
        drawSantaBox();
    else
        santa_b.x = 0;
}


/* return a random color
 */
static uint16_t rcolor(void)
{
    uint8_t h = random(256);
    uint8_t r, g, b;
    hsvtorgb (&r, &g, &b, h, 255, 255);
    return (RGB565(r,g,b));
}

/* take over the screen to show some fireworks at local midnight new year's day
 */
void drawFireworks()
{
    // skip unless local midnight on new year's day or already shown this year
    static int show_year;                       // prevent immediate repeat after click
    time_t local = nowWO() + getTZ (de_tz);
    if (month(local) != 1 || day(local) != 1 || hour(local) != 0
                                        || minute(local) != 0 || year(local) == show_year)
        return;
    uint32_t start_m = millis();
    show_year = year(local);

    // all ours
    eraseScreen();

    // config
    #define FW_MAX_RAYS    30                   // max n rays in each shot
    #define FW_N_KEEP      6                    // n shots to keep then erase
    #define FW_GRAV        (0.2*tft.SCALESZ)    // gravity vel change each iteration, pixels/FLOW_DT/FLOW_DT
    #define FW_VEL         (4*tft.SCALESZ)      // fw velocity, pixels/FLOW_DT
    #define FW_FLOW_DT     2                    // one ray drawing iteration, msec
    #define FW_SHOW_DT     (60*1000)            // show duration, millis()
    #define FW_SHOT_DT     (300)                // max time between shots, msec
    #define FW_SHOOT_COL   DKGRAY               // shoot color

    // counting for FW_N_KEEP
    int n_show = 0;

    // show lasts for FW_SHOW_DT or user does anything
    SCoord tap;
    bool ctrl, shift;
    while (millis() - start_m < FW_SHOW_DT
                        && readCalTouch(tap) == TT_NONE && tft.getChar(&ctrl, &shift) == CHAR_NONE) {

        // shoot upwards from random ground point at random angle and speed
        float shoot_px = 100*tft.SCALESZ + random(600*tft.SCALESZ);     // previous ground x
        float shoot_py = 9*BUILD_H/10;                                  // previous ground y
        float shoot_a = deg2rad (80 + random(20));                      // angle CCW from right
        float shoot_v0 = ((7*tft.SCALESZ) + random(3*tft.SCALESZ))*0.5; // velocity
        float shoot_vx = shoot_v0 * cosf(shoot_a);                      // Vx
        float shoot_vy = -shoot_v0 * sinf(shoot_a);                     // Vy upwards
        while (shoot_vy < 0 && shoot_py > 30) {                         // peak but insure well below top
            shoot_vy += FW_GRAV/7;                                      // slower looks much better
            float x = shoot_px + shoot_vx;
            float y = shoot_py + shoot_vy;
            if (x >= 0 && x < BUILD_W && y >= 0 && y < BUILD_H) {
                tft.drawLineRaw (roundf(shoot_px), roundf(shoot_py), roundf(x), roundf(y), 1, FW_SHOOT_COL);
                shoot_px = x;
                shoot_py = y;
                wdDelay(4);                                             // again, slower looks better
            } else
                break;
        }

        // draw rays outward from center, keep each previous point to connect the dots
        float px[FW_MAX_RAYS];
        float py[FW_MAX_RAYS];
        int n_rays = FW_MAX_RAYS/2 + random(FW_MAX_RAYS/2);

        // starting posiiton of each ray is where shot exploded
        float x = shoot_px;
        float y = shoot_py;
        for (int i = 0; i < n_rays; i++) {
            px[i] = x;
            py[i] = y;
        }

        // set max radius squared
        int maxr2 = 100*tft.SCALESZ+random(100*tft.SCALESZ);
        maxr2 *= maxr2;

        // init velocity change due to gravity
        float vg = 0;

        // point size
        int pr = random(4) == 0 ? tft.SCALESZ : 0;

        // random segment color or whole rays?
        bool rand_seg_col = random(10) < 3;
        bool rand_ray_col = random(10) < 3;
        uint16_t hue0 = rcolor();
        uint16_t cols[FW_MAX_RAYS];
        for (int i = 0; i < n_rays; i++)
            cols[i] = rand_ray_col ? rcolor() : hue0;

        // repeat until hit edge or max radius
        bool done = false;
        while(!done) {
            // add next expanding segment to each ray
            for (int i = 0; i < n_rays; i++) {
                float a = i*2*M_PIF/n_rays;
                float vx = FW_VEL * cosf(a);
                float vy = FW_VEL * sinf(a) + vg;
                x = px[i] + vx;
                y = py[i] + vy;
                if (x >= 0 && x < BUILD_W && y >= 0 && y < BUILD_H
                                        && (x-px[0])*(x-px[0]) + (y-py[0])*(y-py[0]) < maxr2) {
                    uint16_t col = rand_seg_col ? rcolor() : cols[i];
                    tft.drawLineRaw (roundf(px[i]), roundf(py[i]), roundf(x), roundf(y), 1, col);
                    if (pr > 0)
                        tft.fillCircleRaw (roundf(x), roundf(y), pr, RA8875_WHITE);
                    px[i] = x;
                    py[i] = y;
                    wdDelay(FW_FLOW_DT);
                } else
                    done = true;
            }
            vg += FW_GRAV;
        }
        wdDelay(random(FW_SHOT_DT));
        if (++n_show == FW_N_KEEP) {
            eraseScreen();
            n_show = 0;
        }
    }

    // resume
    initScreen();
}
