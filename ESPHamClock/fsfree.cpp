/* file system utility functions.
 */


#include "HamClock.h"


// checkFSFull() configuration
#define CHECK_MS        (60000U)                // check interval, millis
#define MIN_FREE        (50*1000000LL)          // min free bytes


/* OS-dependent FS info
 */


#if defined(_IS_LINUX)

#include <sys/statvfs.h>

/* pass back bytes capacity and used of the dir containing our working directory.
 * return whether successful.
 */
bool getFSSize (long long &cap, long long &used)
{
    const char *dir = our_dir.c_str();
    struct statvfs buf;
    if (statvfs (dir, &buf) < 0) {
        Serial.printf ("Can not get info for FS containing %s: %s\n", dir, strerror(errno));
        return (false);
    }
    cap = buf.f_bsize * buf.f_blocks;
    used = cap - buf.f_bsize * buf.f_bfree;
    return (true);
}



#elif defined (_IS_FREEBSD) || defined(_IS_APPLE)

#include <sys/param.h>
#include <sys/mount.h>

/* pass back bytes capacity and used of the dir containing our working directory.
 * return whether successful.
 */
bool getFSSize (long long &cap, long long &used)
{
    const char *dir = our_dir.c_str();
    struct statfs buf;
    if (statfs (dir, &buf) < 0) {
        Serial.printf ("Can not get info for FS containing %s: %s\n", dir, strerror(errno));
        return (false);
    }
    cap = buf.f_bsize * buf.f_blocks;
    used = cap - buf.f_bsize * buf.f_bfree;
    return (true);
}


#else

/* don't know how to get fs info
 */
bool getFSSize (long &cap, long &used)
{
    printf ("hello???\n");              // TODO
    return (false);
}

#endif


/* throw fatal error if disk containing our working dir is nearly full
 */
void checkFSFull(void)
{
    static uint32_t check_ms;

    if (timesUp (&check_ms, CHECK_MS)) {
        long long cap, used;
        if (getFSSize (cap, used)) {
            long long n_free = cap - used;
            if (n_free < MIN_FREE)
                fatalError ("Disk is nearly full: %lld / %lld = %g%%", used, cap, 100.0*used/cap);
        }
    }
}


/* qsort-style compare two FS_Info by name
 */
static int FSInfoNameQsort (const void *p1, const void *p2)
{
        return (strcmp (((FS_Info *)p1)->name, ((FS_Info *)p2)->name));
}


/* produce a listing of the map storage directory.
 * return malloced array and malloced name -- caller must free() -- else NULL if not available.
 */
FS_Info *getConfigDirInfo (int *n_info, char **fs_name, long long *fs_size, long long *fs_used)
{
        // get basic fs info
        long long cap, used;
        if (!getFSSize (cap, used))
            return (NULL);

        // pass back basic info
        const char *wd = our_dir.c_str();
        *fs_name = strdup (wd);
        *fs_size = cap;
        *fs_used = used;

        // build each entry
        FS_Info *fs_array = NULL;
        int n_fs = 0;
        DIR *dirp = opendir (wd);
        if (!dirp)
            fatalError ("can not open %s: %s", wd, strerror(errno));

        struct dirent *dp;
        while ((dp = readdir(dirp)) != NULL) {

            // extend array
            fs_array = (FS_Info *) realloc (fs_array, (n_fs+1)*sizeof(FS_Info));
            if (!fs_array)
                fatalError ("alloc file name %d failed", n_fs);
            FS_Info *fip = &fs_array[n_fs++];

            // store name
            quietStrncpy (fip->name, dp->d_name, sizeof(fip->name));

            // get full name for stat()
            char full[2000];
            snprintf (full, sizeof(full), "%s/%s", wd, dp->d_name);
            struct stat sbuf;
            if (stat (full, &sbuf) < 0) {
                Serial.printf ("%s: %s\n", full, strerror(errno));
                continue;
            }

            // store UNIX time
            time_t t = fip->t0 = sbuf.st_mtime;

            // store as handy date string too
            int yr = year(t);
            int mo = month(t);
            int dy = day(t);
            int hr = hour(t);
            int mn = minute(t);
            int sc = second(t);
            snprintf (fip->date, sizeof(fip->date), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                yr, mo, dy, hr, mn, sc);

            // store file length
            fip->len = sbuf.st_size;
        }

        closedir (dirp);

        // nice sorted order
        qsort (fs_array, n_fs, sizeof(FS_Info), FSInfoNameQsort);

        // ok
        *n_info = n_fs;
        return (fs_array);
}
