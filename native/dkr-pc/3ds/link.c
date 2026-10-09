// Datagrams between consoles for multiplayer. Three ways to carry them:
//
//   local wireless   the 3DS's own console-to-console mode (UDS), what
//                    Download Play and local multiplayer games use. No
//                    access point; the host creates a network that the
//                    others find by scanning. The default.
//   LAN              UDP between consoles (or emulators) on one access
//                    point, chosen by a file lan.txt in the game's folder:
//                        port <n>            optional, default 6464
//                        host <address>      optional: where to look for a
//                                            host besides a broadcast
//                    The emulator test runs use it over 127.0.0.1.
//
//   online           the same UDP packets across the internet, to a host
//                    whose address the players pass around as a code. See
//                    the section further down and upnp.c.
//
// Either way the layer above (netplay.c) sees the same thing: a host with
// some bytes of description, a search that lists hosts, and small packets
// between the host (node 0) and the joiners (nodes 1 to 3).
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "link.h"
#include "upnp.h"

enum { KIND_NONE, KIND_UDS, KIND_LAN, KIND_NET };
enum { ROLE_NONE, ROLE_HOST, ROLE_JOINER };

static int sKind = KIND_NONE;
static int sRole = ROLE_NONE;

// ---------------------------------------------------------------------------
// Local wireless (UDS)
// ---------------------------------------------------------------------------

#define UDS_COMM_ID 0x0DCB3D50          // this game's networks, as scans see them
#define UDS_CHANNEL 1
#define UDS_SCAN_BYTES 0x4000
static const char sPassphrase[] = "Diddy Kong Racing 3DS local play";

static udsBindContext sBind;
static int sUdsBound;

// The scan blocks for a good part of a second, so it has a thread of its own.
static Thread sScanThread;
static LightEvent sScanEvent;
static LightLock sScanLock;
static volatile int sScanBusy, sScanQuit;
static udsNetworkStruct sFoundNets[LINK_MAX_GAMES];
static LinkGame sFoundGames[LINK_MAX_GAMES];
static int sFoundCount;

static void uds_scan_main(void *arg) {
    u32 *buffer = malloc(UDS_SCAN_BYTES);

    (void) arg;
    for (;;) {
        udsNetworkScanInfo *networks = NULL;
        size_t total = 0, i;
        udsNetworkStruct nets[LINK_MAX_GAMES];
        LinkGame games[LINK_MAX_GAMES];
        int count = 0;

        LightEvent_Wait(&sScanEvent);
        if (sScanQuit) {
            break;
        }
        if (buffer != NULL && R_SUCCEEDED(udsScanBeacons(buffer, UDS_SCAN_BYTES, &networks, &total, UDS_COMM_ID, 0, NULL,
                                                         false))) {
            for (i = 0; i < total && count < LINK_MAX_GAMES; i++) {
                size_t actual = 0;

                memset(&games[count], 0, sizeof(games[count]));
                if (R_SUCCEEDED(udsGetNetworkStructApplicationData(&networks[i].network, games[count].info,
                                                                   LINK_INFO_BYTES, &actual)) &&
                    actual == LINK_INFO_BYTES) {
                    nets[count] = networks[i].network;
                    count++;
                }
            }
            free(networks);
        }
        LightLock_Lock(&sScanLock);
        memcpy(sFoundNets, nets, sizeof(nets[0]) * count);
        memcpy(sFoundGames, games, sizeof(games[0]) * count);
        sFoundCount = count;
        LightLock_Unlock(&sScanLock);
        sScanBusy = 0;
    }
    free(buffer);
}

static int uds_start(void) {
    if (R_FAILED(udsInit(0x3000, NULL))) {
        return 0;
    }
    LightEvent_Init(&sScanEvent, RESET_ONESHOT);
    LightLock_Init(&sScanLock);
    sScanQuit = 0;
    sScanBusy = 0;
    sFoundCount = 0;
    sScanThread = threadCreate(uds_scan_main, NULL, 16 * 1024, 0x30, 0, false);
    return 1;
}

static void uds_leave(void) {
    if (sRole == ROLE_HOST) {
        udsDestroyNetwork();
    } else if (sRole == ROLE_JOINER) {
        udsDisconnectNetwork();
    }
    if (sUdsBound) {
        udsUnbind(&sBind);
        sUdsBound = 0;
    }
}

static void uds_stop(void) {
    uds_leave();
    if (sScanThread != NULL) {
        sScanQuit = 1;
        LightEvent_Signal(&sScanEvent);
        threadJoin(sScanThread, U64_MAX);
        threadFree(sScanThread);
        sScanThread = NULL;
    }
    udsExit();
}

static int uds_host(const unsigned char *info) {
    udsNetworkStruct network;

    while (sScanBusy) {     // a scan in flight and a new network do not mix
        svcSleepThread(10000000);
    }
    udsGenerateDefaultNetworkStruct(&network, UDS_COMM_ID, 0, LINK_MAX_NODES);
    if (R_FAILED(udsCreateNetwork(&network, sPassphrase, sizeof(sPassphrase), &sBind, UDS_CHANNEL,
                                  UDS_DEFAULT_RECVBUFSIZE))) {
        return 0;
    }
    sUdsBound = 1;
    udsSetApplicationData(info, LINK_INFO_BYTES);
    return 1;
}

static int uds_search(LinkGame *games, int max) {
    int count;

    if (!sScanBusy && sScanThread != NULL) {
        sScanBusy = 1;
        LightEvent_Signal(&sScanEvent);
    }
    LightLock_Lock(&sScanLock);
    count = sFoundCount < max ? sFoundCount : max;
    memcpy(games, sFoundGames, sizeof(games[0]) * count);
    LightLock_Unlock(&sScanLock);
    return count;
}

static int uds_join(int game) {
    udsNetworkStruct network;
    int tries;

    LightLock_Lock(&sScanLock);
    if (game < 0 || game >= sFoundCount) {
        LightLock_Unlock(&sScanLock);
        return 0;
    }
    network = sFoundNets[game];
    LightLock_Unlock(&sScanLock);
    // A scan in flight and a connection do not mix.
    while (sScanBusy) {
        svcSleepThread(10000000);
    }
    // The first attempts can fail while the radio changes channel.
    for (tries = 0; tries < 10; tries++) {
        if (R_SUCCEEDED(udsConnectNetwork(&network, sPassphrase, sizeof(sPassphrase), &sBind,
                                          UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, UDS_CHANNEL,
                                          UDS_DEFAULT_RECVBUFSIZE))) {
            sUdsBound = 1;
            return 1;
        }
    }
    return 0;
}

// Node n of this layer is UDS network node n + 1 (the host is node id 1).
static int uds_send(int node, const void *data, int size) {
    Result r = udsSendTo((u16) (node + 1), UDS_CHANNEL, UDS_SENDFLAG_Default, data, (size_t) size);

    return !UDS_CHECK_SENDTO_FATALERROR(r);
}

static int uds_receive(void *data, int size, int *node) {
    size_t actual = 0;
    u16 from = 0;

    if (!sUdsBound || R_FAILED(udsPullPacket(&sBind, data, (size_t) size, &actual, &from)) || actual == 0) {
        return 0;
    }
    *node = (int) from - 1;
    return (int) actual;
}

// ---------------------------------------------------------------------------
// LAN (UDP)
// ---------------------------------------------------------------------------

#define LAN_MAGIC 0x4C4B4444u           // precedes every packet: "DDKL"
#define LAN_PROBE 0xFFFFFFF0u           // a search asking hosts to answer
#define LAN_ANNOUNCE 0xFFFFFFF1u        // a host's answer, followed by its info
#define SOC_BUFFER_BYTES 0x100000

static int sSocket = -1;
static int sLanPort = 6464;
static struct in_addr sLanHostHint;     // lan.txt's host line
static int sLanHasHint;
static struct sockaddr_in sLanPeers[LINK_MAX_NODES];    // by node; [0] is the host on a joiner
static int sLanPeerCount;                               // host: nodes met so far, + 1
static unsigned char sLanInfo[LINK_INFO_BYTES];
static int sLanDoorsClosed;
static struct sockaddr_in sLanFound[LINK_MAX_GAMES];
static LinkGame sLanGames[LINK_MAX_GAMES];
static u64 sLanSeen[LINK_MAX_GAMES];
static int sLanFoundCount;
static u64 sLanLastProbe;

typedef struct {
    u32 magic;
    u32 kind;       // LAN_PROBE, LAN_ANNOUNCE, or anything else for the layer above
} LanHeader;

static int lan_open_socket(int port) {
    struct sockaddr_in addr;
    int yes = 1;

    if (sSocket >= 0) {
        close(sSocket);
    }
    sSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (sSocket < 0) {
        return 0;
    }
    setsockopt(sSocket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u16) port);
    if (bind(sSocket, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        printf("LINK: cannot bind port %d (%d)\n", port, errno);
        close(sSocket);
        sSocket = -1;
        return 0;
    }
    fcntl(sSocket, F_SETFL, fcntl(sSocket, F_GETFL, 0) | O_NONBLOCK);
    return 1;
}

static int lan_start(void) {
    static void *sSocBuffer;

    if (sSocBuffer == NULL) {
        sSocBuffer = memalign(0x1000, SOC_BUFFER_BYTES);
        if (sSocBuffer == NULL || R_FAILED(socInit(sSocBuffer, SOC_BUFFER_BYTES))) {
            return 0;
        }
    }
    return 1;
}

static void lan_leave(void) {
    if (sSocket >= 0) {
        close(sSocket);
        sSocket = -1;
    }
    sLanPeerCount = 0;
    sLanFoundCount = 0;
    sLanDoorsClosed = 0;
}

static int lan_host(const unsigned char *info) {
    memcpy(sLanInfo, info, LINK_INFO_BYTES);
    sLanPeerCount = 1;
    sLanDoorsClosed = 0;
    return lan_open_socket(sLanPort);
}

static void lan_send_raw(const struct sockaddr_in *to, u32 kind, const void *data, int size) {
    u8 packet[sizeof(LanHeader) + 256];
    LanHeader header = { LAN_MAGIC, kind };

    if (sSocket < 0 || size < 0 || size > 256) {
        return;
    }
    memcpy(packet, &header, sizeof(header));
    if (size > 0) {
        memcpy(packet + sizeof(header), data, (size_t) size);
    }
    if (sendto(sSocket, packet, sizeof(header) + (size_t) size, 0, (const struct sockaddr *) to, sizeof(*to)) < 0) {
        static int sReported;

        if (sReported < 4) {
            sReported++;
            printf("LINK: send to %s failed (%d)\n", inet_ntoa(to->sin_addr), errno);
        }
    }
}

static int lan_same(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

// One packet for the layer above, with probes and announcements handled here.
static int lan_receive(void *data, int size, int *node) {
    for (;;) {
        u8 packet[sizeof(LanHeader) + 256];
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        LanHeader header;
        int n, payload, i;

        if (sSocket < 0) {
            return 0;
        }
        n = recvfrom(sSocket, packet, sizeof(packet), 0, (struct sockaddr *) &from, &fromLen);
        if (n < (int) sizeof(header)) {
            return 0;
        }
        memcpy(&header, packet, sizeof(header));
        if (header.magic != LAN_MAGIC) {
            continue;
        }
        payload = n - (int) sizeof(header);
        if (header.kind == LAN_PROBE) {
            if (sRole == ROLE_HOST && !sLanDoorsClosed && sKind == KIND_LAN) {
                lan_send_raw(&from, LAN_ANNOUNCE, sLanInfo, LINK_INFO_BYTES);
            }
            continue;
        }
        if (header.kind == LAN_ANNOUNCE) {
            if (sRole == ROLE_NONE && payload == LINK_INFO_BYTES) {
                for (i = 0; i < sLanFoundCount && !lan_same(&sLanFound[i], &from); i++) {
                }
                if (i < LINK_MAX_GAMES) {
                    sLanFound[i] = from;
                    memcpy(sLanGames[i].info, packet + sizeof(header), LINK_INFO_BYTES);
                    sLanSeen[i] = svcGetSystemTick();
                    if (i == sLanFoundCount) {
                        sLanFoundCount++;
                    }
                }
            }
            continue;
        }
        if (sRole == ROLE_HOST) {
            for (i = 1; i < sLanPeerCount && !lan_same(&sLanPeers[i], &from); i++) {
            }
            if (i == sLanPeerCount) {
                if (sLanPeerCount >= LINK_MAX_NODES || sLanDoorsClosed) {
                    continue;
                }
                sLanPeers[sLanPeerCount++] = from;
            }
            *node = i;
        } else if (sRole == ROLE_JOINER && lan_same(&sLanPeers[0], &from)) {
            *node = 0;
        } else {
            continue;
        }
        if (payload > size) {
            payload = size;
        }
        memcpy(data, packet + sizeof(header), (size_t) payload);
        return payload;
    }
}

static int lan_search(LinkGame *games, int max) {
    u64 now = svcGetSystemTick();
    u8 scratch[8];
    int node, i, count = 0;

    if (sSocket < 0 && !lan_open_socket(0)) {
        return 0;
    }
    if (now - sLanLastProbe > SYSCLOCK_ARM11 / 2) {
        struct sockaddr_in to;

        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons((u16) sLanPort);
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        lan_send_raw(&to, LAN_PROBE, NULL, 0);
        if (sLanHasHint) {
            to.sin_addr = sLanHostHint;
            lan_send_raw(&to, LAN_PROBE, NULL, 0);
        }
        sLanLastProbe = now;
    }
    while (lan_receive(scratch, sizeof(scratch), &node) > 0) {
    }
    now = svcGetSystemTick();
    // A host that has stopped answering drops off the list.
    for (i = 0; i < sLanFoundCount; i++) {
        if (now - sLanSeen[i] < (u64) SYSCLOCK_ARM11 * 2) {
            sLanFound[count] = sLanFound[i];
            sLanGames[count] = sLanGames[i];
            sLanSeen[count] = sLanSeen[i];
            count++;
        }
    }
    sLanFoundCount = count;
    if (count > max) {
        count = max;
    }
    memcpy(games, sLanGames, sizeof(games[0]) * count);
    return count;
}

static int lan_join(int game) {
    if (game < 0 || game >= sLanFoundCount) {
        return 0;
    }
    sLanPeers[0] = sLanFound[game];
    return 1;
}

static int lan_send(int node, const void *data, int size) {
    if (node < 0 || node >= LINK_MAX_NODES || (sRole == ROLE_HOST && node >= sLanPeerCount)) {
        return 0;
    }
    lan_send_raw(&sLanPeers[node], 0, data, size);
    return 1;
}

static void lan_read_config(const char *dir, int *wanted) {
    char path[160], line[96], word[16], value[64];
    FILE *f;

    snprintf(path, sizeof(path), "%s/lan.txt", dir);
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    *wanted = 1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%15s %63s", word, value) != 2) {
            continue;
        }
        if (strcmp(word, "port") == 0 && atoi(value) > 0) {
            sLanPort = atoi(value);
        } else if (strcmp(word, "host") == 0 && inet_aton(value, &sLanHostHint) != 0) {
            sLanHasHint = 1;
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Online: the same UDP packets across the internet. There is no search: the
// host's address is the game's code (netplay.c), given to the others by
// whatever means the players have. The host's router has to let the port in;
// upnp.c asks it to.
//
// online.txt in the game's folder, all optional:
//     port <n>             the UDP port to host on (default 6464)
//     address <a.b.c.d>    the address to put in the code, instead of asking
//                          the router (a port forwarded by hand; the tests)
//     noupnp               do not ask the router for anything
// ---------------------------------------------------------------------------

static int sNetPort = 6464;
static struct in_addr sNetAddressGiven;
static int sNetHasAddress, sNetNoUpnp;
static unsigned sNetHostAddress;        // what the code carries
static int sNetHostPort, sNetOpen, sNetMapped;

static void net_read_config(const char *dir) {
    char path[160], line[96], word[16], value[64];
    FILE *f;

    snprintf(path, sizeof(path), "%s/online.txt", dir);
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        value[0] = '\0';
        if (sscanf(line, "%15s %63s", word, value) < 1) {
            continue;
        }
        if (strcmp(word, "port") == 0 && atoi(value) > 0) {
            sNetPort = atoi(value);
        } else if (strcmp(word, "address") == 0 && inet_aton(value, &sNetAddressGiven) != 0) {
            sNetHasAddress = 1;
        } else if (strcmp(word, "noupnp") == 0) {
            sNetNoUpnp = 1;
        }
    }
    fclose(f);
}

static int net_host(const unsigned char *info) {
    struct in_addr local = { 0 }, mask, broadcast;
    int tries;

    memcpy(sLanInfo, info, LINK_INFO_BYTES);
    sLanPeerCount = 1;
    sLanDoorsClosed = 0;
    // The usual port, or the next free one.
    for (tries = 0; tries < 8 && !lan_open_socket(sNetPort + tries); tries++) {
    }
    if (tries == 8) {
        return 0;
    }
    sNetHostPort = sNetPort + tries;
    SOCU_GetIPInfo(&local, &mask, &broadcast);
    sNetHostAddress = local.s_addr;     // good on the same network, whatever else happens
    sNetOpen = 0;
    sNetMapped = 0;
    if (sNetHasAddress) {
        sNetHostAddress = sNetAddressGiven.s_addr;
        sNetOpen = 1;
    } else if (!sNetNoUpnp) {
        unsigned external = 0;

        if (upnp_open_port(sNetHostPort, local.s_addr)) {
            sNetMapped = 1;
            if (upnp_external_address(&external) && external != 0) {
                sNetHostAddress = external;
                sNetOpen = 1;
            }
        }
    }
    return 1;
}

static void net_leave(void) {
    if (sNetMapped) {
        upnp_close_port(sNetHostPort);
        sNetMapped = 0;
    }
    lan_leave();
}

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

int link_start(const char *dir, int online) {
    int lan = 0;

    if (sKind != KIND_NONE) {
        return 1;
    }
    if (online) {
        // The console has to be on an access point: its wireless switched
        // on and one of its internet connections in reach. The system makes
        // the connection by itself; this only looks whether it has.
        u32 wifi = 0;

        net_read_config(dir);
        if (R_FAILED(acInit())) {
            wifi = 1;       // no way to ask: let the sockets say
        } else {
            ACU_GetWifiStatus(&wifi);
            acExit();
        }
        if (wifi == 0) {
            printf("LINK: the console is not connected to an access point\n");
        } else if (lan_start()) {
            sKind = KIND_NET;
        }
    } else {
        lan_read_config(dir, &lan);
        if (lan) {
            if (lan_start()) {
                sKind = KIND_LAN;
            }
        } else if (uds_start()) {
            sKind = KIND_UDS;
        }
    }
    sRole = ROLE_NONE;
    printf("LINK: %s\n", sKind == KIND_NONE ? "no wireless service" : link_kind());
    return sKind != KIND_NONE;
}

void link_stop(void) {
    if (sKind == KIND_UDS) {
        uds_stop();
    } else if (sKind == KIND_LAN) {
        lan_leave();
    } else if (sKind == KIND_NET) {
        net_leave();
    }
    sKind = KIND_NONE;
    sRole = ROLE_NONE;
}

const char *link_kind(void) {
    return sKind == KIND_NET ? "the internet" : (sKind == KIND_LAN ? "LAN" : "local wireless");
}

int link_host(const unsigned char *info) {
    int ok = 0;

    if (sKind == KIND_UDS) {
        ok = uds_host(info);
    } else if (sKind == KIND_LAN) {
        ok = lan_host(info);
    } else if (sKind == KIND_NET) {
        ok = net_host(info);
    }
    if (ok) {
        sRole = ROLE_HOST;
    }
    return ok;
}

int link_host_address(unsigned *address, int *port) {
    if (sKind != KIND_NET || sRole != ROLE_HOST) {
        return 0;
    }
    *address = sNetHostAddress;
    *port = sNetHostPort;
    return sNetOpen ? 2 : 1;
}

void link_host_set_info(const unsigned char *info) {
    if (sRole != ROLE_HOST) {
        return;
    }
    if (sKind == KIND_UDS) {
        udsSetApplicationData(info, LINK_INFO_BYTES);
    } else {
        memcpy(sLanInfo, info, LINK_INFO_BYTES);
    }
}

void link_host_close_doors(void) {
    if (sRole != ROLE_HOST) {
        return;
    }
    if (sKind == KIND_UDS) {
        udsSetNewConnectionsBlocked(true, true, false);
    } else {
        sLanDoorsClosed = 1;
    }
}

// And open again: the session went back to its lobby (netplay_back_to_lobby).
void link_host_open_doors(void) {
    if (sRole != ROLE_HOST) {
        return;
    }
    if (sKind == KIND_UDS) {
        udsSetNewConnectionsBlocked(false, true, false);
    } else {
        sLanDoorsClosed = 0;
    }
}

int link_search(LinkGame *games, int max) {
    if (sRole != ROLE_NONE) {
        return 0;
    }
    return sKind == KIND_UDS ? uds_search(games, max) : (sKind == KIND_LAN ? lan_search(games, max) : 0);
}

int link_join(int game) {
    int ok = sKind == KIND_UDS ? uds_join(game) : (sKind == KIND_LAN ? lan_join(game) : 0);

    if (ok) {
        sRole = ROLE_JOINER;
    }
    return ok;
}

int link_join_address(unsigned address, int port) {
    if (sKind != KIND_NET || sRole != ROLE_NONE) {
        return 0;
    }
    if (sSocket < 0 && !lan_open_socket(0)) {
        return 0;
    }
    memset(&sLanPeers[0], 0, sizeof(sLanPeers[0]));
    sLanPeers[0].sin_family = AF_INET;
    sLanPeers[0].sin_port = htons((u16) port);
    sLanPeers[0].sin_addr.s_addr = address;
    sRole = ROLE_JOINER;
    return 1;
}

int link_send(int node, const void *data, int size) {
    if (sRole == ROLE_NONE) {
        return 0;
    }
    return sKind == KIND_UDS ? uds_send(node, data, size) : lan_send(node, data, size);
}

int link_receive(void *data, int size, int *node) {
    if (sRole == ROLE_NONE) {
        return 0;
    }
    return sKind == KIND_UDS ? uds_receive(data, size, node) : lan_receive(data, size, node);
}
