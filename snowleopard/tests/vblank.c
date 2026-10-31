#include <GLUT/glut.h>
#include <OpenGL/OpenGL.h>
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
/* Measured display refresh: GL swaps with swap interval 1 wait for vblank,
 * so N swaps take N frame periods.  Prints the mode's nominal rate and the
 * measured one to /tmp/vblank.out. */
static int frames;
static double t0;

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void display(void)
{
    glClearColor((frames & 1) ? 0.2f : 0.3f, 0.2f, 0.4f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glutSwapBuffers();
    if (++frames == 60) {
        t0 = now();                     /* skip the warm-up */
    } else if (frames == 60 + 240) {
        double hz = 240 / (now() - t0);
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
        FILE *f = fopen("/tmp/vblank.out", "w");
        fprintf(f, "mode %zux%zu nominal %.2f Hz, measured %.2f Hz\n",
                CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m),
                CGDisplayModeGetRefreshRate(m), hz);
        fclose(f);
        exit(0);
    }
    glutPostRedisplay();
}

int main(int argc, char **argv)
{
    GLint one = 1;
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(320, 240);
    glutCreateWindow("vblank");
    CGLSetParameter(CGLGetCurrentContext(), kCGLCPSwapInterval, &one);
    glutDisplayFunc(display);
    glutMainLoop();
    return 0;
}
