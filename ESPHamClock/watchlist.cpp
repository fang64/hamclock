/* implement a watch list of prefixes and frequency ranges.
 * the WatchList class compiles a watch list spec then can determine whether a DXSpot qualifies.
 */



#include "HamClock.h"


/* class to manage watch lists
 */
class WatchList {

    private:

        int verbose;

        typedef struct {
            float min_MHz, max_MHz;
        } FreqRange;

        typedef char *Prefix;

        typedef struct {
            FreqRange *freqs;
            int n_freqs;
            Prefix *prefs;
            int n_prefs;
        } OneSpec;

        OneSpec *specs;
        int n_specs;
        bool finished_spec;             // flag set when spec is closed

        void startNewSpec()
        {
            specs = (OneSpec *) realloc (specs, (n_specs + 1) * sizeof(OneSpec));
            OneSpec *sp = &specs[n_specs++];
            memset (sp, 0, sizeof(*sp));
        }

        /* return index of ham_bands[] matching the first name_l chars in name[], if any
         */
        HamBandSetting getHamBand (const char *name, int name_l)
        {
            // make null terminated copy for complete test against bands[].name, eg 2 vs 20
            char bz[50];
            snprintf (bz, sizeof(bz), "%.*s", name_l, name);
            if (verbose > 1)
                Serial.printf ("WLIST: finding band '%s'\n", bz);

            for (int i = 0; i < HAMBAND_N; i++)
                if (strcmp (bz, ham_bands[i].name) == 0)
                    return ((HamBandSetting)i);
            return (HAMBAND_NONE);
        }

        /* start new spec if new or flagged earlier
         */
        void checkNewSpec(void)
        {
            if (finished_spec || specs == NULL) {
                startNewSpec();
                finished_spec = false;
            }
        }


        /* add the given prefix
         * return whether it looks reasonable.
         */
        bool addPrefix (const char *prefix, char ynot[], size_t ynot_len)
        {
            if (verbose > 1)
                Serial.printf ("WLIST: adding pref '%s'\n", prefix);

            size_t p_len = strlen(prefix);

            // just a number?
            if (strspn (prefix, "0123456789") == p_len) {
                snprintf (ynot, ynot_len, "lone number");
                if (verbose)
                    Serial.printf ("WLIST: pref can not be just a number: '%s'\n", prefix);
                return (false);
            }

            // weird chars?
            for (size_t i = 0; i < p_len; i++) {
                if (!isalnum (prefix[i]) && prefix[i] != '/') {
                    snprintf (ynot, ynot_len, "bad char %c", prefix[i]);
                    if (verbose)
                        Serial.printf ("WLIST: pref contains bogus character: '%s'\n", prefix);
                    return (false);
                }
            }

            // slash other than end?
            const char *slash = strchr (prefix, '/');
            if (slash && slash[1] != '\0') {
                snprintf (ynot, ynot_len, "bad /");
                if (verbose)
                    Serial.printf ("WLIST: pref / can only be at end: '%s'\n", prefix);
                return (false);
            }
            
            // ok!

            // start new spec if flagged earlier or we are first
            checkNewSpec();

            // grow list and add malloced copy
            OneSpec &s = specs[n_specs-1];
            s.prefs = (Prefix *) realloc (s.prefs, (s.n_prefs + 1) * sizeof(Prefix));
            s.prefs[s.n_prefs++] = strdup (prefix);

            return (true);
        }

        /* add the given explicit frequency range.
         * return whether reasonable.
         */
        bool addFreqs (float min_MHz, float max_MHz)
        {
            if (verbose > 1)
                Serial.printf ("WLIST: adding freqs '%g %g'\n", min_MHz, max_MHz);

            // backwards?
            if (min_MHz >= max_MHz)
                return (false);

            // crazy values?
            if (min_MHz < 1 || min_MHz > 200 || max_MHz < 1 || max_MHz > 200)
                return (false);

            // start new spec if flagged earlier or we are first
            checkNewSpec();

            OneSpec &s = specs[n_specs-1];
            s.freqs = (FreqRange *) realloc (s.freqs, (s.n_freqs + 1) * sizeof(FreqRange));
            FreqRange &f = s.freqs[s.n_freqs++];
            f.min_MHz = min_MHz;
            f.max_MHz = max_MHz;

            return (true);
        }

        /* add the given frequency range of the form MHz1-MHz2.
         * return whether proper syntax and reasonable range.
         */
        bool addFreqRange (const char *freq_range)
        {
            if (verbose > 1)
                Serial.printf ("WLIST: adding freq range '%s'\n", freq_range);

            // split at - and insure nothing else around
            char *end;
            float min_MHz = strtod (freq_range, &end);
            if (end == freq_range || *end != '-')
                return (false);
            char *start2 = end+1;
            float max_MHz = strtod (start2, &end);
            if (end == start2 || *end != '\0')
                return (false);

            return (addFreqs (min_MHz, max_MHz));
        }

        /* indicate the current specification is complete, prepare to begin another.
         */
        void finishedSpec(void)
        {
            if (verbose > 1 && !finished_spec)
                Serial.printf ("WLIST: spec complete\n");
            finished_spec = true;
        }

        /* reclaim and reset all storage references
         */
        void resetStorage()
        {
            if (specs) {
                for (int i = 0; i < n_specs; i++) {
                    OneSpec &s = specs[i];
                    if (s.freqs) {
                        free (s.freqs);
                        s.freqs = NULL;
                        s.n_freqs = 0;
                    }
                    if (s.prefs) {
                        for (int j = 0; j < s.n_prefs; j++)
                            free (s.prefs[j]);
                        free (s.prefs);
                        s.prefs = NULL;
                        s.n_prefs = 0;
                    }
                }
                free (specs);
                specs = NULL;
                n_specs = 0;
            }
        }


    public:

        /* constructor: init state
         */
        WatchList (void)
        {
            specs = NULL;
            n_specs = 0;
            verbose = 1;
            finished_spec = false;
        }

        /* destructor: release storage
         */
        ~WatchList (void)
        {
            resetStorage();
        }

        /* set verbose level
         */
        void setVerbosity (int v)
        {
            verbose = v;
        }


        /* print the watch list data structure components
         */
        void print (const char *name)
        {
            Serial.printf ("WLIST %s: %d specs:\n", name, n_specs);
            for (int i = 0; i < n_specs; i++) {
                Serial.printf ("  Spec %d:\n", i+1);
                OneSpec &s = specs[i];
                Serial.printf ("    %d Prefixes:", s.n_prefs);
                for (int j = 0; j < s.n_prefs; j++)
                    printf (" %s%s", j > 0 ? "or " : "", s.prefs[j]);
                printf ("\n");
                Serial.printf ("    %d Freq ranges:", s.n_freqs);
                for (int j = 0; j < s.n_freqs; j++)
                    printf (" %s%g-%g", j > 0 ? "or " : "", s.freqs[j].min_MHz, s.freqs[j].max_MHz);
                printf ("\n");
            }
        }

        /* determine whether the given spot is allowed.
         * check each spec:
         *   if spec contains any frequencies spot freq must lie within at least one.
         *   if spec contains any prefixes spot call must match at least one.
         */
        bool onList (const DXSpot &dx)
        {
            // always work with the dx portion if split
            char home_call[NV_CALLSIGN_LEN];
            char dx_call[NV_CALLSIGN_LEN];
            splitCallSign (dx.tx_call, home_call, dx_call);

            // decide whether this is really a proper split call (just containing / is not proof)?
            bool two_part = strcmp (home_call, dx_call) != 0;

            // OneSpec stores freqs in MHz
            float dx_MHz = dx.kHz * 1e-3F;

            bool match = false;

            for (int i = 0; i < n_specs; i++) {
                OneSpec &s = specs[i];

                // check whether dx_call for matching pref and freq -- consider lack as a match
                bool match_pref = s.n_prefs == 0;
                bool match_freq = s.n_freqs == 0;

                // match a pref?
                for (int j = 0; j < s.n_prefs; j++) {
                    // only match against the dx portion of a two-part call
                    const char *pref = s.prefs[j];
                    const char *pref_slash = strchr (pref, '/');
                    size_t pref_len = pref_slash ? pref_slash - pref : strlen(pref);    // sans / if any
                    bool dx_match = strncasecmp (dx_call, pref, pref_len) == 0;
                    bool home_match = strncasecmp (home_call, pref, pref_len) == 0;
                    if ((pref_slash && two_part && dx_match) || (!pref_slash && home_match)) {
                        match_pref = true;
                        break;
                    }
                }

                // match a freq range?
                for (int j = 0; j < s.n_freqs; j++) {
                    if (s.freqs[j].min_MHz <= dx_MHz && dx_MHz <= s.freqs[j].max_MHz) {
                        match_freq = true;
                        break;
                    }
                }

                // we have match if both
                if (match_pref && match_freq) {
                    match = true;
                    break;
                }
            }

            if (verbose > 1)
                Serial.printf ("WLIST: checking %s (%s %s %g): %smatch\n",
                                        dx.tx_call, home_call, dx_call, dx_MHz,
                                        match ? "" : "no ");

            return (match);
        }

        /* try to compile the given watch list specification.
         * if trouble return false with short excuse in ynot[].
         * lists are composed of groups of specs consisting of frequency ranges, band names and prefixes.
         */
        bool compile (const char *wl_specs, char ynot[], size_t n_ynot)
        {
            // fresh start
            resetStorage();

            // delimiters, implicitly including EOS
            static char delims[] = " ,";

            // walk the spec looking for each token and its delimiter
            // N.B. rely on loop body to break at EOS
            size_t n_walk;
            for (const char *wl_walk = wl_specs; true; wl_walk += n_walk) {

                // next token ends at next delim
                n_walk = strcspn (wl_walk, delims);

                // skip if empty, but note EOS or when ',' indicates this section is finished
                if (n_walk == 0) {
                    char delim = wl_walk[0];
                    if (delim == '\0') {
                        finishedSpec();
                        break;
                    }
                    if (delim == ',')
                        finishedSpec();
                    n_walk = 1;                // skip delim
                    continue;
                }

                // handy token as a separate trimmed upper-case string
                char token[50];
                snprintf (token, sizeof(token), "%.*s", (int)n_walk, wl_walk);
                size_t n_token = strlen (strtrim (token));
                strtoupper (token);

                // if contains '-' then freq range?
                if (strchr (token, '-')) {
                    if (!addFreqRange (token)) {
                        snprintf (ynot, n_ynot, "range? %s", token);
                        return (false);
                    }
                    continue;
                }

                // if lone trailing M preceded by all digits then a band name?
                if (token[n_token-1] == 'M' && strspn (token, "0123456789") == n_token - 1) {
                    HamBandSetting b = getHamBand (token, n_token-1); // drop M
                    if (b == HAMBAND_NONE) {
                        snprintf (ynot, n_ynot, "band? %s", token);
                        return (false);
                    }
                    if (!addFreqs (ham_bands[b].min_kHz*1e-3F, ham_bands[b].max_kHz*1e-3F)) {
                        snprintf (ynot, n_ynot, "freqs? %s", token);
                        return (false);
                    }
                    continue;
                }

                // probably a prefix
                if (!addPrefix (token, ynot, n_ynot))
                    return (false);
            }

            // disallow empty
            if (n_specs == 0) {
                snprintf (ynot, n_ynot, "empty");
                return (false);
            }

            // yah!
            return (true);
        }

};


/* one per list
 */
static WatchList wlists[WLID_N];


/* return whether wl_id is safe to use
 */
bool wlIdOk (WatchListId wl_id)
{
    return (wl_id >= 0 && wl_id < WLID_N);
}


/* decide how, or whether, to display the given DXSpot with respect to the given watch list
 */
WatchListShow checkWatchListSpot (WatchListId wl_id, const DXSpot &dxsp)
{
    if (!wlIdOk(wl_id))
        fatalError ("checkWatchList bogus %d", (int)wl_id);

    wlists[wl_id].setVerbosity (gimbal_trace_level);

    switch (getWatchListState (wl_id, NULL)) {
    case WLA_OFF:
        return (WLS_NORM);              // spot always qualfies so no need to check
    case WLA_FLAG:
        return (wlists[wl_id].onList(dxsp) ? WLS_HILITE : WLS_NORM);
    case WLA_ONLY:
        return (wlists[wl_id].onList(dxsp) ? WLS_NORM : WLS_NO);
    case WLA_NOT:
        return (wlists[wl_id].onList(dxsp) ? WLS_NO : WLS_NORM);
    case WLA_N:
        break;
    }

    // lint
    return (WLS_NORM);
}

/* compile the given string on the given watch list.
 * return false with brief reason if trouble.
 */
bool compileWatchList (WatchListId wl_id, const char *new_wlstr, char ynot[], size_t n_ynot)
{
    if (!wlIdOk(wl_id))
        fatalError ("compileWatchList bogus is %d", (int)wl_id);

    bool ok = wlists[wl_id].compile (new_wlstr, ynot, n_ynot);

    Serial.printf ("WLIST %s: compiled %s\n", getWatchListName(wl_id), new_wlstr);
    if (ok)
        wlists[wl_id].print (getWatchListName(wl_id));
    else
        Serial.printf ("WLIST %s: %s\n", getWatchListName(wl_id), ynot);

    return (ok);
}

/* like compileWatchList but uses a temporary anonymous WatchList just to capture any compile errors.
 * N.B. _menu_text->label contains the WL state name, ->text contains the watchlist; feign success if WLA_OFF.
 */
bool compileTestWatchList (struct _menu_text *tfp, char ynot[], size_t n_ynot)
{
    // just say yes if state is Off
    if (lookupWatchListState(tfp->label) == WLA_OFF)
        return (true);

    // temporary compiler
    WatchList wl;
    
    bool ok = wl.compile (tfp->text, ynot, n_ynot);

    if (ok)
        wl.print ("anon");
    else
        Serial.printf ("WLIST anon: state %s compiled %s: %s\n", tfp->label, tfp->text, ynot);

    return (ok);
}


/* handy consolidation of setting up a MENU_TEXT for editing watch lists.
 * N.B. caller must free mi.text
 */
void setupWLMenuText (WatchListId wl_id, MenuText &mt, const SBox &box, char wl_state[WLA_MAXLEN])
{
    memset (&mt, 0, sizeof(mt));                                // easy defaults
    getWatchListState (wl_id, wl_state);                        // get current state name
    getWatchList (wl_id, &mt.text, &mt.t_mem);                  // N.B. must free mt.text
    mt.label = wl_state;                                        // mutable label memory
    mt.l_mem = WLA_MAXLEN;                                      // max label len
    mt.text_fp = compileTestWatchList;                          // watchlist test
    mt.label_fp = rotateWatchListState;                         // label cycler
    mt.to_upper = true;                                         // convert all input to all upper case
    mt.c_pos = mt.w_pos = 0;                                    // start at left
    mt.w_len = box.w/6 - WLA_MAXLEN - 2;                        // n chars to display, use most of the box
}


/* remove all extraneous blanks and commas IN PLACE from the given watch list specification.
 * this means all leading, trailing and consecutive blanks or commas.
 * return s (because good stuff has been shifted to the beginning).
 */
char *wlCompress (char *s)
{
    #define COMMA    ','
    #define BLANK    ' '
    #define EOS      '\0'
    #define SAVE(c)  *s_to++ = (c)

    typedef enum {
        WLC_SEEKLD,             // seeking left delimiter
        WLC_SEEKRD,             // seeking right delimiter
        WLC_INTOKEN,            // in token chars
    } WLCState;
    WLCState wls = WLC_SEEKLD;

    bool saw_comma = false;
    char *s_from = s;
    char *s_to = s;

    for (char c = *s_from; c != EOS; c = *++s_from) {
        switch (wls) {
        case WLC_SEEKLD:
            if (c != BLANK && c != COMMA) {
                SAVE(c);
                wls = WLC_INTOKEN;
            }
            break;
        case WLC_SEEKRD:
            if (c == COMMA) {
                saw_comma = true;
            } else if (c != BLANK) {
                if (saw_comma) {
                    SAVE(COMMA);
                    saw_comma = false;
                } else
                    SAVE(BLANK);
                SAVE(c);
                wls = WLC_INTOKEN;
            }
            break;
        case WLC_INTOKEN:
            if (c == COMMA) {
                saw_comma = true;
                wls = WLC_SEEKRD;
            } else if (c == BLANK) {
                saw_comma = false;
                wls = WLC_SEEKRD;
            } else {
                SAVE(c);
            }
            break;
        }
    }
    *s_to = EOS;

    return (s);

    #undef COMMA
    #undef BLANK
    #undef EOS
    #undef SAVE
}
