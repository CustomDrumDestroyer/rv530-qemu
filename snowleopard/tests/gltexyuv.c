#include <GLUT/glut.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Video and 3D textures, the ones QuickTime uses: Y'CbCr 4:2:2
 * (GL_YCBCR_422_APPLE, both 8_8 orders) and a linearly filtered 3D
 * texture (ColorSync's color lookup).  Each case draws one quad; the
 * colors read back go to /tmp/gltexyuv.out, "name rrggbb".  Run under
 * rv530 vk-verify=on to compare the Vulkan backend with the CPU path.
 * Apple's driver picks the format from the type alone: 8_8 is YVYU422,
 * which reads bytes U Y0 V Y1 ('2vuy', what QuickTime hands iMovie), and
 * 8_8_REV is VYUY422, Y0 U Y1 V.  So "8_8 UYVY" and "8_8_REV YUYV" must
 * read back orange (ff8000 within a step or two); the two mismatched
 * cases read luma as chroma and come out another color. */
static FILE *out;
static GLuint tex[5];
static GLenum tgt[5];
static const char *name[5];
static int ncase;

static void yuv(const char *n, GLenum type, const unsigned char quad[4])
{
    unsigned char buf[16 * 16 * 2];
    int i = ncase++, k;

    for (k = 0; k < 16 * 16 / 2; k++) {
        memcpy(buf + 4 * k, quad, 4);
    }
    name[i] = n;
    tgt[i] = GL_TEXTURE_RECTANGLE_ARB;
    glGenTextures(1, &tex[i]);
    glBindTexture(tgt[i], tex[i]);
    glTexParameteri(tgt[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(tgt[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(tgt[i], 0, GL_RGB, 16, 16, 0, GL_YCBCR_422_APPLE, type, buf);
}

/* 4x4x4 RGB cube: r = x, g = y, b = z, sampled between the lattice points */
static void cube3d(void)
{
    unsigned char buf[4 * 4 * 4 * 4];
    int i = ncase++, x, y, z;

    for (z = 0; z < 4; z++) {
        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                unsigned char *p = buf + 4 * ((z * 4 + y) * 4 + x);
                p[0] = x * 85; p[1] = y * 85; p[2] = z * 85; p[3] = 255;
            }
        }
    }
    name[i] = "3D RGBA8 linear";
    tgt[i] = GL_TEXTURE_3D;
    glGenTextures(1, &tex[i]);
    glBindTexture(GL_TEXTURE_3D, tex[i]);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 4, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
}

static void setup(void)
{
    /* orange (ff 80 00) as Y' 97, Cb 2a, Cr c9, in both byte orders */
    static const unsigned char uyvy[4] = { 0x2a, 0x97, 0xc9, 0x97 };
    static const unsigned char yuyv[4] = { 0x97, 0x2a, 0x97, 0xc9 };

    yuv("YCbCr 8_8 UYVY bytes", GL_UNSIGNED_SHORT_8_8_APPLE, uyvy);
    yuv("YCbCr 8_8_REV UYVY bytes", GL_UNSIGNED_SHORT_8_8_REV_APPLE, uyvy);
    yuv("YCbCr 8_8 YUYV bytes", GL_UNSIGNED_SHORT_8_8_APPLE, yuyv);
    yuv("YCbCr 8_8_REV YUYV bytes", GL_UNSIGNED_SHORT_8_8_REV_APPLE, yuyv);
    cube3d();
}

static void display(void)
{
    int i;

    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 1, 1);
    for (i = 0; i < ncase; i++) {
        float x0 = -1 + (i % 3) * 0.66f + 0.05f, y0 = -1 + (i / 3) * 1.0f + 0.05f;
        float s = tgt[i] == GL_TEXTURE_RECTANGLE_ARB ? 16 : 1;
        glEnable(tgt[i]);
        glBindTexture(tgt[i], tex[i]);
        glBegin(GL_QUADS);
        /* the 3D case walks a diagonal of the cube at r = 0.4 */
        glTexCoord3f(0, 0, 0.4f); glVertex2f(x0, y0);
        glTexCoord3f(s, 0, 0.4f); glVertex2f(x0 + 0.55f, y0);
        glTexCoord3f(s, s, 0.4f); glVertex2f(x0 + 0.55f, y0 + 0.85f);
        glTexCoord3f(0, s, 0.4f); glVertex2f(x0, y0 + 0.85f);
        glEnd();
        glDisable(tgt[i]);
    }
    glFinish();
    rewind(out);
    for (i = 0; i < ncase; i++) {
        unsigned char c[4];
        int px = (int)(((i % 3) * 0.66f + 0.05f + 0.27f) * 128), py = (int)(((i / 3) * 1.0f + 0.5f) * 128);
        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        fprintf(out, "%-26s %02x%02x%02x\n", name[i], c[0], c[1], c[2]);
    }
    fflush(out);
    glutSwapBuffers();
}

static void tick(int v)
{
    if (v >= 4) {
        fclose(out);
        exit(0);
    }
    glutPostRedisplay();
    glutTimerFunc(1000, tick, v + 1);
}

int main(int argc, char **argv)
{
    out = fopen("/tmp/gltexyuv.out", "w");
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(256, 256);
    glutInitWindowPosition(500, 300);
    glutCreateWindow("gltexyuv");
    setup();
    glutDisplayFunc(display);
    glutTimerFunc(1000, tick, 0);
    glutMainLoop();
    return 0;
}
