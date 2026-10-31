#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <GLUT/glut.h>
#include <OpenGL/OpenGL.h>
#include <stdio.h>
/* Core Image into the current GLUT CGL context:
 * q0 (bottom-left)  CICheckerboardGenerator (red/white, 16 px)
 * q1 (bottom-right) CIImage from /tmp/small.jpg
 * q2 (top-left)     /tmp/small.jpg through CIGaussianBlur r=4
 * q3 (top-right)    left clear (blue) */
static FILE *out;
static CIContext *ci;
static CIImage *checker, *photo, *blurred;
static void probe(const char *name, int x, int y)
{
    unsigned char c[4];
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    fprintf(out, " %s(%d,%d)=%02x%02x%02x%02x", name, x, y, c[0], c[1], c[2], c[3]);
}
static void display(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    glViewport(0, 0, 256, 256);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 256, 0, 256, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!ci) {
        CGLContextObj cgl = CGLGetCurrentContext();
        ci = [[CIContext contextWithCGLContext:cgl pixelFormat:CGLGetPixelFormat(cgl) options:nil] retain];
        CIFilter *f = [CIFilter filterWithName:@"CICheckerboardGenerator"];
        [f setDefaults];
        [f setValue:[CIColor colorWithRed:1 green:0 blue:0] forKey:@"inputColor0"];
        [f setValue:[CIColor colorWithRed:1 green:1 blue:1] forKey:@"inputColor1"];
        [f setValue:[NSNumber numberWithFloat:16] forKey:@"inputWidth"];
        checker = [[f valueForKey:@"outputImage"] retain];
        photo = [[CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:@"/tmp/small.jpg"]] retain];
        CIFilter *b = [CIFilter filterWithName:@"CIGaussianBlur"];
        [b setDefaults];
        [b setValue:photo forKey:@"inputImage"];
        [b setValue:[NSNumber numberWithFloat:4] forKey:@"inputRadius"];
        blurred = [[b valueForKey:@"outputImage"] retain];
        fprintf(out, "ci %p checker %p photo %p blurred %p\n", ci, checker, photo, blurred);
    }
    [ci drawImage:checker inRect:CGRectMake(0, 0, 128, 128) fromRect:CGRectMake(0, 0, 128, 128)];
    [ci drawImage:photo inRect:CGRectMake(128, 0, 128, 128) fromRect:CGRectMake(100, 100, 128, 128)];
    [ci drawImage:blurred inRect:CGRectMake(0, 128, 128, 128) fromRect:CGRectMake(100, 100, 128, 128)];
    glFinish();
    fprintf(out, "frame:");
    probe("chk", 8, 8); probe("chk", 24, 8);
    probe("photo", 192, 64); probe("photo", 150, 20);
    probe("blur", 64, 192); probe("blur", 20, 150);
    probe("clear", 192, 192);
    fprintf(out, "\n");
    fflush(out);
    glutSwapBuffers();
    [pool release];
}
static void tick(int v)
{
    if (v >= 5) { fclose(out); exit(0); }
    glutPostRedisplay();
    glutTimerFunc(1000, tick, v + 1);
}
int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    out = fopen("/tmp/glci.out", "w");
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE);
    glutInitWindowSize(256, 256);
    glutInitWindowPosition(500, 300);
    glutCreateWindow("glci");
    glutDisplayFunc(display);
    glutTimerFunc(1000, tick, 0);
    glutMainLoop();
    [pool release];
    return 0;
}
