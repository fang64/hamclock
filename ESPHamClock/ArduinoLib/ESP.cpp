#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Arduino.h"
#include "ESP8266WiFi.h"
#include "ESP.h"

class ESP ESP;

ESP::ESP()
{
        sn = 0;
}

void ESP::restart(bool minus_K)
{
        // add non-persistent -K to our_argv if desired (ok if more than once)
        char **use_argv = our_argv;
        if (minus_K) {
            // copy our_argv to add -K
            int argc;
            for (argc = 0; our_argv[argc] != NULL; argc++)
                continue;
            use_argv = (char **) malloc ((argc+2)*sizeof(char*)); // +1 -k +1 NULL
            memcpy (use_argv, our_argv, argc*sizeof(char*));
            use_argv[argc++] = strdup("-K");    // dup just to avoid "char* = const char*" warning
            use_argv[argc] = NULL;
        }

        // log
        printf ("Restarting -- args will be:\n");
        for (int i = 0; use_argv[i] != NULL; i++)
            printf ("  argv[%d]: %s\n", i, use_argv[i]);
        printf ("see you there!\n\n");

        // close all but basic fd
        for (int i = 3; i < 100; i++)
            (void) ::close(i);

        // go
        execvp (use_argv[0], use_argv);

        printf ("execvp(%s): %s\n", use_argv[0], strerror(errno));
        exit(1);
}

/* try to get some sort of system serial number.
 * return 0xFFFFFFFF if unknown.
 */
uint32_t ESP::getChipId()
{
        // reuse once found
        if (sn)
            return (sn);

#if defined(_IS_LINUX)
        // try cpu serial number
        FILE *fp = fopen ("/proc/cpuinfo", "r");
        if (fp) {
            static const char serial[] = "Serial";
            char buf[1024];
            while (fgets (buf, sizeof(buf), fp)) {
                if (strncmp (buf, serial, sizeof(serial)-1) == 0) {
                    int l = strlen(buf);                        // includes nl
                    sn = strtoul (&buf[l-9], NULL, 16);         // 8 LSB
                    if (sn) {
                        // printf ("Found ChipId '%.*s' -> 0x%X = %u\n", l-1, buf, sn, sn);
                        break;
                    }
                }
            }
            fclose (fp);
            if (sn)
                return (sn);
        }
#endif

        // try MAC address
        std::string mac = WiFi.macAddress();
        unsigned int m1, m2, m3, m4, m5, m6;
        if (sscanf (mac.c_str(), "%x:%x:%x:%x:%x:%x", &m1, &m2, &m3, &m4, &m5, &m6) == 6) {
            sn = (m3<<24) + (m4<<16) + (m5<<8) + m6;
            // printf ("Found ChipId from MAC '%s' -> 0x%x = %u\n", mac.c_str(), sn, sn);
        } else {
            printf ("No ChipId\n");
            sn = 0xFFFFFFFF;
        }

        return (sn);
}

void yield(void)
{
}
