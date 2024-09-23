/* open a cached file backed by a backend file.
 */

#include "HamClock.h"

FILE *openCachedFile (const char *fn, const char *url, long max_age)
{
    // try local first
    char local_path[1000];
    snprintf (local_path, sizeof(local_path), "%s/%s", our_dir.c_str(), fn);
    FILE *fp = fopen (local_path, "r");
    if (fp) {
        // file exists, now check the age
        struct stat sbuf;
        if (fstat (fileno(fp), &sbuf) == 0 && myNow() - sbuf.st_mtime < max_age) {
            // new enough
            Serial.printf ("Cache: using %s cache: age %ld < %ld\n", fn,
                                                (long)(myNow() - sbuf.st_mtime), max_age);
            return (fp);
        } else {
            fclose (fp);
        }
    }

    // download
    WiFiClient cache_client;
    Serial.println (url);
    if (wifiOk() && cache_client.connect(backend_host, backend_port)) {

        updateClocks(false);

        // query web page
        httpHCGET (cache_client, backend_host, url);

        // skip header
        if (!httpSkipHeader (cache_client)) {
            Serial.printf ("Cache: %s head short\n", url);
            goto out;
        }

        // start new temp file near first so it can be renamed
        char tmp_path[1000];
        snprintf (tmp_path, sizeof(tmp_path), "%s/x.%s", our_dir.c_str(), fn);
        fp = fopen (tmp_path, "w");
        if (!fp) {
            Serial.printf ("Cache: %s: x.%s\n", fn, strerror(errno));
            goto out;
        }

        // copy
        char buf[1024];
        while (getTCPLine (cache_client, buf, sizeof(buf), NULL))
            fprintf (fp, "%s\n", buf);

        // check io before closing
        bool ok = !ferror (fp);
        fclose (fp);

        // rename if ok, else remove tmp
        if (ok && rename (tmp_path, local_path) == 0) {
            Serial.printf ("Cache: download %s ok\n", url);
        } else {
            (void) unlink (tmp_path);
            Serial.printf ("Cache: download %s failed\n", url);
        }
    }

  out:
    
    // insure socket is closed
    cache_client.stop();

    // try again whether new or reusing old
    fp = fopen (local_path, "r");
    if (fp)
        return (fp);
    Serial.printf ("Cache: %s: %s\n", fn, strerror(errno));
    return (NULL);
}
