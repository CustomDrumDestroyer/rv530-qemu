# Core Image Gaussian blur on a GPU CIContext vs the software renderer.
# Run in the guest with the system Python (2.6 on 10.6, 2.7 on 10.7):
# python ci_blur_check.py
# Prints the red channel at a few probes for both and the max difference
# (expect 0).
import sys, time
from Quartz import *
w = h = 256
cs = CGColorSpaceCreateDeviceRGB()
def bitmap():
    return CGBitmapContextCreate(None, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little)
src = bitmap()
CGContextSetRGBFillColor(src, 0, 0, 0, 1); CGContextFillRect(src, CGRectMake(0, 0, w, h))
CGContextSetRGBFillColor(src, 1, 0.5, 0, 1); CGContextFillRect(src, CGRectMake(64, 64, 128, 128))
img = CIImage.imageWithCGImage_(CGBitmapContextCreateImage(src))
f = CIFilter.filterWithName_("CIGaussianBlur"); f.setDefaults()
f.setValue_forKey_(img, "inputImage"); f.setValue_forKey_(10.0, "inputRadius")
out = f.valueForKey_("outputImage")
res = {}
for name, sw in (("gpu", False), ("cpu", True)):
    dst = bitmap()
    ctx = CIContext.contextWithCGContext_options_(dst, {"kCIContextUseSoftwareRenderer": sw})
    t = time.time()
    ctx.drawImage_inRect_fromRect_(out, CGRectMake(0, 0, w, h), CGRectMake(0, 0, w, h))
    CGContextFlush(dst)
    dt = time.time() - t
    data = CGDataProviderCopyData(CGImageGetDataProvider(CGBitmapContextCreateImage(dst))).bytes()
    # 10.6 (Python 2.6) gets a buffer, 10.7 (2.7) a memoryview
    buf = data.tobytes() if hasattr(data, "tobytes") else str(data)
    vals = [ord(buf[(y * w + x) * 4 + 2]) for (x, y) in ((128, 128), (64, 128), (60, 128), (52, 128), (30, 128), (5, 5))]
    res[name] = vals
    print name, "%.3fs" % dt, "red channel at probes:", vals
print "max diff", max(abs(a - b) for a, b in zip(res["gpu"], res["cpu"]))
