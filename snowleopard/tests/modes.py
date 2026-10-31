# Display modes offered by the driver (from the device's EDID), and mode
# switching.  Run in the guest's GUI session with the system Python 2.6:
#
#   python modes.py                 list the 32-bpp modes
#   python modes.py 2560 1600       switch to 2560x1600 for 10 s, then back
#
# While it is switched, check the host side: `./slctl.py shot x.png` should
# be a 2560x1600 image.
import sys, time
import Quartz

d = Quartz.CGMainDisplayID()
modes = [m for m in Quartz.CGDisplayAvailableModes(d) if m.get('BitsPerPixel') == 32]
print(sorted(set((int(m['Width']), int(m['Height'])) for m in modes)))
if len(sys.argv) == 3:
    w, h = int(sys.argv[1]), int(sys.argv[2])
    want = [m for m in modes if int(m['Width']) == w and int(m['Height']) == h]
    if not want:
        print('%dx%d not offered' % (w, h))
        sys.exit(1)
    old = Quartz.CGDisplayCurrentMode(d)
    cfg = Quartz.CGBeginDisplayConfiguration(None)[1]
    Quartz.CGConfigureDisplayMode(cfg, d, want[0])
    err = Quartz.CGCompleteDisplayConfiguration(cfg, Quartz.kCGConfigureForAppOnly)
    print('switch to %dx%d: error %d, now %dx%d' % (w, h, err, Quartz.CGDisplayPixelsWide(d),
                                                   Quartz.CGDisplayPixelsHigh(d)))
    sys.stdout.flush()
    time.sleep(10)
    cfg = Quartz.CGBeginDisplayConfiguration(None)[1]
    Quartz.CGConfigureDisplayMode(cfg, d, old)
    Quartz.CGCompleteDisplayConfiguration(cfg, Quartz.kCGConfigureForAppOnly)
    print('back to %dx%d' % (Quartz.CGDisplayPixelsWide(d), Quartz.CGDisplayPixelsHigh(d)))
