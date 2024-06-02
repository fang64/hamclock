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
void strtolower (char *str)
{
    for (char c = *str; c != '\0'; c = *++str)
        if (isupper(c))
            *str = tolower(c);
}

/* convert any lower case letter in str to upper case IN PLACE
 */
void strtoupper (char *str)
{
    for (char c = *str; c != '\0'; c = *++str)
        if (islower(c))
            *str = toupper(c);
}


/* remove leading and trailing white space IN PLACE, return str.
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
