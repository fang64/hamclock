/* this file manages the background maps, both static styles and VOACAP area propagation.
 *
 * all map files are RGB565 BMP V4 format.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>


#include "HamClock.h"

// persistent state of open files, allows restarting
static File day_file, night_file;                       // open LittleFS file handles
static int day_fbytes, night_fbytes;                    // bytes mmap'ed
static char *day_pixels, *night_pixels;                 // pixels mmap'ed


// BMP file format parameters
#define COREHDRSZ 14                                    // always 14 bytes at front of header
#define HDRVER 108                                      // BITMAPV4HEADER, these many more bytes in subheader
#define BHDRSZ (COREHDRSZ+HDRVER)                       // total header size
#define BPERBMPPIX 2                                    // bytes per BMP pixel

// box in which to draw map scales
SBox mapscale_b;

// current CoreMap designation
CoreMaps core_map = CM_NONE;                            // current core map, if any

// mask of 1<<CoreMaps currently rotating. N.B. must always include 1<<core_map
uint16_t map_rotset;

// supporting info for each core map style
#define X(a,b,c,d,e,f)  {b,c,d,e,f},                    // expands COREMAPS entries for CoreMapInfo
CoreMapInfo cm_info[CM_N] = {
    COREMAPS
};
#undef X


// prop and muf style names
static const char prop_style[] = "PropMap";
static const char muf_v_style[] = "MUFMap";

// handy zoomed w and h
#define ZOOM_W  (HC_MAP_W*pan_zoom.zoom)
#define ZOOM_H  (HC_MAP_H*pan_zoom.zoom)


/* marshall the day and night file names and display titles for the given style.
 * N.B. we do not check for suffient room in the arrays
 * N.B. CM_DRAP name adds -S for no-scale version as of HamClock V2.67
 * N.B. CM_WX name depends on user units
 * N.B. file names are not for query files on UNIX
 */
static void buildMapNames (const char *style, char *dfile, char *nfile, char *dtitle, char *ntitle)
{
        if (strcmp (style, "DRAP") == 0) {
            snprintf (dfile, 32, "/map-D-%dx%d-DRAP-S.bmp", ZOOM_W, ZOOM_H);
            snprintf (nfile, 32, "/map-N-%dx%d-DRAP-S.bmp", ZOOM_W, ZOOM_H);
        } else if (strcmp (style, "Weather") == 0) {
            const char *units = showATMhPa() ? "mB" : "in";
            snprintf (dfile, 32, "/map-D-%dx%d-Wx-%s.bmp", ZOOM_W, ZOOM_H, units);
            snprintf (nfile, 32, "/map-N-%dx%d-Wx-%s.bmp", ZOOM_W, ZOOM_H, units);
        } else {
            snprintf (dfile, 32, "/map-D-%dx%d-%s.bmp", ZOOM_W, ZOOM_H, style);
            snprintf (nfile, 32, "/map-N-%dx%d-%s.bmp", ZOOM_W, ZOOM_H, style);
        }

        snprintf (dtitle, NV_COREMAPSTYLE_LEN+10, "%s D map", style);
        snprintf (ntitle, NV_COREMAPSTYLE_LEN+10, "%s N map", style);
}



/* don't assume we can access unaligned 32 bit values
 */
static uint32_t unpackLE4 (char *buf)
{
        union {
            uint32_t le4;
            char a[4];
        } le4;

        le4.a[0] = buf[0];
        le4.a[1] = buf[1];
        le4.a[2] = buf[2];
        le4.a[3] = buf[3];

        return (le4.le4);
}

/* return whether the given header is the correct BMP format and the total expected file size.
 */
static bool bmpHdrOk (char *buf, uint32_t w, uint32_t h, uint32_t *filesizep)
{
        if (buf[0] != 'B' || buf[1] != 'M') {
            Serial.printf ("Hdr err: ");
            for (int i = 0; i < 10; i++)
                Serial.printf ("0x%02X %c, ", (unsigned)buf[i], (unsigned char)buf[i]);
            Serial.printf ("\n");
            return (false);
        }

        *filesizep = unpackLE4(buf+2);
        uint32_t type = unpackLE4(buf+14);
        uint32_t nrows = - (int32_t)unpackLE4(buf+22);          // nrows<0 means display upside down
        uint32_t ncols = unpackLE4(buf+18);
        uint32_t pixbytes = unpackLE4(buf+34);

        if (pixbytes != nrows*ncols*BPERBMPPIX || type != HDRVER || w != ncols || h != nrows) {
            Serial.printf ("Hdr err: %d %d %d %d\n", pixbytes, type, nrows, ncols);
            return (false);
        }

        return (true);
}

/* download and save the given file.
 * client is already postioned at first byte of image.
 */
static bool downloadMapFile (WiFiClient &client, const char *filename, const char *title)
{

        // set if all ok
        bool ok = false;
        uint32_t filesize;

        // alloc copy buffer
        const uint32_t npixbytes = ZOOM_W*ZOOM_H*BPERBMPPIX;
        char hdr[BHDRSZ];

        // (re)create file
        // extra open/close/remove avoids LitteLFS duplicate COW behavior
        File f = LittleFS.open (filename, "r");
        if (f) {
            f.close();
            LittleFS.remove(filename);
        }
        f = LittleFS.open (filename, "w");
        if (!f) {
            fatalError ("Error creating required file:\n%s\n%s", f.fpath.c_str(), f.errstr.c_str());
            // never returns
        }

        // read and check remote header
        if (!client.readArray (hdr, BHDRSZ)) {
            mapMsg (1000, "%s: header is short", title);
            goto out;
        }
        if (!bmpHdrOk (hdr, ZOOM_W, ZOOM_H, &filesize)) {
            Serial.printf ("bad header: %.*s\n", BHDRSZ, hdr); // might be err message
            mapMsg (1000, "%s: bad header", title);
            goto out;
        }
        if (filesize != npixbytes + BHDRSZ) {
            Serial.printf ("%s: wrong size %u != %u\n", title, filesize, npixbytes);
            mapMsg (1000, "%s: wrong size", title);
            goto out;
        }

        // write header
        f.write (hdr, BHDRSZ);

        // copy pixels
        {   // statement block just to avoid complaint about goto bypassing t0

            // interesting to measure time, but N.B. it also includes file write and graphics. tried just
            // measuring client.readArray but res was too coarse. clock_gettime(CLOCK_MONOTONIC) no better.
            struct timeval tv0, tv1;
            gettimeofday (&tv0, NULL);

            const uint32_t n_report = 10;                       // n mapMsgs reports
            const uint32_t chunk_size = npixbytes/n_report;
            StackMalloc chunk_mem (chunk_size);
            char *chunk = (char *) chunk_mem.getMem();
            for (uint32_t n_read = 0; n_read < npixbytes; ) {
                updateClocks(false);

                // read and save another chunk or final piece
                uint32_t n_next = npixbytes - n_read;
                if (n_next > chunk_size)
                    n_next = chunk_size;
                if (!client.readArray (chunk, n_next)) {
                    mapMsg (1000, "%s: file is short", title);
                    goto out;
                }
                if (f.write (chunk, n_next) != n_next) {
                    mapMsg (1000, "%s: local write failed", title);
                    goto out;
                }
                n_read += n_next;

                // report progress
                if (pan_zoom.zoom > MIN_ZOOM)
                    mapMsg (0, "%s %dx: %3u%%", title, pan_zoom.zoom, 100*n_read/chunk_size/n_report);
                else
                    mapMsg (0, "%s: %3u%%", title, 100*n_read/chunk_size/n_report);
            }

            gettimeofday (&tv1, NULL);
            long network_us = (tv1.tv_sec - tv0.tv_sec)*1000000 + (tv1.tv_usec - tv0.tv_usec);
            Serial.printf ("%s: %lu B/s\n", filename, 1000000UL*npixbytes/network_us);
        }

        // if get here, it worked!
        ok = true;

    out:

        f.close();
        if (!ok)
            LittleFS.remove (filename);

        return (ok);
}


/* invalidate pixel connection until proven good again.
 */
static void invalidatePixels()
{
        // disconnect from tft thread
        tft.setEarthPix (NULL, NULL, 0, 0);

        if (getGrayDisplay() == GRAY_OFF) {
            // unmap pixel arrays
            if (day_pixels) {
                munmap (day_pixels, day_fbytes);
                day_pixels = NULL;
            }
            if (night_pixels) {
                munmap (night_pixels, day_fbytes);
                night_pixels = NULL;
            }
        } else {
            // gray scale pixels are local arrays
            free (day_pixels);
            day_pixels = NULL;
            free (night_pixels);
            night_pixels = NULL;
        }
}

/* convert the given RGB565 from color to gray
 */
static uint16_t RGB565TOGRAY (uint16_t c)
{
        uint16_t r = RGB565_R(c);
        uint16_t g = RGB565_G(c);
        uint16_t b = RGB565_B(c);
        uint16_t gray = RGB2GRAY(r,g,b);
        return (RGB565 (gray, gray, gray));
}

/* prepare open day_file and night_file for pixel access.
 * gray images are converted into memory arrays.
 * return whether ok
 */
static bool installFilePixels (const char *dfile, const char *nfile)
{
        bool ok = false;

        // mmap pixels if both files are open
        if (day_file && night_file) {

            day_fbytes = BHDRSZ + ZOOM_W*ZOOM_H*2;          // n bytes of 16 bit RGB565 pixels
            night_fbytes = BHDRSZ + ZOOM_W*ZOOM_H*2;
            day_pixels = (char *)                               // allow OS to choose addrs
                    mmap (NULL, day_fbytes, PROT_READ, MAP_PRIVATE, day_file.fileno(), 0);
            night_pixels = (char *)
                    mmap (NULL, night_fbytes, PROT_READ, MAP_PRIVATE, night_file.fileno(), 0);

            ok = day_pixels != MAP_FAILED && night_pixels != MAP_FAILED;
        }

        // install pixels if ok
        if (ok) {

            // Serial.println (F("both mmaps good"));

            // don't need files open once mmap has been established
            day_file.close();
            night_file.close();;

            if (getGrayDisplay() != GRAY_OFF) {

                // convert to gray images in memory

                // prep new arrays
                const int n_mem_bytes = ZOOM_W*ZOOM_H*2;  // don't need BHDRSZ
                char *mem_day_pixels = (char *) malloc (n_mem_bytes);
                char *mem_night_pixels = (char *) malloc (n_mem_bytes);
                if (!mem_day_pixels || !mem_night_pixels)
                    fatalError ("No memory for gray scale image");

                // handy pixel pointers from mmap to memory
                uint16_t *fdp = (uint16_t *) (day_pixels + BHDRSZ);
                uint16_t *fnp = (uint16_t *) (night_pixels + BHDRSZ);
                uint16_t *tdp = (uint16_t *) (mem_day_pixels);
                uint16_t *tnp = (uint16_t *) (mem_night_pixels);

                // convert each pixel
                int n_mem_pix = n_mem_bytes/2;
                struct timeval tv0, tv1;
                gettimeofday (&tv0, NULL);
                while (--n_mem_pix >= 0) {
                    *tdp++ = RGB565TOGRAY(*fdp++);
                    *tnp++ = RGB565TOGRAY(*fnp++);
                }
                gettimeofday (&tv1, NULL);
                Serial.printf ("gray conversion took %ld us\n",
                                            (tv1.tv_sec-tv0.tv_sec)*1000000 + (tv1.tv_usec - tv0.tv_usec));

                // replace mmap with gray memory copy
                munmap (day_pixels, day_fbytes);
                day_pixels = mem_day_pixels;
                munmap (night_pixels, night_fbytes);
                night_pixels = mem_night_pixels;

                // install in tft at start of pixels
                tft.setEarthPix (day_pixels, night_pixels, ZOOM_W, ZOOM_H);

            } else {
                // install in tft at start of pixels
                tft.setEarthPix (day_pixels+BHDRSZ, night_pixels+BHDRSZ, ZOOM_W, ZOOM_H);
            }

        } else {

            // no go -- clean up

            if (day_file)
                day_file.close();
            else
                Serial.printf ("%s not open\n", dfile);
            if (day_pixels == MAP_FAILED)
                Serial.printf ("%s mmap failed: %s\n", dfile, strerror(errno));
            else if (day_pixels)
                munmap (day_pixels, day_fbytes);
            day_pixels = NULL;

            if (night_file)
                night_file.close();
            else
                Serial.printf ("%s not open\n", nfile);
            if (night_pixels == MAP_FAILED)
                Serial.printf ("%s mmap failed: %s\n", nfile, strerror(errno));
            else if (night_pixels)
                munmap (night_pixels, night_fbytes);
            night_pixels = NULL;

        }

        return (ok);
}

/* clean up old bmp files
 */
static void cleanupMaps (void)
{
        // open our working directory
        DIR *dirp = opendir (our_dir.c_str());
        if (dirp == NULL) {
            Serial.printf ("RM: %s: %s\n", our_dir.c_str(), strerror(errno));
            return;
        }

        // malloced list of malloced names to be removed (so we don't modify dir while scanning)
        typedef struct {
            char *fn;                                   // malloced file name
            int age;                                    // age, seconds
        } RMFile;
        RMFile *rm_files = NULL;                        // malloced list
        int n_rm = 0;                                   // n in list

        // scan for bmp files at least a few days old
        const int max_age = 2*3600*24;
        time_t now = myNow();
        struct dirent *dp;
        while ((dp = readdir(dirp)) != NULL) {
            if (strstr (dp->d_name, ".bmp") != NULL) {
                // file name matches now check age
                char fpath[10000];
                struct stat sbuf;
                snprintf (fpath, sizeof(fpath), "%s/%s", our_dir.c_str(), dp->d_name);
                if (stat (fpath, &sbuf) < 0)
                    Serial.printf ("RM: %s: %s\n", fpath, strerror(errno));
                else {
                    int age = now - sbuf.st_mtime;      // last modified time
                    if (age > max_age) {
                        // add to list to be removed
                        rm_files = (RMFile *) realloc (rm_files, (n_rm+1)*sizeof(RMFile));
                        rm_files[n_rm].fn = strdup (fpath);
                        rm_files[n_rm].age = age;
                        n_rm++;
                    }
                }
            }
        }
        closedir (dirp);

        // remove files and clean up rm_files along the way
        for (int i = 0; i < n_rm; i++) {
            char *fn = rm_files[i].fn;
            Serial.printf ("RM: %6.1f days old %s\n", rm_files[i].age/(3600.0F*24.0F), fn);
            if (unlink (fn) < 0)
                Serial.printf ("RM: unlink(%s): %s\n", fn, strerror(errno));
            free (fn);
        }
        free (rm_files);
}

/* install maps that require a query unless file with same query already exists.
 * page is the fetch*.pl CGI handler, we add the query here based on current circumstances.
 * return whether ok
 */
static bool installQueryMaps (const char *page, const char *msg, const char *style, const float MHz)
{
        // get clock time
        time_t t = nowWO();
        int yr = year(t);
        int mo = month(t);
        int hr = hour(t);

        // prepare query
        char query[200];
        snprintf (query, sizeof(query),
            "YEAR=%d&MONTH=%d&UTC=%d&TXLAT=%.3f&TXLNG=%.3f&PATH=%d&WATTS=%d&WIDTH=%d&HEIGHT=%d&MHZ=%.2f&TOA=%.1f&MODE=%d&TOA=%.1f",
            yr, mo, hr, de_ll.lat_d, de_ll.lng_d, show_lp, bc_power, ZOOM_W, ZOOM_H,
            MHz, bc_toa, bc_modevalue, bc_toa);

        // Serial.printf ("%s query: %s\n", style, query);

        // assign a style and compose names and titles
        char dfile[32];                                                         // not used
        char nfile[32];                                                         // not used
        char dtitle[NV_COREMAPSTYLE_LEN+10];
        char ntitle[NV_COREMAPSTYLE_LEN+10];
        buildMapNames (style, dfile, nfile, dtitle, ntitle);

        // insure fresh start
        invalidatePixels();
        cleanupMaps();

        // by storing the entire query in the file name we easily know if a new download is needed.
        // N.B. this violates the maximum file name limit in the real LitteFS

        // create file names containing query
        char q_dfn[200];
        char q_nfn[200];
        snprintf (q_dfn, sizeof(q_dfn), "map-D-%s-%s-%010u.bmp", style, page, stringHash(query));
        snprintf (q_nfn, sizeof(q_nfn), "map-N-%s-%s-%010u.bmp", style, page, stringHash(query));

        // check if both exist
        bool ok = false;
        File d_f = LittleFS.open (q_dfn, "r");
        if (d_f) {
            d_f.close();
            File n_f = LittleFS.open (q_nfn, "r");
            if (n_f) {
                n_f.close();
                ok = true;
                Serial.printf ("%s: D and N files already downlaoded\n", style);
            }
        }

        // if not, download both
        if (!ok) {

            // download new voacap maps
            updateClocks(false);
            WiFiClient client;
            if (wifiOk() && client.connect(backend_host, backend_port)) {
                mapMsg (0, "%s", msg);
                char full_page[300];
                snprintf (full_page, sizeof(full_page), "/%s?%s", page, query);
                Serial.printf ("%s\n", full_page);
                httpHCGET (client, backend_host, full_page);
                ok = httpSkipHeader (client) && downloadMapFile (client, q_dfn, dtitle)
                                             && downloadMapFile (client, q_nfn, ntitle);
                client.stop();
            }
        }

        // install if ok
        if (ok) {
            day_file = LittleFS.open (q_dfn, "r");
            night_file = LittleFS.open (q_nfn, "r");
            ok = installFilePixels (q_dfn, q_nfn);
        }

        if (!ok)
            Serial.printf ("%s: fail\n", style);

        return (ok);
}



/* qsort-style compare two FS_Info by name
 */
static int FSInfoNameQsort (const void *p1, const void *p2)
{
        return (strcmp (((FS_Info *)p1)->name, ((FS_Info *)p2)->name));
}


/* open the given CoreMaps RGB565 BMP file, downloading fresh if absent or too old.
 * if ok, return open File positioned at first pixel, else return with File closed.
 */
static File openMapFile (CoreMaps cm, const char *file, const char *title)
{
        // putting all variables up here avoids pendantic goto warnings
        File f;
        WiFiClient client;
        uint32_t filesize;
        int age = 0;
        char hdr_buf[BHDRSZ];
        int nr = 0;
        bool file_ok = false;


        // open local file, "bad" if absent
        f = LittleFS.open (file, "r");
        if (!f) {
            Serial.printf ("%s: not local\n", file);
            goto out;
        }

        // file is "bad" if too old
        age = myNow() - f.getCreationTime();
        if (age > cm_info[cm].maxage) {
            Serial.printf ("%s too old: %d > %d secs\n", file, age, cm_info[cm].maxage);
            goto out;
        } else
            Serial.printf ("%s: %s %d secs old, max %d\n", title, file, age, cm_info[cm].maxage);

        // read header
        nr = f.read ((uint8_t*)hdr_buf, BHDRSZ);
        if (nr != BHDRSZ) {
            Serial.printf ("%s: header short\n", file);
            goto out;
        }

        // check type and size
        if (!bmpHdrOk (hdr_buf, ZOOM_W, ZOOM_H, &filesize)) {
            Serial.printf ("%s: bad format\n", file);
            goto out;
        }
        if (filesize != f.size()) {
            Serial.printf ("%s: wrong size", file);
            goto out;
        }

        // all good
        file_ok = true;

    out:

        // download if not ok for any reason
        if (!file_ok) {

            if (f) {
                // file exists but is not correct in some way
                f.close();
                LittleFS.remove(file);
            }

            // download and open again if success
            if (wifiOk() && client.connect(backend_host, backend_port)) {
                snprintf (hdr_buf, sizeof(hdr_buf), "/maps/%s", file);
                Serial.printf ("%s\n", hdr_buf);
                httpHCGET (client, backend_host, hdr_buf);
                if (httpSkipHeader(client) && downloadMapFile (client, file, title))
                    f = LittleFS.open (file, "r");
                client.stop();
            }
            if (!f)
                mapMsg (1000, "%s: download failed", title);
        }

        // return result, open if good or closed if not
        return (f);
}

/* install maps for the given CoreMap that are just files maintained on the server, no update query required.
 * Download only if absent or stale.
 * return whether ok
 */
static bool installFileMaps (CoreMaps cm)
{
        // confirm core_map is one of the file styles
        if (cm != CM_COUNTRIES && cm != CM_TERRAIN && cm != CM_DRAP
                            && cm != CM_AURORA && cm != CM_WX && cm != CM_MUF_RT)
            fatalError ("installFileMaps(%d) invalid", cm);        // does not return

        // create names and titles
        const char *style = cm_info[cm].name;
        char dfile[LFS_NAME_MAX];
        char nfile[LFS_NAME_MAX];
        char dtitle[NV_COREMAPSTYLE_LEN+10];
        char ntitle[NV_COREMAPSTYLE_LEN+10];
        buildMapNames (style, dfile, nfile, dtitle, ntitle);

        // insure fresh start
        invalidatePixels();
        cleanupMaps();
        if (day_file)
            day_file.close();
        if (night_file)
            night_file.close();

        // open each file, downloading if newer or not found locally
        day_file = openMapFile (cm, dfile, dtitle);
        night_file = openMapFile (cm, nfile, ntitle);

        // install pixels
        return (installFilePixels (dfile, nfile));
}

/* install fresh core_map.
 * return whether ok
 * N.B. drain pending clicks that may have accumulated during slow downloads.
 */
bool installFreshMaps()
{
        char s[NV_COREMAPSTYLE_LEN];
        char msg[100];
        snprintf (msg, sizeof(msg), "Calculating %s...", getCoreMapStyle(core_map, s));

        bool ok = false;

        switch (core_map) {
        case CM_PMTOA:
            ok = installQueryMaps("fetchVOACAP-TOA.pl", msg, prop_style,propBand2MHz(cm_info[CM_PMTOA].band));
            break;
        case CM_PMREL:
            ok = installQueryMaps("fetchVOACAPArea.pl", msg, prop_style,propBand2MHz(cm_info[CM_PMREL].band));
            break;
        case CM_MUF_V:
            ok = installQueryMaps ("fetchVOACAP-MUF.pl", msg, muf_v_style, 0);
            break;
        case CM_COUNTRIES:
        case CM_TERRAIN:
        case CM_DRAP:
        case CM_MUF_RT:
        case CM_AURORA:
        case CM_WX:
            ok = installFileMaps (core_map);
            break;
        case CM_N:              // lint
            break;
        }

        drainTouch();

        return (ok);
}

/* init core_map from NV, or set a default
 * return whether ok
 */
void initCoreMaps()
{
        // init map from NV if present and valid
        char s[NV_COREMAPSTYLE_LEN];
        core_map = CM_NONE;
        if (NVReadString (NV_COREMAPSTYLE, s)) {
            for (int i = 0; i < CM_N; i++) {
                if (strcmp (cm_info[i].name, s) == 0) {
                    core_map = (CoreMaps)i;
                    break;
                }
            }
        }

        // init map_rotset
        if (!NVReadUInt16 (NV_MAPROTSET, &map_rotset))
            map_rotset = 0;

        // init BC bands
        uint8_t band;
        if (!NVReadUInt8 (NV_BCTOABAND, &band)) {
            band = PROPBAND_NONE;
            NVWriteUInt8 (NV_BCTOABAND, band);
        }
        if (band >= PROPBAND_NONE)
            RM_CMROT (CM_PMTOA);
        cm_info[CM_PMTOA].band = (PropMapBand) band;

        if (!NVReadUInt8 (NV_BCRELBAND, &band)) {
            band = PROPBAND_NONE;
            NVWriteUInt8 (NV_BCRELBAND, band);
        }
        if (band >= PROPBAND_NONE)
            RM_CMROT (CM_PMREL);
        cm_info[CM_PMREL].band = (PropMapBand) band;

        // insure sane
        insureCoreMap();

        // log initial settings
        logMapRotSet();
}

/* save map_rotset core_map and BC bands
 */
void saveCoreMaps(void)
{
        if ((int)core_map >= CM_N)
            fatalError ("saveMapRotSet() core_map %d\n", core_map);

        NVWriteString (NV_COREMAPSTYLE, cm_info[core_map].name);
        NVWriteUInt16 (NV_MAPROTSET, map_rotset);
        NVWriteUInt8 (NV_BCTOABAND, cm_info[CM_PMTOA].band);
        NVWriteUInt8 (NV_BCRELBAND, cm_info[CM_PMREL].band);
}

/* log map_rotset and core_map
 */
void logMapRotSet(void)
{
        char s[NV_COREMAPSTYLE_LEN];
        char line[512];
        size_t ll = 0;
        ll += snprintf (line+ll, sizeof(line)-ll, "Active Map styles: ");
        for (int i = 0; i < CM_N; i++) {
            int ci = (core_map + i) % CM_N;             // start with core_map
            if (IS_CMROT(ci))
                ll += snprintf (line+ll, sizeof(line)-ll, "%s ", getCoreMapStyle ((CoreMaps)ci, s));
        }
        Serial.printf ("%s\n", line);
}


/* produce a listing of the map storage directory.
 * N.B. return malloced array and malloced name -- caller must free()
 */
FS_Info *getConfigDirInfo (int *n_info, char **fs_name, uint64_t *fs_size, uint64_t *fs_used)
{
        // get basic fs info
        FSInfo fs_info;
        LittleFS.info(fs_info);

        // pass back basic info
        *fs_name = strdup ("HamClock file system");
        *fs_size = fs_info.totalBytes;
        *fs_used = fs_info.usedBytes;

        // build each entry
        FS_Info *fs_array = NULL;
        int n_fs = 0;
        Dir dir = LittleFS.openDir("/");
        while (dir.next()) {

            // extend array
            fs_array = (FS_Info *) realloc (fs_array, (n_fs+1)*sizeof(FS_Info));
            if (!fs_array)
                fatalError ("alloc dir failed: %d", n_fs);     // no _FX if alloc failing
            FS_Info *fip = &fs_array[n_fs++];

            // store name
            strncpy (fip->name, dir.fileName().c_str(), sizeof(fip->name)-1);
            fip->name[sizeof(fip->name)-1] = 0;

            // store time
            time_t t = dir.fileCreationTime();
            fip->t0 = t;

            // as handy date string too
            int yr = year(t);
            int mo = month(t);
            int dy = day(t);
            int hr = hour(t);
            int mn = minute(t);
            int sc = second(t);
            snprintf (fip->date, sizeof(fip->date), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                yr, mo, dy, hr, mn, sc);

            // store length
            fip->len = dir.fileSize();
        }
        // Dir has no close method, hope destructor cleans up

        // nice sorted order
        qsort (fs_array, n_fs, sizeof(FS_Info), FSInfoNameQsort);

        // ok
        *n_info = n_fs;
        return (fs_array);
}

/* return the given map style name
 */
const char *getCoreMapStyle (CoreMaps cm, char s[NV_COREMAPSTYLE_LEN])
{
        const char *name = cm_info[cm].name;

        if (cm == CM_PMTOA || cm == CM_PMREL)
            snprintf (s, NV_COREMAPSTYLE_LEN, "%dm/%s", propBand2Band (cm_info[cm].band), name);
        else
            snprintf (s, NV_COREMAPSTYLE_LEN, "%s", name);

        return (s);
}

/* return MHz for the given PropMapBand
 * N.B. match column headings in voacapx.out
 */
float propBand2MHz (PropMapBand band)
{
        switch (band) {
        case PROPBAND_80M: return ( 3.6);
        case PROPBAND_40M: return ( 7.1);
        case PROPBAND_30M: return (10.1);
        case PROPBAND_20M: return (14.1);
        case PROPBAND_17M: return (18.1);
        case PROPBAND_15M: return (21.1);
        case PROPBAND_12M: return (24.9);
        case PROPBAND_10M: return (28.2);
        default: fatalError ("propBand2MHz MHz %d", band);
        }

        // lint
        return (0);
}

/* return band for the given PropMapBand
 */
int propBand2Band (PropMapBand band)
{
        switch (band) {
        case PROPBAND_80M: return (80);
        case PROPBAND_40M: return (40);
        case PROPBAND_30M: return (30);
        case PROPBAND_20M: return (20);
        case PROPBAND_17M: return (17);
        case PROPBAND_15M: return (15);
        case PROPBAND_12M: return (12);
        case PROPBAND_10M: return (10);
        default: fatalError ("propBand2Band %d", band);
        }

        // lint
        return (0);
}


/* return whether the map scale is (or should be) visible now
 * N.B. must agree with drawMapScale()
 */
bool mapScaleIsUp(void)
{
    switch (core_map) {
    case CM_DRAP:
    case CM_MUF_V:
    case CM_MUF_RT:
    case CM_AURORA:
    case CM_WX:
    case CM_PMTOA:
    case CM_PMREL:
        return (true);
    case CM_COUNTRIES:
    case CM_TERRAIN:
    case CM_N:          // lint
        return (false);
    }
    return (false);
}

/* draw the appropriate scale at mapscale_b depending on core_map, if any.
 * N.B. we move mapscale_b depending on rss_on
 */
void drawMapScale()
{
    // color scale. values must be monotonically increasing.
    typedef struct {
        float value;                                    // world value
        uint32_t color;                                 // 24 bit RGB scale color
        bool black_text;                                // black text, else white
    } MapScalePoint;

    // CM_DRAP and CM_MUF_V and CM_MUF_RT
    static const MapScalePoint d_scale[] = {            // see fetchDRAP.pl and fetchVOACAP-MUF.pl
        {0,  0x000000, 0},
        {4,  0x4E138A, 0},
        {9,  0x001EF5, 0},
        {15, 0x78FBD6, 1},
        {20, 0x78FA4D, 1},
        {27, 0xFEFD54, 1},
        {30, 0xEC6F2D, 1},
        {35, 0xE93323, 1},
    };

    // CM_AURORA
    static const MapScalePoint a_scale[] = {            // see fetchAurora.pl
        {0,   0x282828, 0},
        {25,  0x00FF00, 1},
        {50,  0xFFFF00, 1},
        {75,  0xEA6D2D, 1},
        {100, 0xFF0000, 1},
    };

    // CM_WX
    static const MapScalePoint w_scale[] = {            // see fetchWordWx.pl
        // values are degs C
        {-50,  0xD1E7FF, 1},
        {-40,  0xB5D5FF, 1},
        {-30,  0x88BFFF, 1},
        {-20,  0x73AAFF, 1},
        {-10,  0x4078D9, 0},
        {0,    0x2060A6, 0},
        {10,   0x009EDC, 1},
        {20,   0xBEE5B4, 1},
        {30,   0xFF8C24, 1},
        {40,   0xEE0051, 1},
        {50,   0x5B0023, 1},
    };

    // CM_PMTOA
    static const MapScalePoint t_scale[] = {            // see fetchVOACAP-TOA.pl
        {0,    0x0000F0, 0},
        {6,    0xF0B060, 1},
        {30,   0xF00000, 1},
    };

    // CM_PMREL
    static const MapScalePoint r_scale[] = {            // see fetchVOACAPArea.pl
        {0,    0x666666, 0},
        {21,   0xEE6766, 0},
        {40,   0xEEEE44, 1},
        {60,   0xEEEE44, 1},
        {83,   0x44CC44, 0},
        {100,  0x44CC44, 0},
    };


    // set these depending on map
    const MapScalePoint *msp = NULL;                    // one of above tables
    unsigned n_scale = 0;                               // n entries in table
    unsigned n_labels = 0;                              // n labels in scale
    const char *title = NULL;                           // scale title

    switch (core_map) {
    case CM_COUNTRIES:
    case CM_TERRAIN:
    case CM_N:                                          // lint
        // no scale
        return;
    case CM_MUF_V:          // fallthru
    case CM_MUF_RT:         // fallthru
    case CM_DRAP:
        msp = d_scale;
        n_scale = NARRAY(d_scale);
        n_labels = 8;
        title = "MHz";
        break;
    case CM_AURORA:
        msp = a_scale;
        n_scale = NARRAY(a_scale);
        n_labels = 11;
        title = "% Chance";
        break;
    case CM_WX:
        msp = w_scale;
        n_scale = NARRAY(w_scale);
        n_labels = showTempC() ? 11 : 10;
        title = "Degs C";
        break;
    case CM_PMTOA:
        msp = t_scale;
        n_scale = NARRAY(t_scale);
        n_labels = 7;
        title = "DE TOA, degs";
        break;
    case CM_PMREL:
        msp = r_scale;
        n_scale = NARRAY(r_scale);
        n_labels = 6;
        title = "% Reliability";
        break;
    }


    // handy accessors
    #define _MS_PTV(i)  (msp[i].value)                          // handy access to msp[i].value
    #define _MS_PTC(i)  (msp[i].color)                          // handy access to msp[i].color
    #define _MS_PTB(i)  (msp[i].black_text)                     // handy access to msp[i].black_text

    // geometry setup
    #define _MS_X0      mapscale_b.x                            // left x
    #define _MS_X1      (mapscale_b.x + mapscale_b.w)           // right x
    #define _MS_DX      (_MS_X1-_MS_X0)                         // width
    #define _MS_MINV    _MS_PTV(0)                              // min value
    #define _MS_MAXV    _MS_PTV(n_scale-1)                      // max value
    #define _MS_DV      (_MS_MAXV-_MS_MINV)                     // value span
    #define _MS_V2X(v)  (_MS_X0 + _MS_DX*((v)-_MS_MINV)/_MS_DV) // convert value to x
    #define _MS_PRY     (mapscale_b.y+1U)                       // text y

    // set mapscale_b.y above RSS if on else at the bottom
    mapscale_b.y = rss_on ? rss_bnr_b.y - mapscale_b.h: map_b.y + map_b.h - mapscale_b.h;

    // draw smoothly-interpolated color scale
    for (unsigned i = 1; i < n_scale; i++) {
        uint8_t dm = _MS_PTV(i) - _MS_PTV(i-1);
        uint8_t r0 = _MS_PTC(i-1) >> 16;
        uint8_t g0 = (_MS_PTC(i-1) >> 8) & 0xFF;
        uint8_t b0 = _MS_PTC(i-1) & 0xFF;
        uint8_t r1 = _MS_PTC(i) >> 16;
        uint8_t g1 = (_MS_PTC(i) >> 8) & 0xFF;
        uint8_t b1 = _MS_PTC(i) & 0xFF;
        for (uint16_t x = _MS_V2X(_MS_PTV(i-1)); x <= _MS_V2X(_MS_PTV(i)); x++) {
            if (x < mapscale_b.x + mapscale_b.w) {              // the _MS macros can overflow slightlty
                float value = _MS_MINV + (float)_MS_DV*(x - _MS_X0)/_MS_DX;
                float frac = CLAMPF ((value - _MS_PTV(i-1))/dm,0,1);
                uint16_t new_c = RGB565(r0+frac*(r1-r0), g0+frac*(g1-g0), b0+frac*(b1-b0));
                tft.drawLine (x, mapscale_b.y, x, mapscale_b.y+mapscale_b.h-1, 1, new_c);
            }
        }
    }

    // determine marker location, if used
    uint16_t marker_x = 0;
    float v = 0;
    bool v_ok = false;
    if (core_map == CM_DRAP) {
        (void) checkForNewDRAP();
        if (space_wx[SPCWX_DRAP].value_ok) {
            v = space_wx[SPCWX_DRAP].value;
            v_ok = true;
        }
    } else if (core_map == CM_AURORA) {
        (void) checkForNewAurora();
        if (space_wx[SPCWX_AURORA].value_ok) {
            v = space_wx[SPCWX_AURORA].value;
            v_ok = true;
        }
    }
    if (v_ok) {
        // find marker but beware range overflow and leave room for full width
        float clamp_v = CLAMPF (v, _MS_MINV, _MS_MAXV);
        marker_x = CLAMPF (_MS_V2X(clamp_v), mapscale_b.x + 3, mapscale_b.x + mapscale_b.w - 4);
    }

    // draw labels inside mapscale_b but may need to build F scale for WX

    // use labels directly unless need to create F weather scale
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    StackMalloc ticks_v_mem((n_labels+2)*sizeof(float));
    StackMalloc ticks_x_mem((n_labels+2)*sizeof(uint16_t));
    float *ticks_v = (float *) ticks_v_mem.getMem();
    uint16_t *ticks_x = (uint16_t *) ticks_x_mem.getMem();
    int n_ticks;
    const char *my_title;

    // prep values and center x locations
    if (core_map == CM_WX && !showTempC()) {

        // switch to F scale
        my_title = "Degs F";
        n_ticks = tickmarks (CEN2FAH(_MS_MINV), CEN2FAH(_MS_MAXV), n_labels, ticks_v);
        for (int i = 0; i < n_ticks; i++) {
            float value = roundf(ticks_v[i]);                   // value printed is F ...
            float value_c = FAH2CEN(value);                     // ... but position is based on C
            ticks_v[i] = value;
            ticks_x[i] = _MS_V2X(value_c);
        }

    } else {

        // generate evenly-spaced labels from min to max, inclusive
        my_title = title;
        n_ticks = n_labels;
        for (unsigned i = 0; i < n_labels; i++) {
            ticks_v[i] = _MS_MINV + _MS_DV*i/(n_labels-1);
            ticks_x[i] = _MS_V2X(ticks_v[i]);
        }

    }

    // print tick marks across mapscale_b but avoid marker
    for (int i = 1; i < n_ticks; i++) {                         // skip first for title

        // skip if off scale or near marker
        const uint16_t ti_x = ticks_x[i];
        if (ti_x < _MS_X0 || ti_x > _MS_X1 || (marker_x && ti_x >= marker_x - 15 && ti_x <= marker_x + 15))
            continue;

        // center but beware edges (we already skipped first so left edge is never a problem)
        char buf[20];
        snprintf (buf, sizeof(buf), "%.0f", ticks_v[i]);
        uint16_t buf_w = getTextWidth(buf);
        uint16_t buf_lx = ti_x - buf_w/2;
        uint16_t buf_rx = buf_lx + buf_w;
        if (buf_rx > _MS_X1 - 2) {
            buf_rx = _MS_X1 - 2;
            buf_lx = buf_rx - buf_w;
        }
        tft.setCursor (buf_lx, _MS_PRY);
        tft.setTextColor (_MS_PTB(i*n_scale/n_ticks) ? RA8875_BLACK : RA8875_WHITE);
        tft.print (buf);
    }

    // draw scale meaning
    tft.setTextColor (_MS_PTB(0) ? RA8875_BLACK : RA8875_WHITE);
    tft.setCursor (_MS_X0 + 4, _MS_PRY);
    tft.print (my_title);

    // draw marker
    if (marker_x) {
        // use lines, not rect, for a perfect vetical match to scale
        tft.drawLine (marker_x-2, mapscale_b.y, marker_x-2, mapscale_b.y+mapscale_b.h-1, 1, RA8875_BLACK);
        tft.drawLine (marker_x-1, mapscale_b.y, marker_x-1, mapscale_b.y+mapscale_b.h-1, 1, RA8875_RED);
        tft.drawLine (marker_x,   mapscale_b.y, marker_x,   mapscale_b.y+mapscale_b.h-1, 1, RA8875_RED);
        tft.drawLine (marker_x+1, mapscale_b.y, marker_x+1, mapscale_b.y+mapscale_b.h-1, 1, RA8875_RED);
        tft.drawLine (marker_x+2, mapscale_b.y, marker_x+2, mapscale_b.y+mapscale_b.h-1, 1, RA8875_BLACK);
    }

}

/* log and show message in a nice box over map_b.
 */
void mapMsg (uint32_t dwell_ms, const char *fmt, ...)
{
    // format msg
    va_list ap;
    va_start(ap, fmt);
    char msg[500];
    vsnprintf (msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // log
    Serial.printf ("mapMsg: %s\n", msg);

    // get msg width
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor (RA8875_WHITE);
    uint16_t msg_w = getTextWidth(msg);

    // set constant box size but allow larger
    const uint16_t margin = 50;
    uint16_t box_w = map_b.w - 200;
    if (msg_w + margin > box_w) {
        box_w = map_b.w - 20;
        msg_w = maxStringW (msg, box_w-margin);
    }

    // draw both centered
    uint16_t msg_y = map_b.y + map_b.h/10;
    tft.fillRect (map_b.x + (map_b.w-box_w)/2, msg_y, box_w, 30, RA8875_BLUE);
    tft.setCursor (map_b.x + (map_b.w-msg_w)/2, msg_y + 12);
    tft.print(msg);
    tft.drawPR();

    // dwell with clocks
    while (dwell_ms > 200) {
        usleep (200000);
        updateClocks (false);
        dwell_ms -= 200;
    }
}
/* return whether background maps are rotating
 */
bool mapIsRotating()
{
    // rotating if more than 1 bit is on
    return ((map_rotset & (map_rotset-1U)) != 0);       // removes lowest set bit
}

/* make sure core_map is set to one of the entries in map_rotset, else set a default
 */
void insureCoreMap()
{
    // done if core_map is already in the set
    if (IS_CMROT(core_map))
        return;

    // pick another
    core_map = CM_NONE;
    for (int i = 0; i < CM_N && core_map == CM_NONE; i++)
        if (IS_CMROT(i))
            core_map = (CoreMaps)i;

    // if none at all, set a default
    if (core_map == CM_NONE)
        DO_CMROT(CM_COUNTRIES);
}


/* return next map refresh time: if rotating use getMapRotationPeriod() else the given interval
 */
time_t nextMapUpdate (int interval)
{
    time_t next_t = myNow();
    if (mapIsRotating())
        next_t += getMapRotationPeriod();
    else
        next_t += interval;
    return (next_t);
}

/* update core_map per map_rotset.
 * N.B. we assume mapIsRotating() is rtue.
 */
void rotateNextMap()
{
    // rotate to the "next" CoreMaps bit after core_map
    int new_cm = -1;
    for (int i = 1; i < CM_N; i++) {
        int ci = (core_map + i) % CM_N;
        if (IS_CMROT(ci)) {
            new_cm = ci;
            break;
        }
    }
    if (new_cm < 0)
        fatalError ("Bogus map rotation set: 0x%x\n", map_rotset);
    core_map = (CoreMaps) new_cm;
}

