// The macOS app. Objective-C++, so AppKit and our C++ share a file.

#import <Cocoa/Cocoa.h>

#include <tt/agent.hpp>
#include <tt/timeutil.hpp>
#include <tt/tracker.hpp>
#include <tt/voice.hpp>

#include <memory>
#include <string>
#include <vector>

// palette: the hex from the design

static NSColor *hex(unsigned rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

#define C_SIDEBAR   hex(0x1C1C1E)
#define C_BG        hex(0xF5F5F7)
#define C_CARD      NSColor.whiteColor
#define C_BORDER    hex(0xE5E5EA)
#define C_TEXT      hex(0x1C1C1E)
#define C_MUTED     hex(0x8E8E93)
#define C_BLUE      hex(0x007AFF)
#define C_BLUETINT  hex(0xE2F1FF)
#define C_PILL      hex(0xE5E5EA)
#define C_ICON      hex(0x8E8E93)

namespace
{
    // ~/Library/Application Support/TimeTracker/tt.db, not the bundle: a signed .app is read only.
    std::string database_path()
    {
        NSArray *dirs = NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES);
        NSString *dir = [dirs.firstObject stringByAppendingPathComponent:@"TimeTracker"];

        [NSFileManager.defaultManager createDirectoryAtPath:dir
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];

        return [[dir stringByAppendingPathComponent:@"tt.db"] UTF8String];
    }

    NSString *str(const std::string &s) { return @(s.c_str()); }

    // nil, not a throw: a malformed tool result must not reach AppKit.
    NSString *text(const nlohmann::json &j, const char *key)
    {
        const auto at = j.find(key);
        return (at != j.end() && at->is_string()) ? str(at->get<std::string>()) : nil;
    }

    // Shown by both -buildList and -reload.
    NSString *const kNoMatters =
        @"No matters yet.\nAdd one, or press AI Assistant and say what you worked on.";

    // Colour comes from the button's contentTintColor, so no tint argument.
    NSImage *symbol(NSString *name, CGFloat size)
    {
        NSImage *img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
        NSImageSymbolConfiguration *cfg =
            [NSImageSymbolConfiguration configurationWithPointSize:size
                                                            weight:NSFontWeightMedium];
        img = [img imageWithSymbolConfiguration:cfg];
        [img setTemplate:YES];
        return img;
    }

    NSTextField *label(NSString *text, CGFloat size, NSFontWeight weight, NSColor *color)
    {
        NSTextField *f = [NSTextField labelWithString:text];
        f.font = [NSFont systemFontOfSize:size weight:weight];
        f.textColor = color;
        f.translatesAutoresizingMaskIntoConstraints = NO;
        return f;
    }
}

// NSView has no backgroundColor. On the layer, so a corner radius clips it.

namespace
{
    NSView *fill(NSColor *color)
    {
        NSView *v = [[NSView alloc] init];
        v.wantsLayer = YES;
        v.layer.backgroundColor = color.CGColor;
        return v;
    }
}

// The white card behind a run of task rows, round at the ends only.

@interface TTRowView : NSTableRowView
@property(nonatomic) BOOL isTask;
@property(nonatomic) BOOL isFirst;
@property(nonatomic) BOOL isLast;
@end

@implementation TTRowView
- (void)drawBackgroundInRect:(NSRect)dirty
{
    (void)dirty;
    if (!_isTask) { return; }   // group rows sit on the page background

    // 20 each side. Row text at 34 is 14 inside the card.
    NSRect r = NSInsetRect(self.bounds, 20.5, 0);
    const CGFloat radius = 10;

    NSBezierPath *p;
    if (_isFirst && _isLast)
    {
        p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 0, 0.5)
                                            xRadius:radius yRadius:radius];
    }
    else if (_isFirst || _isLast)
    {
        // Two rounded corners: a taller rounded rect, clipped by the row.
        NSRect grown = _isFirst ? NSMakeRect(r.origin.x, r.origin.y, r.size.width, r.size.height + radius)
                                : NSMakeRect(r.origin.x, r.origin.y - radius, r.size.width, r.size.height + radius);
        p = [NSBezierPath bezierPathWithRoundedRect:grown xRadius:radius yRadius:radius];
    }
    else
    {
        p = [NSBezierPath bezierPathWithRect:r];
    }

    [C_CARD set];
    [p fill];
    [C_BORDER set];
    p.lineWidth = 1;
    [p stroke];

    // hairline between rows inside the card
    if (!_isLast)
    {
        NSRect line = NSMakeRect(r.origin.x + 14, r.origin.y, r.size.width - 28, 1);
        [C_BORDER set];
        NSRectFill(line);
    }
}
- (void)drawSelectionInRect:(NSRect)r { (void)r; }   // no blue selection band
@end

// A pill button that draws itself; NSButton offers no padding or gap control.

@interface TTPill : NSButton
@property(nonatomic, strong) NSColor  *bgColor;
@property(nonatomic, strong) NSColor  *fgColor;
@property(nonatomic, copy)   NSString *symbolName;   // may be nil: title only
@property(nonatomic) CGFloat hPad;
@property(nonatomic) CGFloat gap;
@property(nonatomic) CGFloat radius;
@property(nonatomic) CGFloat fontSize;
@property(nonatomic) CGFloat height;
@property(nonatomic) BOOL centered;   // for a pill stretched wider than its contents
@end

@implementation TTPill

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
    {
        _hPad     = 11;
        _gap      = 6;
        _radius   = 8;
        _fontSize = 13;
        _height   = 30;
        _bgColor  = NSColor.clearColor;
        _fgColor  = NSColor.labelColor;

        self.bordered = NO;
        self.title = @"";   // NSButton starts out titled "Button"
    }
    return self;
}

// NO, or AppKit draws its stock button over everything -drawRect: paints.
- (BOOL)wantsUpdateLayer { return NO; }

// Glyph is one point smaller than the text.
- (NSImage *)glyph
{
    return _symbolName.length ? symbol(_symbolName, _fontSize - 1) : nil;
}

- (NSDictionary *)titleAttributes:(CGFloat)alpha
{
    return @{
        NSFontAttributeName : [NSFont systemFontOfSize:_fontSize weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName :
            [(_fgColor ?: NSColor.labelColor) colorWithAlphaComponent:alpha],
    };
}

- (NSSize)intrinsicContentSize
{
    NSImage *img = [self glyph];
    const NSSize title = self.title.length ? [self.title sizeWithAttributes:[self titleAttributes:1.0]]
                                           : NSZeroSize;

    CGFloat w = _hPad * 2 + img.size.width + title.width;
    if (img && self.title.length) { w += _gap; }
    return NSMakeSize(ceil(w), _height);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;

    const CGFloat alpha = self.enabled ? 1.0 : 0.45;
    const NSRect b = self.bounds;

    [[(_bgColor ?: NSColor.clearColor) colorWithAlphaComponent:alpha] set];
    [[NSBezierPath bezierPathWithRoundedRect:b xRadius:_radius yRadius:_radius] fill];

    NSImage *img = [self glyph];
    NSDictionary *attrs = [self titleAttributes:alpha];
    const NSSize title = self.title.length ? [self.title sizeWithAttributes:attrs] : NSZeroSize;

    CGFloat content = img.size.width + title.width;
    if (img && self.title.length) { content += _gap; }

    // [hPad][symbol][gap][title][hPad]. A titleless or centred pill centres its contents.
    CGFloat x = (self.title.length && !_centered) ? _hPad
                                                 : floor((b.size.width - content) / 2);

    if (img)
    {
        const NSRect r = NSMakeRect(x, floor((b.size.height - img.size.height) / 2),
                                    img.size.width, img.size.height);

        // A hand-drawn template image gets no tint from the cell; flood it here.
        // Source-atop needs the transparency layer or it repaints the filled pill.
        [NSGraphicsContext saveGraphicsState];
        CGContextRef cg = NSGraphicsContext.currentContext.CGContext;
        CGContextBeginTransparencyLayerWithRect(cg, NSRectToCGRect(r), NULL);
        [img drawInRect:r
               fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver
               fraction:alpha
         respectFlipped:YES
                  hints:nil];
        [(_fgColor ?: NSColor.labelColor) set];
        NSRectFillUsingOperation(r, NSCompositingOperationSourceAtop);
        CGContextEndTransparencyLayer(cg);
        [NSGraphicsContext restoreGraphicsState];

        x += img.size.width + _gap;
    }

    if (self.title.length)
    {
        [self.title drawAtPoint:NSMakePoint(x, floor((b.size.height - title.height) / 2))
                 withAttributes:attrs];
    }
}

// Width comes from the contents, so a new title changes the width.
- (void)setTitle:(NSString *)title
{
    [super setTitle:title];
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)setEnabled:(BOOL)enabled
{
    [super setEnabled:enabled];
    [self setNeedsDisplay:YES];   // we draw the disabled look ourselves
}

@end

// Transcript document view. Flipped, or turns start at the bottom.

@interface TTStack : NSStackView
@end

@implementation TTStack
- (BOOL)isFlipped { return YES; }
@end


@interface AppDelegate : NSObject <NSApplicationDelegate,
                                   NSOutlineViewDataSource,
                                   NSOutlineViewDelegate>
@end

@implementation AppDelegate
{
    // The only references in the app. Views get strings, never these.
    std::unique_ptr<tt::Tracker> _tracker;
    std::unique_ptr<tt::Agent>   _agent;
    // Same model, different prompt and tools. Shares the one Tracker.
    std::unique_ptr<tt::Agent>   _preflight;
    std::unique_ptr<tt::Voice>   _voice;

    NSWindow      *_window;
    NSOutlineView *_outline;
    TTPill        *_assistantBtn;
    TTPill        *_newMatter;
    NSTextField   *_title;
    NSTextField   *_count;
    NSTextField   *_status;
    NSTextField   *_empty;
    NSTimer       *_tick;

    // _panelEdge is what slides: 0 shows the panel, 340 parks it off the right.
    NSLayoutConstraint *_panelEdge;
    NSTextField        *_panelStatus;
    TTStack            *_transcript;
    TTPill             *_talkBtn;
    // Under ARC a local NSAlert dies when promptWithTitle: returns, before the sheet has run.
    NSAlert       *_sheet;

    // nil or empty means no filter.
    NSString      *_query;

    // Groups of {client, total, tasks[]}, formatted in -reload.
    NSArray<NSDictionary *> *_groups;

    BOOL _listening;
    BOOL _busy;
    BOOL _panelOpen;
    BOOL _preflightMode;   // which of the two agents the next turn goes to
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    // models/ and prompts/ are relative paths; chdir to Resources.
    [NSFileManager.defaultManager changeCurrentDirectoryPath:
        NSBundle.mainBundle.resourcePath];

    [self buildWindow];

    // Half a gigabyte of whisper and Kokoro; not on the thread that draws.
    _status.stringValue = @"Loading models...";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::string error;
        try
        {
            _tracker = std::make_unique<tt::Tracker>(database_path());
            _agent   = std::make_unique<tt::Agent>(*_tracker, tt::options_from_env());

            // Overrides TT_PROMPT/TT_TOOLS. Always the preflight agent.
            tt::AgentOptions pf = tt::options_from_env();
            pf.prompt_path = "prompts/preflight.md";
            pf.tools_path  = "prompts/preflight_tools.json";
            // compliance_path stays relative; data/ is copied in beside prompts/.
            _preflight = std::make_unique<tt::Agent>(*_tracker, pf);

            _voice   = std::make_unique<tt::Voice>(tt::VoiceOptions{});
        }
        catch (const std::exception &e) { error = e.what(); }

        dispatch_async(dispatch_get_main_queue(), ^{
            if (!error.empty())
            {
                _status.stringValue = str("Could not start: " + error);
                return;
            }
            _assistantBtn.enabled = YES;
            _status.stringValue = @"";
            [self reload];
        });
    });
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }


- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1024, 680)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskFullSizeContentView
                    backing:NSBackingStoreBuffered
                      defer:NO];
    // Pinned to Aqua: the palette is fixed hex, and dark mode hid the text on the system parts.
    _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    // Content runs under the traffic lights.
    _window.titlebarAppearsTransparent = YES;
    _window.titleVisibility = NSWindowTitleHidden;
    _window.movableByWindowBackground = YES;
    [_window center];

    NSView *root = fill(C_BG);
    _window.contentView = root;

    // The shut panel parks off the right edge and would still draw.
    root.layer.masksToBounds = YES;

    NSView *sidebar = [self buildSidebar];
    NSView *header  = [self buildHeader];
    NSView *list    = [self buildList];
    NSView *panel   = [self buildPanel];

    // Panel last, so it overlays the list.
    for (NSView *v in @[sidebar, header, list, panel]) { [root addSubview:v]; }

    _panelEdge = [panel.trailingAnchor constraintEqualToAnchor:root.trailingAnchor
                                                      constant:340];   // starts shut

    [NSLayoutConstraint activateConstraints:@[
        [sidebar.topAnchor      constraintEqualToAnchor:root.topAnchor],
        [sidebar.bottomAnchor   constraintEqualToAnchor:root.bottomAnchor],
        [sidebar.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor],
        [sidebar.widthAnchor    constraintEqualToConstant:68],

        [header.topAnchor       constraintEqualToAnchor:root.topAnchor],
        [header.leadingAnchor   constraintEqualToAnchor:sidebar.trailingAnchor],
        [header.trailingAnchor  constraintEqualToAnchor:root.trailingAnchor],
        [header.heightAnchor    constraintEqualToConstant:60],

        [list.topAnchor         constraintEqualToAnchor:header.bottomAnchor],
        [list.leadingAnchor     constraintEqualToAnchor:sidebar.trailingAnchor],
        [list.trailingAnchor    constraintEqualToAnchor:root.trailingAnchor],
        [list.bottomAnchor      constraintEqualToAnchor:root.bottomAnchor],

        [panel.topAnchor        constraintEqualToAnchor:root.topAnchor],
        [panel.bottomAnchor     constraintEqualToAnchor:root.bottomAnchor],
        [panel.widthAnchor      constraintEqualToConstant:340],
        _panelEdge,
    ]];

    [_window makeKeyAndOrderFront:nil];
}

- (NSView *)buildSidebar
{
    NSView *bar = fill(C_SIDEBAR);
    bar.translatesAutoresizingMaskIntoConstraints = NO;

    // Plain circle; no account system to draw from.
    NSView *avatar = fill(hex(0x5A646E));
    avatar.layer.cornerRadius = 19;
    avatar.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:avatar];

    NSArray *icons = @[@"timer", @"list.bullet", @"chart.bar", @"calendar"];
    NSMutableArray *buttons = [NSMutableArray array];
    for (NSUInteger i = 0; i < icons.count; ++i)
    {
        NSButton *b = [NSButton buttonWithImage:symbol(icons[i], 17) target:nil action:nil];
        b.bordered = NO;
        b.contentTintColor = (i == 0) ? C_BLUE : C_ICON;
        b.translatesAutoresizingMaskIntoConstraints = NO;
        if (i == 0)   // the active one gets the lighter rounded square
        {
            b.wantsLayer = YES;
            b.layer.backgroundColor = hex(0x2C2C2E).CGColor;
            b.layer.cornerRadius = 10;
        }
        [bar addSubview:b];
        [buttons addObject:b];
    }

    NSButton *gear = [NSButton buttonWithImage:symbol(@"gearshape", 17) target:nil action:nil];
    gear.bordered = NO;
    gear.contentTintColor = C_ICON;
    gear.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:gear];

    NSMutableArray *cs = [NSMutableArray arrayWithArray:@[
        // clear of the traffic lights
        [avatar.topAnchor      constraintEqualToAnchor:bar.topAnchor constant:48],
        [avatar.centerXAnchor  constraintEqualToAnchor:bar.centerXAnchor],
        [avatar.widthAnchor    constraintEqualToConstant:38],
        [avatar.heightAnchor   constraintEqualToConstant:38],

        [gear.centerXAnchor    constraintEqualToAnchor:bar.centerXAnchor],
        [gear.bottomAnchor     constraintEqualToAnchor:bar.bottomAnchor constant:-18],
        [gear.widthAnchor      constraintEqualToConstant:44],
        [gear.heightAnchor     constraintEqualToConstant:44],
    ]];
    NSView *above = avatar;
    for (NSButton *b in buttons)
    {
        [cs addObjectsFromArray:@[
            [b.topAnchor      constraintEqualToAnchor:above.bottomAnchor
                                             constant:(above == avatar ? 30 : 6)],
            [b.centerXAnchor  constraintEqualToAnchor:bar.centerXAnchor],
            [b.widthAnchor    constraintEqualToConstant:44],
            [b.heightAnchor   constraintEqualToConstant:44],
        ]];
        above = b;
    }
    [NSLayoutConstraint activateConstraints:cs];
    return bar;
}

- (NSView *)buildHeader
{
    NSView *bar = [[NSView alloc] init];
    bar.translatesAutoresizingMaskIntoConstraints = NO;

    _title = label(@"Time Tracker", 19, NSFontWeightBold, C_TEXT);

    _count = label(@"0 Matters", 12, NSFontWeightSemibold, hex(0x48484A));
    _count.wantsLayer = YES;
    _count.layer.backgroundColor = C_PILL.CGColor;
    _count.layer.cornerRadius = 9;

    _status = label(@"", 12, NSFontWeightRegular, C_MUTED);
    _status.lineBreakMode = NSLineBreakByTruncatingTail;

    NSSearchField *search = [[NSSearchField alloc] init];
    search.placeholderString = @"Search matters...";
    search.translatesAutoresizingMaskIntoConstraints = NO;
    search.bezelStyle = NSTextFieldRoundedBezel;
    search.target = self;
    search.action = @selector(search:);
    // Filter as the keys land, not on return.
    search.sendsSearchStringImmediately = YES;
    search.sendsWholeSearchString = NO;

    // Only shows and hides the panel.
    _assistantBtn = [self pill:@"AI Assistant" symbol:@"waveform" bg:C_BLUETINT fg:C_BLUE];
    _assistantBtn.target = self;
    _assistantBtn.action = @selector(toggleAssistant:);
    _assistantBtn.enabled = NO;   // until the models are up

    _newMatter = [self pill:@"New Matter" symbol:@"plus" bg:C_BLUE fg:NSColor.whiteColor];
    _newMatter.target = self;
    _newMatter.action = @selector(newMatter:);

    for (NSView *v in @[_title, _count, _status, search, _assistantBtn, _newMatter]) { [bar addSubview:v]; }

    [NSLayoutConstraint activateConstraints:@[
        [_title.leadingAnchor    constraintEqualToAnchor:bar.leadingAnchor constant:20],
        [_title.centerYAnchor    constraintEqualToAnchor:bar.centerYAnchor],

        [_count.leadingAnchor    constraintEqualToAnchor:_title.trailingAnchor constant:10],
        [_count.centerYAnchor    constraintEqualToAnchor:bar.centerYAnchor],
        [_count.heightAnchor     constraintEqualToConstant:18],

        [_status.leadingAnchor   constraintEqualToAnchor:_count.trailingAnchor constant:12],
        [_status.trailingAnchor  constraintLessThanOrEqualToAnchor:search.leadingAnchor constant:-12],
        [_status.centerYAnchor   constraintEqualToAnchor:bar.centerYAnchor],

        [search.trailingAnchor   constraintEqualToAnchor:_assistantBtn.leadingAnchor constant:-10],
        [search.centerYAnchor    constraintEqualToAnchor:bar.centerYAnchor],
        [search.widthAnchor      constraintEqualToConstant:210],

        [_assistantBtn.trailingAnchor constraintEqualToAnchor:_newMatter.leadingAnchor constant:-10],
        [_assistantBtn.centerYAnchor  constraintEqualToAnchor:bar.centerYAnchor],

        [_newMatter.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-20],
        [_newMatter.centerYAnchor  constraintEqualToAnchor:bar.centerYAnchor],
    ]];
    return bar;
}

// Width and height come from the pill's intrinsic size.
- (TTPill *)pill:(NSString *)text symbol:(NSString *)sym bg:(NSColor *)bg fg:(NSColor *)fg
{
    TTPill *b = [[TTPill alloc] init];
    b.title = text;
    b.symbolName = sym;
    b.bgColor = bg;
    b.fgColor = fg;
    b.translatesAutoresizingMaskIntoConstraints = NO;
    return b;
}

- (NSView *)buildList
{
    // Siblings: the empty-state text sits over the list, not scrolling.
    NSView *box = [[NSView alloc] init];
    box.translatesAutoresizingMaskIntoConstraints = NO;

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    // The clip view would otherwise paint over the page colour behind the rows.
    scroll.contentView.drawsBackground = NO;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    _outline = [[NSOutlineView alloc] init];
    _outline.dataSource = self;
    _outline.delegate = self;
    _outline.headerView = nil;
    _outline.backgroundColor = NSColor.clearColor;
    _outline.rowSizeStyle = NSTableViewRowSizeStyleCustom;
    _outline.intercellSpacing = NSMakeSize(0, 0);
    _outline.indentationPerLevel = 0;
    _outline.floatsGroupRows = NO;
    _outline.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;

    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"main"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_outline addTableColumn:col];
    _outline.outlineTableColumn = col;

    scroll.documentView = _outline;

    _empty = label(kNoMatters, 14, NSFontWeightRegular, C_MUTED);
    _empty.alignment = NSTextAlignmentCenter;
    // labelWithString: is single-line; this text wraps.
    _empty.maximumNumberOfLines = 0;
    _empty.lineBreakMode = NSLineBreakByWordWrapping;
    _empty.preferredMaxLayoutWidth = 320;
    // -reload decides this once the database is open.
    _empty.hidden = YES;

    [box addSubview:scroll];
    [box addSubview:_empty];

    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor       constraintEqualToAnchor:box.topAnchor],
        [scroll.bottomAnchor    constraintEqualToAnchor:box.bottomAnchor],
        [scroll.leadingAnchor   constraintEqualToAnchor:box.leadingAnchor],
        [scroll.trailingAnchor  constraintEqualToAnchor:box.trailingAnchor],

        [_empty.centerXAnchor   constraintEqualToAnchor:box.centerXAnchor],
        [_empty.centerYAnchor   constraintEqualToAnchor:box.centerYAnchor],
        [_empty.widthAnchor     constraintLessThanOrEqualToConstant:320],
    ]];
    return box;
}

// The assistant, as a drawer over the right of the window.
- (NSView *)buildPanel
{
    NSView *panel = fill(C_CARD);
    panel.translatesAutoresizingMaskIntoConstraints = NO;

    // A layer border runs all four sides; only this edge is wanted.
    NSView *edge = fill(C_BORDER);
    edge.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *head = [[NSView alloc] init];
    head.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *headLine = fill(C_BORDER);
    headLine.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *heading = label(@"Assistant", 15, NSFontWeightBold, C_TEXT);

    // Which agent hears the next turn. Small: the status word shares the 340.
    NSSegmentedControl *mode = [[NSSegmentedControl alloc] init];
    mode.segmentCount = 2;
    [mode setLabel:@"Time"  forSegment:0];
    [mode setLabel:@"Rules" forSegment:1];
    mode.segmentStyle = NSSegmentStyleRounded;
    mode.controlSize = NSControlSizeSmall;
    mode.selectedSegment = 0;
    mode.target = self;
    mode.action = @selector(switchMode:);
    mode.translatesAutoresizingMaskIntoConstraints = NO;

    // Idle, Listening, Thinking, Speaking.
    _panelStatus = label(@"Idle", 12, NSFontWeightSemibold, C_MUTED);
    _panelStatus.alignment = NSTextAlignmentRight;

    TTPill *close = [self pill:@"" symbol:@"xmark" bg:C_PILL fg:C_TEXT];
    close.fontSize = 12;   // no title, so this only picks the glyph's point size
    close.radius = 7;
    close.height = 26;
    close.target = self;
    close.action = @selector(toggleAssistant:);
    [close.widthAnchor constraintEqualToConstant:26].active = YES;   // square

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    // As in -buildList: the clip view would paint over the card colour.
    scroll.contentView.drawsBackground = NO;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    _transcript = [[TTStack alloc] init];
    _transcript.orientation = NSUserInterfaceLayoutOrientationVertical;
    _transcript.alignment = NSLayoutAttributeLeading;
    _transcript.spacing = 12;
    _transcript.edgeInsets = NSEdgeInsetsMake(14, 16, 14, 16);
    _transcript.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = _transcript;

    NSView *foot = [[NSView alloc] init];
    foot.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *footLine = fill(C_BORDER);
    footLine.translatesAutoresizingMaskIntoConstraints = NO;

    _talkBtn = [self pill:@"Start talking" symbol:@"mic.fill" bg:C_BLUE fg:NSColor.whiteColor];
    _talkBtn.fontSize = 14;
    _talkBtn.radius = 10;
    _talkBtn.height = 42;
    _talkBtn.centered = YES;
    _talkBtn.target = self;
    _talkBtn.action = @selector(talk:);
    _talkBtn.keyEquivalent = @" ";   // space bar
    // It spans the panel: the pins must beat the pill's own width.
    [_talkBtn setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationHorizontal];

    [head addSubview:heading];
    [head addSubview:mode];
    [head addSubview:_panelStatus];
    [head addSubview:close];
    [head addSubview:headLine];
    [foot addSubview:_talkBtn];
    [foot addSubview:footLine];
    for (NSView *v in @[head, scroll, foot, edge]) { [panel addSubview:v]; }

    [NSLayoutConstraint activateConstraints:@[
        [edge.topAnchor         constraintEqualToAnchor:panel.topAnchor],
        [edge.bottomAnchor      constraintEqualToAnchor:panel.bottomAnchor],
        [edge.leadingAnchor     constraintEqualToAnchor:panel.leadingAnchor],
        [edge.widthAnchor       constraintEqualToConstant:1],

        [head.topAnchor         constraintEqualToAnchor:panel.topAnchor],
        [head.leadingAnchor     constraintEqualToAnchor:panel.leadingAnchor],
        [head.trailingAnchor    constraintEqualToAnchor:panel.trailingAnchor],
        [head.heightAnchor      constraintEqualToConstant:60],

        [heading.leadingAnchor  constraintEqualToAnchor:head.leadingAnchor constant:16],
        [heading.centerYAnchor  constraintEqualToAnchor:head.centerYAnchor],

        [mode.leadingAnchor     constraintEqualToAnchor:heading.trailingAnchor constant:8],
        [mode.centerYAnchor     constraintEqualToAnchor:head.centerYAnchor],

        [_panelStatus.trailingAnchor constraintEqualToAnchor:close.leadingAnchor constant:-8],
        // The switch sits between the heading and the status word.
        [_panelStatus.leadingAnchor  constraintGreaterThanOrEqualToAnchor:mode.trailingAnchor
                                                                 constant:8],
        [_panelStatus.centerYAnchor  constraintEqualToAnchor:head.centerYAnchor],

        [close.trailingAnchor   constraintEqualToAnchor:head.trailingAnchor constant:-16],
        [close.centerYAnchor    constraintEqualToAnchor:head.centerYAnchor],

        [headLine.leadingAnchor  constraintEqualToAnchor:head.leadingAnchor],
        [headLine.trailingAnchor constraintEqualToAnchor:head.trailingAnchor],
        [headLine.bottomAnchor   constraintEqualToAnchor:head.bottomAnchor],
        [headLine.heightAnchor   constraintEqualToConstant:1],

        [scroll.topAnchor       constraintEqualToAnchor:head.bottomAnchor],
        [scroll.leadingAnchor   constraintEqualToAnchor:panel.leadingAnchor],
        [scroll.trailingAnchor  constraintEqualToAnchor:panel.trailingAnchor],
        [scroll.bottomAnchor    constraintEqualToAnchor:foot.topAnchor],

        // Matched in width so turns wrap instead of scrolling sideways.
        [_transcript.topAnchor     constraintEqualToAnchor:scroll.contentView.topAnchor],
        [_transcript.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor],
        [_transcript.widthAnchor   constraintEqualToAnchor:scroll.contentView.widthAnchor],

        [foot.leadingAnchor     constraintEqualToAnchor:panel.leadingAnchor],
        [foot.trailingAnchor    constraintEqualToAnchor:panel.trailingAnchor],
        [foot.bottomAnchor      constraintEqualToAnchor:panel.bottomAnchor],

        [footLine.leadingAnchor  constraintEqualToAnchor:foot.leadingAnchor],
        [footLine.trailingAnchor constraintEqualToAnchor:foot.trailingAnchor],
        [footLine.topAnchor      constraintEqualToAnchor:foot.topAnchor],
        [footLine.heightAnchor   constraintEqualToConstant:1],

        // 14 16 18, and the foot takes its height from this.
        [_talkBtn.leadingAnchor  constraintEqualToAnchor:foot.leadingAnchor constant:16],
        [_talkBtn.trailingAnchor constraintEqualToAnchor:foot.trailingAnchor constant:-16],
        [_talkBtn.topAnchor      constraintEqualToAnchor:foot.topAnchor constant:14],
        [_talkBtn.bottomAnchor   constraintEqualToAnchor:foot.bottomAnchor constant:-18],
    ]];
    return panel;
}


- (void)search:(NSSearchField *)sender
{
    _query = sender.stringValue;
    [self reload];
}

// An empty query matches everything.
- (BOOL)matches:(NSString *)text
{
    if (_query.length == 0) { return YES; }
    return [text rangeOfString:_query options:NSCaseInsensitiveSearch].location != NSNotFound;
}


- (void)reload
{
    if (!_tracker) { return; }

    // Remember what was open: every row is rebuilt, and the 1 s tick would snap them shut.
    NSMutableSet *openTasks = [NSMutableSet set];
    for (NSDictionary *g in _groups)
    {
        for (NSDictionary *t in g[@"tasks"])
        {
            if ([_outline isItemExpanded:t]) { [openTasks addObject:t[@"id"]]; }
        }
    }

    // listClients(), not find(): find() returns tasks, so a client with none never appears.
    NSMutableArray *groups = [NSMutableArray array];
    NSMutableDictionary *byClient = [NSMutableDictionary dictionary];
    BOOL anyRunning = NO;

    for (const tt::Client &c : _tracker->listClients())
    {
        NSString *client = str(c.name);
        // client_id for the group row's Add Task.
        NSMutableDictionary *g = [@{ @"client": client, @"client_id": @(c.id),
                                     @"secs": @0, @"tasks": [NSMutableArray array] } mutableCopy];
        byClient[client] = g;
        [groups addObject:g];
    }

    for (const tt::TaskInfo &t : _tracker->find())
    {
        NSString *client = str(t.client_name);
        NSString *name   = str(t.task.name);
        if (![self matches:client] && ![self matches:name]) { continue; }

        NSMutableDictionary *g = byClient[client];
        if (!g) { continue; }   // a client find() knows about and listClients() did not
        g[@"secs"] = @([g[@"secs"] longLongValue] + t.total_all_time.count());

        // find() leaves TaskInfo::intervals empty => N+1 queries. Cheap at a few dozen tasks.
        NSMutableArray *intervals = [NSMutableArray array];
        if (const auto info = _tracker->getTask(t.task.id))
        {
            for (const tt::Interval &iv : info->intervals)
            {
                // Adjustments have no span; nonsense in a list of sessions.
                if (iv.kind != tt::IntervalKind::Work) { continue; }
                const tt::Dur span = (iv.end ? *iv.end : _tracker->now()) - iv.start;
                [intervals addObject:@{
                    @"interval_id" : @(iv.id),
                    @"when"        : str(tt::format_local(iv.start)),
                    @"duration"    : str(tt::format_duration(span)),
                    @"running"     : @(!iv.end.has_value()),
                }];
            }
        }

        [g[@"tasks"] addObject:@{
            @"id"        : @(t.task.id),
            @"name"      : name,
            @"desc"      : str(t.task.description.value_or("")),
            @"duration"  : str(tt::format_duration(t.total_all_time)),
            @"running"   : @(t.running),
            @"intervals" : intervals,
        }];
        if (t.running) { anyRunning = YES; }
    }

    // Hidden only when neither the client nor a task matched.
    if (_query.length > 0)
    {
        NSMutableArray *kept = [NSMutableArray array];
        for (NSMutableDictionary *g in groups)
        {
            if ([self matches:g[@"client"]] || [g[@"tasks"] count] > 0)
            {
                [kept addObject:g];
            }
        }
        groups = kept;
    }

    for (NSMutableDictionary *g in groups)
    {
        g[@"total"] = str(tt::format_duration(tt::Dur{[g[@"secs"] longLongValue]}));
    }

    _groups = groups;
    _count.stringValue = [NSString stringWithFormat:@" %lu %@ ",
        (unsigned long)groups.count, groups.count == 1 ? @"Matter" : @"Matters"];
    [_outline reloadData];
    for (NSDictionary *g in groups)
    {
        [_outline expandItem:g];
        // Never opened for the user, only re-opened where they were.
        for (NSDictionary *t in g[@"tasks"])
        {
            if ([openTasks containsObject:t[@"id"]]) { [_outline expandItem:t]; }
        }
    }

    // Nothing found is different news from an empty database.
    _empty.stringValue = _query.length > 0
        ? [NSString stringWithFormat:@"Nothing matches \"%@\".", _query]
        : kNoMatters;
    _empty.hidden = (_groups.count > 0);

    // A running timer means the numbers change every second.
    if (anyRunning && !_tick)
    {
        _tick = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
                                                  block:^(NSTimer *t) { (void)t; [self reload]; }];
    }
    else if (!anyRunning && _tick)
    {
        [_tick invalidate];
        _tick = nil;
    }
}


- (void)toggleTimer:(NSButton *)sender
{
    const tt::Id task = (tt::Id)sender.tag;
    try
    {
        if (_tracker->getTask(task)->running) { _tracker->stopWork(task); }
        else                                  { _tracker->startWork(task); }
    }
    catch (const std::exception &e) { _status.stringValue = str(e.what()); }
    [self reload];
}

- (void)deleteTask:(NSButton *)sender
{
    try { _tracker->deleteTask((tt::Id)sender.tag); }
    catch (const std::exception &e) { _status.stringValue = str(e.what()); }
    [self reload];
}

// No confirmation: re-logging one entry is a sentence.
- (void)deleteEntry:(NSButton *)sender
{
    try { _tracker->deleteInterval((tt::Id)sender.tag); }
    catch (const std::exception &e) { _status.stringValue = str(e.what()); }
    [self reload];
}

// deleteClient cascades every task and interval under it.
- (void)deleteMatter:(NSButton *)sender
{
    // Read now: -reload rebuilds every row, sender may be gone by then.
    const tt::Id client = (tt::Id)sender.tag;
    NSString *name = @"";
    for (NSDictionary *g in _groups)
    {
        if ([g[@"client_id"] longLongValue] == (long long)client)
        {
            name = g[@"client"];
            break;
        }
    }

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:@"Delete %@?", name];
    alert.informativeText = @"This removes the matter and every task and entry under it.";
    [alert addButtonWithTitle:@"Delete"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.buttons.firstObject.hasDestructiveAction = YES;

    // Ivar for the same reason as in promptWithTitle:.
    _sheet = alert;
    [alert beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        self->_sheet = nil;
        if (response != NSAlertFirstButtonReturn) { return; }
        try { self->_tracker->deleteClient(client); }
        catch (const std::exception &e) { self->_status.stringValue = str(e.what()); }
        [self reload];
    }];
}


// One line of text as a sheet. `done` runs only on Create with text.
- (void)promptWithTitle:(NSString *)title
            placeholder:(NSString *)placeholder
             completion:(void (^)(NSString *value))done
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = title;
    [alert addButtonWithTitle:@"Create"];
    [alert addButtonWithTitle:@"Cancel"];

    NSTextField *input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
    input.placeholderString = placeholder;
    alert.accessoryView = input;
    // Otherwise Create takes focus and the first keystroke is lost.
    alert.window.initialFirstResponder = input;

    _sheet = alert;
    [alert beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        self->_sheet = nil;
        if (response != NSAlertFirstButtonReturn) { return; }
        NSString *value = [input.stringValue stringByTrimmingCharactersInSet:
            NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (value.length == 0) { return; }
        done(value);
    }];
}

- (void)newMatter:(id)sender
{
    (void)sender;
    if (!_tracker) { return; }   // still loading the models, and the database with them

    [self promptWithTitle:@"New matter" placeholder:@"Client name"
               completion:^(NSString *value) {
        // createClient throws Conflict on a duplicate name.
        try { _tracker->createClient([value UTF8String]); }
        catch (const std::exception &e) { _status.stringValue = str(e.what()); }
        [self reload];
    }];
}

- (void)addTask:(NSButton *)sender
{
    // Read now: -reload rebuilds every row, sender may be gone by then.
    const tt::Id client = (tt::Id)sender.tag;

    [self promptWithTitle:@"New task" placeholder:@"Task name"
               completion:^(NSString *value) {
        try { _tracker->createTask(client, [value UTF8String]); }
        catch (const std::exception &e) { _status.stringValue = str(e.what()); }
        [self reload];
    }];
}


- (void)toggleAssistant:(id)sender
{
    (void)sender;
    _panelOpen = !_panelOpen;

    // A live mic behind a shut panel cannot be stopped. Drop it.
    if (!_panelOpen && _listening)
    {
        _listening = NO;
        try { (void)_voice->stop_listening(); }
        catch (const std::exception &) { }   // nothing was going to be used anyway
        [self talkButton:@"Start talking" symbol:@"mic.fill" bg:C_BLUE];
        [self panelStatus:@"Idle" color:C_MUTED];
    }

    _assistantBtn.bgColor = _panelOpen ? C_BLUE : C_BLUETINT;
    _assistantBtn.fgColor = _panelOpen ? NSColor.whiteColor : C_BLUE;
    [_assistantBtn setNeedsDisplay:YES];

    NSView *root = _window.contentView;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.22;
        // A constraint has no animator; the layout pass is what moves.
        ctx.allowsImplicitAnimation = YES;
        self->_panelEdge.constant = self->_panelOpen ? 0 : 340;
        [root layoutSubtreeIfNeeded];
    }];
}

// Time or Rules. The transcript stays: one log, both conversations.
- (void)switchMode:(NSSegmentedControl *)sender
{
    // Mid-turn, the answer would land on the agent that never heard the question.
    if (_busy || _listening)
    {
        sender.selectedSegment = _preflightMode ? 1 : 0;
        return;
    }
    _preflightMode = (sender.selectedSegment == 1);
}

- (void)talkButton:(NSString *)title symbol:(NSString *)sym bg:(NSColor *)bg
{
    _talkBtn.title = title;
    _talkBtn.symbolName = sym;
    _talkBtn.bgColor = bg;
    [_talkBtn setNeedsDisplay:YES];
}

- (void)panelStatus:(NSString *)word color:(NSColor *)color
{
    _panelStatus.stringValue = word;
    _panelStatus.textColor = color;
}

// A fixed 340 panel => the wrap widths below are constants.
- (void)appendTurn:(NSString *)text from:(NSString *)who
{
    const BOOL you = [who isEqualToString:@"You"];

    NSView *box = [[NSView alloc] init];
    box.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *tag = label(who.uppercaseString, 11, NSFontWeightBold, C_MUTED);

    NSTextField *body = label(text, 13, NSFontWeightRegular, C_TEXT);
    body.lineBreakMode = NSLineBreakByWordWrapping;
    body.maximumNumberOfLines = 0;
    body.preferredMaxLayoutWidth = you ? 288 : 308;

    // What was said sits in a tinted box; the reply is bare text.
    NSView *bubble = nil;
    if (you)
    {
        bubble = fill(C_BG);
        bubble.layer.cornerRadius = 9;
        bubble.translatesAutoresizingMaskIntoConstraints = NO;
        [bubble addSubview:body];
    }
    NSView *block = bubble ?: body;

    [box addSubview:tag];
    [box addSubview:block];

    NSMutableArray *cs = [NSMutableArray arrayWithArray:@[
        // The stack aligns leading edges but does not stretch: full width less 32.
        [box.widthAnchor    constraintEqualToAnchor:_transcript.widthAnchor constant:-32],

        [tag.topAnchor      constraintEqualToAnchor:box.topAnchor],
        [tag.leadingAnchor  constraintEqualToAnchor:box.leadingAnchor],

        [block.topAnchor      constraintEqualToAnchor:tag.bottomAnchor constant:3],
        [block.leadingAnchor  constraintEqualToAnchor:box.leadingAnchor],
        [block.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
        [block.bottomAnchor   constraintEqualToAnchor:box.bottomAnchor],
    ]];
    if (bubble)
    {
        [cs addObjectsFromArray:@[
            [body.topAnchor      constraintEqualToAnchor:bubble.topAnchor constant:8],
            [body.bottomAnchor   constraintEqualToAnchor:bubble.bottomAnchor constant:-8],
            [body.leadingAnchor  constraintEqualToAnchor:bubble.leadingAnchor constant:10],
            [body.trailingAnchor constraintEqualToAnchor:bubble.trailingAnchor constant:-10],
        ]];
    }
    // Added first: a constraint across two views with no shared ancestor throws.
    [_transcript addArrangedSubview:box];
    [NSLayoutConstraint activateConstraints:cs];

    // No height until the layout pass has run.
    [_transcript layoutSubtreeIfNeeded];
    [_transcript scrollRectToVisible:NSMakeRect(0, NSMaxY(_transcript.bounds) - 1, 1, 1)];
}

// Cards carry the citation and the draft verbatim.
- (void)appendCard:(NSString *)title lines:(NSArray<NSString *> *)lines accent:(NSColor *)accent
{
    NSView *box = fill(C_BG);
    box.layer.cornerRadius = 9;
    box.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *head = label(title.uppercaseString, 11, NSFontWeightBold, accent);
    [box addSubview:head];

    NSMutableArray *cs = [NSMutableArray arrayWithArray:@[
        // As in -appendTurn:from:, the full width less the transcript's insets.
        [box.widthAnchor    constraintEqualToAnchor:_transcript.widthAnchor constant:-32],

        [head.topAnchor     constraintEqualToAnchor:box.topAnchor constant:10],
        [head.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:10],
    ]];

    NSView *above = head;
    for (NSString *line in lines)
    {
        NSTextField *body = label(line, 12, NSFontWeightRegular, C_TEXT);
        body.lineBreakMode = NSLineBreakByWordWrapping;
        // labelWithString: is one line; a draft body has newlines.
        body.maximumNumberOfLines = 0;
        body.preferredMaxLayoutWidth = 288;   // 340 panel, 16 of inset and 10 of padding each side
        [box addSubview:body];

        [cs addObjectsFromArray:@[
            [body.topAnchor      constraintEqualToAnchor:above.bottomAnchor constant:4],
            [body.leadingAnchor  constraintEqualToAnchor:box.leadingAnchor constant:10],
            [body.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-10],
        ]];
        above = body;
    }
    [cs addObject:[above.bottomAnchor constraintEqualToAnchor:box.bottomAnchor constant:-10]];

    // Added first: a constraint across two views with no shared ancestor throws.
    [_transcript addArrangedSubview:box];
    [NSLayoutConstraint activateConstraints:cs];

    [_transcript layoutSubtreeIfNeeded];
    [_transcript scrollRectToVisible:NSMakeRect(0, NSMaxY(_transcript.bounds) - 1, 1, 1)];
}

// Only these two tools get a card; the rest is left to the prose.
- (void)appendCards:(const nlohmann::json &)results
{
    if (!results.is_array()) { return; }

    for (const nlohmann::json &entry : results)
    {
        if (!entry.is_object()) { continue; }
        NSString *tool = text(entry, "tool");

        const auto found = entry.find("result");
        if (found == entry.end() || !found->is_object()) { continue; }
        const nlohmann::json &result = *found;

        if      ([tool isEqualToString:@"check_rules"])    { [self ruleCard:result];  }
        else if ([tool isEqualToString:@"draft_approval"]) { [self draftCard:result]; }
    }
}

- (void)ruleCard:(const nlohmann::json &)result
{
    // A failed tool comes back as {"error": ...}; an empty card would read as a finding.
    NSString *finding = text(result, "finding");
    if (!finding) { return; }

    NSColor *accent = C_MUTED;   // needs_information, and anything we cannot read
    if ([finding isEqualToString:@"satisfied"] ||
        [finding isEqualToString:@"no_restriction_found"])   { accent = hex(0x34C759); }
    else if ([finding isEqualToString:@"action_required"])   { accent = hex(0xFF9500); }

    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSString *summary = text(result, "summary");
    if (summary.length) { [lines addObject:summary]; }

    const auto cite = result.find("citation");
    if (cite != result.end() && cite->is_object())
    {
        const auto page = cite->find("page");
        [lines addObject:[NSString stringWithFormat:@"%@ — %@, p.%d",
            text(*cite, "section") ?: @"", text(*cite, "document") ?: @"",
            (page != cite->end() && page->is_number_integer()) ? page->get<int>() : 0]];

        NSString *excerpt = text(*cite, "excerpt");
        if (excerpt.length) { [lines addObject:[NSString stringWithFormat:@"“%@”", excerpt]]; }
    }

    // Example: "pending since 6 Aug"
    NSString *onFile = text(result, "approval_on_file");
    if (onFile.length) { [lines addObject:onFile]; }

    // to_string(Finding) is snake_case; a card title is not.
    [self appendCard:[finding stringByReplacingOccurrencesOfString:@"_" withString:@" "]
               lines:lines
              accent:accent];
}

- (void)draftCard:(const nlohmann::json &)result
{
    NSString *to = text(result, "to");
    if (!to) { return; }   // an error result, as in -ruleCard:

    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    [lines addObject:[NSString stringWithFormat:@"To: %@ <%@>", to, text(result, "email") ?: @""]];
    [lines addObject:[NSString stringWithFormat:@"Subject: %@", text(result, "subject") ?: @""]];

    NSString *body = text(result, "body");
    if (body.length) { [lines addObject:body]; }

    // Nothing here sends mail; only the title says so.
    [self appendCard:@"Draft — not sent" lines:lines accent:C_BLUE];
}


- (void)talk:(id)sender
{
    (void)sender;
    if (_busy) { return; }        // a turn is still running
    // The space bar still reaches this button with the panel parked off-screen.
    if (!_panelOpen) { return; }

    if (!_listening)
    {
        try { _voice->start_listening(); }
        catch (const std::exception &e)
        {
            // This is where a refused microphone shows up.
            _status.stringValue = str(e.what());
            return;
        }
        _listening = YES;
        [self talkButton:@"Stop" symbol:@"stop.fill" bg:hex(0xFF3B30)];
        [self panelStatus:@"Listening" color:hex(0xFF3B30)];
        return;
    }

    _listening = NO;
    [self talkButton:@"Start talking" symbol:@"mic.fill" bg:C_BLUE];

    std::string heard;
    try { heard = _voice->stop_listening(); }
    catch (const std::exception &e)
    {
        _status.stringValue = str(e.what());
        [self panelStatus:@"Idle" color:C_MUTED];
        return;
    }

    if (heard.empty())
    {
        // Nothing was said, so nothing goes in the transcript.
        [self panelStatus:@"Idle" color:C_MUTED];
        _status.stringValue = @"Didn't catch that.";
        return;
    }

    [self appendTurn:str(heard) from:@"You"];
    [self panelStatus:@"Thinking" color:hex(0xFF9500)];
    _busy = YES;
    _talkBtn.enabled = NO;

    // Read on the main thread: a mid-turn switch cannot redirect it.
    const BOOL preflight = _preflightMode;

    // say() and speak() take seconds; neither runs on the thread that draws.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::string reply;
        BOOL failed = NO;
        // Copied: last_results() is the agent's own array, read on the main thread later.
        nlohmann::json results = nlohmann::json::array();
        try
        {
            tt::Agent &agent = preflight ? *_preflight : *_agent;
            reply   = agent.say(heard);
            results = agent.last_results();
        }
        catch (const std::exception &e)
        {
            reply = std::string("Error: ") + e.what();
            failed = YES;
        }

        // Sync, so the reply is on screen before speak() reads it out.
        // The main thread never waits on this queue, so no deadlock.
        dispatch_sync(dispatch_get_main_queue(), ^{
            [self appendTurn:str(reply) from:@"Assistant"];
            [self appendCards:results];   // under the prose, in the order they were called
            if (!failed) { [self panelStatus:@"Speaking" color:hex(0x34C759)]; }
        });

        if (!failed)
        {
            try { _voice->speak(reply); }
            catch (const std::exception &) { }   // the reply is already on screen
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [self panelStatus:@"Idle" color:C_MUTED];
            _busy = NO;
            _talkBtn.enabled = YES;
            [self reload];      // the agent may have written something
        });
    });
}


// Three levels, told apart by the dictionary key: @"tasks", @"intervals", @"interval_id".
- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item
{
    (void)ov;
    if (!item) { return (NSInteger)_groups.count; }
    if (item[@"tasks"]) { return (NSInteger)[item[@"tasks"] count]; }
    return (NSInteger)[item[@"intervals"] count];
}

- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)i ofItem:(id)item
{
    (void)ov;
    if (!item) { return _groups[(NSUInteger)i]; }
    if (item[@"tasks"]) { return item[@"tasks"][(NSUInteger)i]; }
    return item[@"intervals"][(NSUInteger)i];
}

- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item
{
    (void)ov;
    // Otherwise a disclosure triangle that opens onto nothing.
    return [item[@"tasks"] count] > 0 || [item[@"intervals"] count] > 0;
}

- (CGFloat)outlineView:(NSOutlineView *)ov heightOfRowByItem:(id)item
{
    (void)ov;
    // 54: 18 of air, 4 of padding, 22 of content, 10 of padding.
    if (item[@"tasks"]) { return 54; }
    return item[@"interval_id"] ? 34 : 56;   // entry rows, then task rows
}

- (NSTableRowView *)outlineView:(NSOutlineView *)ov rowViewForItem:(id)item
{
    (void)ov;
    TTRowView *row = [[TTRowView alloc] init];
    // Entry rows are part of their task's card.
    row.isTask = (item[@"tasks"] == nil);
    if (row.isTask) { [self setCorners:row forItem:item]; }
    return row;
}

// Which end of the card a row is, counted over what is on screen.
- (void)setCorners:(TTRowView *)row forItem:(id)item
{
    for (NSDictionary *g in _groups)
    {
        NSMutableArray *flat = [NSMutableArray array];
        for (NSDictionary *t in g[@"tasks"])
        {
            [flat addObject:t];
            if ([_outline isItemExpanded:t])
            {
                [flat addObjectsFromArray:t[@"intervals"]];
            }
        }

        const NSUInteger at = [flat indexOfObjectIdenticalTo:item];
        if (at != NSNotFound)
        {
            row.isFirst = (at == 0);
            row.isLast  = (at == flat.count - 1);
            return;
        }
    }
}

// NSOutlineView does not rebuild rows already on screen; they keep their old corner flags.
- (void)outlineViewItemDidExpand:(NSNotification *)note
{
    (void)note;
    [self recorner];
}

- (void)outlineViewItemDidCollapse:(NSNotification *)note
{
    (void)note;
    [self recorner];
}

- (void)recorner
{
    [_outline enumerateAvailableRowViewsUsingBlock:^(NSTableRowView *rv, NSInteger i) {
        TTRowView *row = (TTRowView *)rv;
        if (!row.isTask) { return; }
        [self setCorners:row forItem:[self->_outline itemAtRow:i]];
        [row setNeedsDisplay:YES];
    }];
}

- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item
{
    (void)ov; (void)col;
    if (item[@"tasks"]) { return [self groupRow:item]; }
    return item[@"interval_id"] ? [self intervalRow:item] : [self taskRow:item];
}

- (NSView *)groupRow:(NSDictionary *)g
{
    NSView *v = [[NSView alloc] init];

    NSTextField *name  = label(g[@"client"], 16, NSFontWeightBold, C_TEXT);
    NSTextField *total = label(g[@"total"], 14, NSFontWeightSemibold, C_TEXT);
    // A ticking total must not jitter: fixed-width digits.
    total.font = [NSFont monospacedDigitSystemFontOfSize:14 weight:NSFontWeightSemibold];

    TTPill *add = [self pill:@"Add Task" symbol:@"plus" bg:C_PILL fg:hex(0x48484A)];
    add.fontSize = 12;
    add.radius = 7;
    add.hPad = 9;
    add.gap = 4;
    add.height = 22;
    add.target = self;
    add.action = @selector(addTask:);
    add.tag = (NSInteger)[g[@"client_id"] longLongValue];

    // A matter with no tasks has no other way off the list.
    TTPill *bin = [self iconButton:@"trash"
                            action:@selector(deleteMatter:)
                               tag:(NSInteger)[g[@"client_id"] longLongValue]];

    for (NSView *s in @[name, add, total, bin]) { [v addSubview:s]; }
    [NSLayoutConstraint activateConstraints:@[
        // The triangle ends at 25, the cell starts at 16 (NSTableViewStyleInset): 18 is a 9pt gap.
        [name.leadingAnchor  constraintEqualToAnchor:v.leadingAnchor constant:18],
        [name.bottomAnchor   constraintEqualToAnchor:v.bottomAnchor constant:-10],

        [add.leadingAnchor   constraintEqualToAnchor:name.trailingAnchor constant:9],
        [add.centerYAnchor   constraintEqualToAnchor:name.centerYAnchor],

        // The bin takes the row's -24 edge; the total steps left, 14 of air.
        [bin.trailingAnchor   constraintEqualToAnchor:v.trailingAnchor constant:-24],
        [bin.centerYAnchor    constraintEqualToAnchor:name.centerYAnchor],

        [total.trailingAnchor constraintEqualToAnchor:bin.leadingAnchor constant:-14],
        [total.centerYAnchor  constraintEqualToAnchor:name.centerYAnchor],
    ]];
    return v;
}

- (NSView *)taskRow:(NSDictionary *)t
{
    NSView *v = [[NSView alloc] init];
    const BOOL running = [t[@"running"] boolValue];

    NSTextField *name = label(t[@"name"], 14, NSFontWeightSemibold, C_TEXT);
    NSTextField *time = label(t[@"duration"], 14, NSFontWeightSemibold,
                              running ? C_BLUE : C_TEXT);
    // Redrawn every second: fixed-width digits.
    time.font = [NSFont monospacedDigitSystemFontOfSize:14 weight:NSFontWeightSemibold];

    // Add Task collects only a name; most tasks have no second line.
    NSString *descText = t[@"desc"];
    NSTextField *desc = descText.length
        ? label(descText, 12.5, NSFontWeightRegular, C_MUTED) : nil;

    TTPill *play = [self iconButton:(running ? @"stop.fill" : @"play.fill")
                             action:@selector(toggleTimer:)
                                tag:[t[@"id"] longLongValue]];
    TTPill *bin  = [self iconButton:@"trash"
                             action:@selector(deleteTask:)
                                tag:[t[@"id"] longLongValue]];
    if (running)
    {
        play.bgColor = C_BLUE;
        play.fgColor = NSColor.whiteColor;
    }

    for (NSView *s in @[name, time, play, bin]) { [v addSubview:s]; }
    if (desc) { [v addSubview:desc]; }

    NSMutableArray *cs = [NSMutableArray arrayWithArray:@[
        [name.leadingAnchor  constraintEqualToAnchor:v.leadingAnchor constant:34],

        [bin.trailingAnchor  constraintEqualToAnchor:v.trailingAnchor constant:-34],
        [bin.centerYAnchor   constraintEqualToAnchor:v.centerYAnchor],

        [play.trailingAnchor constraintEqualToAnchor:bin.leadingAnchor constant:-8],
        [play.centerYAnchor  constraintEqualToAnchor:v.centerYAnchor],

        [time.trailingAnchor constraintEqualToAnchor:play.leadingAnchor constant:-14],
        [time.centerYAnchor  constraintEqualToAnchor:v.centerYAnchor],
    ]];

    // Centre the pair, not the name. With no description the name centres alone.
    if (desc)
    {
        [cs addObjectsFromArray:@[
            [name.bottomAnchor  constraintEqualToAnchor:v.centerYAnchor],
            [desc.leadingAnchor constraintEqualToAnchor:name.leadingAnchor],
            [desc.topAnchor     constraintEqualToAnchor:name.bottomAnchor constant:2],
        ]];
    }
    else
    {
        [cs addObject:[name.centerYAnchor constraintEqualToAnchor:v.centerYAnchor]];
    }

    [NSLayoutConstraint activateConstraints:cs];
    return v;
}

// One logged session, droppable when the model picked the wrong task.
- (NSView *)intervalRow:(NSDictionary *)iv
{
    NSView *v = [[NSView alloc] init];
    const BOOL running = [iv[@"running"] boolValue];

    NSTextField *when = label(iv[@"when"], 12, NSFontWeightRegular, C_MUTED);
    NSTextField *time = label(iv[@"duration"], 12, NSFontWeightRegular,
                              running ? C_BLUE : C_MUTED);
    time.alignment = NSTextAlignmentRight;
    // Redrawn every second: fixed-width digits.
    time.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];

    for (NSView *s in @[when, time]) { [v addSubview:s]; }

    NSMutableArray *cs = [NSMutableArray arrayWithArray:@[
        // A step in from the task name at 34, not another task.
        [when.leadingAnchor  constraintEqualToAnchor:v.leadingAnchor constant:52],
        [when.centerYAnchor  constraintEqualToAnchor:v.centerYAnchor],

        // Clear of the task row's two pills, which end at -34.
        [time.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-68],
        [time.centerYAnchor  constraintEqualToAnchor:v.centerYAnchor],
    ]];

    // A running entry has no end yet; stop the timer first.
    if (!running)
    {
        TTPill *bin = [self iconButton:@"trash"
                                action:@selector(deleteEntry:)
                                   tag:(NSInteger)[iv[@"interval_id"] longLongValue]];
        [v addSubview:bin];
        [cs addObjectsFromArray:@[
            [bin.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-34],
            [bin.centerYAnchor  constraintEqualToAnchor:v.centerYAnchor],
        ]];
    }

    [NSLayoutConstraint activateConstraints:cs];
    return v;
}

- (TTPill *)iconButton:(NSString *)sym action:(SEL)action tag:(NSInteger)tag
{
    TTPill *b = [self pill:@"" symbol:sym bg:C_PILL fg:hex(0x48484A)];
    b.fontSize = 12;   // no title, so this only picks the glyph's point size
    b.radius = 7;
    b.height = 28;
    b.target = self;
    b.action = action;
    b.tag = tag;
    // A square: the glyph centres itself.
    [b.widthAnchor  constraintEqualToConstant:28].active = YES;
    [b.heightAnchor constraintEqualToConstant:28].active = YES;
    return b;
}

@end


int main(void)
{
    @autoreleasepool
    {
        NSApplication *app = NSApplication.sharedApplication;
        // Regular, not accessory: Dock icon and menu bar.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;

        [app run];
    }
    return 0;
}
