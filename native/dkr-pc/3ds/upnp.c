// Asking the home router to let an online game in.
//
// A console that hosts an online game has to be reachable from the internet
// on one UDP port. Home routers refuse that unless told otherwise; most can
// be told by a program on the network, through UPnP ("Internet Gateway
// Device"): find the router (a multicast question, SSDP), read its
// description (HTTP, XML), and ask its WAN connection service to forward the
// port and to say what the router's public address is (SOAP over HTTP).
//
// Nothing here is needed to join a game, and nothing is kept on the router
// after the game: the forwarding is removed when the host closes it.
//
// When the router has UPnP switched off this fails, and the host has to
// forward the port by hand in the router's settings (the menu says so).
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "upnp.h"

#define RESPONSE_BYTES 12288
#define TICKS_PER_MS (SYSCLOCK_ARM11 / 1000)

static char sResponse[RESPONSE_BYTES];
static char sControlHost[64];
static int sControlPort;
static char sControlPath[160];
static char sServiceType[96];
static int sFound;

static u64 elapsed_ms(u64 since) {
    return (svcGetSystemTick() - since) / TICKS_PER_MS;
}

// "http://host:port/path" into its parts.
static int split_url(const char *url, char *host, size_t hostSize, int *port, char *path, size_t pathSize) {
    const char *p = url, *slash, *colon;
    size_t hostLen;

    if (strncasecmp(p, "http://", 7) != 0) {
        return 0;
    }
    p += 7;
    slash = strchr(p, '/');
    if (slash == NULL) {
        slash = p + strlen(p);
    }
    colon = memchr(p, ':', (size_t) (slash - p));
    hostLen = (size_t) ((colon != NULL ? colon : slash) - p);
    if (hostLen == 0 || hostLen >= hostSize) {
        return 0;
    }
    memcpy(host, p, hostLen);
    host[hostLen] = '\0';
    *port = colon != NULL ? atoi(colon + 1) : 80;
    snprintf(path, pathSize, "%s", *slash != '\0' ? slash : "/");
    return 1;
}

// The router's answer to the SSDP question: the URL of its description.
static int discover(char *location, size_t size) {
    static const char *const targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    };
    struct sockaddr_in to;
    char request[256];
    u64 start = svcGetSystemTick();
    int s = socket(AF_INET, SOCK_DGRAM, 0), i, found = 0;

    if (s < 0) {
        return 0;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    to.sin_addr.s_addr = inet_addr("239.255.255.250");
    for (i = 0; i < 2; i++) {
        snprintf(request, sizeof(request),
                 "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n",
                 targets[i]);
        sendto(s, request, strlen(request), 0, (struct sockaddr *) &to, sizeof(to));
    }
    while (!found && elapsed_ms(start) < 2500) {
        int n = recv(s, sResponse, RESPONSE_BYTES - 1, 0);

        if (n > 0) {
            char *line;

            sResponse[n] = '\0';
            for (line = sResponse; line != NULL && *line != '\0'; line = strchr(line, '\n')) {
                if (*line == '\n') {
                    line++;
                }
                if (strncasecmp(line, "LOCATION:", 9) == 0) {
                    const char *value = line + 9;
                    size_t len;

                    while (*value == ' ') {
                        value++;
                    }
                    len = strcspn(value, "\r\n");
                    if (len < size) {
                        memcpy(location, value, len);
                        location[len] = '\0';
                        found = 1;
                    }
                    break;
                }
            }
        } else {
            svcSleepThread(20000000);
        }
    }
    close(s);
    return found;
}

// One HTTP exchange; the whole answer in sResponse. Returns its length.
static int http_exchange(const char *host, int port, const char *request) {
    struct sockaddr_in to;
    u64 start = svcGetSystemTick();
    int s = socket(AF_INET, SOCK_STREAM, 0), total = 0;

    if (s < 0) {
        return 0;
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((u16) port);
    if (inet_aton(host, &to.sin_addr) == 0 || connect(s, (struct sockaddr *) &to, sizeof(to)) != 0) {
        close(s);
        return 0;
    }
    send(s, request, strlen(request), 0);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    while (total < RESPONSE_BYTES - 1 && elapsed_ms(start) < 4000) {
        int n = recv(s, sResponse + total, (size_t) (RESPONSE_BYTES - 1 - total), 0);

        if (n > 0) {
            total += n;
        } else if (n == 0) {
            break;      // the router closed the connection: that was all
        } else {
            svcSleepThread(10000000);
        }
    }
    sResponse[total] = '\0';
    close(s);
    return total;
}

// The WAN connection service in the router's description: its type and the
// URL its actions are posted to.
static int find_service(const char *descriptionHost, int descriptionPort) {
    static const char *const kinds[] = { "WANIPConnection:", "WANPPPConnection:" };
    int i;

    for (i = 0; i < 2; i++) {
        char *service = strstr(sResponse, kinds[i]);
        char *typeStart, *typeEnd, *control, *controlEnd;

        if (service == NULL) {
            continue;
        }
        // Back to the start of "urn:...", on to the end of the element.
        for (typeStart = service; typeStart > sResponse && typeStart[-1] != '>'; typeStart--) {
        }
        typeEnd = strchr(service, '<');
        control = strstr(service, "<controlURL>");
        if (typeEnd == NULL || control == NULL || (size_t) (typeEnd - typeStart) >= sizeof(sServiceType)) {
            continue;
        }
        control += 12;
        controlEnd = strchr(control, '<');
        if (controlEnd == NULL) {
            continue;
        }
        memcpy(sServiceType, typeStart, (size_t) (typeEnd - typeStart));
        sServiceType[typeEnd - typeStart] = '\0';
        *controlEnd = '\0';
        if (strncasecmp(control, "http://", 7) == 0) {
            return split_url(control, sControlHost, sizeof(sControlHost), &sControlPort, sControlPath,
                             sizeof(sControlPath));
        }
        snprintf(sControlHost, sizeof(sControlHost), "%s", descriptionHost);
        sControlPort = descriptionPort;
        snprintf(sControlPath, sizeof(sControlPath), "%s%s", control[0] == '/' ? "" : "/", control);
        return 1;
    }
    return 0;
}

static int find_router(void) {
    char location[200], host[64], path[160], request[400];
    int port;

    if (sFound) {
        return 1;
    }
    if (!discover(location, sizeof(location))) {
        printf("UPNP: no router answered\n");
        return 0;
    }
    if (!split_url(location, host, sizeof(host), &port, path, sizeof(path))) {
        return 0;
    }
    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
    if (http_exchange(host, port, request) == 0 || !find_service(host, port)) {
        printf("UPNP: the router's description has no WAN connection service\n");
        return 0;
    }
    printf("UPNP: router at %s:%d%s\n", sControlHost, sControlPort, sControlPath);
    sFound = 1;
    return 1;
}

// One action of the service; the answer is left in sResponse.
static int soap(const char *action, const char *arguments) {
    static char body[1024], request[1700];

    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\"?>\r\n"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
             "<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
             action, sServiceType, arguments, action);
    snprintf(request, sizeof(request),
             "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "SOAPAction: \"%s#%s\"\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
             sControlPath, sControlHost, sControlPort, sServiceType, action, (unsigned) strlen(body), body);
    if (http_exchange(sControlHost, sControlPort, request) == 0) {
        return 0;
    }
    return strstr(sResponse, " 200 ") != NULL;
}

int upnp_external_address(unsigned *address) {
    char *value, *end;
    struct in_addr parsed;

    if (!find_router() || !soap("GetExternalIPAddress", "")) {
        return 0;
    }
    value = strstr(sResponse, "<NewExternalIPAddress>");
    if (value == NULL) {
        return 0;
    }
    value += 22;
    end = strchr(value, '<');
    if (end == NULL) {
        return 0;
    }
    *end = '\0';
    if (inet_aton(value, &parsed) == 0) {
        return 0;
    }
    printf("UPNP: public address %s\n", value);
    *address = parsed.s_addr;
    return 1;
}

int upnp_open_port(int port, unsigned localAddress) {
    char arguments[512];
    struct in_addr local;

    if (!find_router()) {
        return 0;
    }
    local.s_addr = localAddress;
    snprintf(arguments, sizeof(arguments),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
             "<NewEnabled>1</NewEnabled><NewPortMappingDescription>Diddy Kong Racing 3DS</NewPortMappingDescription>"
             "<NewLeaseDuration>0</NewLeaseDuration>",
             port, port, inet_ntoa(local));
    if (!soap("AddPortMapping", arguments)) {
        printf("UPNP: the router did not open port %d\n", port);
        return 0;
    }
    printf("UPNP: port %d open\n", port);
    return 1;
}

void upnp_close_port(int port) {
    char arguments[200];

    if (!sFound) {
        return;
    }
    snprintf(arguments, sizeof(arguments),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>", port);
    soap("DeletePortMapping", arguments);
}
