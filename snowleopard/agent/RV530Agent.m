/*
 * RV530Agent: guest end of host -> guest drag and drop.
 *
 * Connects to 10.0.2.100:7650 (slirp guestfwd to scripts/dnd/dnd-host.py),
 * receives dropped files into ~/Library/Caches/RV530Agent/<n>/ and delivers
 * them where the host dropped them: it starts a real AppKit drag of the
 * staged files at that point and releases it at once, so the application
 * under the pointer (Finder window, desktop, Dock icon, TextEdit, Mail ...)
 * gets an ordinary drop.  If nothing accepts the drop the files are copied
 * to the Desktop.  Protocol: see dnd-host.py.
 *
 *   gcc -framework Cocoa -framework ApplicationServices -framework IOKit -o RV530Agent RV530Agent.m
 *
 * Runs in the login session (LaunchAgent, see install.sh); reconnects every
 * few seconds while the host side is absent.
 *
 * It also follows the window size: the rv530 swaps the monitor (new
 * EDID with a new serial, hot-plug pulse) when the QEMU window is resized,
 * but WindowServer does not always pick the new monitor's preferred mode.
 * The agent watches the EDID and switches to its preferred mode whenever
 * the serial changes, so a mode chosen by hand otherwise stays.
 *
 * And it syncs the text clipboard over a second connection to
 * 10.0.2.100:7651, which sl-vm.sh CLIPBOARD=1 relays to the rv530's QEMU
 * clipboard peer: "TEXT <bytes>\n" + UTF-8 both ways.  The general
 * pasteboard has no change notification on 10.6, so it is polled.
 */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define HOST "10.0.2.100"
#define PORT 7650

static NSString *staging_root(void)
{
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches/RV530Agent"];
}

/* a relative path from the host: no absolute paths, no "..", no empty parts */
static NSString *safe_rel(NSString *rel)
{
    NSArray *parts = [rel componentsSeparatedByString:@"/"];
    if ([rel length] == 0 || [rel hasPrefix:@"/"]) {
        return nil;
    }
    for (NSString *p in parts) {
        if ([p length] == 0 || [p isEqualToString:@".."] || [p isEqualToString:@"."]) {
            return nil;
        }
    }
    return rel;
}

@interface Agent : NSObject {
    volatile int fd;
    NSArray *dragPaths;         /* files being dragged in the guest */
    BOOL dragActive, delivering;
    NSInteger dragSeen;
    NSMutableData *buf;
    NSArray *dropFiles;
    NSPoint dropAt;            /* Cocoa screen coordinates */
    NSDragOperation result;
    NSWindow *win;
}
@end

@interface DragView : NSView {
    Agent *agent;
}
- (id)initWithFrame:(NSRect)r agent:(Agent *)a;
@end

@implementation Agent

- (id)init
{
    if ((self = [super init])) {
        fd = -1;
    }
    return self;
}

- (BOOL)fill:(NSUInteger)want
{
    while ([buf length] < want) {
        char tmp[65536];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) {
            return NO;
        }
        [buf appendBytes:tmp length:n];
    }
    return YES;
}

- (NSString *)readLine
{
    for (;;) {
        const char *b = [buf bytes];
        NSUInteger i, len = [buf length];
        for (i = 0; i < len; i++) {
            if (b[i] == '\n') {
                NSString *s = [[[NSString alloc] initWithBytes:b length:i
                                                      encoding:NSUTF8StringEncoding] autorelease];
                [buf replaceBytesInRange:NSMakeRange(0, i + 1) withBytes:NULL length:0];
                return s ? s : @"";
            }
        }
        if (![self fill:len + 1]) {
            return nil;
        }
    }
}

- (void)send:(NSString *)line
{
    const char *s = [[line stringByAppendingString:@"\n"] UTF8String];
    @synchronized(self) {
        if (fd >= 0) {
            write(fd, s, strlen(s));
        }
    }
}

/* ---- guest -> host drags ---- */

static NSString *pct(NSString *s)
{
    return [(NSString *)CFURLCreateStringByAddingPercentEscapes(NULL, (CFStringRef)s, NULL,
             CFSTR(" %/?#&=+:;"), kCFStringEncodingUTF8) autorelease];
}

/* main thread, 10 times a second: announce file drags the user makes */
- (void)watchDrag:(NSTimer *)t
{
    BOOL down = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                         kCGMouseButtonLeft);
    NSPasteboard *pb = [NSPasteboard pasteboardWithName:NSDragPboard];
    NSInteger cc = [pb changeCount];

    if (!down) {
        if (dragActive) {
            dragActive = NO;
            [self send:@"DRAG-END"];
        }
        dragSeen = cc;
        return;
    }
    if (dragActive || delivering || cc == dragSeen) {
        return;
    }
    dragSeen = cc;
    if ([[pb types] containsObject:NSFilenamesPboardType]) {
        NSArray *files = [pb propertyListForType:NSFilenamesPboardType];
        NSMutableString *msg = [NSMutableString stringWithFormat:@"DRAG-START %lu",
                                (unsigned long)[files count]];
        for (NSString *f in files) {
            [msg appendFormat:@"\nPATH %@", pct(f)];
        }
        @synchronized(self) {
            [dragPaths release];
            dragPaths = [files copy];
        }
        dragActive = YES;
        [self send:msg];
    }
}

- (void)writeBytes:(const void *)b length:(NSUInteger)len
{
    @synchronized(self) {
        while (len && fd >= 0) {
            ssize_t n = write(fd, b, len);
            if (n <= 0) {
                break;
            }
            b = (const char *)b + n;
            len -= n;
        }
    }
}

/* serve thread: send one dragged item (file or folder tree) to the host */
- (void)serveGet:(NSString *)path
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *base = [path stringByDeletingLastPathComponent];
    NSMutableArray *items = [NSMutableArray arrayWithObject:path];
    BOOL allowed, dir;

    @synchronized(self) {
        allowed = [dragPaths containsObject:path];
    }
    if (!allowed || ![fm fileExistsAtPath:path isDirectory:&dir]) {
        [self send:@"ERROR not being dragged"];
        return;
    }
    if (dir) {
        for (NSString *sub in [fm enumeratorAtPath:path]) {
            [items addObject:[path stringByAppendingPathComponent:sub]];
        }
    }
    for (NSString *p in items) {
        NSDictionary *a = [fm attributesOfItemAtPath:p error:NULL];
        NSString *rel = [p substringFromIndex:[base length] + 1];
        if ([[a fileType] isEqualToString:NSFileTypeDirectory]) {
            [self send:[NSString stringWithFormat:@"DIR %@", pct(rel)]];
        } else if ([[a fileType] isEqualToString:NSFileTypeRegular]) {
            NSData *d = [NSData dataWithContentsOfFile:p options:NSDataReadingMapped error:NULL];
            if (!d) {
                continue;
            }
            @synchronized(self) {
                [self send:[NSString stringWithFormat:@"FILE %lu %@",
                            (unsigned long)[d length], pct(rel)]];
                [self writeBytes:[d bytes] length:[d length]];
            }
        }
    }
    [self send:@"END"];
}

/* receive one drop into a fresh staging folder; returns the top-level items */
- (NSArray *)receiveDrop:(NSUInteger)count error:(NSString **)err
{
    static unsigned serial;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dir = [staging_root() stringByAppendingPathComponent:
                     [NSString stringWithFormat:@"%d-%u", getpid(), ++serial]];
    NSMutableArray *top = [NSMutableArray array];
    NSUInteger k;

    /* earlier drops: targets copy asynchronously, so keep them an hour */
    for (NSString *old in [fm contentsOfDirectoryAtPath:staging_root() error:NULL]) {
        NSString *p = [staging_root() stringByAppendingPathComponent:old];
        NSDate *m = [[fm attributesOfItemAtPath:p error:NULL] fileModificationDate];
        if (m && [m timeIntervalSinceNow] < -3600) {
            [fm removeItemAtPath:p error:NULL];
        }
    }
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    for (k = 0; k < count; k++) {
        NSString *line = [self readLine];
        NSArray *f = [line componentsSeparatedByString:@" "];
        NSString *rel, *path;
        if (!line) {
            *err = @"connection lost";
            return nil;
        }
        rel = safe_rel([[f lastObject] stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding]);
        if (!rel) {
            *err = [NSString stringWithFormat:@"rejected path %@", [f lastObject]];
            return nil;
        }
        path = [dir stringByAppendingPathComponent:rel];
        if ([[f objectAtIndex:0] isEqualToString:@"DIR"]) {
            [fm createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL];
        } else if ([[f objectAtIndex:0] isEqualToString:@"FILE"] && [f count] == 3) {
            unsigned long long size = strtoull([[f objectAtIndex:1] UTF8String], NULL, 10);
            NSFileHandle *h;
            [fm createDirectoryAtPath:[path stringByDeletingLastPathComponent]
          withIntermediateDirectories:YES attributes:nil error:NULL];
            [fm createFileAtPath:path contents:nil attributes:nil];
            h = [NSFileHandle fileHandleForWritingAtPath:path];
            while (size) {
                NSUInteger chunk = size > 65536 ? 65536 : (NSUInteger)size;
                NSData *d;
                if (![self fill:chunk]) {
                    *err = @"connection lost";
                    return nil;
                }
                d = [buf subdataWithRange:NSMakeRange(0, chunk)];
                [h writeData:d];
                [buf replaceBytesInRange:NSMakeRange(0, chunk) withBytes:NULL length:0];
                size -= chunk;
            }
            [h closeFile];
        } else {
            *err = [NSString stringWithFormat:@"bad entry %@", line];
            return nil;
        }
        if ([rel rangeOfString:@"/"].location == NSNotFound) {
            [top addObject:path];
        }
    }
    return top;
}

/* ---- delivery (main thread) ---- */

- (NSDragOperation)draggingSourceOperationMaskForLocal:(BOOL)local
{
    /* copy: Finder would otherwise make aliases or move the staged files */
    return NSDragOperationCopy;
}

- (void)draggedImage:(NSImage *)img endedAt:(NSPoint)p operation:(NSDragOperation)op
{
    NSLog(@"RV530Agent: drag ended at %@, operation %lu", NSStringFromPoint(p), (unsigned long)op);
    result = op;
}

- (void)startDragFrom:(NSView *)v event:(NSEvent *)e
{
    NSPasteboard *pb = [NSPasteboard pasteboardWithName:NSDragPboard];
    NSImage *img = [[NSWorkspace sharedWorkspace] iconForFiles:dropFiles];
    [img setSize:NSMakeSize(64, 64)];
    [pb declareTypes:[NSArray arrayWithObject:NSFilenamesPboardType] owner:nil];
    [pb setPropertyList:dropFiles forType:NSFilenamesPboardType];
    /* let the window under ours receive the drop */
    [win setIgnoresMouseEvents:YES];
    /* release the button shortly after the drag loop starts */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * 1000000ull),
                   dispatch_get_global_queue(0, 0), ^{
        CGPoint q = CGPointMake(dropAt.x,
                                NSMaxY([[[NSScreen screens] objectAtIndex:0] frame]) - dropAt.y);
        CGEventRef m = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDragged, q, kCGMouseButtonLeft);
        CGEventPost(kCGSessionEventTap, m);
        CFRelease(m);
        usleep(150000);
        m = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp, q, kCGMouseButtonLeft);
        CGEventPost(kCGSessionEventTap, m);
        CFRelease(m);
    });
    [v dragImage:img at:NSMakePoint(-32, -32) offset:NSZeroSize event:e pasteboard:pb
          source:self slideBack:NO];
}

- (NSString *)deliver
{
    NSRect r = NSMakeRect(dropAt.x - 8, dropAt.y - 8, 16, 16);
    CGPoint q = CGPointMake(dropAt.x, NSMaxY([[[NSScreen screens] objectAtIndex:0] frame]) - dropAt.y);
    CGEventRef m;
    NSDate *until;

    result = NSDragOperationNone;
    NSLog(@"RV530Agent: delivering %lu items at %@", (unsigned long)[dropFiles count], NSStringFromPoint(dropAt));
    win = [[NSWindow alloc] initWithContentRect:r styleMask:NSBorderlessWindowMask
                                        backing:NSBackingStoreBuffered defer:NO];
    [win setOpaque:NO];
    [win setBackgroundColor:[NSColor colorWithCalibratedWhite:0.5 alpha:0.25]];
    [win setLevel:NSScreenSaverWindowLevel];
    [win setContentView:[[[DragView alloc] initWithFrame:NSMakeRect(0, 0, 16, 16) agent:self] autorelease]];
    [win orderFrontRegardless];
    [NSApp activateIgnoringOtherApps:YES];
    CGWarpMouseCursorPosition(q);
    /* let the window server put the window on screen before pressing on it */
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
    /* press the button for the window server's drag tracking, and start the
     * drag from our window with a matching mouse-down event */
    m = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown, q, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, m);
    CFRelease(m);
    {
        NSEvent *down = [NSEvent mouseEventWithType:NSLeftMouseDown location:NSMakePoint(8, 8)
                                      modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime]
                                       windowNumber:[win windowNumber] context:nil eventNumber:0
                                         clickCount:1 pressure:1.0];
        [self startDragFrom:[win contentView] event:down];
    }
    until = [NSDate dateWithTimeIntervalSinceNow:5];
    while (!result && [until timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        if (![win isVisible]) {
            break;
        }
    }
    [win orderOut:nil];
    [win release];
    win = nil;
    if (result != NSDragOperationNone) {
        return [NSString stringWithFormat:@"ok dropped (operation %lu)", (unsigned long)result];
    }
    /* nobody took it: put the files on the Desktop */
    for (NSString *f in dropFiles) {
        NSString *dst = [[NSHomeDirectory() stringByAppendingPathComponent:@"Desktop"]
                         stringByAppendingPathComponent:[f lastPathComponent]];
        [[NSFileManager defaultManager] copyItemAtPath:f toPath:dst error:NULL];
    }
    return @"ok copied to the Desktop (no drop target accepted it)";
}

- (void)deliverOnMain:(NSMutableArray *)answer
{
    delivering = YES;           /* our own drag is not a user drag */
    [answer addObject:[self deliver]];
    dragSeen = [[NSPasteboard pasteboardWithName:NSDragPboard] changeCount];
    delivering = NO;
}

- (void)serve
{
    for (;;) {
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(PORT);
        inet_pton(AF_INET, HOST, &a.sin_addr);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            NSString *line;
            buf = [[NSMutableData alloc] init];
            [self send:@"HELLO 1"];
            while ((line = [self readLine])) {
                NSArray *f = [line componentsSeparatedByString:@" "];
                NSString *err = nil, *end;
                NSArray *files;
                NSMutableArray *answer = [NSMutableArray array];
                if ([[f objectAtIndex:0] isEqualToString:@"GET"] && [f count] == 2) {
                    [self serveGet:[[f objectAtIndex:1]
                                    stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding]];
                    continue;
                }
                if (![[f objectAtIndex:0] isEqualToString:@"DROP"] || [f count] != 4) {
                    continue;
                }
                files = [self receiveDrop:[[f objectAtIndex:3] integerValue] error:&err];
                end = [self readLine];
                if (!files || ![end isEqualToString:@"END"]) {
                    [self send:[NSString stringWithFormat:@"DONE error %@", err ? err : @"protocol"]];
                    continue;
                }
                dropFiles = [files retain];
                /* host pixels from the top left -> Cocoa points from the bottom left */
                dropAt = NSMakePoint([[f objectAtIndex:1] doubleValue],
                                     NSMaxY([[[NSScreen screens] objectAtIndex:0] frame]) -
                                     [[f objectAtIndex:2] doubleValue]);
                [self performSelectorOnMainThread:@selector(deliverOnMain:) withObject:answer
                                    waitUntilDone:YES];
                [self send:[@"DONE " stringByAppendingString:[answer lastObject]]];
                [dropFiles release];
                dropFiles = nil;
            }
            [buf release];
        }
        @synchronized(self) {
            close(fd);
            fd = -1;
        }
        [pool release];
        sleep(3);
    }
}
@end

@implementation DragView
- (id)initWithFrame:(NSRect)r agent:(Agent *)a
{
    if ((self = [super initWithFrame:r])) {
        agent = a;
    }
    return self;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)e
{
    return YES;
}
- (void)mouseDown:(NSEvent *)e
{
    NSLog(@"RV530Agent: mouseDown, starting the drag");
    [agent startDragFrom:self event:e];
}
@end

/* ---- window-size following ---- */

/* serial and preferred (first detailed timing) size of the main display's EDID */
static BOOL main_edid(uint32_t *serial, int *w, int *h)
{
    io_service_t svc = CGDisplayIOServicePort(CGMainDisplayID());
    NSDictionary *info;
    NSData *edid;
    const uint8_t *e;

    if (!svc) {
        return NO;
    }
    info = [(NSDictionary *)IODisplayCreateInfoDictionary(svc, kIODisplayOnlyPreferredName) autorelease];
    edid = [info objectForKey:@kIODisplayEDIDKey];
    if ([edid length] < 128) {
        return NO;
    }
    e = [edid bytes];
    *serial = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
    *w = e[56] | (e[58] >> 4) << 8;
    *h = e[59] | (e[61] >> 4) << 8;
    return YES;
}

@interface Follower : NSObject {
    uint32_t seen;
    int tries;                  /* attempts left for the current monitor */
    int faster;                 /* seconds left to raise the login mode's refresh rate */
}
@end

@implementation Follower
- (id)init
{
    int w, h;
    if ((self = [super init])) {
        main_edid(&seen, &w, &h);    /* the monitor at login: keep its size */
        faster = 20;
        [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(check:)
                                       userInfo:nil repeats:YES];
    }
    return self;
}

/*
 * The device's EDID offers each size at 60 Hz and, with sl-vm.sh REFRESH,
 * at a faster rate too; WindowServer may pick either, and keeps the
 * 60 Hz one if it saw the monitor at 60 Hz before.  Pick the fastest.
 */
static double mode_hz(CGDisplayModeRef m)
{
    double hz = CGDisplayModeGetRefreshRate(m);
    return hz > 0 ? hz : 60.0;      /* flat panels may report 0 */
}

/* the 32 bpp mode of w x h with the highest refresh rate, retained; NULL if none */
static CGDisplayModeRef best_mode(CGDirectDisplayID d, int w, int h)
{
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, NULL);
    CGDisplayModeRef best = NULL;
    CFIndex i;

    for (i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        CFStringRef enc = CGDisplayModeCopyPixelEncoding(m);
        BOOL rgb32 = CFStringCompare(enc, CFSTR(IO32BitDirectPixels), 0) == kCFCompareEqualTo;
        CFRelease(enc);
        if (rgb32 && (int)CGDisplayModeGetWidth(m) == w && (int)CGDisplayModeGetHeight(m) == h &&
            (!best || mode_hz(m) > mode_hz(best))) {
            best = m;
        }
    }
    if (best) {
        CGDisplayModeRetain(best);
    }
    if (modes) {
        CFRelease(modes);
    }
    return best;
}

/* switch to w x h at 32 bpp and the fastest rate offered: 1 switched,
 * 0 already there, -1 the mode is not (yet) available */
static int set_mode(CGDirectDisplayID d, int w, int h)
{
    CGDisplayModeRef best = best_mode(d, w, h), cur = CGDisplayCopyDisplayMode(d);
    int r = -1;

    if (best && cur && (int)CGDisplayModeGetWidth(cur) == w &&
        (int)CGDisplayModeGetHeight(cur) == h && mode_hz(cur) >= mode_hz(best)) {
        r = 0;
    } else if (best && CGDisplaySetDisplayMode(d, best, NULL) == kCGErrorSuccess) {
        NSLog(@"RV530Agent: switched to %dx%d at %.0f Hz", w, h, mode_hz(best));
        r = 1;
    }
    CGDisplayModeRelease(best);
    CGDisplayModeRelease(cur);
    return r;
}

- (void)check:(NSTimer *)t
{
    CGDirectDisplayID d = CGMainDisplayID();
    uint32_t serial;
    int w, h;

    if (faster > 0) {
        /* the login mode: same size, fastest rate.  Right after login
         * WindowServer may not list the faster modes yet, so keep trying
         * for a while (set_mode does nothing once the fastest is set). */
        faster--;
        if (set_mode(d, (int)CGDisplayPixelsWide(d), (int)CGDisplayPixelsHigh(d)) > 0) {
            faster = 0;
        }
    }
    if (!main_edid(&serial, &w, &h)) {
        return;
    }
    if (serial != seen) {
        /* a new monitor: WindowServer may still be reconfiguring, so keep
         * trying for a while */
        seen = serial;
        tries = 10;
    }
    if (tries <= 0) {
        return;
    }
    tries--;
    if (set_mode(d, w, h) >= 0) {
        tries = 0;
    }
}
@end

/* ---- clipboard ---- */

#define CLIP_PORT 7651

@interface Clip : NSObject {
    volatile int fd;
    NSInteger seen;             /* changeCount already handled */
    NSInteger own;              /* changeCount of text we set from the host */
}
@end

@implementation Clip
- (id)init
{
    if ((self = [super init])) {
        fd = -1;
        seen = own = [[NSPasteboard generalPasteboard] changeCount];
        [NSThread detachNewThreadSelector:@selector(serve) toTarget:self withObject:nil];
        [NSTimer scheduledTimerWithTimeInterval:0.3 target:self selector:@selector(poll:)
                                       userInfo:nil repeats:YES];
    }
    return self;
}

- (void)setText:(NSString *)text
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb declareTypes:[NSArray arrayWithObject:NSStringPboardType] owner:nil];
    [pb setString:text forType:NSStringPboardType];
    own = seen = [pb changeCount];
}

/* main thread: send guest copies to the host */
- (void)poll:(NSTimer *)t
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSInteger cc = [pb changeCount];
    NSString *text;
    NSData *d;
    int s = fd;

    if (cc == seen) {
        return;
    }
    seen = cc;
    if (cc == own || s < 0 || !(text = [pb stringForType:NSStringPboardType])) {
        return;
    }
    d = [text dataUsingEncoding:NSUTF8StringEncoding];
    {
        const char *hdr = [[NSString stringWithFormat:@"TEXT %lu\n",
                            (unsigned long)[d length]] UTF8String];
        write(s, hdr, strlen(hdr));
        write(s, [d bytes], [d length]);
    }
}

static BOOL read_full(int s, void *buf, size_t len)
{
    while (len) {
        ssize_t n = read(s, buf, len);
        if (n <= 0) {
            return NO;
        }
        buf = (char *)buf + n;
        len -= n;
    }
    return YES;
}

/* background thread: host copies into the guest */
- (void)serve
{
    for (;;) {
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        struct sockaddr_in a;
        int s = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(CLIP_PORT);
        inet_pton(AF_INET, HOST, &a.sin_addr);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            char line[64];
            size_t k = 0;
            write(s, "HELLO 1\n", 8);
            fd = s;
            for (;;) {
                char c;
                unsigned long n;
                if (read(s, &c, 1) != 1) {
                    break;
                }
                if (c != '\n') {
                    if (k < sizeof(line) - 1) {
                        line[k++] = c;
                    }
                    continue;
                }
                line[k] = 0;
                k = 0;
                if (sscanf(line, "TEXT %lu", &n) == 1 && n <= 4 * 1024 * 1024) {
                    NSMutableData *d = [NSMutableData dataWithLength:n];
                    NSString *text;
                    if (!read_full(s, [d mutableBytes], n)) {
                        break;
                    }
                    text = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] autorelease];
                    if (text) {
                        [self performSelectorOnMainThread:@selector(setText:) withObject:text
                                            waitUntilDone:YES];
                    }
                }
            }
            fd = -1;
        }
        close(s);
        [pool release];
        sleep(3);
    }
}
@end

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    Agent *agent = [[Agent alloc] init];
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    NSLog(@"RV530Agent: started");
    [NSThread detachNewThreadSelector:@selector(serve) toTarget:agent withObject:nil];
    [[Follower alloc] init];
    [[Clip alloc] init];
    [NSTimer scheduledTimerWithTimeInterval:0.1 target:agent selector:@selector(watchDrag:)
                                   userInfo:nil repeats:YES];
    [NSApp run];
    [pool release];
    return 0;
}
