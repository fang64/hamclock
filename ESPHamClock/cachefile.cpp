/* open a cached file backed by a backend file.
 */

#include "HamClock.h"

FILE *openCachedFile (const char *fn, const char *url, int max_age, int min_size)
{
    // try local first
    char local_path[1000];
    snprintf (local_path, sizeof(local_path), "%s/%s", our_dir.c_str(), fn);
    FILE *fp = fopen (local_path, "r");
    if (fp) {
        // file exists, now check the age and finite size
        struct stat sbuf;
        if (fstat (fileno(fp), &sbuf) == 0 && myNow() - sbuf.st_mtime < max_age && sbuf.st_size > min_size) {
            Serial.printf ("Cache: using cached %s of %s: age %ld < %d\n", fn, url,
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

/* remove files that contain the given string and older than 2x the given age in seconds.
 */
void cleanCache (const char *contains, int max_age)
{
    // 2x just to let it linger
    max_age *= 2;

    // open our working directory
    DIR *dirp = opendir (our_dir.c_str());
    if (dirp == NULL) {
        Serial.printf ("Cache: %s: %s\n", our_dir.c_str(), strerror(errno));
        return;
    }

    // malloced list of malloced names to be removed (so we don't modify dir while scanning)
    typedef struct {
        char *ffn;                                  // malloced full file name
        char *bfn;                                  // malloced base name
        long age;                                   // age, seconds
    } RMFile;
    RMFile *rm_files = NULL;                        // malloced list
    int n_rm = 0;                                   // n in list

    // add matching files to rm_files
    const time_t now = myNow();
    struct dirent *dp;
    while ((dp = readdir(dirp)) != NULL) {
        if (strstr (dp->d_name, contains)) {
            // file name matches now check age
            char fpath[10000];
            struct stat sbuf;
            snprintf (fpath, sizeof(fpath), "%s/%s", our_dir.c_str(), dp->d_name);
            if (stat (fpath, &sbuf) < 0)
                Serial.printf ("Cache: %s: %s\n", fpath, strerror(errno));
            else {
                long age = now - sbuf.st_mtime;      // last modified time
                if (age > max_age) {
                    // add to list to be removed
                    rm_files = (RMFile *) realloc (rm_files, (n_rm+1)*sizeof(RMFile));
                    rm_files[n_rm].ffn = strdup (fpath);
                    rm_files[n_rm].bfn = strdup (dp->d_name);
                    rm_files[n_rm].age = age;
                    n_rm++;
                }
            }
        }
    }
    closedir (dirp);

    // remove files and clean up rm_files along the way
    for (int i = 0; i < n_rm; i++) {
        RMFile &rmf = rm_files[i];
        Serial.printf ("Cache: rm %s %ld > %d s old\n", rmf.bfn, rmf.age, max_age);
        if (unlink (rmf.ffn) < 0)
            Serial.printf ("Cache: unlink(%s): %s\n", rmf.ffn, strerror(errno));
        free (rmf.ffn);
        free (rmf.bfn);
    }
    free (rm_files);
}
