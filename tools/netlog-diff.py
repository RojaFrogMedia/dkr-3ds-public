#!/usr/bin/env python3
"""Says where two consoles of a multiplayer session parted ways.

    python tools/netlog-diff.py [out/localplay-run/host.log out/localplay-run/join.log]

Run the session with NETLOG.TXT in each console's game folder
(tools/run-localplay.sh ... netlog). The logs then carry, per controller
poll, who drew the game's random numbers (RND), the parts of the state check
(NETPARTS) and every racer's position and data sums (NETRACER). This prints
the first difference of each kind; the earliest one is where to look.
"""
import collections, os, re, subprocess, sys

SKIP_POLLS = 40     # the lobby's start: the consoles are not in step yet
# 32-byte pieces of Object_Racer that hold sound handles: each console hears
# from its own player, so these differ between consoles and mean nothing.
SOUND_CHUNKS = {0x000 // 32, 0x020 // 32, 0x180 // 32, 0x200 // 32, 0x220 // 32}


def load(path):
    rnd = collections.defaultdict(list)
    parts, racers = {}, {}
    for line in open(path, errors="replace"):
        m = re.match(r"RND (\d+) (\w+)", line)
        if m:
            rnd[int(m.group(1))].append(m.group(2))
            continue
        m = re.match(r"NETPARTS: poll (\d+) seed (\w+) positions (\w+) menu (\w+)", line)
        if m:
            parts[int(m.group(1))] = m.groups()[1:]
            continue
        m = re.match(r"NETRACER (\d+) (\d+) (\w+) (\w+) (\w+) \|(.*)", line)
        if m:
            racers[(int(m.group(1)), int(m.group(2)))] = (m.groups()[2:5], m.group(6).split())
    return rnd, parts, racers


def load_objects(path):
    totals, each = {}, collections.defaultdict(dict)
    for line in open(path, errors="replace"):
        m = re.match(r"NETOBJS (\d+) (\d+) (\w+)", line)
        if m:
            totals[int(m.group(1))] = (m.group(2), m.group(3))
            continue
        m = re.match(r"NETOBJ (\d+) (\d+) (-?\d+) (\w+)", line)
        if m:
            each[int(m.group(1))][int(m.group(2))] = (m.group(3), m.group(4))
    return totals, each


def functions(addresses):
    cmd = ("${DEVKITPRO:-/opt/devkitpro}/devkitARM/bin/arm-none-eabi-addr2line -f -e "
           "$HOME/${DKR_BUILD:-dkr-native-build}/dkracing.elf " + " ".join(addresses) + " | paste - -")
    out = subprocess.run(["wsl", "-e", "bash", "-lc", cmd], capture_output=True, text=True,
                         env={**os.environ, "MSYS_NO_PATHCONV": "1"}).stdout
    # Function names only matter; a source path is shortened to what follows native/dkr-pc/.
    return [l.split("native/dkr-pc/")[-1] for l in out.strip().split("\n")]


def main():
    a = sys.argv[1] if len(sys.argv) > 2 else "out/localplay-run/host.log"
    b = sys.argv[2] if len(sys.argv) > 2 else "out/localplay-run/join.log"
    rnd_a, parts_a, racers_a = load(a)
    rnd_b, parts_b, racers_b = load(b)
    last = min(max(parts_a, default=0), max(parts_b, default=0)) - 2   # the run's ragged end

    for poll in sorted(set(rnd_a) | set(rnd_b)):
        if SKIP_POLLS < poll < last and rnd_a.get(poll) != rnd_b.get(poll):
            x, y = rnd_a.get(poll, []), rnd_b.get(poll, [])
            i = 0
            while i < min(len(x), len(y)) and x[i] == y[i]:
                i += 1
            print("random numbers: poll %d, call %d: first console %s, second %s" % (poll, i, x[i:i + 3], y[i:i + 3]))
            addresses = sorted(set(x[i:i + 3] + y[i:i + 3]))
            for address, name in zip(addresses, functions(addresses)):
                print("   ", address, name)
            break
    else:
        print("random numbers: the same")

    for index, name in enumerate(("seed", "positions", "menu")):
        for poll in sorted(set(parts_a) & set(parts_b)):
            if SKIP_POLLS < poll < last and parts_a[poll][index] != parts_b[poll][index]:
                print("state check, %s: first differs at poll %d" % (name, poll))
                break
        else:
            print("state check, %s: the same" % name)

    # every object of the level
    objs_a, objs_b = load_objects(a), load_objects(b)
    for poll in sorted(set(objs_a[0]) & set(objs_b[0])):
        if SKIP_POLLS < poll < last and objs_a[0][poll] != objs_b[0][poll]:
            print("objects: first differ at poll %d (count and sum: %s against %s)" % (poll, objs_a[0][poll], objs_b[0][poll]))
            later = [p for p in sorted(set(objs_a[1]) & set(objs_b[1])) if p >= poll - 16
                     and objs_a[1][p] != objs_b[1][p]]
            for p in later[:3]:
                x, y = objs_a[1][p], objs_b[1][p]
                differing = [(i, x[i], y.get(i)) for i in sorted(x) if x[i] != y.get(i)]
                print("   poll %d: objects that differ (index, behaviour id): %s"
                      % (p, ", ".join("%d (%s)" % (i, v[0]) for i, v, w in differing[:10]) or "none"))
            break
    else:
        print("objects: the same")

    shown = 0
    seen = set()
    for key in sorted(set(racers_a) & set(racers_b)):
        poll, racer = key
        if poll <= SKIP_POLLS or poll >= last:
            continue
        (pos_a, sums_a), (pos_b, sums_b) = racers_a[key], racers_b[key]
        chunks = [i for i, (p, q) in enumerate(zip(sums_a, sums_b)) if p != q and i not in SOUND_CHUNKS]
        new = [c for c in chunks if (racer, c) not in seen]
        if pos_a != pos_b or new:
            for c in chunks:
                seen.add((racer, c))
            print("racer %d at poll %d: position %s, racer data differs at bytes %s"
                  % (racer, poll, "differs" if pos_a != pos_b else "same",
                     ", ".join("0x%03x-0x%03x" % (c * 32, c * 32 + 31) for c in chunks) or "nowhere"))
            shown += 1
            if shown >= 12:
                break
    if shown == 0:
        print("racers: the same")


if __name__ == "__main__":
    main()
