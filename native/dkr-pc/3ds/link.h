// Datagrams between consoles for Local Play (link.c): finding a host,
// joining it, and small packets either way. netplay.c is the only user.
#ifndef DKR_3DS_LINK_H
#define DKR_3DS_LINK_H

#define LINK_MAX_NODES 4        // the host and three joiners
#define LINK_INFO_BYTES 32      // what a host tells searching consoles about its game
#define LINK_MAX_GAMES 6

// A game found by a search.
typedef struct {
    unsigned char info[LINK_INFO_BYTES];
} LinkGame;

// Starts the service: local wireless (or its LAN stand-in, when a file
// lan.txt in `dir`, the game's folder, asks for it), or the internet when
// `online` is set. Returns 0 when there is nothing to use.
int link_start(const char *dir, int online);
void link_stop(void);
const char *link_kind(void);    // "local wireless", "LAN" or "the internet"

// Online only. Hosting: where the others reach this console (network byte
// order). Returns 0 when not hosting online, 1 when the address is only good
// on this console's own network (the router let nothing in), 2 when it is
// the public one. Joining: go to a host directly instead of searching.
int link_host_address(unsigned *address, int *port);
int link_join_address(unsigned address, int port);

// Hosting: become findable, with `info` shown to searching consoles.
int link_host(const unsigned char *info);
void link_host_set_info(const unsigned char *info);
void link_host_close_doors(void);       // no new joiners: the game has started
void link_host_open_doors(void);        // joiners again: back in the lobby

// Searching. link_search runs a search in the background when none is in
// flight and returns the games the last one found.
int link_search(LinkGame *games, int max);
int link_join(int game);                // index into the last search's results

// Packets. Node 0 is the host; a joiner only ever talks to node 0. On the
// host, joiners are nodes 1..3 in the order the link layer met them.
int link_send(int node, const void *data, int size);
int link_receive(void *data, int size, int *node);      // 0 when nothing is waiting

#endif
