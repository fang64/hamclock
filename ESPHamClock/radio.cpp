/* initial seed of radio control idea.
 * first attempt was simple bit-bang serial to kx3 to set frequency for a spot.
 * then we added hamlib's rigctld and w1hkj's flrig for CAT commands.
 * next we add ability to send mode as well as frequency, based on mode-table.txt.
 */


#include "HamClock.h"



// one entry
typedef struct {
    int f1, f2;                                 // applicable range, kHz
    const char *fl_mode;                        // mode string for flrig
    const char *hc_mode;                        // mode string for hamlib
} ModeEntry;


#if defined (_USE_MODE_TABLE)

/* this can't work because each radio has a different set of names for its modes.
 */

/******************************
 *
 * manage the mode table
 *
 ******************************/


// page with mode definitions
static const char mode_page[] = "/mode-table.txt";

// mode table
static ModeEntry *mentries;
static int n_mmalloced, n_mentries;


/* read mentries[]
 * N.B. set mentries even if find none as a marker to not repeat attempt
 */
static void readModeEntries(void)
{
    WiFiClient mode_client;

    // init table
    mentries = (ModeEntry *) calloc (n_mmalloced = 1, sizeof(ModeEntry));

    Serial.printf ("RADIO: %s\n", mode_page);
    if (wifiOk() && mode_client.connect (backend_host, backend_port)) {

        updateClocks(false);

        // send query
        httpHCGET (mode_client, backend_host, mode_page);

        // skip http header
        if (!httpSkipHeader (mode_client)) {
            Serial.print ("RADIO: bad header\n");
            goto out;
        }

        char line[100];
        uint16_t ll;
        while (getTCPLine (mode_client, line, sizeof(line), &ll)) {

            // skip comments or blank lines
            if (ll < 5 || line[0] == '#')
                continue;

            // crack qualifying entry
            char fl_mode[20], hc_mode[20];
            int f1, f2;
            if (sscanf (line, "%d %d %19s %19s", &f1, &f2, fl_mode, hc_mode) != 4
                        || f1 >= f2 || f1 < 1000 || f1 > 150000 || f2 < 1000 || f2 > 150000) {
                Serial.printf ("RADIO: bad line: %s\n", line);
                continue;
            }

            // add to list
            if (n_mentries + 1 < n_mmalloced) {
                mentries = (ModeEntry *) realloc (mentries, (n_mmalloced += 20)*sizeof(ModeEntry));
                if (!mentries)
                    fatalError ("No memory for %d modes", n_mentries);
            }
            ModeEntry &me = mentries[n_mentries++];
            me.f1 = f1;
            me.f2 = f2;
            me.fl_mode = strdup (strtoupper(fl_mode));
            me.hc_mode = strdup (strtoupper(hc_mode));
        }

        Serial.printf ("RADIO: found %d entries\n", n_mentries);

    } else {

        Serial.printf ("RADIO: failed to download modes page\n");
    }
 
  out:
    mode_client.stop();
}

static const ModeEntry *findRadioMode (int kHz)
{
    // read table first time, bale if none
    if (!mentries) {
        readModeEntries();
        if (!mentries)
            return (NULL);
    }

    // find smallest region containing kHz
    ModeEntry *best_mep = NULL;
    int min_range = 10000000;
    for (ModeEntry *mep = &mentries[n_mentries]; --mep >= mentries; ) {
        if (mep->f1 <= kHz && kHz <= mep->f2) {
            int r = mep->f2 - mep->f1;
            if (r < min_range) {
                r = min_range;
                best_mep = mep;
            }
        }
    }

    // best mode, if any
    return (best_mep);
}

#else


// dummy that never succeeds in getting mode
static const ModeEntry *findRadioMode (int kHz) { return NULL; }


#endif // _USE_MODE_TABLE





/******************************
 *
 * hamlib
 *
 ******************************/


/* Hamlib helper to send the given command then read and discard response until find RPRT
 * N.B. we assume cmd already includes trailing \n
 */
static void sendHamlibCmd (WiFiClient &client, const char cmd[])
{
    // send
    Serial.printf ("RADIO: HL: %s", cmd);
    client.print(cmd);

    // absorb reply until fine RPRT
    char buf[64];
    bool ok = 0;
    do {
        ok = getTCPLine (client, buf, sizeof(buf), NULL);
        if (ok)
            Serial.printf ("  %s\n", buf);
    } while (ok && !strstr (buf, "RPRT"));
}

/* connect to rigctld, return whether successful.
 * N.B. caller must close
 */
static bool tryHamlibConnect (WiFiClient &client)
{
    // get host and port, bale if nothing
    char host[NV_RIGHOST_LEN];
    int port;
    if (!getRigctld (host, &port))
        return (false);

    // connect, bale if can't
    Serial.printf ("RADIO: HL: %s:%d\n", host, port);
    if (!wifiOk() || !client.connect(host, port)) {
        Serial.printf ("RADIO: HL: %s:%d failed\n", host, port);
        return (false);
    }

    // ok
    return (true);
}



/******************************
 *
 * flrig
 *
 ******************************/

/* helper to send a flrig xml-rpc command and discard response
 */
static void sendFlrigCmd (WiFiClient &client, const char cmd[], const char value[], const char type[])
{
    static const char hdr_fmt[] =
        "POST /RPC2 HTTP/1.1\r\n"
        "Content-Type: text/xml\r\n"
        "Content-length: %d\r\n"
        "\r\n"
    ;
    static const char body_fmt[] =
        "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\r\n"
        "<methodCall>\r\n"
        "    <methodName>%.50s</methodName>\r\n"
        "    <params>\r\n"
        "        <param><value><%.10s>%.50s</%.10s></value></param>\r\n"
        "    </params>\r\n"
        "</methodCall>\r\n"
    ;

    // create body text first
    char xml_body[sizeof(body_fmt) + 100];
    int body_l = snprintf (xml_body, sizeof(xml_body), body_fmt, cmd, type, value, type);

    // create header text including length of body text
    char xml_hdr[sizeof(hdr_fmt) + 100];
    snprintf (xml_hdr, sizeof(xml_hdr), hdr_fmt, body_l);

    // send hdr then body
    Serial.printf ("RADIO: FLRIG: sending:\n");
    printf ("%s", xml_hdr);
    printf ("%s", xml_body);
    client.print(xml_hdr);
    client.print(xml_body);

    // just log reply until </methodResponse>
    Serial.printf ("RADIO: FLRIG: reply:\n");
    char reply_buf[200];
    bool ok = false;
    do {
        ok = getTCPLine (client, reply_buf, sizeof(reply_buf), NULL);
        if (ok)
            printf ("%s\n", reply_buf);
    } while (ok && !strstr (reply_buf, "</methodResponse>"));
}

/* connect to flig, return whether successful.
 * N.B. caller must close
 */
static bool tryFlrigConnect (WiFiClient &client)
{
    // get host and port, bale if nothing
    char host[NV_FLRIGHOST_LEN];
    int port;
    if (!getFlrig (host, &port))
        return (false);

    // connect, bale if can't
    Serial.printf ("RADIO: FLRIG: %s:%d\n", host, port);
    if (!wifiOk() || !client.connect(host, port)) {
        Serial.printf ("RADIO: FLRIG: %s:%d failed\n", host, port);
        return (false);
    }

    // ok
    return (true);
}



#if defined(_SUPPORT_KX3)



/**********************************************************************************
 *
 *
 * hack to send spot frequency to Elecraft radio on RPi GPIO 14 (header pin 8).
 * can not use HW serial because Electraft wants inverted mark/space, thus
 * timing will not be very good.
 *
 *
 **********************************************************************************/
 

#include <time.h>


/* setup commands before changing freq:
 *
 *   SB0 = Set Sub Receiver or Dual Watch off
 *   FR0 = Cancel Split on K2, set RX vfo A
 *   FT0 = Set tx vfo A
 *   RT0 = Set RIT off
 *   XT0 = Set XIT off
 *   RC  = Set RIT / XIT to zero
 */
static const char KX3setup_cmds[] = ";SB0;FR0;FT0;RT0;XT0;RC;";

/* snprintf format to set new frequency, requires float in Hz
 */
static const char KX3setfreq_fmt[] = ";FA%011.0f;";


/* send one bit @ getKX3Baud(), bit time multiplied by correction factor.
 * N.B. they want mark/sense inverted
 * N.B. this can be too long depending on kernel scheduling. Performance might be improved by
 *      assigning this process to a dedicated processor affinity and disable being scheduled using isolcpus.
 *      man pthread_setaffinity_np
 *      https://www.kernel.org/doc/html/v4.10/admin-guide/kernel-parameters.html
 */
static void KX3sendOneBit (int hi, float correction)
{
    // get time now
    struct timespec t0, t1;
    clock_gettime (CLOCK_MONOTONIC, &t0);

    // set bit (remember: Elecraft wants inverted mark/sense)
    mcp.digitalWrite (MCP_FAKE_KX3, !hi);

    // wait for one bit duration with modified correction including nominal correction
    uint32_t baud = getKX3Baud();
    float overhead = 1.0F - 0.04F*baud/38400;          // measured on pi 4
    unsigned long bit_ns = 1000000000UL/baud*overhead*correction;
    unsigned long dt_ns;
    do {
        clock_gettime (CLOCK_MONOTONIC, &t1);
        dt_ns = 1000000000UL*(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec);
    } while (dt_ns < bit_ns);
}


/* send the given string with the given time correction factor.
 * return total nsecs.
 */
static uint32_t KX3sendOneString (float correction, const char str[])
{
    // get current scheduler and priority
    int orig_sched = sched_getscheduler(0);
    struct sched_param orig_param;
    sched_getparam (0, &orig_param);

    // attempt setting high priority
    struct sched_param hi_param;
    hi_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    bool hipri_ok = sched_setscheduler (0, SCHED_FIFO, &hi_param) == 0;
    if (!hipri_ok)
        Serial.printf ("RADIO: Failed to set new prioity %d: %s\n", hi_param.sched_priority, strerror(errno));

    // get starting time
    struct timespec t0, t1;
    clock_gettime (CLOCK_MONOTONIC, &t0);

    // send each char, 8N1, MSByte first
    char c;
    while ((c = *str++) != '\0') {
        KX3sendOneBit (0, correction);                  // start bit
        for (uint8_t bit = 0; bit < 8; bit++) {         // LSBit first
            KX3sendOneBit (c & 1, correction);          // data bit
            c >>= 1;
        }
        KX3sendOneBit (1, correction);                     // stop bit
    }

    // record finish time
    clock_gettime (CLOCK_MONOTONIC, &t1);

    // restore original priority
    if (hipri_ok)
        sched_setscheduler (0, orig_sched, &orig_param);

    // return duration in nsec
    return (1000000000UL*(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec));
}

/* send the given command.
 */
static void KX3sendOneMessage (const char cmd[])
{
    // len
    size_t cmd_l = strlen(cmd);

    // compute ideal time to send command
    uint32_t bit_ns = 1000000000UL/getKX3Baud();        // ns per bit
    uint32_t cmd_ns = cmd_l*10*bit_ns;                  // start + 8N1
  
    // send with no speed correction
    uint32_t ns0 = KX3sendOneString (1.0F, cmd);

    // compute measured correction factor
    float correction = (float)cmd_ns/ns0;

    // repeat if correction more than 1 percent
    uint32_t ns1 = 0;
    if (correction < 0.99F || correction > 1.01F) {
        usleep (500000);    // don't pummel
        ns1 = KX3sendOneString (correction, cmd);
    }

    Serial.printf ("RADIO: Elecraft correction= %g cmd= %u ns0= %u ns1= %u ns\n", correction, cmd_ns, ns0, ns1);

}

/* perform one-time preparation for sending commands
 */
static void KX3prepIO()
{
    // init Elecraft pin
    mcp.pinMode (MCP_FAKE_KX3, OUTPUT);
    KX3sendOneBit (1, 1.0F);
}

/* return our gpio pins to quiescent state
 */
static void KX3radioResetIO()
{
    mcp.pinMode (MCP_FAKE_KX3, INPUT);
}

#endif  // _SUPPORT_KX3




/* try setting freq and possibly mode all possible ways.
 */
void setRadioSpot (float kHz)
{
    // connection to each
    WiFiClient client;

    if (getRigctld(NULL,NULL) && tryHamlibConnect (client)) {

        // find mode at this freq, if known .. put query here to avoid if hamlib not used
        const ModeEntry *mep = findRadioMode (kHz);

        // stay awake
        updateClocks(false);

        // send setup commands, require RPRT for each but ignore error values
        static const char *setup_cmds[] = {
            "+\\set_split_vfo 0 VFOA\n",
            "+\\set_vfo VFOA\n",
            "+\\set_func RIT 0\n",
            "+\\set_rit 0\n",
            "+\\set_func XIT 0\n",
            "+\\set_xit 0\n",
        };
        for (int i = 0; i < NARRAY(setup_cmds); i++)
            sendHamlibCmd (client, setup_cmds[i]);

        // stay awake
        updateClocks(false);

        char cmd[128];
        snprintf (cmd, sizeof(cmd), "+\\set_freq %.0f\n", 1000.0F * kHz); // wants Hz
        sendHamlibCmd (client, cmd);

        if (mep && mep->hc_mode) {
            int bw = strcmp (mep->hc_mode, "CW") == 0 ? 500 : 2700;
            snprintf (cmd, sizeof(cmd), "+\\set_mode %s %d\n", mep->hc_mode, bw);
            sendHamlibCmd (client, cmd);
        }

        client.stop();
    }

    if (getFlrig(NULL,NULL) && tryFlrigConnect (client)) {

        // find mode at this freq, if known .. put query here to avoid if flrig not used
        const ModeEntry *mep = findRadioMode (kHz);

        // stay awake
        updateClocks(false);

        char value[20];
        snprintf (value, sizeof(value), "%.0f", kHz*1000);
        sendFlrigCmd (client, "rig.set_split", "0", "int");
        sendFlrigCmd (client, "rig.set_vfoA", value, "double");

        if (mep && mep->fl_mode)
            sendFlrigCmd (client, "rig.set_mode", mep->fl_mode, "string");

        client.stop();
    }


#if defined(_SUPPORT_KX3)

    // even if have proper io still ignore if not supposed to use GPIO or baud is 0
    if (!GPIOOk() || getKX3Baud() == 0)
        return;

    // one-time IO setup
    static bool ready;
    if (!ready) {
        KX3prepIO();
        ready = true;
        Serial.println ("RADIO: Elecraft: ready");
    }

    // send setup commands
    KX3sendOneMessage (KX3setup_cmds);

    // format and send command to change frequency
    char buf[30];
    (void) snprintf (buf, sizeof(buf), KX3setfreq_fmt, kHz*1e3);
    KX3sendOneMessage (buf);

#endif // _SUPPORT_KX3

}

void radioResetIO(void)
{
#if defined(_SUPPORT_KX3)
    if (GPIOOk())
        KX3radioResetIO();
#endif // _SUPPORT_KX3
}
