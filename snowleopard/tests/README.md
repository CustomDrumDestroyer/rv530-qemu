# Guest-side OpenGL / Core Image checks

Small programs to build and run **inside** the Snow Leopard guest (Xcode 3.2
from the install DVD provides gcc).  Each opens a 256x256 window, reads pixels
back from the GL back buffer once per second and writes them to
`/tmp/<name>.out`, then exits after a few seconds.

| Program | Checks |
| --- | --- |
| `gltest.c` | clear + flat-shaded triangle (expect red corners, green center) |
| `glms.c` | the same with `GLUT_MULTISAMPLE` (MSAA render + AA resolve) |
| `gltex.c` | four textured quads: `GL_TEXTURE_2D`, rectangle, rectangle with client storage, rectangle with client storage + shared texture range (expect blue/yellow checkers) |
| `gltexfmt.c` | 16 texture cases in a 4x4 grid, each a uniform orange (`ff8000`): every upload format/type (8-bit, 16-bit, half, and full float, client storage with texture range) and render-to-texture through RGBA8 and half-float FBOs; `/tmp/gltexfmt.out` lists each with `ok`/`BAD` and the FBO contents read back directly |
| `glvbo.c` | vertex buffer objects laid out like a game's world model (one 2.5 MB buffer: positions, 36 bytes of other data, texture coordinates), drawn from low to high vertex indices from client arrays and from VBOs filled by `glBufferData`, `glBufferSubData`, and `glMapBuffer`; `/tmp/glvbo.out` lists 24 quads with `ok`/`BAD` (catches VBO uploads that land in the wrong place) |
| `modes.py` | lists the 32-bpp display modes the driver builds from the device EDID (expect 640x480 through 2560x1600); with `W H` arguments switches to that mode for 10 s and back (take a host screenshot meanwhile: it should be W x H) |
| `vblank.c` | the display refresh the guest really gets: 240 GL swaps with swap interval 1, timed; `/tmp/vblank.out` has the mode's nominal rate and the measured one (on the rv530 they match, e.g., 59.90 Hz at 60 and about 115 Hz at 120) |
| `gamma.py` | sets a red-only hardware gamma table (`CGSetDisplayTransferByFormula`) for 5 s, then restores ColorSync's; take a host screenshot meanwhile (`slctl.py shot`): green and blue should drop to ~20% and come back |
| `gltrace.c` | not a test but a probe: a `DYLD_INSERT_LIBRARIES` dylib that counts any application's GL draw calls, `glBegin`/`glEnd` pairs (display-list compile or immediate), GL errors after each call, and NaN vertices into `/tmp/gltrace.out`; tells "the app never drew it" from "the GPU dropped it" (usage in the file header) |
| `glstrip.c` | a cylinder of quad-strip (or, with argument 1, triangle-strip) bands over a plane, depth test LESS with back faces culled, redrawn every frame (expect an all-`R` probe column; catches a depth buffer that is not cleared between frames) |
| `gltexyuv.c` | Y'CbCr 4:2:2 textures (`GL_YCBCR_422_APPLE`) in both 8_8 types and both byte orders, plus a linearly filtered 3D texture; expect `8_8 UYVY` and `8_8_REV YUYV` orange (`fe8000`); the two mismatched cases come out another color |
| `glci.m` | Core Image drawing into the GL context: generator, image file (`/tmp/small.jpg`), Gaussian blur |
| `ci_blur_check.py` | Core Image blur on a GPU `CIContext` vs the software renderer (expect max diff 0); `ssh guest python - < ci_blur_check.py` works, no window needed |
| `ci_blur_bench.py` | Core Image blur 1024x1024, ms per frame |
| `stale_rows.py` | Host side, over QMP (`slctl.py`): Finder list selection + Quick Look rounds, counting rows that keep their old selection color (stale window content) |

```
scp tests/*.c tests/*.m guest:rvtests/      # or paste through ssh
cd ~/rvtests
gcc -o gltest gltest.c -framework GLUT -framework OpenGL
gcc -o glci glci.m -framework GLUT -framework OpenGL -framework Cocoa -framework QuartzCore
```

GUI programs must start in the login session's bootstrap namespace; over
ssh (sshd runs as a system daemon, see "ssh into the guest" in
`../README.md`) use

```
sudo launchctl bsexec $(ps ax | grep '[l]oginwindow.app' | awk '{print $1}') sudo -u test open ~/rvtests/gltest
```
