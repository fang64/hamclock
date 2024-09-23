/* misc string utils
 */

#include "HamClock.h"


/* string hash, commonly referred to as djb2
 * see eg http://www.cse.yorku.ca/~oz/hash.html
 */
uint32_t stringHash (const char *str)
{
    uint32_t hash = 5381;
    int c;

    while ((c = *str++) != '\0')
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

/* convert any upper case letter in str to lower case IN PLACE
 */
char *strtolower (char *str)
{
    char *str0 = str;
    for (char c = *str; c != '\0'; c = *++str)
        *str = tolower(c);
    return (str0);
}

/* convert any lower case letter in str to upper case IN PLACE
 */
char *strtoupper (char *str)
{
    char *str0 = str;
    for (char c = *str; c != '\0'; c = *++str)
        *str = toupper(c);
    return (str0);
}

/* my own portable implementation of strcasestr
 */
const char *strcistr (const char *haystack, const char *needle)
{
    if (needle[0] == '\0')
        return (haystack);
    for (; *haystack; haystack++) {
        const char *h = haystack;
        for (const char *n = needle; ; n++, h++) {
            if (*n == '\0')
                return (haystack);
            if (toupper(*h) != toupper(*n))
                break;
        }
    }
    return (NULL);
}

/* remove leading and trailing white space IN PLACE, return new beginning.
 */
char *strtrim (char *str)
{
    if (!str)
        return (str);

    // skip leading blanks
    char *blank = str;
    while (isspace(*blank))
        blank++;

    // copy from first-nonblank back to beginning
    size_t sl = strlen (blank);
    if (blank > str)
        memmove (str, blank, sl+1);             // include \0

    // skip back from over trailing blanks
    while (sl > 0 && isspace(str[sl-1]))
        str[--sl] = '\0';

    // return same starting point
    return (str);
}


/* return the bounding box of the given string in the current font.
 */
void getTextBounds (const char str[], uint16_t *wp, uint16_t *hp)
{
    int16_t x, y;
    tft.getTextBounds ((char*)str, 100, 100, &x, &y, wp, hp);
}

/* return width in pixels of the given string in the current font
 */
uint16_t getTextWidth (const char str[])
{
    uint16_t w, h;
    getTextBounds (str, &w, &h);
    return (w);
}

/* shorten str IN PLACE as needed to be less that maxw pixels wide.
 * return final width in pixels.
 */
uint16_t maxStringW (char *str, uint16_t maxw)
{
    uint8_t strl = strlen (str);
    uint16_t bw = 0;

    while (strl > 0 && (bw = getTextWidth(str)) >= maxw)
        str[--strl] = '\0';

    return (bw);
}

/* expand any ENV in the given file.
 * return malloced result -- N.B. caller must free!
 */
char *expandENV (const char *fn)
{
    // build fn with any embedded info expanded
    char *fn_exp = NULL;
    int exp_len = 0;
    for (const char *fn_walk = fn; *fn_walk; fn_walk++) {

        // check for embedded terms

        char *embed_value = NULL;
        if (*fn_walk == '$') {
            // expect ENV all caps, digits or _
            const char *dollar = fn_walk;
            const char *env = dollar + 1;
            while (isupper(*env) || isdigit(*env) || *env=='_')
                env++;
            int env_len = env - dollar - 1;             // env now points to first invalid char; len w/o EOS
            StackMalloc env_mem(env_len+1);             // +1 for EOS
            char *env_tmp = (char *) env_mem.getMem();
            memcpy (env_tmp, dollar+1, env_len);
            env_tmp[env_len] = '\0';
            embed_value = getenv(env_tmp);
            fn_walk += env_len;

        } else if (*fn_walk == '~') {
            // expand ~ as $HOME
            embed_value = getenv("HOME");
            // fn_walk++ in for loop is sufficient
        }

        // append to fn_exp
        if (embed_value) {
            // add embedded value to fn_exp
            int val_len = strlen (embed_value);
            fn_exp = (char *) realloc (fn_exp, exp_len + val_len);
            memcpy (fn_exp + exp_len, embed_value, val_len);
            exp_len += val_len;
        } else {
            // no embedded found, just add fn_walk to fn_exp
            fn_exp = (char *) realloc (fn_exp, exp_len + 1);
            fn_exp[exp_len++] = *fn_walk;
        }
    }

    // add EOS
    fn_exp = (char *) realloc (fn_exp, exp_len + 1);
    fn_exp[exp_len++] = '\0';

    // ok
    return (fn_exp);
}
