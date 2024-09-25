/* very basic radio control.
 * support hamlib and flrig, plus legacy KX3 as a special case, from main thread.
 * separate thread for all io polls PTT and sends new frequency.
 */


#include "HamClock.h"


// config
#define RADIOPOLL_MS    1000                            // PTT polling period, ms
#define RADIOLOOP_MS    250                             // overall thread polling period
#define ERRDWELL_MS     4000                            // error message display time, ms
#define WARN_MS         15000                           // reporting interval between repeating errs, ms


// shared thread comm variables
static volatile int set_hl_Hz, set_fl_Hz;                // set by main, reset to 0 when sent
static volatile int thread_onair;                        // set by thread, main calls setOnAirSW if valid
static char * volatile thread_msg;                       // malloced by thread, freed/zerod by main
static pthread_mutex_t msg_lock = PTHREAD_MUTEX_INITIALIZER;    // guard thread_msg


/* called by io thread to post a message to the main thread
 */
static void postMsgToMain (const char *fmt, ...)
{
    // format
    char buf[200];
    va_list ap;
    va_start (ap, fmt);
    vsnprintf (buf, sizeof(buf), fmt, ap);
    va_end (ap);

    if (gimbal_trace_level)
        Serial.printf ("RADIO: posting %s\n", buf);

    // inform main thread of new malloced message, discard are message not yet picked up.
    if (pthread_mutex_lock (&msg_lock) == 0) {
        if (thread_msg)
            free (thread_msg);
        thread_msg = strdup (buf);
        if (!thread_msg)
            fatalError ("No memory for radio message: %s", buf);
        pthread_mutex_unlock (&msg_lock);
    }
}






/**********************************************************************************
 *
 * hamlib
 *
 **********************************************************************************/


/* Hamlib helper to send the given command then read and discard response until find RPRT.
 * return whether io was ok.
 */
static bool sendHamlibCmd (WiFiClient &client, const char cmd[])
{
    // send
    if (gimbal_trace_level > 1)
        Serial.printf ("RADIO: HAMLIB: send: %s\n", cmd);
    client.println(cmd);

    // absorb reply until find RPRT
    char buf[64];
    bool ok = 0;
    do {
        ok = getTCPLine (client, buf, sizeof(buf), NULL);
        if (ok) {
            if (gimbal_trace_level > 1)
                Serial.printf ("RADIO: HAMLIB reply: %s\n", buf);
        } else {
            Serial.printf ("RADIO: HAMLIB cmd %s: no reply\n", cmd);
        }
    } while (ok && !strstr (buf, "RPRT"));

    return (ok);
}

/* Hamlib helper to send the given command, look for given key word to collect reply, until find RPRT.
 * return whether io ok regardless of reply value.
 */
static bool intHamlibCmd (WiFiClient &client, const char cmd[], const char kw[], int &reply)
{
    // send
    if (gimbal_trace_level > 1)
        Serial.printf ("RADIO: HAMLIB: send: %s\n", cmd);
    client.println(cmd);

    // absorb reply watching for keyword
    size_t kw_l = strlen (kw);
    char buf[64];
    reply = -1;
    bool ok = false;
    do {
        ok = getTCPLine (client, buf, sizeof(buf), NULL);
        if (ok) {
            if (gimbal_trace_level > 1)
                Serial.printf ("RADIO: HAMLIB reply: %s\n", buf);
            if (strncmp (buf, kw, kw_l) == 0) {
                reply = atoi (buf + kw_l);
                if (reply < 0)
                    Serial.printf ("RADIO: HAMLIB %s -> %d", cmd, reply);
            }
         } else {
            Serial.printf ("RADIO: HAMLIB cmd %s: no reply\n", cmd);
        }
    } while (ok && !strstr (buf, "RPRT"));

    return (ok);
}

/* set the given freq via hamlib
 */
static bool setHamlibFreq (WiFiClient &client, int Hz)
{
    // send setup commands, require RPRT for each but ignore error values
    bool ok = true;
    static const char *setup_cmds[] = {
        "+\\set_split_vfo 0 VFOA",
        "+\\set_vfo VFOA",
        "+\\set_func RIT 0",
        "+\\set_rit 0",
        "+\\set_func XIT 0",
        "+\\set_xit 0",
    };
    for (int i = 0; ok && i < NARRAY(setup_cmds); i++)
        ok = sendHamlibCmd (client, setup_cmds[i]);

    // send freq if all still ok
    if (ok) {
        char cmd[128];
        snprintf (cmd, sizeof(cmd), "+\\set_freq %d", Hz);
        ok = sendHamlibCmd (client, cmd);
    }

    // ask for confirmation if still ok
    if (ok) {
        int reply = -1;
        ok = intHamlibCmd (client, "+\\get_freq", "Frequency:", reply) && reply == Hz;
        if (!ok)
            Serial.printf ("RADIO: HAMLIB set %d Hz but acked %d\n", Hz, reply);
    }

    if (ok && gimbal_trace_level)
        Serial.printf ("RADIO: HAMLIB: set %d Hz\n", Hz);

    return (ok);
}



/* connect to rigctld, return whether successful.
 * N.B. we assume getRigctld will be true.
 */
static bool tryHamlibConnect (WiFiClient &client)
{
    // get host and port
    char host[NV_RIGHOST_LEN];
    int port;
    if (!getRigctld (host, &port))
        fatalError ("tryHamlibConnect no control");

    // connect, bale if can't
    if (!client.connect(host, port))
        return (false);

    if (gimbal_trace_level)
        Serial.printf ("RADIO: HAMLIB: %s:%d connect ok\n", host, port);

    // ok
    return (true);
}




/**********************************************************************************
 *
 * flrig
 *
 **********************************************************************************/

/* send an flrig xml-rpc command.
 * N.B. caller must still handle reply after return.
 */
static void coreFlrigCmd (WiFiClient &client, const char cmd[], const char value[], const char type[])
{
    // printf-style format template for HTML header
    static const char hdr_fmt[] =
        "POST /RPC2 HTTP/1.1\r\n"
        "Content-Type: text/xml\r\n"
        "Content-length: %d\r\n"
        "\r\n"
    ;

    // printf-style format template for XML contents
    static const char body_fmt[] =
        "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\r\n"
        "<methodCall>\r\n"
        "    <methodName>%.50s</methodName>\r\n"
        "    <params>\r\n"
        "        <param><value><%.10s>%.50s</%.10s></value></param>\r\n"
        "    </params>\r\n"
        "</methodCall>\r\n"
    ;

    // create body text first to get length
    char xml_body[sizeof(body_fmt) + 100];
    int body_l = snprintf (xml_body, sizeof(xml_body), body_fmt, cmd, type, value, type);

    // create header text including length of body text
    char xml_hdr[sizeof(hdr_fmt) + 100];
    snprintf (xml_hdr, sizeof(xml_hdr), hdr_fmt, body_l);

    if (gimbal_trace_level > 1) {
        Serial.printf ("RADIO: FLRIG: sending:\n");
        printf ("%s", xml_hdr);
        printf ("%s", xml_body);

    } 

    // send hdr then body
    client.print(xml_hdr);
    client.print(xml_body);
}

/* helper to send a flrig xml-rpc command and discard response
 */
static bool sendFlrigCmd (WiFiClient &client, const char cmd[], const char value[], const char type[])
{
    // send core command
    coreFlrigCmd (client, cmd, value, type);

    // skip through reply until </methodResponse>
    if (gimbal_trace_level > 1)
        Serial.printf ("RADIO: FLRIG: reply:\n");
    char reply_buf[200];
    bool ok = false;
    do {
        ok = getTCPLine (client, reply_buf, sizeof(reply_buf), NULL);
        if (ok) {
            if (gimbal_trace_level > 1)
                printf ("%s\n", reply_buf);
        } else {
            Serial.printf ("RADIO: FLRIG cmd %s: no reply\n", cmd);
        }
    } while (ok && !strstr (reply_buf, "</methodResponse>"));

    return (ok);
}

/* helper to send a flrig xml-rpc command and report integer reply.
 * reply whether io ok independent of reply value.
 */
static bool intFlrigCmd (WiFiClient &client, const char cmd[], const char value[], const char type[],
int &reply)
{
    // send core command
    coreFlrigCmd (client, cmd, value, type);

    // skip through response until find </methodResponse>, watch for <value> reply along the way
    if (gimbal_trace_level > 1)
        Serial.printf ("RADIO: FLRIG: reply:\n");
    char reply_buf[200];
    reply = -1;
    bool ok = false;
    do {
        ok = getTCPLine (client, reply_buf, sizeof(reply_buf), NULL);
        if (ok) {
            if (gimbal_trace_level > 1)
                printf ("%s\n", reply_buf);
            if (sscanf (reply_buf, " <value><i4>%d", &reply) == 1
                                        || sscanf (reply_buf, " <value>%d", &reply) == 1) {
                if (reply < 0)
                    Serial.printf ("RADIO: FLRIG: cmd %s reply %d\n", cmd, reply);
            }
        } else {
            Serial.printf ("RADIO: FLRIG cmd %s: no reply\n", cmd);
        }
    } while (ok && !strstr (reply_buf, "</methodResponse>"));

    return (ok);
}

/* set the given freq via flrig
 */
static bool setFlrigFreq (WiFiClient &client, int Hz)
{
    int reply;
    char Hzstr[20];
    snprintf (Hzstr, sizeof(Hzstr), "%d", Hz);
    return (sendFlrigCmd (client, "rig.set_split", "0", "int")
                        && sendFlrigCmd (client, "rig.set_vfoA", Hzstr, "double")
                        && intFlrigCmd (client, "rig.get_vfoA", "0", "int", reply)
                        && reply == Hz);

}

/* connect to flig, return whether successful.
 * N.B. we assume getFlrig will be true.
 */
static bool tryFlrigConnect (WiFiClient &client)
{
    // get host and port, bale if nothing
    char host[NV_FLRIGHOST_LEN];
    int port;
    if (!getFlrig (host, &port))
        fatalError ("tryFlrigConnect no control");

    // connect, bale if can't
    if (!client.connect(host, port))
        return (false);

    if (gimbal_trace_level)
        Serial.printf ("RADIO: FLRIG: %s:%d connect ok\n", host, port);

    // ok
    return (true);
}




#if defined(_SUPPORT_KX3)

/**********************************************************************************
 *
 * kx3
 *
 **********************************************************************************/



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
        Serial.printf ("RADIO: KX3: sched_setscheduler(%d): %s\n", hi_param.sched_priority, strerror(errno));

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

    Serial.printf ("RADIO: KX3: correction= %g cmd= %u ns0= %u ns1= %u ns\n", correction, cmd_ns, ns0, ns1);

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

/* command KX3 frequency
 */
static void setKX3Spot (float kHz)
{
    // even if have proper io still ignore if not supposed to use GPIO or baud is 0
    if (!GPIOOk() || getKX3Baud() == 0)
        return;

    // one-time IO setup
    static bool ready;
    if (!ready) {
        KX3prepIO();
        ready = true;
        Serial.println ("RADIO: KX3: ready");
    }

    // send setup commands
    KX3sendOneMessage (KX3setup_cmds);

    // format and send command to change frequency
    char buf[30];
    (void) snprintf (buf, sizeof(buf), KX3setfreq_fmt, kHz*1e3);
    KX3sendOneMessage (buf);
}


#endif  // _SUPPORT_KX3





/******************************************************************************
 *
 * io thread
 *
 ******************************************************************************/

/* perpetual thread to establish and maintain contact with rig control server(s).
 * communicate with main thread via a few shared variables.
 * N.B. not used for KX3.
 */
static void *radioThread (void *unused)
{
    (void) unused;

    // forever
    pthread_detach(pthread_self());

    WiFiClient hl_client, fl_client;
    uint32_t hlpoll_ms = 0, hlwarn_ms;
    uint32_t flpoll_ms = 0, flwarn_ms;

    while (true) {

        if (getRigctld (NULL, NULL)) {

            // insure connected
            if (!hl_client.connected()) {
                if (tryHamlibConnect (hl_client))
                    postMsgToMain ("HAMLIB connection successful");
                else if (timesUp (&flwarn_ms, WARN_MS))
                    postMsgToMain ("HAMLIB no connection");
            }

            // check for setting new frequency -- set by human clicking a spot so post all errors
            if (set_hl_Hz) {
                if (hl_client.connected()) {
                    if (!setHamlibFreq (hl_client, set_hl_Hz))
                        postMsgToMain ("HAMLIB failed to verify %d Hz", set_hl_Hz);
                } else 
                    postMsgToMain ("HAMLIB not connected to set %d Hz", set_hl_Hz);

                // ack this attempt to set freq
                set_hl_Hz = 0;
            }

            // continuous automatic poll PTT every RADIOPOLL_MS -- only post errors every WARN_MS
            if (timesUp (&hlpoll_ms, RADIOPOLL_MS) && hl_client.connected()) {
                int reply = -1;
                if (intHamlibCmd (hl_client, "+\\get_ptt", "PTT:", reply) && reply >= 0)
                    thread_onair = reply;
                else if (timesUp (&hlwarn_ms, WARN_MS)) {
                    postMsgToMain ("HAMLIB PTT query failed");
                    thread_onair = 0;
                }
            }
        }

        if (getFlrig (NULL, NULL)) {

            // insure connected
            if (!fl_client.connected()) {
                if (tryFlrigConnect (fl_client))
                    postMsgToMain ("FLRIG connection successful");
                else if (timesUp (&flwarn_ms, WARN_MS))
                    postMsgToMain ("FLRIG no connection");
            }

            // check for setting new frequency -- set by human clicking a spot so post all errors
            if (set_fl_Hz) {
                if (fl_client.connected()) {
                    if (!setFlrigFreq (fl_client, set_fl_Hz))
                        postMsgToMain ("FLRIG failed to verify %d Hz", set_fl_Hz);
                } else 
                    postMsgToMain ("FLRIG not connected to set %d Hz", set_fl_Hz);

                // ack this attempt to set freq
                set_fl_Hz = 0;
            }

            // continuous automatic poll PTT every RADIOPOLL_MS -- only post errors every WARN_MS
            if (timesUp (&flpoll_ms, RADIOPOLL_MS) && fl_client.connected()) {
                int reply = -1;
                if (intFlrigCmd (fl_client, "rig.get_ptt", "0", "int", reply) && reply >= 0)
                    thread_onair = reply;
                else if (timesUp (&flwarn_ms, WARN_MS)) {
                    postMsgToMain ("FLRIG PTT query failed");
                    thread_onair = 0;
                }
            }
        }

        delay (RADIOLOOP_MS);
    }

    fatalError ("radioThread failure");
    return (NULL);
}


/* insure radioThread is running, harmless if called repeatedly, fatal if fails.
 * return whether ok to proceed with rig io.
 */
static bool startRadioThread (void)
{
    if (!getFlrig(NULL,NULL) && !getRigctld(NULL,NULL))
        return (false);

    // start first time
    static bool thread_running;
    if (!thread_running) {
        pthread_t tid;
        int e = pthread_create (&tid, NULL, radioThread, NULL);
        if (e)
            fatalError ("radioThread failed: %s", strerror(e));
        thread_running = true;
    }
    return (thread_running);
}



/******************************************************************************
 *
 * public interface
 *
 ******************************************************************************/


/* try setting freq to all desired radios.
 */
void setRadioSpot (float kHz)
{
    // set kx3
#if defined(_SUPPORT_KX3)
    setKX3Spot (kHz);
#endif // _SUPPORT_KX3

    // insure thread running or bale if not needed
    if (!startRadioThread())
        return;

    // inform thread
    set_fl_Hz = set_hl_Hz = (int) (kHz * 1000);
}

/* leisurely poll radio for state
 */
void pollRadio(void)
{
    // insure thread running or bale if not needed
    if (!startRadioThread())
        return;

    // show any pending messages
    if (pthread_mutex_lock (&msg_lock) == 0) {
        if (thread_msg) {
            mapMsg (ERRDWELL_MS, "%s", thread_msg);
            free (thread_msg);
            thread_msg = NULL;
        }
        pthread_mutex_unlock (&msg_lock);
    }

    // show current state of ptt
    setOnAirSW (thread_onair);
}

void radioResetIO(void)
{
#if defined(_SUPPORT_KX3)
    if (GPIOOk())
        KX3radioResetIO();
#endif // _SUPPORT_KX3
}
