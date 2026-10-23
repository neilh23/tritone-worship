#import <Cocoa/Cocoa.h>

#include "tw_gui.h"
#include "tw_ui.h"

/* Each plugin format names its own class, so a host can load several formats at once */
#ifndef TW_COCOA_VIEW
#define TW_COCOA_VIEW TritoneWorshipView
#endif

@interface TW_COCOA_VIEW : NSView {
@public
    TwGuiCallbacks cb;
    void *ctx;
    uint32_t px[TW_UI_W * TW_UI_H];
    float drawn[TW_P_COUNT];
    int drawn_active;
    int active; /* knob being dragged, -1 for none */
    CGFloat drag_y;
    float drag_start;
    NSTimer *timer;
}
@end

@implementation TW_COCOA_VIEW

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return YES;
}

- (void)readValues:(float *)v
{
    for (int p = 0; p < TW_P_COUNT; p++)
        v[p] = cb.get(ctx, p);
}

- (void)drawRect:(NSRect)dirty
{
    [self readValues:drawn];
    drawn_active = active;
    tw_ui_draw(px, drawn, active);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef bitmap = CGBitmapContextCreate(px, TW_UI_W, TW_UI_H, 8, TW_UI_W * 4, space,
                                                kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = CGBitmapContextCreateImage(bitmap);
    CGContextRef gc = [[NSGraphicsContext currentContext] CGContext];
    /* The view is flipped, so flip back for CoreGraphics' bottom-up images */
    CGContextSaveGState(gc);
    CGContextTranslateCTM(gc, 0, TW_UI_H);
    CGContextScaleCTM(gc, 1, -1);
    CGContextDrawImage(gc, CGRectMake(0, 0, TW_UI_W, TW_UI_H), image);
    CGContextRestoreGState(gc);
    CGImageRelease(image);
    CGContextRelease(bitmap);
    CGColorSpaceRelease(space);
}

- (NSPoint)pointOf:(NSEvent *)event
{
    return [self convertPoint:[event locationInWindow] fromView:nil];
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint pt = [self pointOf:event];
    int p = tw_ui_hit((int)pt.x, (int)pt.y);
    if (p < 0)
        return;
    cb.begin(ctx, p);
    if (p == TW_P_BLACKEN || p == TW_P_BYPASS) {
        cb.set(ctx, p, cb.get(ctx, p) > 0.5f ? 0.0f : 1.0f);
        cb.end(ctx, p);
    } else {
        active = p;
        drag_y = pt.y;
        drag_start = cb.get(ctx, p);
    }
    [self refreshIfChanged];
}

- (void)mouseDragged:(NSEvent *)event
{
    if (active < 0)
        return;
    cb.set(ctx, active, tw_ui_drag_value(active, drag_start, (int)(drag_y - [self pointOf:event].y)));
    [self refreshIfChanged];
}

- (void)mouseUp:(NSEvent *)event
{
    if (active < 0)
        return;
    cb.end(ctx, active);
    active = -1;
    [self refreshIfChanged];
}

/* Repaints when a value has changed, from the host or from the mouse */
- (void)refreshIfChanged
{
    float v[TW_P_COUNT];
    [self readValues:v];
    if (active != drawn_active || memcmp(v, drawn, sizeof v))
        [self setNeedsDisplay:YES];
}

@end

struct TwGui {
    void *view; /* retained TW_COCOA_VIEW */
};

TwGui *tw_gui_create(void *parent, const TwGuiCallbacks *cb, void *ctx)
{
    TwGui *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    TW_COCOA_VIEW *view = [[TW_COCOA_VIEW alloc] initWithFrame:NSMakeRect(0, 0, TW_UI_W, TW_UI_H)];
    view->cb = *cb;
    view->ctx = ctx;
    view->active = -1;
    view->drawn_active = -1;
    view->timer = [NSTimer scheduledTimerWithTimeInterval:0.03
                                                   target:view
                                                 selector:@selector(refreshIfChanged)
                                                 userInfo:nil
                                                  repeats:YES];
    [(__bridge NSView *)parent addSubview:view];
    g->view = (void *)CFBridgingRetain(view);
    return g;
}

void tw_gui_destroy(TwGui *g)
{
    TW_COCOA_VIEW *view = CFBridgingRelease(g->view);
    [view->timer invalidate];
    view->timer = nil;
    [view removeFromSuperview];
    free(g);
}

void tw_gui_show(TwGui *g, int visible)
{
    [(__bridge TW_COCOA_VIEW *)g->view setHidden:!visible];
}

void tw_gui_idle(TwGui *g)
{
    [(__bridge TW_COCOA_VIEW *)g->view refreshIfChanged];
}
