#include <GLUT/glut.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Vertex buffer objects laid out like DarkPlaces' world model: one large
 * buffer of N vertices holding positions (3 floats) at offset 0, 36 bytes of
 * other per-vertex data, then texture coordinates (2 floats) at 48*N.  Each
 * case draws quads from low, middle and high vertex indices with 16-bit
 * glDrawElements, textured from a 4-texel strip (red, green, blue, white), and
 * reads them back.  Client arrays are the control; the VBO is filled with
 * glBufferData, glBufferSubData in chunks or glMapBuffer.  Results in
 * /tmp/glvbo.out, one line per quad: "case base expected got ok|BAD". */
#define N 40000
#define NQ 6
static FILE *out;
static float *data;
static GLuint vbo[3], tex;
static const unsigned bases[NQ] = { 0, 1000, 9443, 21065, 30816, N - 4 };
static const char *casename[4] = { "client arrays", "VBO BufferData", "VBO BufferSubData", "VBO MapBuffer" };
static const unsigned char texel[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };

/* quad q (vertices bases[q]..+3) sits in column q of the bottom row and
 * picks texel q % 4; each case draws its own row, shifted up */
static void fill(void)
{
    float *pos = data, *st = data + 12 * N;
    int q, k;
    memset(data, 0, 64 * N);
    for (k = 0; k < N; k++) {
        /* other vertices: off screen, with a texcoord that picks red */
        pos[3 * k + 0] = 5; pos[3 * k + 1] = 5;
        st[2 * k + 0] = 0.125f; st[2 * k + 1] = 0.5f;
        data[3 * N + 9 * k] = 1000 + k;       /* the 36 other bytes: junk */
    }
    for (q = 0; q < NQ; q++) {
        unsigned b = bases[q];
        float x0 = -1 + q * (2.0f / NQ) + 0.03f, y0 = -0.95f;
        float x1 = x0 + 2.0f / NQ - 0.06f, y1 = y0 + 0.4f;
        float *p = pos + 3 * b;
        p[0] = x0; p[1] = y0; p[3] = x1; p[4] = y0; p[6] = x1; p[7] = y1; p[9] = x0; p[10] = y1;
        for (k = 0; k < 4; k++) {
            st[2 * (b + k)] = ((q % 4) + 0.5f) / 4;
        }
    }
}

static void setup(void)
{
    static unsigned char strip[4][4];
    int k;
    for (k = 0; k < 4; k++) {
        memcpy(strip[k], texel[k], 3);
        strip[k][3] = 255;
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, strip);

    data = malloc(64 * N);
    fill();
    glGenBuffersARB(3, vbo);
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, vbo[0]);
    glBufferDataARB(GL_ARRAY_BUFFER_ARB, 64 * N, data, GL_STATIC_DRAW_ARB);
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, vbo[1]);
    glBufferDataARB(GL_ARRAY_BUFFER_ARB, 64 * N, NULL, GL_STATIC_DRAW_ARB);
    for (k = 0; k < 64 * N; k += 65536) {
        int len = 64 * N - k < 65536 ? 64 * N - k : 65536;
        glBufferSubDataARB(GL_ARRAY_BUFFER_ARB, k, len, (char *)data + k);
    }
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, vbo[2]);
    glBufferDataARB(GL_ARRAY_BUFFER_ARB, 64 * N, NULL, GL_STATIC_DRAW_ARB);
    memcpy(glMapBufferARB(GL_ARRAY_BUFFER_ARB, GL_WRITE_ONLY_ARB), data, 64 * N);
    glUnmapBufferARB(GL_ARRAY_BUFFER_ARB);
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
}

static void draw_case(int c)
{
    const char *base = NULL;
    int q;
    if (c == 0) {
        base = (const char *)data;
    } else {
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, vbo[c - 1]);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 12, base);
    glTexCoordPointer(2, GL_FLOAT, 8, base + 48 * N);
    for (q = 0; q < NQ; q++) {
        unsigned short idx[6];
        unsigned b = bases[q];
        idx[0] = b; idx[1] = b + 1; idx[2] = b + 2; idx[3] = b; idx[4] = b + 2; idx[5] = b + 3;
        glPushMatrix();
        glTranslatef(0, c * 0.5f, 0);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
        glPopMatrix();
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
}

static void display(void)
{
    int c, q;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (c = 0; c < 4; c++) {
        draw_case(c);
    }
    glDisable(GL_TEXTURE_2D);
    glFinish();
    rewind(out);
    for (c = 0; c < 4; c++) {
        for (q = 0; q < NQ; q++) {
            unsigned char px[4];
            const unsigned char *e = texel[q % 4];
            int x = (int)((q + 0.5f) * 384 / NQ), y = c * 96 + 33;
            int ok;
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            ok = abs(px[0] - e[0]) < 16 && abs(px[1] - e[1]) < 16 && abs(px[2] - e[2]) < 16;
            fprintf(out, "%-18s base %5u expect %02x%02x%02x got %02x%02x%02x %s\n", casename[c],
                    bases[q], e[0], e[1], e[2], px[0], px[1], px[2], ok ? "ok" : "BAD");
        }
    }
    fflush(out);
    glutSwapBuffers();
}

static void tick(int v)
{
    if (v >= 4) { fclose(out); exit(0); }
    glutPostRedisplay();
    glutTimerFunc(1000, tick, v + 1);
}

int main(int argc, char **argv)
{
    out = fopen("/tmp/glvbo.out", "w");
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(384, 384);
    glutInitWindowPosition(500, 300);
    glutCreateWindow("glvbo");
    setup();
    glutDisplayFunc(display);
    glutTimerFunc(1000, tick, 0);
    glutMainLoop();
    return 0;
}
