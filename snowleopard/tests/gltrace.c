/*
 * gltrace.dylib: count an application's OpenGL draw calls without touching
 * it.  DYLD_INSERT_LIBRARIES interposer; every 2 s it writes a histogram to
 * /tmp/gltrace.out: draw calls by function, mode and size, glBegin/glEnd
 * pairs (and whether a display list was being compiled), GL errors right
 * after a call, NaN/Inf vertices and the bounding box of glVertex data.
 *
 *   gcc -dynamiclib -arch x86_64 -arch i386 -o /tmp/gltrace.dylib gltrace.c -framework OpenGL
 *   DYLD_INSERT_LIBRARIES=/tmp/gltrace.dylib /Applications/Utilities/Grapher.app/Contents/MacOS/Grapher
 *   sort -rn /tmp/gltrace.out
 *
 * Tells apart "the application never drew it" from "the GPU dropped it".
 */
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DYLD_INTERPOSE(_r, _o) \
    __attribute__((used)) static struct { const void *r; const void *o; } _i_##_o \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)&_r, (const void *)&_o };

#define NK 256
static struct { char key[96]; unsigned long n; } tab[NK];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static time_t last;
static int in_list, verts;
static double vmin[3] = { 1e30, 1e30, 1e30 }, vmax[3] = { -1e30, -1e30, -1e30 };

/* called with mu held */
static void dump(void)
{
    FILE *f = fopen("/tmp/gltrace.out", "w");
    int i;

    if (!f) {
        return;
    }
    for (i = 0; i < NK && tab[i].key[0]; i++) {
        fprintf(f, "%8lu %s\n", tab[i].n, tab[i].key);
    }
    fprintf(f, "       1 vertex box [%.3g..%.3g] [%.3g..%.3g] [%.3g..%.3g]\n",
            vmin[0], vmax[0], vmin[1], vmax[1], vmin[2], vmax[2]);
    fclose(f);
}

static void note(const char *fmt, ...)
{
    char k[96];
    va_list ap;
    int i;

    va_start(ap, fmt);
    vsnprintf(k, sizeof(k), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    for (i = 0; i < NK; i++) {
        if (!tab[i].key[0]) {
            strcpy(tab[i].key, k);
            tab[i].n = 1;
            break;
        }
        if (!strcmp(tab[i].key, k)) {
            tab[i].n++;
            break;
        }
    }
    if (time(NULL) - last >= 2) {
        last = time(NULL);
        dump();
    }
    pthread_mutex_unlock(&mu);
}

static void err(const char *fn)
{
    GLenum e = glGetError();
    if (e) {
        note("GL ERROR 0x%x after %s", e, fn);
    }
}

static void vtx(double x, double y, double z)
{
    double v[3] = { x, y, z };
    int i;

    verts++;
    if (isnan(x) || isnan(y) || isnan(z) || isinf(x) || isinf(y) || isinf(z)) {
        note("glVertex NaN/Inf");
        return;
    }
    for (i = 0; i < 3; i++) {
        if (v[i] < vmin[i]) vmin[i] = v[i];
        if (v[i] > vmax[i]) vmax[i] = v[i];
    }
}

static void my_glDrawArrays(GLenum m, GLint first, GLsizei c)
{
    note("glDrawArrays mode %u count %s", m, c > 64 ? ">64" : "<=64");
    glDrawArrays(m, first, c);
    err("glDrawArrays");
}
static void my_glDrawElements(GLenum m, GLsizei c, GLenum t, const GLvoid *i)
{
    note("glDrawElements mode %u count %s type 0x%x", m, c > 64 ? ">64" : "<=64", t);
    glDrawElements(m, c, t, i);
    err("glDrawElements");
}
static void my_glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t,
                                   const GLvoid *i)
{
    note("glDrawRangeElements mode %u count %s", m, c > 64 ? ">64" : "<=64");
    glDrawRangeElements(m, s, e, c, t, i);
    err("glDrawRangeElements");
}
static void my_glMultiDrawArrays(GLenum m, const GLint *f, const GLsizei *c, GLsizei n)
{
    note("glMultiDrawArrays mode %u", m);
    glMultiDrawArrays(m, f, c, n);
    err("glMultiDrawArrays");
}
static void my_glBegin(GLenum m) { note("glBegin mode %u", m); verts = 0; glBegin(m); }
static void my_glEnd(void)
{
    note("glEnd after %s verts (%s)",
         verts == 0 ? "0" : verts < 8 ? "<8" : verts < 100 ? "<100" : ">=100",
         in_list ? "compiling list" : "immediate");
    glEnd();
    err("glEnd");
}
static void my_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { vtx(x, y, z); glVertex3f(x, y, z); }
static void my_glVertex3d(GLdouble x, GLdouble y, GLdouble z) { vtx(x, y, z); glVertex3d(x, y, z); }
static void my_glVertex3fv(const GLfloat *v) { vtx(v[0], v[1], v[2]); glVertex3fv(v); }
static void my_glVertex3dv(const GLdouble *v) { vtx(v[0], v[1], v[2]); glVertex3dv(v); }
static void my_glNewList(GLuint l, GLenum m) { in_list = 1; note("glNewList mode 0x%x", m); glNewList(l, m); }
static void my_glEndList(void) { in_list = 0; note("glEndList"); glEndList(); err("glEndList"); }
static void my_glCallList(GLuint l) { note("glCallList"); glCallList(l); err("glCallList"); }
static void my_glLineWidth(GLfloat w) { note("glLineWidth %.1f", w); glLineWidth(w); }
static void my_glEnable(GLenum c) { note("glEnable 0x%x", c); glEnable(c); err("glEnable"); }
static void my_glBindBuffer(GLenum t, GLuint b) { note("glBindBuffer 0x%x %s", t, b ? "obj" : "0"); glBindBuffer(t, b); }
static void my_glUseProgramObjectARB(GLhandleARB p)
{
    note("glUseProgramObjectARB %s", p ? "prog" : "0");
    glUseProgramObjectARB(p);
    err("glUseProgramObjectARB");
}

DYLD_INTERPOSE(my_glDrawArrays, glDrawArrays)
DYLD_INTERPOSE(my_glDrawElements, glDrawElements)
DYLD_INTERPOSE(my_glDrawRangeElements, glDrawRangeElements)
DYLD_INTERPOSE(my_glMultiDrawArrays, glMultiDrawArrays)
DYLD_INTERPOSE(my_glBegin, glBegin)
DYLD_INTERPOSE(my_glEnd, glEnd)
DYLD_INTERPOSE(my_glVertex3f, glVertex3f)
DYLD_INTERPOSE(my_glVertex3d, glVertex3d)
DYLD_INTERPOSE(my_glVertex3fv, glVertex3fv)
DYLD_INTERPOSE(my_glVertex3dv, glVertex3dv)
DYLD_INTERPOSE(my_glNewList, glNewList)
DYLD_INTERPOSE(my_glEndList, glEndList)
DYLD_INTERPOSE(my_glCallList, glCallList)
DYLD_INTERPOSE(my_glLineWidth, glLineWidth)
DYLD_INTERPOSE(my_glEnable, glEnable)
DYLD_INTERPOSE(my_glBindBuffer, glBindBuffer)
DYLD_INTERPOSE(my_glUseProgramObjectARB, glUseProgramObjectARB)
