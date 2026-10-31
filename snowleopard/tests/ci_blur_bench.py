# Core Image Gaussian blur 1024x1024 on a GPU CIContext: ms per frame
# (mean of FRAMES frames after a warm-up).  python ci_blur_bench.py [FRAMES]
import sys, time
from Quartz import *
frames = int(sys.argv[1]) if len(sys.argv) > 1 else 20
w = h = 1024
cs = CGColorSpaceCreateDeviceRGB()
def bitmap():
    return CGBitmapContextCreate(None, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little)
src = bitmap()
for i in range(0, 1024, 64):
    CGContextSetRGBFillColor(src, (i % 256) / 255.0, 0.5, 1 - (i % 256) / 255.0, 1)
    CGContextFillRect(src, CGRectMake(i, 0, 32, h))
img = CIImage.imageWithCGImage_(CGBitmapContextCreateImage(src))
f = CIFilter.filterWithName_("CIGaussianBlur"); f.setDefaults()
f.setValue_forKey_(img, "inputImage"); f.setValue_forKey_(8.0, "inputRadius")
dst = bitmap()
ctx = CIContext.contextWithCGContext_options_(dst, {"kCIContextUseSoftwareRenderer": False})
ctx.drawImage_inRect_fromRect_(f.valueForKey_("outputImage"), CGRectMake(0, 0, w, h), CGRectMake(0, 0, w, h))
t = time.time()
for i in range(frames):
    f.setValue_forKey_(8.0 + i * 0.1, "inputRadius")
    ctx.drawImage_inRect_fromRect_(f.valueForKey_("outputImage"), CGRectMake(0, 0, w, h), CGRectMake(0, 0, w, h))
    CGContextFlush(dst)
print "CI GPU blur 1024x1024: %.1f ms per frame" % ((time.time() - t) * 1000.0 / frames)
