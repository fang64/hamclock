/* set and get debug levels via RESTful web commands.
 */

#include "HamClock.h"


// system name and current level
typedef struct {
    const char *name;                   // fifo name
    int level;                          // current level
} DebugLevel;


#define X(a,b) {b, 0},                  // expands DEBUG_SUBSYS to each name string and initial level
static DebugLevel db_level[DEBUG_SUBSYS_N] = {
    DEBUG_SUBSYS
};
#undef X

bool setDebugLevel (const char *name, int level)
{
    for (int i = 0; i < DEBUG_SUBSYS_N; i++) {
        if (strcasecmp (name, db_level[i].name) == 0) {
            db_level[i].level = level;
            Serial.printf ("DEBUG: set %s=%d\n", db_level[i].name, db_level[i].level);
            return (true);
        }
    }
    Serial.printf ("DEBUG: set unknown %s\n", name);
    return (false);
}

void prDebugLevels (WiFiClient &client, int indent)
{
    char line[100];

    snprintf (line, sizeof(line), "%-*s%s=%d\n", indent, "Debugs", db_level[0].name, db_level[0].level);
    client.print (line);
    for (int i = 1; i < DEBUG_SUBSYS_N; i++) {
        snprintf (line, sizeof(line), "%*s%s=%d\n", indent, "", db_level[i].name, db_level[i].level);
        client.print (line);
    }
}


/* return whether to activate the given debug subsys at the given level of detail.
 */
bool debugLevel (DebugSubsys s, int level)
{
    return (db_level[s].level >= level);
}
