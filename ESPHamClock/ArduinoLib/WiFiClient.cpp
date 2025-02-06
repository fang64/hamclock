/* implement WiFiClient using normal UNIX sockets
 */

#include <signal.h>

#include "IPAddress.h"
#include "WiFiClient.h"

// default constructor
WiFiClient::WiFiClient()
{
        // init
	socket = -1;
	n_peek = 0;
        next_peek = 0;
        eof = false;
}

// constructor handed an open socket to use
WiFiClient::WiFiClient(int fd)
{
        // init
	socket = -1;
	n_peek = 0;
        next_peek = 0;
        eof = false;

        if (fd >= 0 && debugLevel (DEBUG_NET, 1))
            printf ("WiFiCl: new WiFiClient inheriting fd %d\n", fd);

	socket = fd;
	n_peek = 0;
        next_peek = 0;
        eof = false;
}

// return whether this socket is active and make sure it's closed if not
WiFiClient::operator bool()
{
        bool is_active = socket != -1 && !eof;
        if (debugLevel (DEBUG_NET, 3))
            printf ("WiFiCl: bool fd %d: active?%d eof?%d\n", socket, is_active, eof);
        if (!is_active)
            stop();
	return (is_active);
}

int WiFiClient::connect_to (int sockfd, struct sockaddr *serv_addr, int addrlen, int to_ms)
{
        unsigned int len;
        int err;
        int flags;
        int ret;

        /* set socket non-blocking */
        flags = fcntl (sockfd, F_GETFL, 0);
        (void) fcntl (sockfd, F_SETFL, flags | O_NONBLOCK);

        /* start the connect */
        ret = ::connect (sockfd, serv_addr, addrlen);
        if (ret < 0 && errno != EINPROGRESS)
            return (-1);

        /* wait for sockfd to become useable */
        ret = tout (to_ms, sockfd);
        if (ret < 0)
            return (-1);

        /* verify connection really completed */
        len = sizeof(err);
        err = 0;
        ret = getsockopt (sockfd, SOL_SOCKET, SO_ERROR, (char *) &err, &len);
        if (ret < 0)
            return (-1);
        if (err != 0) {
            errno = err;
            return (-1);
        }

        /* looks good - restore blocking */
        if (fcntl (sockfd, F_SETFL, flags) < 0)
            printf ("WiFiCl: fcntl fd %d: %s\n", sockfd, strerror(errno));

        return (0);
}

int WiFiClient::tout (int to_ms, int fd)
{
        fd_set rset, wset;
        struct timeval tv;
        int ret;

        FD_ZERO (&rset);
        FD_ZERO (&wset);
        FD_SET (fd, &rset);
        FD_SET (fd, &wset);

        tv.tv_sec = to_ms / 1000;
        tv.tv_usec = to_ms % 1000;

        ret = select (fd + 1, &rset, &wset, NULL, &tv);
        if (ret > 0)
            return (0);
        if (ret == 0)
            errno = ETIMEDOUT;
        return (-1);
}


bool WiFiClient::connect(const char *host, int port)
{
        struct addrinfo hints, *aip;
        char port_str[16];
        int sockfd;

        /* lookup host address.
         * N.B. must call freeaddrinfo(aip) after successful call before returning
         */
        memset (&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        snprintf (port_str, sizeof(port_str), "%d", port);
        int error = ::getaddrinfo (host, port_str, &hints, &aip);
        if (error) {
            printf ("WiFiCl: getaddrinfo(%s:%d): %s\n", host, port, gai_strerror(error));
            return (false);
        }

        /* create socket */
        sockfd = ::socket (aip->ai_family, aip->ai_socktype, aip->ai_protocol);
        if (sockfd < 0) {
            freeaddrinfo (aip);
            printf ("WiFiCl: socket(%s:%d): %s\n", host, port, strerror(errno));
	    return (false);
        }

        /* connect */
        if (connect_to (sockfd, aip->ai_addr, aip->ai_addrlen, 8000) < 0) {
            printf ("WiFiCl: connect(%s:%d): %s\n", host, port, strerror(errno));
            freeaddrinfo (aip);
            close (sockfd);
            return (false);
        }

        /* handle write errors inline */
        signal (SIGPIPE, SIG_IGN);

        /* ok start fresh */
        if (debugLevel (DEBUG_NET, 1))
            printf ("WiFiCl: new %s:%d fd %d\n", host, port, sockfd);
        freeaddrinfo (aip);

        // init much like constructors
	socket = sockfd;
	n_peek = 0;
        next_peek = 0;
        eof = false;

        return (true);
}

bool WiFiClient::connect(IPAddress ip, int port)
{
        char host[32];
        snprintf (host, sizeof(host), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        return (connect (host, port));
}

void WiFiClient::setNoDelay(bool on)
{
        // control Nagle algorithm
        socklen_t flag = on;
        if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (void *) &flag, sizeof(flag)) < 0)
            printf ("WiFiCl: TCP_NODELAY(%d): %s\n", on, strerror(errno));     // not fatal
}

void WiFiClient::stop()
{
	if (socket >= 0) {
            if (debugLevel (DEBUG_NET, 1))
                printf ("WiFiCl: stopping fd %d\n", socket);
	    shutdown (socket, SHUT_RDWR);
	    close (socket);
	    socket = -1;
	    n_peek = 0;
            next_peek = 0;
	} else if (debugLevel (DEBUG_NET, 3))
            printf ("WiFiCl: fd %d already stopped\n", socket);
}

bool WiFiClient::connected()
{
	return (socket >= 0);
}

int WiFiClient::available()
{
        // none if closed or eof
        if (socket < 0 || eof)
            return (0);

        // simple if unread bytes already available
	if (next_peek < n_peek)
	    return (1);

        // don't block if nothing more is available
        if (!pending(0))
            return (0);

        // read more
	int nr = ::read(socket, peek, sizeof(peek));
	if (nr > 0) {
            if (debugLevel (DEBUG_NET, 1))
                printf ("WiFiCl: read(%d) %d\n", socket, nr);
            if (debugLevel (DEBUG_NET, 2))
                logBuffer (peek, nr);
	    n_peek = nr;
            next_peek = 0;
	    return (1);
	} else if (nr == 0) {
            if (debugLevel (DEBUG_NET, 1))
                printf ("WiFiCl: read(%d) EOF\n", socket);
            eof = true;
	    stop();
	    return (0);
        } else {
            if (debugLevel (DEBUG_NET, 1))
                printf ("WiFiCl: read(%d): %s\n", socket, strerror(errno));
	    stop();
	    return (0);
	}
}

int WiFiClient::read()
{
	if (available()) {
            if (debugLevel (DEBUG_NET, 3))
                printf ("WiFiCl: read(%d) returning %c %d\n", socket, peek[next_peek], peek[next_peek]);
            return (peek[next_peek++]);
        }
	return (-1);
}

int WiFiClient::write (const uint8_t *buf, int n)
{
        // can't if closed
        if (socket < 0)
            return (0);

	int nw = 0;
	for (int ntot = 0; ntot < n; ntot += nw) {
	    nw = ::write (socket, buf+ntot, n-ntot);
	    if (nw < 0) {
                // select says it won't block but it still might be temporarily EAGAIN
                if (errno != EAGAIN) {
                    printf ("WiFiCl: write(%d) after %d: %s\n", socket, ntot, strerror(errno));
                    stop();             // avoid repeated failed attempts
                    return (0);
                } else
                    nw = 0;             // act like nothing happened
	    } else if (nw == 0) {
                printf ("WiFiCl: write(%d) returns 0 after %d\n", socket, ntot);
                stop();             // avoid repeated failed attempts
                return (0);
            }
	}

        if (debugLevel (DEBUG_NET, 1))
            printf ("WiFiCl: write(%d) %d\n", socket, n);
        if (debugLevel (DEBUG_NET, 2))
            logBuffer (buf, n);

	return (n);
}

/* log the given buffer contents
 */
void WiFiClient::logBuffer (const uint8_t *buf, int nbuf)
{
        const int max_pg_w = 100;
        int pg_w = 0;
        bool last_nl = false;
        while (nbuf-- > 0) {
            uint8_t c = *buf++;
            if (isprint(c)) {
                pg_w += printf ("%c", c);
                last_nl = false;
            } else {
                pg_w += printf (" %02X", c);
                last_nl = c == '\n';
            }
            if (pg_w > max_pg_w) {
                printf ("\n");
                pg_w = 0;
                last_nl = true;
            }
        }
        if (!last_nl)
            printf ("\n");
}

void WiFiClient::print (void)
{
}

void WiFiClient::print (String s)
{
	const uint8_t *sp = (const uint8_t *) s.c_str();
	int n = strlen ((char*)sp);
	write (sp, n);
}

void WiFiClient::print (const char *str)
{
	write ((const uint8_t *) str, strlen(str));
}

void WiFiClient::print (float f)
{
	char buf[32];
	int n = snprintf (buf, sizeof(buf), "%g", f);
	write ((const uint8_t *) buf, n);
}

void WiFiClient::print (float f, int s)
{
	char buf[32];
	int n = snprintf (buf, sizeof(buf), "%.*f", s, f);
	write ((const uint8_t *) buf, n);
}

void WiFiClient::println (void)
{
	write ((const uint8_t *) "\r\n", 2);
}

void WiFiClient::println (String s)
{
	const uint8_t *sp = (const uint8_t *) s.c_str();
	int n = strlen ((char*)sp);
	write (sp, n);
	write ((const uint8_t *) "\r\n", 2);
}

void WiFiClient::println (const char *str)
{
	write ((const uint8_t *) str, strlen(str));
	write ((const uint8_t *) "\r\n", 2);
}

void WiFiClient::println (float f)
{
	char buf[32];
	int n = snprintf (buf, sizeof(buf), "%g\r\n", f);
	write ((const uint8_t *) buf, n);
}

void WiFiClient::println (float f, int s)
{
	char buf[32];
	int n = snprintf (buf, sizeof(buf), "%.*f\r\n", s, f);
	write ((const uint8_t *) buf, n);
}

void WiFiClient::println (int i)
{
	char buf[32];
	int n = snprintf (buf, sizeof(buf), "%d\r\n", i);
	write ((const uint8_t *) buf, n);
}

void WiFiClient::println (uint32_t i)
{
	char buf[32];
	int n = snprintf (buf, sizeof(buf), "%u\r\n", i);
	write ((const uint8_t *) buf, n);
}

IPAddress WiFiClient::remoteIP()
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);

	getpeername(socket, (struct sockaddr *)&sa, &len);
	struct in_addr ip_addr = sa.sin_addr;

	char *s = inet_ntoa (ip_addr);
        int oct0, oct1, oct2, oct3;
        sscanf (s, "%d.%d.%d.%d", &oct0, &oct1, &oct2, &oct3);
	return (IPAddress(oct0,oct1,oct2,oct3));
}

/* return whether more is available after waiting up to ms.
 * non-standard
 */
bool WiFiClient::pending(int ms)
{
        struct timeval tv;
        fd_set rset;
        FD_ZERO (&rset);
        FD_SET (socket, &rset);
        tv.tv_sec = ms/1000;
        tv.tv_usec = (ms-1000*tv.tv_sec)*1000;
        int s = select (socket+1, &rset, NULL, NULL, &tv);
        if (s < 0) {
            printf ("WiFiCl: fd %d select(%d ms): %s\n", socket, ms, strerror(errno));
	    stop();
	    return (false);
	}
        return (s != 0);
}

/* read n bytes unless EOF. return whether ok.
 */
bool WiFiClient::readArray (char buf[], int n)
{
        // first use any in peek[]
        if (n_peek > next_peek) {
            int n_more = n_peek - next_peek;
            int n_use = n < n_more ? n : n_more;
            memcpy (buf, peek+next_peek, n_use);
            next_peek += n_use;
            buf += n_use;
            n -= n_use;
        }

        // if need more bypass peek[] which will now be empty
        while (n > 0) {
            if (!pending(5000))
                return (false);
            int nr = ::read (socket, buf, n);
            if (nr < 0) {
                printf ("WiFiCl: read(%d,%d): %s\n", socket, n, strerror(errno));
                return (false);
            }
            if (nr == 0) {
                if (debugLevel (DEBUG_NET, 1))
                    printf ("WiFiCl: readArray(%d) EOF\n", socket);
                eof = true;
                stop();
                return (false);
            }
            buf += nr;
            n -= nr;
        }

        return (true);
}
