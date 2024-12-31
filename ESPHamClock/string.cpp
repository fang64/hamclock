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
    // empty needle is always considered a match to the entire haystack
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

/* split str into as many as max_tokens whitespace-delimited tokens.
 * tokens[i] points to the i'th token within str, which has been null-terminated IN PLACE.
 * return number of tokens.
 * N.B any additional tokens are silently lost.
 */
int strtokens (char *str, char *tokens[], int max_tokens)
{
    int n_tokens = 0;
    char *string = str;
    char *token;

    while (n_tokens < max_tokens && (token = strtok (string, " \t")) != NULL) {
        tokens[n_tokens++] = token;
        string = NULL;
    }

    return (n_tokens);
}


/* remove leading, multiple-embedded and trailing white space IN PLACE; return str.
 */
char *strtrim (char *str)
{
    if (!str)
        return (NULL);

    char *to = str;
    char *from = str;
    while (isspace(*from))
        from++;
    for ( ; *from; from++)
        if (!isspace (*from) || (from[1] && !isspace(from[1])))
            *to++ = *from;
    *to = '\0';
    return (str);
}

/* return whether the given string contains at least one character as defined by the ctype.h test function.
 */
static bool strHasCType (const char *s, int (fp)(int))
{
    for (; *s; s++)
        if ((*fp) (*s))
            return (true);
    return (false);
}

/* return whether the given string contains at least one alpha character
 */
bool strHasAlpha (const char *s)
{
    return (strHasCType (s, isalpha));
}

/* return whether the given string contains at least one digit.
 */
bool strHasDigit (const char *s)
{
    return (strHasCType (s, isdigit));
}

/* return whether the given string contains at least one punct
 */
bool strHasPunct (const char *s)
{
    return (strHasCType (s, ispunct));
}

/* return whether the given string contains at least one space
 */
bool strHasSpace (const char *s)
{
    return (strHasCType (s, isspace));
}

/* strncpy that insures "to" has EOS (and avoids the g++ fussing)
 */
void quietStrncpy (char *to, const char *from, int len)
{
    snprintf (to, len, "%.*s", len-1, from);
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

/* handy means to break time interval into HHhMM or MM:SS given dt in hours.
 * return each component and the appropriate separate, the expectation is the time
 * can then be printed using *printf (%02d%c%02d", a, sep, b);
 */
void formatSexa (float dt_hrs, int &a, char &sep, int &b)
{
    if (dt_hrs < 1) {
        // next event is less than 1 hour away, show time in MM:SS
        dt_hrs *= 60;                           // dt_hrs is now minutes
        sep = ':';
    } else {
        // next event is at least an hour away, show time in HH:MM
        sep = 'h';
    }

    // same hexa conversion either way
    a = (int)dt_hrs;
    b = (int)((dt_hrs-(int)dt_hrs)*60);
}

/* format a representation of the given age into line[] exactly cols chars long.
 * return line.
 */
char *formatAge (time_t age, char *line, int line_l, int cols)
{
    // eh?
    if (age < 0) {
        Serial.printf ("formatAge(%ld,%d) resetting negative age to zero\n", (long)age, cols);
        age = 0;
    }

    switch (cols) {

    case 1:

        // show a few symbols
        if (age < 10*60)
            snprintf (line, line_l, " ");
        else if (age < 60*60)
            snprintf (line, line_l, "m");
        else
            snprintf (line, line_l, "h");
        break;

    case 2:

        // show minutes up thru 59 else hrs up thru 9 then +
        if (age < 60*60-30)
            snprintf (line, line_l, "%2d", (int)(age/60));
        else if (age < 10*60*60-1800)
            snprintf (line, line_l, "%dh", (int)(age/(60*60)));
        else
            strcpy (line, "+");
        break;

    case 3:

        // show 2 digits then s, m, h, d, M, y
        if (age < 60) {
            snprintf (line, line_l, "%2lds", (long)age);
        } else if (age < (60*60)) {
            snprintf (line, line_l, "%2ldm", (long)age/60);
        } else if (age < 3600*24) {
            snprintf (line, line_l, "%2ldh", (long)age/3600);
        } else if (age < 100L*3600*24) {
            snprintf (line, line_l, "%2ldd", (long)age/(3600L*24));
        } else if (age < 365L*3600*24) {
            snprintf (line, line_l, "%2ldM", (long)age/(31L*3600*24));
        } else {
            snprintf (line, line_l, "%2ldy", (long)age/(365L*3600*24));
        }

        break;

    case 4:

        // show seconds thru years(!)
        if (age < 60) {
            snprintf (line, line_l, "%3lds", (long)age);
        } else if (age < (60*60)) {
            snprintf (line, line_l, "%3ldm", (long)age/60);
        } else if (age < (24*60*60-1800)) {
            float hours = age/3600.0F;
            if (hours < 9.95F)
                snprintf (line, line_l, "%3.1fh", hours);
            else
                snprintf (line, line_l, "%3.0fh", hours);
        } else if (age < 3600L*(24*365-12)) {
            float days = age/(3600*24.0F);
            if (days < 9.95F)
                snprintf (line, line_l, "%3.1fd", days);
            else
                snprintf (line, line_l, "%3.0fd", days);
        } else {
            float years = age/(3600*24*365.0F);
            if (years < 9.95F)
                snprintf (line, line_l, "%3.1fy", years);
            else
                snprintf (line, line_l, "%3.0fy", years);
        }
        break;

    default:
        fatalError("formatAge bad cols %d", cols);
        break;
    }

    return (line);
}
