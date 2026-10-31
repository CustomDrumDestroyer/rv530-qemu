#include <GLUT/glut.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
/* A cylinder of quad-strip bands (like Chess's lathe pieces) standing on a
 * plane, depth test LESS, back faces culled, seen from above at an angle.
 * Expect a solid red cylinder over a green plane: the probes down the
 * cylinder's middle must all be red.  Mode from argv[1]: 0 = quad strip
 * bands, 1 = triangle strip bands, 2 = no culling. */
static FILE *out;
static int mode;
static void band(float y0, float y1, float r)
{
    int i;
    glBegin(mode == 1 ? GL_TRIANGLE_STRIP : GL_QUAD_STRIP);
    for (i = 0; i <= 32; i++) {
        float a = i * 2.0f * (float)M_PI / 32;
        glVertex3f(r * cosf(a), y0, r * sinf(a));
        glVertex3f(r * cosf(a), y1, r * sinf(a));
    }
    glEnd();
}
static void display(void)
{
    int y;
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (mode != 2) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1.5, 20);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glTranslatef(0, -0.5f, -5);
    glRotatef(35, 1, 0, 0);
    glColor3f(0, 1, 0);
    glBegin(GL_QUADS);
    glVertex3f(-3, 0, -3); glVertex3f(-3, 0, 3); glVertex3f(3, 0, 3); glVertex3f(3, 0, -3);
    glEnd();
    glColor3f(1, 0, 0);
    for (y = 0; y < 8; y++) {
        band(y * 0.2f, (y + 1) * 0.2f, 0.6f);
    }
    glFinish();
    fprintf(out, "mode %d column x=128:", mode);
    for (y = 40; y < 256; y += 12) {
        unsigned char c[4];
        glReadPixels(128, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        fprintf(out, " %c", c[0] > 128 && c[1] < 64 ? 'R' : c[1] > 128 ? 'G' : '.');
    }
    fprintf(out, "\n");
    fflush(out);
    glutSwapBuffers();
}
static void tick(int v)
{
    if (v >= 3) { fclose(out); exit(0); }
    glutPostRedisplay();
    glutTimerFunc(700, tick, v + 1);
}
int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    mode = argc > 1 ? atoi(argv[1]) : 0;
    out = fopen("/tmp/glstrip.out", "a");
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(256, 256);
    glutInitWindowPosition(500, 300);
    glutCreateWindow("glstrip");
    glutDisplayFunc(display);
    glutTimerFunc(700, tick, 0);
    glutMainLoop();
    return 0;
}
