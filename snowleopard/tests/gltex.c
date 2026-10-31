#include <GLUT/glut.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
/* Four textured quads, one per quadrant, each from a 64x64 checker (blue/yellow):
 * 0: GL_TEXTURE_2D, plain upload          1: GL_TEXTURE_RECTANGLE_ARB, plain
 * 2: RECTANGLE + client storage           3: RECTANGLE + client storage + shared texture range (BGRA/8888_REV) */
static FILE *out;
static GLuint tex[4];
static unsigned int *pix[4];
static void make(int i, GLenum target, int client, int range)
{
    int x, y;
    pix[i] = valloc(64 * 64 * 4);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            pix[i][y * 64 + x] = (((x >> 3) ^ (y >> 3)) & 1) ? 0xff0000ffu : 0xffffff00u; /* ARGB: blue / yellow */
    glGenTextures(1, &tex[i]);
    glBindTexture(target, tex[i]);
    if (range) {
        glTextureRangeAPPLE(target, 64 * 64 * 4, pix[i]);
        glTexParameteri(target, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
    }
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, client);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(target, 0, GL_RGBA, 64, 64, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pix[i]);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, 0);
}
static void quad(int i, GLenum target, float x0, float y0)
{
    float s = target == GL_TEXTURE_2D ? 1 : 64;
    glEnable(target);
    glBindTexture(target, tex[i]);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(s, 0); glVertex2f(x0 + 0.9f, y0);
    glTexCoord2f(s, s); glVertex2f(x0 + 0.9f, y0 + 0.9f);
    glTexCoord2f(0, s); glVertex2f(x0, y0 + 0.9f);
    glEnd();
    glDisable(target);
}
static void display(void)
{
    static const int px[4][2] = {{36, 36}, {164, 36}, {36, 164}, {164, 164}};
    int i;
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 1, 1);
    quad(0, GL_TEXTURE_2D, -0.95f, -0.95f);
    quad(1, GL_TEXTURE_RECTANGLE_ARB, 0.05f, -0.95f);
    quad(2, GL_TEXTURE_RECTANGLE_ARB, -0.95f, 0.05f);
    quad(3, GL_TEXTURE_RECTANGLE_ARB, 0.05f, 0.05f);
    glFinish();
    fprintf(out, "frame:");
    for (i = 0; i < 4; i++) {
        unsigned char c[4][4];
        glReadPixels(px[i][0], px[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c[0]);
        glReadPixels(px[i][0] + 8, px[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c[1]);
        fprintf(out, " q%d=%02x%02x%02x/%02x%02x%02x", i, c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2]);
    }
    fprintf(out, "\n");
    fflush(out);
    glutSwapBuffers();
}
static void tick(int v)
{
    if (v >= 6) { fclose(out); exit(0); }
    glutPostRedisplay();
    glutTimerFunc(1000, tick, v + 1);
}
int main(int argc, char **argv)
{
    out = fopen("/tmp/gltex.out", "w");
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(256, 256);
    glutInitWindowPosition(500, 300);
    glutCreateWindow("gltex");
    make(0, GL_TEXTURE_2D, 0, 0);
    make(1, GL_TEXTURE_RECTANGLE_ARB, 0, 0);
    make(2, GL_TEXTURE_RECTANGLE_ARB, 1, 0);
    make(3, GL_TEXTURE_RECTANGLE_ARB, 1, 1);
    glutDisplayFunc(display);
    glutTimerFunc(1000, tick, 0);
    glutMainLoop();
    return 0;
}
