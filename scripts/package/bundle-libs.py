#!/usr/bin/env python3
"""bundle-libs.py: make a QEMU install tree relocatable (release packages).

  bundle-libs.py ROOT BINARY...      (BINARY paths are relative to ROOT)

Copies the shared libraries the binaries need into ROOT/lib and points the
binaries and libraries at them, then checks that nothing still refers to the
machine it was built on.

Linux: the libraries the binaries need, and what those need in turn, go into
lib/, except those a desktop distribution provides and that must match the
system (glibc, the GPU and window-system stacks, GTK and GLib, sound servers;
EXCLUDE below).  RUNPATH
becomes $ORIGIN/../lib for the binaries and $ORIGIN for the libraries
(patchelf).

Windows (MSYS2): every DLL outside the Windows directory goes into bin/,
next to the programs, where the Windows loader looks first; GTK's runtime
data (GSettings schemas, gdk-pixbuf loaders) goes into share/ and lib/.

macOS: every dependency outside /usr/lib and /System goes into lib/, with
@executable_path/../lib/NAME in the binaries and @loader_path/NAME in the
libraries (install_name_tool); rpaths that point at the build's package
manager are removed, and everything is signed again ad hoc (codesign), since
editing load commands invalidates the signature.  The caller signs the
emulator itself again afterwards when it needs entitlements.
"""
import os
import re
import shutil
import subprocess
import sys

EXCLUDE = re.compile(r'''^(
    ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libnsl|libmvec|
    libgcc_s|libstdc\+\+|
    libGL|libGLX|libGLdispatch|libEGL|libGLES.*|libOpenGL|libvulkan|libdrm.*|libgbm|
    libX.*|libxcb.*|libxkbcommon.*|libwayland-.*|libdecor.*|libepoxy|
    libgtk-3|libgdk-3|libgdk_pixbuf-2\.0|libatk.*|libatspi|libpango.*|libcairo.*|
    libharfbuzz.*|libfontconfig|libfreetype|libfribidi|libthai|libdatrie|libgraphite2|
    libglib-2\.0|libgobject-2\.0|libgio-2\.0|libgmodule-2\.0|libgthread-2\.0|
    libffi|libpcre2-.*|libpcre|libmount|libblkid|libselinux|libuuid|libz|libexpat|
    libpng16|libjpeg|libtiff|libwebp.*|liblzma|libzstd|liblz4|libbrotli.*|
    libdbus-1|libsystemd|libudev|libcap|libgcrypt|libgpg-error|
    libasound|libpulse.*|libpipewire-.*|libjack|libsndio|
    libgnutls|libnettle|libhogweed|libgmp|libtasn1|libidn2|libunistring|libp11-kit
)\.so''', re.X)


def run(*cmd, check=True, env=None):
    return subprocess.run(cmd, check=check, capture_output=True, text=True, env=env).stdout


# ----------------------------------------------------------------- Linux

def ldd_deps(path, env=None):
    deps = {}
    for line in run('ldd', path, check=False, env=env).splitlines():
        m = re.match(r'\s*(\S+) => (\S+)', line)
        if m and m.group(2) != 'not':
            deps[m.group(1)] = m.group(2)
        elif 'not found' in line:
            sys.exit(f'{path}: {line.strip()}')
    return deps


def needed(path):
    """The libraries an object asks for itself (DT_NEEDED), unlike ldd's closure."""
    return run('patchelf', '--print-needed', path).split()


def bundle_linux(root, bins):
    lib = os.path.join(root, 'lib')
    os.makedirs(lib, exist_ok=True)
    binaries = [os.path.join(root, b) for b in bins]
    # Follow DT_NEEDED from the binaries, and stop at the excluded libraries:
    # what those need is the system's business (libXdmcp needs libbsd, say),
    # and a copy in lib/ would not be used for them anyway, since RUNPATH does
    # not apply to a library's dependencies.
    found = {}
    for b in binaries:
        found.update(ldd_deps(b))
    wanted = {}
    todo = list(binaries)
    while todo:
        for soname in needed(todo.pop()):
            if EXCLUDE.match(soname) or soname in wanted:
                continue
            if soname not in found:
                sys.exit(f'{soname} is needed but ldd did not find it')
            wanted[soname] = os.path.realpath(found[soname])
            todo.append(wanted[soname])
    for soname, path in sorted(wanted.items()):
        dst = os.path.join(lib, soname)
        shutil.copy2(path, dst)
        os.chmod(dst, 0o755)
        run('patchelf', '--set-rpath', '$ORIGIN', dst)
        print(f'bundled {soname}')
    # the binaries, and QEMU's modules in lib/qemu/
    for b in binaries:
        run('patchelf', '--set-rpath', '$ORIGIN/' + os.path.relpath(lib, os.path.dirname(b)), b)
    # check: what each of these objects asks for comes from lib/ (without
    # LD_LIBRARY_PATH, which would take precedence over the package's RUNPATH)
    clean = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
    for f in binaries + [os.path.join(lib, n) for n in wanted]:
        deps = ldd_deps(f, env=clean)
        for soname in needed(f):
            # ldd prints RUNPATH's $ORIGIN/../lib unnormalized (bin/../lib)
            if not EXCLUDE.match(soname) and \
                    not os.path.realpath(deps[soname]).startswith(os.path.realpath(lib) + '/'):
                sys.exit(f'{f}: {soname} resolves outside the package ({deps[soname]})')
    return wanted


# ----------------------------------------------------------------- macOS

SYSTEM = ('/usr/lib/', '/System/')


def macho_deps(path):
    out = run('otool', '-L', path).splitlines()[1:]
    deps = [l.strip().split(' (')[0] for l in out]
    me = run('otool', '-D', path).splitlines()[1:]
    return [d for d in deps if not me or d != me[0].strip()]


def rpaths(path):
    lines = run('otool', '-l', path).splitlines()
    res = []
    for i, l in enumerate(lines):
        if l.strip() == 'cmd LC_RPATH':
            res.append(lines[i + 2].strip().split(' ')[1])
    return res


def resolve(dep, loader):
    if dep.startswith('@rpath/'):
        name = dep[len('@rpath/'):]
        for rp in rpaths(loader) + ['/opt/homebrew/lib', '/opt/local/lib', '/usr/local/lib']:
            rp = rp.replace('@loader_path', os.path.dirname(loader))
            if os.path.exists(os.path.join(rp, name)):
                return os.path.join(rp, name)
        sys.exit(f'{loader}: cannot resolve {dep}')
    if dep.startswith('@loader_path/'):
        return os.path.join(os.path.dirname(loader), dep[len('@loader_path/'):])
    return dep


def bundle_macos(root, bins, extra_libs=()):
    lib = os.path.join(root, 'lib')
    os.makedirs(lib, exist_ok=True)
    copied = {}                       # basename -> source
    todo = [os.path.join(root, b) for b in bins] + list(extra_libs)
    seen = set()
    edits = []                        # (file, [(old, new)])
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        changes = []
        for dep in macho_deps(f):
            if dep.startswith(SYSTEM) or dep.startswith('@executable_path/') or \
                    (dep.startswith('@loader_path/') and os.path.dirname(f) == lib):
                continue
            src = os.path.realpath(resolve(dep, f))
            name = os.path.basename(dep)
            if name not in copied:
                dst = os.path.join(lib, name)
                shutil.copy2(src, dst)
                os.chmod(dst, 0o755)
                copied[name] = src
                todo.append(dst)
                print(f'bundled {name}')
            is_bin = os.path.dirname(f) != lib
            changes.append((dep, ('@executable_path/../lib/' if is_bin else '@loader_path/') + name))
        edits.append((f, changes))
    for f, changes in edits:
        args = []
        for old, new in changes:
            args += ['-change', old, new]
        if os.path.dirname(f) == lib:
            args += ['-id', '@loader_path/' + os.path.basename(f)]
        for rp in rpaths(f):
            if not rp.startswith(('@executable_path', '@loader_path')):
                args += ['-delete_rpath', rp]
        if args:
            run('install_name_tool', *args, f)
    for f in sorted(seen):
        run('codesign', '--force', '--sign', '-', f)
    # check: nothing left that points outside the system or the package
    for f in sorted(seen):
        for dep in macho_deps(f):
            if not (dep.startswith(SYSTEM) or dep.startswith('@executable_path/../lib/')
                    or dep.startswith('@loader_path/')):
                sys.exit(f'{f}: still refers to {dep}')
        for rp in rpaths(f):
            if not rp.startswith(('@executable_path', '@loader_path')):
                sys.exit(f'{f}: still has rpath {rp}')
    return copied


# --------------------------------------------------------------- Windows

def winpath(msys_path):
    return run('cygpath', '-m', msys_path).strip()


def bundle_windows(root, bins):
    bindir = os.path.join(root, 'bin')
    copied = {}
    for b in bins:
        for name, path in ldd_deps(os.path.join(root, b)).items():
            if path.lower().startswith('/c/windows/'):
                continue
            src = winpath(path)
            dst = os.path.join(bindir, os.path.basename(src))
            if not os.path.exists(dst):
                shutil.copy2(src, dst)
            copied[os.path.basename(src)] = src
    # GTK finds its data from the folder above its DLL (bin/..)
    prefix = winpath('/ucrt64')
    schemas = os.path.join('share', 'glib-2.0', 'schemas')
    os.makedirs(os.path.join(root, schemas), exist_ok=True)
    shutil.copy2(os.path.join(prefix, schemas, 'gschemas.compiled'), os.path.join(root, schemas))
    pixbuf = os.path.join('lib', 'gdk-pixbuf-2.0')
    if os.path.isdir(os.path.join(prefix, pixbuf)):
        shutil.copytree(os.path.join(prefix, pixbuf), os.path.join(root, pixbuf), dirs_exist_ok=True)
    # what is still not in bin/ or the Windows directory comes from MSYS2
    for b in bins:
        for name, path in ldd_deps(os.path.join(root, b)).items():
            p = os.path.normcase(winpath(path))
            if not (path.lower().startswith('/c/windows/') or
                    p.startswith(os.path.normcase(os.path.abspath(bindir)))):
                sys.exit(f'{b}: {name} still comes from {path}')
    return copied


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root, bins = sys.argv[1], sys.argv[2:]
    extra = os.environ.get('BUNDLE_EXTRA_LIBS', '').split(os.pathsep)
    if sys.platform == 'darwin':
        srcs = bundle_macos(root, bins, [e for e in extra if e])
    elif sys.platform == 'win32':
        srcs = bundle_windows(root, bins)
    else:
        srcs = bundle_linux(root, bins)
    os.makedirs(os.path.join(root, 'lib'), exist_ok=True)
    # where each library came from, for notices.py
    with open(os.path.join(root, 'lib', 'bundled.txt'), 'w') as f:
        for name, src in sorted(srcs.items()):
            f.write(f'{name}\t{src}\n')
    print(f'{len(srcs)} libraries in {os.path.join(root, "lib")}')


main()
