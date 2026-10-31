#!/usr/bin/env python3
"""Drop host files into the Snow Leopard guest, or fetch the files
being dragged in the guest.

    dnd-send.py [--sock work/dnd.sock] --at X,Y FILE_OR_FOLDER...
    dnd-send.py --query      names being dragged in the guest (none: no drag)
    dnd-send.py --fetch      copy them to the host; prints one path per line

X,Y are guest screen pixels: the guest agent (RV530Agent) delivers the
files to whatever is there (a Finder window, the desktop, an application
window or Dock icon), as if they had been dragged there.  The QEMU GTK
window runs this on drops and drags when started with DND=1.
"""
import argparse, json, os, socket, sys


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--sock', default=os.environ.get(
        'RV530_DND_SOCK', os.path.join(here, '..', '..', 'snowleopard', 'work', 'dnd.sock')))
    ap.add_argument('--at')
    ap.add_argument('--query', action='store_true')
    ap.add_argument('--fetch', action='store_true')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    if a.query:
        req = {'op': 'drag-query'}
    elif a.fetch:
        req = {'op': 'fetch'}
    elif a.at and a.files:
        x, y = (int(v) for v in a.at.split(','))
        req = {'x': x, 'y': y, 'files': [os.path.abspath(f) for f in a.files]}
    else:
        ap.error('--at X,Y FILE..., --query or --fetch')
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(a.sock)
    except OSError as e:
        sys.exit('no guest agent connected (%s: %s)' % (a.sock, e.strerror))
    if a.query or a.fetch:
        # QEMU's GTK window waits for these with its main loop stopped:
        # never hold it for long, whatever happens on the host side
        s.settimeout(5)
    with s, s.makefile('rwb') as f:
        f.write((json.dumps(req) + '\n').encode())
        f.flush()
        try:
            r = json.loads(f.readline() or '{"ok": false, "message": "no answer"}')
        except socket.timeout:
            sys.exit('no answer from dnd-host.py')
    if a.query:
        print('\n'.join(r.get('files', [])))
        sys.exit(0 if r.get('files') else 1)
    if a.fetch:
        print('\n'.join(r.get('paths', [])))
        sys.exit(0 if r.get('ok') else 1)
    print(('dropped' if r['ok'] else 'failed') + (': ' + r['message'] if r['message'] else ''))
    sys.exit(0 if r['ok'] else 1)


if __name__ == '__main__':
    main()
