// Asking the home router to let an online game in (upnp.c). Addresses are
// in network byte order, as in struct in_addr.
#ifndef DKR_3DS_UPNP_H
#define DKR_3DS_UPNP_H

// The router's public address. 0 when the router does not answer.
int upnp_external_address(unsigned *address);
// Forwards UDP `port` to this console. 0 when the router will not.
int upnp_open_port(int port, unsigned localAddress);
void upnp_close_port(int port);

#endif
