# Guest-side OpenGL / Core Image checks

These are small programs that you build and run **in** the Snow Leopard guest.
Xcode 3.2 from the install DVD gives gcc. Each program does these steps:

1. It opens a 256x256 window.
2. One time each second, it reads pixels from the GL back buffer and writes
   them to `/tmp/<name>.out`.
3. After some seconds, it stops.

| Program | Checks |
| --- | --- |
| `gltest.c` | Clear and a flat-shaded triangle. Expect red corners and a green center |
| `glms.c` | The same, with `GLUT_MULTISAMPLE` (MSAA render and AA resolve) |
| `gltex.c` | Four textured quads: `GL_TEXTURE_2D`, rectangle, rectangle with client storage, and rectangle with client storage and shared texture range. Expect blue and yellow checkers |
| `gltexfmt.c` | 16 texture cases in a 4x4 grid. Each case is uniform orange (`ff8000`). The cases are all upload formats and types (8-bit, 16-bit, half float, full float, and client storage with texture range), and render-to-texture through RGBA8 and half-float FBOs. `/tmp/gltexfmt.out` lists each case with `ok` or `BAD`, and the FBO contents that it reads directly |
| `glvbo.c` | Vertex buffer objects with the layout of a world model in a game: one 2.5 MB buffer with positions, 36 bytes of other data, and texture coordinates. It draws from low to high vertex indices, from client arrays and from VBOs that `glBufferData`, `glBufferSubData`, and `glMapBuffer` fill. `/tmp/glvbo.out` lists 24 quads with `ok` or `BAD`. It finds VBO uploads that go to the wrong location |
| `modes.py` | Lists the 32-bpp display modes that the driver makes from the device EDID. Expect 640x480 through 2560x1600. With the arguments `W H`, it changes to that mode for 10 s and then changes back. During this time, take a host screenshot: its size must be W x H |
| `vblank.c` | Measures the display refresh that the guest gets: 240 GL swaps with swap interval 1. `/tmp/vblank.out` shows the nominal rate of the mode and the measured rate. On the rv530 they are the same, for example 59.90 Hz at 60 and approximately 115 Hz at 120 |
| `gamma.py` | Sets a hardware gamma table with only red (`CGSetDisplayTransferByFormula`) for 5 s, then sets the ColorSync table again. During this time, take a host screenshot (`slctl.py shot`). Green and blue must decrease to approximately 20% and then return |
| `gltrace.c` | Not a test but a probe: a `DYLD_INSERT_LIBRARIES` dylib. It counts the GL draw calls of any application, `glBegin`/`glEnd` pairs (display-list compile or immediate), GL errors after each call, and NaN vertices. It writes the counts to `/tmp/gltrace.out`. It tells you if the application did not draw an item or if the GPU did not show it. The file header shows how to use it |
| `glstrip.c` | A cylinder of quad-strip bands over a plane (or triangle-strip bands, with argument 1). Depth test LESS, with back faces culled. It draws again each frame. Expect a probe column with only `R`. It finds a depth buffer that is not cleared between frames |
| `gltexyuv.c` | Y'CbCr 4:2:2 textures (`GL_YCBCR_422_APPLE`) in the two 8_8 types and the two byte orders, and a 3D texture with linear filtering. Expect `8_8 UYVY` and `8_8_REV YUYV` to be orange (`fe8000`). The two cases that do not match show a different color |
| `glci.m` | Core Image draws into the GL context: generator, image file (`/tmp/small.jpg`), and Gaussian blur |
| `ci_blur_check.py` | Compares a Core Image blur on a GPU `CIContext` with the software renderer. Expect max diff 0. `ssh guest python - < ci_blur_check.py` works. You do not need a window |
| `ci_blur_bench.py` | Core Image blur 1024x1024, in ms per frame |
| `stale_rows.py` | Runs on the host, through QMP (`slctl.py`). It selects rows in the Finder list and opens Quick Look in rounds. It counts rows that keep their old selection color (stale window content) |

```
scp tests/*.c tests/*.m guest:rvtests/      # or paste through ssh
cd ~/rvtests
gcc -o gltest gltest.c -framework GLUT -framework OpenGL
gcc -o glci glci.m -framework GLUT -framework OpenGL -framework Cocoa -framework QuartzCore
```

GUI programs must start in the bootstrap namespace of the login session. If you
use ssh, sshd runs as a system daemon (see "ssh into the guest" in
`../README.md`). In that case, use this command:

```
sudo launchctl bsexec $(ps ax | grep '[l]oginwindow.app' | awk '{print $1}') sudo -u test open ~/rvtests/gltest
```
