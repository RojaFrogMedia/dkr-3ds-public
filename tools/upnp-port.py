#!/usr/bin/env python3
"""Lists or removes a UDP port forwarding on the home router, through UPnP.

    python tools/upnp-port.py show 6464
    python tools/upnp-port.py remove 6464

The game removes its own forwarding when a host closes an online game; this
is for the case where it could not (the console lost power, an emulator was
killed), since the forwarding is otherwise left on the router.
"""
import re, socket, sys, urllib.request
from urllib.parse import urljoin


def find_service():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    for target in ("urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                   "urn:schemas-upnp-org:device:InternetGatewayDevice:2"):
        s.sendto(("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n"
                  % target).encode(), ("239.255.255.250", 1900))
    try:
        data = s.recv(4096).decode(errors="replace")
    except socket.timeout:
        return None
    location = re.search(r"(?im)^location:\s*(\S+)", data).group(1)
    xml = urllib.request.urlopen(location, timeout=5).read().decode(errors="replace")
    m = re.search(r"<serviceType>(urn:[^<]*WAN(?:IP|PPP)Connection:\d)</serviceType>.*?<controlURL>([^<]*)</controlURL>",
                  xml, re.S)
    return (m.group(1), urljoin(location, m.group(2))) if m else None


def soap(service, url, action, arguments):
    body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:%s xmlns:u="%s">%s</u:%s>'
            '</s:Body></s:Envelope>' % (action, service, arguments, action))
    request = urllib.request.Request(url, body.encode(), {
        "Content-Type": 'text/xml; charset="utf-8"', "SOAPAction": '"%s#%s"' % (service, action)})
    try:
        return urllib.request.urlopen(request, timeout=5).read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return "HTTP %d %s" % (e.code, e.read().decode(errors="replace")[:300])


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("show", "remove"):
        sys.exit(__doc__)
    port = int(sys.argv[2])
    found = find_service()
    if not found:
        sys.exit("no UPnP router answered")
    service, url = found
    key = ("<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>" % port)
    answer = soap(service, url, "GetSpecificPortMappingEntry", key)
    m = re.search(r"<NewInternalClient>([^<]*)</NewInternalClient>", answer)
    d = re.search(r"<NewPortMappingDescription>([^<]*)</NewPortMappingDescription>", answer)
    if not m:
        print("UDP %d: not forwarded" % port)
        return
    print("UDP %d: forwarded to %s (%s)" % (port, m.group(1), d.group(1) if d else "?"))
    if sys.argv[1] == "remove":
        answer = soap(service, url, "DeletePortMapping", key)
        print("removed" if "DeletePortMappingResponse" in answer else "the router refused: " + answer[:200])


if __name__ == "__main__":
    main()
