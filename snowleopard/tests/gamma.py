# Hardware gamma check: set a red-only display transfer table for a few
# seconds, then restore ColorSync's.  The guest cannot see the result itself
# (CGDisplayCreateImage reads the framebuffer before the LUT), so take a host
# screenshot meanwhile:
#   ssh guest python - < gamma.py &  sleep 2; ./slctl.py shot tinted.png
# Expect green and blue at ~20% of normal in tinted.png and the original
# colors once the script has exited.
import sys, time
import Quartz
d = Quartz.CGMainDisplayID()
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
err = Quartz.CGSetDisplayTransferByFormula(d, 0, 1, 1, 0, 0.2, 1, 0, 0.2, 1)
print "CGSetDisplayTransferByFormula:", err
time.sleep(secs)
Quartz.CGDisplayRestoreColorSyncSettings()
print "restored"
