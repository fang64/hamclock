/* manage cached text files.
 */

#include "HamClock.h"



/* return whether the given file is at least the minimum size
 */
static bool fileSizeOk (const char *path, int min_size)
{
    struct stat sbuf;
    if (stat (path, &sbuf) < 0)
        Serial.printf ("Cache: size stat(%s) %s\n", path, strerror(errno));
    else if (sbuf.st_size < min_size)
        Serial.printf ("Cache: %s too small %ld < %d\n", path, (long)sbuf.st_size, min_size);
    else {
        const char *basename = strrchr (path, '/');
        if (basename)
            path = basename + 1;
        Serial.printf ("Cache: %s size ok: %ld >= %d\n", path, (long)sbuf.st_size, min_size);
        return (true);
    }
    return (false);
}

/* return whether the given file is no older than the given age in seconds
 */
static bool fileAgeOk (const char *path, int max_age)
{
    struct stat sbuf;
    if (stat (path, &sbuf) < 0)
        Serial.printf ("Cache: age stat(%s) %s\n", path, strerror(errno));
    else {
        int age = myNow() - sbuf.st_mtime;
        if (age > max_age)
            Serial.printf ("Cache: %s too old %d > %d\n", path, age, max_age);
        else {
            const char *basename = strrchr (path, '/');
            if (basename)
                path = basename + 1;
            Serial.printf ("Cache: %s age ok: %d <= %d\n", path, age, max_age);
            return (true);
        }
    }
    return (false);
}


/* open the given local file or download fresh if too old or too small.
 * if download fails retain fn as long as it's large enough, tolerating too old.
 */
FILE *openCachedFile (const char *fn, const char *url, int max_age, int min_size)
{
    // try local first
    char fn_path[1000];
    snprintf (fn_path, sizeof(fn_path), "%s/%s", our_dir.c_str(), fn);
    FILE *fp = fopen (fn_path, "r");
    if (fp) {
        // file exists, now check the age and size
        if (fileSizeOk (fn_path, min_size) && fileAgeOk (fn_path, max_age)) {
            // still good!
            return (fp);
        } else {
            // open again after download
            fclose (fp);
            Serial.printf ("Cache: %s not suitable -- downloading %s\n", fn, url);
        }
    } else
        Serial.printf ("Cache: %s not found -- downloading %s\n", fn, url);

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

        // friendly
        if (fchown (fileno(fp), getuid(), getgid()) < 0)
            Serial.printf ("Cache: chown(%s,%d,%d) %s\n", tmp_path, getuid(), getgid(), strerror(errno));

        // download
        char buf[1024];
        bool io_ok = true;
        while (io_ok && getTCPLine (cache_client, buf, sizeof(buf), NULL)) {
            if (fprintf (fp, "%s\n", buf) < 1) {
                io_ok = false;
                Serial.printf ("Cache: write(%s) %s\n", tmp_path, strerror(errno));
            }
        }
        fclose (fp);

        // tmp replaces fn_path if io and size ok
        if (io_ok && fileSizeOk (tmp_path, min_size)) {
            if (rename (tmp_path, fn_path) == 0)
                Serial.printf ("Cache: fresh %s installed\n", fn);
            else
                Serial.printf ("Cache: rename(%s,%s) %s\n", tmp_path, fn_path, strerror(errno));
        }

        // clean up tmp
        if (access (tmp_path, F_OK) == 0) {
            if (unlink (tmp_path) < 0)
                Serial.printf ("Cache: unlink(%s): %s\n", tmp_path, strerror(errno));
        }
    }

  out:

    // insure socket is closed
    cache_client.stop();

    // open again but now tolerate too old if must
    fp = fopen (fn_path, "r");
    if (fp && fileSizeOk (fn_path, min_size))
        return (fp);

    Serial.printf ("Cache: updating %s failed\n", fn);
    return (NULL);
}

/* remove files that contain the given string and older than 2x the given age in seconds.
 * return whether any where removed.
 */
bool cleanCache (const char *contains, int max_age)
{
    // 2x just to let it linger
    max_age *= 2;

    // open our working directory
    DIR *dirp = opendir (our_dir.c_str());
    if (dirp == NULL) {
        Serial.printf ("Cache: %s: %s\n", our_dir.c_str(), strerror(errno));
        return (false);
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
    bool rm_any = false;
    for (int i = 0; i < n_rm; i++) {
        RMFile &rmf = rm_files[i];
        if (unlink (rmf.ffn) == 0) {
            Serial.printf ("Cache: rm %s %ld > %d s old\n", rmf.bfn, rmf.age, max_age);
            rm_any = true;
        } else {
            Serial.printf ("Cache: unlink(%s): %s\n", rmf.ffn, strerror(errno));
        }
        free (rmf.ffn);
        free (rmf.bfn);
    }
    free (rm_files);

    // return whether any were removed
    return (rm_any);
}
