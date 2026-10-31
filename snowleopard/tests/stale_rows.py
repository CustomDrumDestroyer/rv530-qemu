#!/usr/bin/env python3
"""stale_rows.py: reproduce stale window content (a list row keeps its old
selection color).

  stale_rows.py [N] [LOG]

Needs a 1440x900 guest with Finder's Utilities folder open in list view, its
window where a fresh login puts it (list rows at y = 456..760).  Each round
selects a row, opens Quick Look on it with the space bar, closes it, then
checks every other row for a piece of selection blue left behind (20-70% of
the row; a fully blue other row is a click Finder did not take).  With LOG
(work/vm.out or work/qemu.log) the number of lines from RV530_VK_CHECK_MIRROR and vk-verify so far is
printed with each round, to line them up with the glitches.
"""
import os, sys, tempfile, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import slctl  # noqa: E402

ROWS = [456, 475, 494, 513, 532, 551, 570, 589, 608, 627, 646, 665, 684, 703, 722, 741, 760]
X0, X1 = 410, 965                   # the list's columns
MARKS = ('missed write: page', 'reverted', 'rv530_vk_verify draw')


def screen():
    fd, path = tempfile.mkstemp(suffix='.ppm')
    os.close(fd)
    slctl.qmp({"execute": "screendump", "arguments": {"filename": path, "format": "ppm"}})
    data = open(path, 'rb').read()
    os.unlink(path)
    head = data.split(b'\n', 3)
    w, h = map(int, head[1].split())
    return w, head[3]


def blue(w, px, y):
    n = 0
    for x in range(X0, X1):
        r, g, b = px[(y * w + x) * 3:(y * w + x) * 3 + 3]
        n += b > 150 and r < 120 and g < 170 and b - r > 60
    return n / (X1 - X0)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    log = sys.argv[2] if len(sys.argv) > 2 else None
    found = 0
    for it in range(n):
        y = ROWS[(it * 7) % len(ROWS)]
        slctl.tablet(500, y, 1440, 900, True)
        time.sleep(1.5)
        slctl.keys(['spc'])
        time.sleep(7)
        slctl.keys(['esc'])
        time.sleep(3)
        w, px = screen()
        # partly blue: a stale piece of an old selection.  A fully blue other
        # row is a click Finder did not take; a few percent is its pale
        # alternate-row tint.
        stale = ['row %d %.0f%% blue' % (ry, blue(w, px, ry) * 100)
                 for ry in ROWS if ry != y and 0.2 < blue(w, px, ry) < 0.7]
        marks = ''
        if log:
            marks = ' (%d checker/verify lines)' % sum(
                1 for l in open(log, errors='replace') if any(m in l for m in MARKS))
        found += bool(stale)
        print('%2d row %d: %s%s' % (it, y, 'STALE ' + '; '.join(stale) if stale else 'ok',
                                    marks), flush=True)
    print('rounds with a stale row: %d of %d' % (found, n))


main()
