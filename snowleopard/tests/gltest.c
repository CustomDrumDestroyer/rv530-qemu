#include <GLUT/glut.h>
#include <stdio.h>
#include <stdlib.h>
static int frame;
static FILE *out;
static void probe(const char *what)
{
    unsigned char px[4][4];
    int pts[4][2] = {{10, 10}, {128, 100}, {128, 200}, {250, 250}};
    int i;
    for (i = 0; i < 4; i++)
        glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[i]);
    fprintf(out, "%s:", what);
    for (i = 0; i < 4; i++)
        fprintf(out, " (%d,%d)=%02x%02x%02x%02x", pts[i][0], pts[i][1], px[i][0], px[i][1], px[i][2], px[i][3]);
    fprintf(out, "\n");
    fflush(out);
}
static void display(void)
{
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(0, 1, 0);
    glVertex2f(-0.8f, -0.8f); glVertex2f(0.8f, -0.8f); glVertex2f(0, 0.8f);
    glEnd();
    glFinish();
    probe("back");
    glutSwapBuffers();
    frame++;
}
static void tick(int v)
{
    if (v >= 8) {
        fprintf(out, "renderer %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        fclose(out);
        exit(0);
    }
    glutPostRedisplay();
    glutTimerFunc(1000, tick, v + 1);
}
int main(int argc, char **argv)
{
    out = fopen("/tmp/gltest.out", "w");
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(256, 256);
    glutInitWindowPosition(500, 300);
    glutCreateWindow("gltest");
    glutDisplayFunc(display);
    glutTimerFunc(1000, tick, 0);
    glutMainLoop();
    return 0;
}
