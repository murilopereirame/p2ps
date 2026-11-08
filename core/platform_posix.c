/* Platform layer for Linux and macOS (used for the desktop test build). */
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "p2f_internal.h"

int p2f_platform_random(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

int p2f_platform_resolve(const char *host, struct in_addr *out,
                         char *err, size_t errlen)
{
    struct addrinfo hints, *res = NULL;
    int rc;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(err, errlen, "%s", gai_strerror(rc));
        return -1;
    }
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}
