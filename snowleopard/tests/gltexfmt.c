#include <GLUT/glut.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Texture upload formats and render-to-texture: every case draws a quad of
 * uniform orange (R ff, G 80, B 00) and reads it back.  A component order
 * mismatch shows up as a swapped color (e.g. blue 0080ff).  Results in
 * /tmp/gltexfmt.out, one line per case: "name rrggbb ok|BAD". */
#define N 16
static FILE *out;
static GLuint tex[N];
static GLenum tgt[N];
static const char *name[N];
static int ncase;
static char direct[2][64];
static int ndirect;

static void params(GLenum target)
{
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* upload a 16x16 texture from 'texel' (one texel, bpp bytes) */
static void upload(const char *n, GLenum target, GLint ifmt, GLenum fmt, GLenum type,
                   const void *texel, int bpp, int client, int range)
{
    int i = ncase++;
    unsigned char *buf = valloc(16 * 16 * bpp);
    int k;
    for (k = 0; k < 16 * 16; k++) {
        memcpy(buf + k * bpp, texel, bpp);
    }
    name[i] = n;
    tgt[i] = target;
    glGenTextures(1, &tex[i]);
    glBindTexture(target, tex[i]);
    if (range) {
        glTextureRangeAPPLE(target, 16 * 16 * bpp, buf);
        glTexParameteri(target, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
    }
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, client);
    params(target);
    glTexImage2D(target, 0, ifmt, 16, 16, 0, fmt, type, buf);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, 0);
}

/* texture rendered through an FBO: clear to orange, draw an orange quad */
static void rtt(const char *n, GLint ifmt, GLenum type)
{
    int i = ncase++;
    GLuint fbo;
    name[i] = n;
    tgt[i] = GL_TEXTURE_2D;
    glGenTextures(1, &tex[i]);
    glBindTexture(GL_TEXTURE_2D, tex[i]);
    params(GL_TEXTURE_2D);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, 16, 16, 0, GL_RGBA, type, NULL);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, tex[i], 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
        name[i] = "fbo-incomplete";
    }
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1.0f, 128 / 255.0f, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    {
        unsigned char c[4];
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        snprintf(direct[ndirect++], sizeof(direct[0]), "%s read back from the FBO: %02x%02x%02x",
                 n, c[0], c[1], c[2]);
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glViewport(0, 0, 256, 256);
}

static void setup(void)
{
    static const unsigned char rgba[4] = { 0xff, 0x80, 0x00, 0xff };
    static const unsigned char bgra[4] = { 0x00, 0x80, 0xff, 0xff };
    static const unsigned char abgr[4] = { 0xff, 0x00, 0x80, 0xff };
    static const unsigned char rgb[3] = { 0xff, 0x80, 0x00 };
    static const unsigned char bgr[3] = { 0x00, 0x80, 0xff };
    unsigned int argb = 0xffff8000u;      /* 8_8_8_8_REV + BGRA: AARRGGBB */
    unsigned int bgra_be = 0x0080ffffu;   /* 8_8_8_8 + BGRA: BBGGRRAA */
    unsigned int rgba_be = 0xff8000ffu;   /* 8_8_8_8 + RGBA: RRGGBBAA */
    unsigned int abgr_le = 0xff0080ffu;   /* 8_8_8_8_REV + RGBA: AABBGGRR */
    float f[4] = { 1.0f, 128 / 255.0f, 0.0f, 1.0f };
    unsigned short u16[4] = { 0xffff, 0x8080, 0, 0xffff };

    upload("RGBA/UBYTE", GL_TEXTURE_2D, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, rgba, 4, 0, 0);
    upload("BGRA/UBYTE", GL_TEXTURE_2D, GL_RGBA, GL_BGRA, GL_UNSIGNED_BYTE, bgra, 4, 0, 0);
    upload("BGRA/8888_REV", GL_TEXTURE_2D, GL_RGBA, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &argb, 4, 0, 0);
    upload("BGRA/8888", GL_TEXTURE_2D, GL_RGBA, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, &bgra_be, 4, 0, 0);
    upload("RGBA/8888", GL_TEXTURE_2D, GL_RGBA, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, &rgba_be, 4, 0, 0);
    upload("RGBA/8888_REV", GL_TEXTURE_2D, GL_RGBA, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, &abgr_le, 4, 0, 0);
    upload("RGB/UBYTE", GL_TEXTURE_2D, GL_RGB, GL_RGB, GL_UNSIGNED_BYTE, rgb, 3, 0, 0);
    upload("BGR/UBYTE", GL_TEXTURE_2D, GL_RGB, GL_BGR, GL_UNSIGNED_BYTE, bgr, 3, 0, 0);
    upload("ABGR_EXT/UBYTE", GL_TEXTURE_2D, GL_RGBA, GL_ABGR_EXT, GL_UNSIGNED_BYTE, abgr, 4, 0, 0);
    upload("RGBA16/USHORT", GL_TEXTURE_2D, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, u16, 8, 0, 0);
    upload("RGBA_FLOAT16/FLOAT", GL_TEXTURE_2D, GL_RGBA_FLOAT16_APPLE, GL_RGBA, GL_FLOAT, f, 16, 0, 0);
    upload("RGBA_FLOAT32/FLOAT", GL_TEXTURE_2D, GL_RGBA_FLOAT32_APPLE, GL_RGBA, GL_FLOAT, f, 16, 0, 0);
    upload("RECT BGRA/8888 client+range", GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, &bgra_be, 4, 1, 1);
    upload("RECT RGBA/8888 client+range", GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, &rgba_be, 4, 1, 1);
    rtt("FBO RGBA8", GL_RGBA8, GL_UNSIGNED_BYTE);
    rtt("FBO RGBA_FLOAT16", GL_RGBA_FLOAT16_APPLE, GL_FLOAT);
}

static void display(void)
{
    int i;
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 1, 1);
    for (i = 0; i < ncase; i++) {
        float x0 = -1 + (i % 4) * 0.5f + 0.05f, y0 = -1 + (i / 4) * 0.5f + 0.05f;
        float s = tgt[i] == GL_TEXTURE_2D ? 1 : 16;
        glEnable(tgt[i]);
        glBindTexture(tgt[i], tex[i]);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x0, y0);
        glTexCoord2f(s, 0); glVertex2f(x0 + 0.4f, y0);
        glTexCoord2f(s, s); glVertex2f(x0 + 0.4f, y0 + 0.4f);
        glTexCoord2f(0, s); glVertex2f(x0, y0 + 0.4f);
        glEnd();
        glDisable(tgt[i]);
    }
    glFinish();
    rewind(out);
    for (i = 0; i < ncase; i++) {
        unsigned char c[4];
        int px = (i % 4) * 64 + 32, py = (i / 4) * 64 + 32;
        int ok;
        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        ok = c[0] > 0xf0 && c[1] > 0x70 && c[1] < 0x90 && c[2] < 0x10;
        fprintf(out, "%-30s %02x%02x%02x %s\n", name[i], c[0], c[1], c[2], ok ? "ok" : "BAD");
    }
    for (i = 0; i < ndirect; i++) {
        fprintf(out, "%s\n", direct[i]);
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
    out = fopen("/tmp/gltexfmt.out", "w");
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(256, 256);
    glutInitWindowPosition(500, 300);
    glutCreateWindow("gltexfmt");
    setup();
    glutDisplayFunc(display);
    glutTimerFunc(1000, tick, 0);
    glutMainLoop();
    return 0;
}
