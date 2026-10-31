#!/usr/bin/env python3
"""Host end of the drag-and-drop channel.

slirp runs this once per guest connection (sl-vm.sh with DND=1):

    -netdev user,...,guestfwd=tcp:10.0.2.100:7650-cmd:dnd-host.py SOCK

stdin/stdout are the TCP connection of the guest agent (RV530Agent).  It
listens on the unix socket SOCK for requests from dnd-send.py (run by the
QEMU GTK window, or by hand) and talks to the agent.  The guest can only
ever receive what a request names, and can only send what it announced as
being dragged.

Agent protocol (lines are UTF-8, paths percent-encoded):
    agent -> host   HELLO 1
  host -> guest drops:
    host -> agent   DROP <x> <y> <entries>
                    DIR <rel> | FILE <size> <rel> + <size> raw bytes ...
                    END
    agent -> host   DONE ok|error <message>
  guest -> host drags:
    agent -> host   DRAG-START <n>, then n lines PATH <guest path>
    agent -> host   DRAG-END
    host -> agent   GET <guest path>     (one of the announced paths)
    agent -> host   DIR/FILE entries as above (rel to the path's folder), END
Requests on SOCK: one JSON line, answered with one JSON line:
    {"x":..,"y":..,"files":[abs paths]}  -> {"ok":bool,"message":str}
    {"op":"drag-query"}                  -> {"files":[names]} (empty: no drag)
    {"op":"fetch"}                       -> {"ok":bool,"paths":[host paths]}

drag-query and fetch never wait for the guest: the QEMU GTK window
runs them with its main loop blocked, and the agent's connection is a
slirp guestfwd served by that same loop, so a GET from inside them would
deadlock the VM.  Instead the dragged files are copied to the host as soon
as the agent announces the drag (prefetch); drag-query reports the drag
only once that copy is complete, and fetch hands out the copy.
"""
import json, os, queue, shutil, socket, stat, sys, tempfile, threading, urllib.parse

CHUNK = 1 << 16
q = lambda r: urllib.parse.quote(r, safe='')


def log(*a):
    print('dnd-host:', *a, file=sys.stderr, flush=True)


def entries(paths):
    """(kind, relpath, abspath) for the dropped files and folder trees;
    only regular files and directories, no symlinks."""
    out = []
    for p in paths:
        p = os.path.abspath(p)
        base = os.path.dirname(p)
        st = os.lstat(p)
        if stat.S_ISREG(st.st_mode):
            out.append(('FILE', os.path.basename(p), p))
        elif stat.S_ISDIR(st.st_mode):
            for root, dirs, files in os.walk(p):
                dirs[:] = sorted(d for d in dirs
                                 if not os.path.islink(os.path.join(root, d)))
                out.append(('DIR', os.path.relpath(root, base), root))
                for f in sorted(files):
                    fp = os.path.join(root, f)
                    if stat.S_ISREG(os.lstat(fp).st_mode):
                        out.append(('FILE', os.path.relpath(fp, base), fp))
    return out


def safe_rel(rel):
    parts = rel.split('/')
    if not rel or rel.startswith('/') or any(p in ('', '.', '..') for p in parts):
        raise ValueError('rejected path %r' % rel)
    return rel


class Agent:
    def __init__(self, rfile, wfile):
        self.r, self.w = rfile, wfile
        self.lock = threading.Lock()        # one conversation at a time
        self.done = queue.Queue()           # DONE lines for drops
        self.stream = queue.Queue()         # DIR/FILE/END items for GET
        self.drag = []                      # guest paths being dragged
        self.alive = True
        self.gen = 0                        # drags announced so far
        self.pre = None                     # (gen, ok, host paths) of the last copy
        self.pre_dirs = []                  # copies kept for drops still in progress

    def reader(self):
        try:
            while True:
                line = self.r.readline()
                if not line:
                    break
                line = line.decode('utf-8', 'replace').rstrip('\n')
                f = line.split(' ')
                if f[0] == 'DONE':
                    self.done.put(line)
                elif f[0] == 'DRAG-START':
                    paths = []
                    for _ in range(int(f[1])):
                        p = self.r.readline().decode('utf-8', 'replace').rstrip('\n')
                        if p.startswith('PATH '):
                            paths.append(urllib.parse.unquote(p[5:]))
                    self.drag = paths
                    self.gen += 1
                    threading.Thread(target=self.prefetch, args=(self.gen, paths),
                                     daemon=True).start()
                elif f[0] == 'DRAG-END':
                    self.drag = []
                elif f[0] == 'DIR' and len(f) == 2:
                    self.stream.put(('DIR', urllib.parse.unquote(f[1]), None))
                elif f[0] == 'FILE' and len(f) == 3:
                    size = int(f[1])
                    data = bytearray()
                    while len(data) < size:
                        b = self.r.read(min(CHUNK, size - len(data)))
                        if not b:
                            raise EOFError
                        data += b
                    self.stream.put(('FILE', urllib.parse.unquote(f[2]), bytes(data)))
                elif f[0] in ('END', 'ERROR'):
                    self.stream.put((f[0], line, None))
        except (EOFError, OSError, ValueError):
            pass
        self.alive = False
        self.done.put(None)
        self.stream.put(None)

    def send(self, b):
        self.w.write(b)
        self.w.flush()

    def drop(self, req):
        items = entries(req['files'])
        with self.lock:
            self.send(('DROP %d %d %d\n' % (req['x'], req['y'], len(items))).encode())
            for kind, rel, path in items:
                if kind == 'DIR':
                    self.send(('DIR %s\n' % q(rel)).encode())
                    continue
                size = os.path.getsize(path)
                self.w.write(('FILE %d %s\n' % (size, q(rel))).encode())
                with open(path, 'rb') as fh:
                    left = size
                    while left:
                        b = fh.read(min(CHUNK, left))
                        if not b:
                            raise IOError('%s shrank while sending' % path)
                        self.w.write(b)
                        left -= len(b)
            self.send(b'END\n')
            line = self.done.get()
        if line is None:
            raise EOFError('agent disconnected')
        parts = line.split(' ', 2)
        return parts[1:2] == ['ok'], parts[2] if len(parts) > 2 else ''

    def prefetch(self, gen, drag):
        """copy the files of drag number gen to the host, in the background"""
        try:
            ok, paths, dest = self.copy(drag)
        except Exception as e:              # agent gone, unreadable file, bad path
            log('drag copy failed: %s' % e)
            ok, paths, dest = False, [], None
        if gen != self.gen:                 # a newer drag started meanwhile
            if dest:
                shutil.rmtree(dest, ignore_errors=True)
            return
        self.pre = (gen, ok, paths)
        if dest:
            self.pre_dirs.append(dest)
            # the host copies a drop right away; keep the last few anyway
            while len(self.pre_dirs) > 3:
                shutil.rmtree(self.pre_dirs.pop(0), ignore_errors=True)

    def ready(self):
        """the files of the current drag, once they are on the host"""
        pre = self.pre
        return bool(self.drag) and pre is not None and pre[0] == self.gen and pre[1]

    def fetch(self):
        """the host copies of the files being dragged (no guest round trip)"""
        pre = self.pre
        if pre is None or pre[0] != self.gen:
            return False, []
        return pre[1], pre[2]

    def copy(self, drag):
        """copy the guest paths drag into a new host folder"""
        if not drag:
            return False, [], None
        dest = tempfile.mkdtemp(prefix='rv530-drag-',
                                dir=os.environ.get('XDG_RUNTIME_DIR') or None)
        top = []
        with self.lock:
            for gpath in drag:
                self.send(('GET %s\n' % q(gpath)).encode())
                while True:
                    item = self.stream.get()
                    if item is None:
                        raise EOFError('agent disconnected')
                    kind, rel, data = item
                    if kind == 'END':
                        break
                    if kind == 'ERROR':
                        raise IOError(rel)
                    path = os.path.join(dest, safe_rel(rel))
                    if kind == 'DIR':
                        os.makedirs(path, exist_ok=True)
                    else:
                        os.makedirs(os.path.dirname(path), exist_ok=True)
                        with open(path, 'wb') as fh:
                            fh.write(data)
                    if '/' not in rel and path not in top:
                        top.append(path)
        return True, top, dest


def main():
    sockpath = sys.argv[1]
    agent = Agent(sys.stdin.buffer, sys.stdout.buffer)
    hello = agent.r.readline().decode('utf-8', 'replace').strip()
    if not hello.startswith('HELLO '):
        log('unexpected greeting %r' % hello)
        return
    threading.Thread(target=agent.reader, daemon=True).start()
    # newest agent connection wins the socket
    try:
        os.unlink(sockpath)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sockpath)
    os.chmod(sockpath, 0o600)
    mine = os.stat(sockpath).st_ino
    srv.listen(4)
    srv.settimeout(1.0)
    log('agent connected (%s), listening on %s' % (hello, sockpath))
    try:
        while agent.alive:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            with conn, conn.makefile('rwb') as f:
                try:
                    req = json.loads(f.readline())
                    op = req.get('op', 'drop')
                    if op == 'drag-query':
                        ans = {'files': [os.path.basename(p) for p in agent.drag]
                               if agent.ready() else []}
                    elif op == 'fetch':
                        ok, paths = agent.fetch()
                        ans = {'ok': ok, 'paths': paths}
                    else:
                        ok, msg = agent.drop(req)
                        ans = {'ok': ok, 'message': msg}
                except EOFError:
                    ans = {'ok': False, 'message': 'agent disconnected'}
                except Exception as e:      # bad request or unreadable file
                    ans = {'ok': False, 'message': str(e)}
                f.write((json.dumps(ans) + '\n').encode())
    finally:
        # only our own socket: when the agent reconnects, the next
        # dnd-host.py has already bound the path while this one winds down
        try:
            if os.stat(sockpath).st_ino == mine:
                os.unlink(sockpath)
        except OSError:
            pass


if __name__ == '__main__':
    main()
