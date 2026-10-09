#!/usr/bin/env python3
"""Writes the two-console multiplayer test scripts into native/tests/.

    lp-host.txt / lp-join.txt                    local wireless (LAN stand-in), a race
    lp-host-finish.txt / lp-join-finish.txt      the same, both players finish
    lp-host-left.txt / lp-join-leaves.txt        player 2 quits in mid-race
    lp-host-backout.txt / lp-join-backout.txt    the race ends, player 1 backs out to the title screen
    on-host.txt / on-join.txt                    online by code, both players finish

and prints the game code of an online host at a given address, which
tools/run-localplay.sh puts into the joiner's online.txt:

    python tools/make-lp-tests.py code 127.0.0.1 6464
"""
import os, socket, struct, sys

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
MASK = bytes([0x5D, 0xA3, 0x17, 0xC9, 0x62, 0x8E])


def game_code(address, port):
    """The same as code_from_address in native/dkr-pc/3ds/netplay.c."""
    data = bytearray(socket.inet_aton(address) + struct.pack(">H", port))
    total = 0x3B
    for b in data:
        total = (total * 31 + b) & 0xFFFFFFFF
    check = (total ^ (total >> 8)) & 0xFF
    data = bytearray(b ^ m for b, m in zip(data, MASK)) + bytes([check])
    bits = int.from_bytes(data, "big") << 4
    text = "".join(ALPHABET[(bits >> (55 - 5 * i)) & 31] for i in range(12))
    return "-".join((text[0:4], text[4:8], text[8:12]))


TITLE = """1000 8 START
1100 8 START
1250 8 A
1330 6 STICK_DOWN
1370 6 STICK_DOWN
1420 8 A
"""


def lobby(online, host):
    # The Multiplayer screen: mode, then host or join.
    s = TITLE
    if online:
        s += "1560 6 STICK_DOWN\n1620 8 A\n"
    else:
        s += "1620 8 A\n"
    if host:
        s += "1760 8 A\n2000 SHOT\n2020 BSHOT\n2200 SHOT\n2300 8 A\n"
    else:
        s += "1760 6 STICK_DOWN\n1840 8 A\n2000 SHOT\n"
        if not online:
            s += "2060 8 A\n"
        s += "2200 SHOT\n"
    return s


def menus(first_tap):
    # Character, track and vehicle select: a tap of A every so often.
    s = "2500 SHOT\n2520 BSHOT\n"
    for f in range(first_tap, 5400, 150):
        s += "%d 8 A\n" % f
    for f in (2750, 3350, 3950, 4550, 5150):
        s += "%d SHOT\n" % f
    return s


def write(name, text):
    path = os.path.join(os.path.dirname(__file__), "..", "native", "tests", name)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "code":
        print(game_code(sys.argv[2], int(sys.argv[3])))
        return
    race_h = "5500 5000 A\n"
    race_j = "5500 5000 A+STICK_LEFT\n"
    shots = "".join("%d SHOT\n" % f for f in (5700, 6100, 7100, 8100, 9100, 10100))
    for online, tag in ((0, "lp"), (1, "on")):
        what = "online by code" if online else "local wireless"
        finish_h = ("5500 2400 A\n5700 SHOT\n6300 SHOT\n6600 8 FINISH\n6800 SHOT\n7100 SHOT\n7400 SHOT\n8000 SHOT\n"
                    "8300 SHOT\n8600 SHOT\n8900 SHOT\n9100 8 A\n9400 SHOT\n9700 SHOT\n9800 EXIT\n")
        finish_j = ("5500 2400 A+STICK_LEFT\n5700 SHOT\n6300 SHOT\n6800 SHOT\n7100 SHOT\n7400 SHOT\n7700 8 FINISH\n"
                    "8000 SHOT\n8300 SHOT\n8600 SHOT\n8900 SHOT\n9400 SHOT\n9700 SHOT\n")
        if online:
            write("on-host.txt", "# Player 1, %s: hosts, starts, finishes first.\n" % what + lobby(1, 1) + menus(2600) + finish_h)
            write("on-join.txt", "# Player 2, %s: joins with the code, finishes second.\n" % what + lobby(1, 0) + menus(2680) + finish_j)
            continue
        write("lp-host.txt", "# Player 1, %s: hosts, starts, races.\n" % what + lobby(0, 1) + menus(2600) + race_h + shots + "10700 EXIT\n")
        write("lp-join.txt", "# Player 2, %s: searches, joins, races.\n" % what + lobby(0, 0) + menus(2680) + race_j + shots)
        write("lp-host-finish.txt", "# Player 1, %s: finishes first.\n" % what + lobby(0, 1) + menus(2600) + finish_h)
        write("lp-join-finish.txt", "# Player 2, %s: finishes second.\n" % what + lobby(0, 0) + menus(2680) + finish_j)
        # The race ends, the winner (player 1) goes on from the rankings to the
        # track screen and backs out of it and of the character select: both
        # consoles must show the title screen's own scene from its start.
        title = "".join("%d SHOT\n" % f for f in range(10750, 11801, 150))
        write("lp-host-backout.txt", "# Player 1, %s: wins the race, then backs out of the track menu\n"
              "# and the character select to the title screen.\n" % what + lobby(0, 1) + menus(2600)
              + "5500 1000 A\n6300 SHOT\n6600 8 FINISH\n8900 SHOT\n9100 8 A\n9700 SHOT\n9900 8 B\n10200 SHOT\n"
              "10300 8 B\n10600 8 B\n" + title + "11900 EXIT\n")
        write("lp-join-backout.txt", "# Player 2, %s: comes second; player 1 then takes everybody back\n"
              "# to the title screen.\n" % what + lobby(0, 0) + menus(2680)
              + "5500 1000 A+STICK_LEFT\n6300 SHOT\n7700 8 FINISH\n8900 SHOT\n9700 SHOT\n10200 SHOT\n" + title)
        write("lp-host-left.txt", "# Player 1, %s: the other player quits in mid-race.\n" % what + lobby(0, 1) + menus(2600)
              + "5500 2000 A\n5700 SHOT\n6700 SHOT\n6900 SHOT\n7100 SHOT\n7400 SHOT\n7500 EXIT\n")
        write("lp-join-leaves.txt", "# Player 2, %s: quits in mid-race.\n" % what + lobby(0, 0) + menus(2680)
              + "5500 1000 A+STICK_LEFT\n5700 SHOT\n6600 EXIT\n")
    print("written")


if __name__ == "__main__":
    main()
