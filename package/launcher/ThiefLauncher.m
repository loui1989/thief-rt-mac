// Thief RT launcher: the window "Thief RT.app" opens -- which game (Thief Gold or
// Thief 2), the original campaign or a fan mission from that game's FMs folder,
// the renderer (ray traced or OpenGL), the ray tracing quality preset or the
// OpenGL renderer's options (resolution, real-time lights, shadows, bump maps,
// v-sync: gl_settings.txt, the same as its panel in the game). Play
// starts it through launch.sh (in this app's Resources), which this process
// becomes (exec).
//
// Everything Thief RT keeps is in ~/Library/Application Support/Thief RT:
//   games.conf     where the games' files are (TG_DIR=..., T2_DIR=...: the installer's)
//   launcher.conf  the choices last made here
//   renderer.txt   rt or gl (the game's own Options > Video changes it too)
// and the engine's own settings in ~/Library/Application Support/Dark Engine Restored
// (rt_settings.txt: quality_preset, the preset the game's settings are).
//
//   clang -fobjc-arc -O2 -mmacosx-version-min=13.0 ThiefLauncher.m -framework AppKit -o "Thief RT"

#import <AppKit/AppKit.h>
#include <unistd.h>

static NSString *const kTitle = @"Thief RT";

static NSString *SupportDir(void)
{
   return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Thief RT"];
}
static NSString *EngineSettingsDir(void)
{
   return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Dark Engine Restored"];
}
static NSString *RtSettingsPath(void) { return [EngineSettingsDir() stringByAppendingPathComponent:@"rt_settings.txt"]; }

// ---- KEY=value files ------------------------------------------------------------

static NSMutableDictionary<NSString *, NSString *> *ReadConf(NSString *path)
{
   NSMutableDictionary *d = [NSMutableDictionary dictionary];
   NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
   for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
      NSRange eq = [line rangeOfString:@"="];
      if (eq.location == NSNotFound || [line hasPrefix:@"#"])
         continue;
      NSString *k = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
      NSString *v = [line substringFromIndex:eq.location + 1];
      if (k.length)
         d[k] = v;
   }
   return d;
}

static void WriteConf(NSDictionary<NSString *, NSString *> *d, NSString *path)
{
   NSMutableString *s = [NSMutableString string];
   for (NSString *k in [d.allKeys sortedArrayUsingSelector:@selector(compare:)])
      [s appendFormat:@"%@=%@\n", k, d[k]];
   [[NSFileManager defaultManager] createDirectoryAtPath:path.stringByDeletingLastPathComponent
                             withIntermediateDirectories:YES attributes:nil error:nil];
   [s writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

// rt_settings.txt: "key = value" lines
static NSString *RtSetting(NSString *key)
{
   NSString *text = [NSString stringWithContentsOfFile:RtSettingsPath() encoding:NSUTF8StringEncoding error:nil];
   for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
      NSArray *kv = [line componentsSeparatedByString:@"="];
      if (kv.count == 2 &&
          [[kv[0] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] isEqualToString:key])
         return [kv[1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
   }
   return nil;
}

static void SetRtSetting(NSString *key, NSString *value)
{
   NSString *text = [NSString stringWithContentsOfFile:RtSettingsPath() encoding:NSUTF8StringEncoding error:nil] ?: @"";
   NSMutableArray *out = [NSMutableArray array];
   BOOL done = NO;
   for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
      if (!line.length)
         continue;
      NSArray *kv = [line componentsSeparatedByString:@"="];
      if (kv.count == 2 &&
          [[kv[0] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] isEqualToString:key]) {
         [out addObject:[NSString stringWithFormat:@"%@ = %@", key, value]];
         done = YES;
      } else {
         [out addObject:line];
      }
   }
   if (!done)
      [out addObject:[NSString stringWithFormat:@"%@ = %@", key, value]];
   [[NSFileManager defaultManager] createDirectoryAtPath:EngineSettingsDir()
                             withIntermediateDirectories:YES attributes:nil error:nil];
   [[[out componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"]
      writeToFile:RtSettingsPath() atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

// gl_settings.txt (the OpenGL renderer's): "key value" lines
static NSString *GlSettingsPath(void) { return [EngineSettingsDir() stringByAppendingPathComponent:@"gl_settings.txt"]; }
static NSString *GlSetting(NSString *key)
{
   NSString *text = [NSString stringWithContentsOfFile:GlSettingsPath() encoding:NSUTF8StringEncoding error:nil];
   for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
      NSArray *kv = [line componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
      if (kv.count >= 2 && [kv[0] isEqualToString:key])
         return kv[1];
   }
   return nil;
}
static void SetGlSetting(NSString *key, NSString *value)
{
   NSString *text = [NSString stringWithContentsOfFile:GlSettingsPath() encoding:NSUTF8StringEncoding error:nil] ?: @"";
   NSMutableArray *out = [NSMutableArray array];
   BOOL done = NO;
   for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
      if (!line.length)
         continue;
      NSArray *kv = [line componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
      if (kv.count >= 1 && [kv[0] isEqualToString:key]) {
         [out addObject:[NSString stringWithFormat:@"%@ %@", key, value]];
         done = YES;
      } else {
         [out addObject:line];
      }
   }
   if (!done)
      [out addObject:[NSString stringWithFormat:@"%@ %@", key, value]];
   [[NSFileManager defaultManager] createDirectoryAtPath:EngineSettingsDir()
                             withIntermediateDirectories:YES attributes:nil error:nil];
   [[[out componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"]
      writeToFile:GlSettingsPath() atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

// ---- the games' files ----------------------------------------------------------

// the names in a folder, lower case
static NSSet<NSString *> *LowerNames(NSString *dir)
{
   NSMutableSet *s = [NSMutableSet set];
   for (NSString *n in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
      [s addObject:n.lowercaseString];
   return s;
}

// "tg" (Thief: The Dark Project or Thief Gold), "t2" (Thief II), or nil: no game there
static NSString *GameKind(NSString *dir)
{
   NSSet *n = LowerNames(dir);
   if (![n containsObject:@"cam.cfg"])
      return nil;
   if ([n containsObject:@"thief2.exe"] || [n containsObject:@"t2.exe"])
      return @"t2";
   if ([n containsObject:@"thief.exe"])
      return @"tg";
   // by the missions: Thief II has a miss8 (and no miss3); Thief has a miss3
   if ([n containsObject:@"miss8.mis"] && ![n containsObject:@"miss3.mis"])
      return @"t2";
   if ([n containsObject:@"miss3.mis"] || [n containsObject:@"miss18.mis"] || [n containsObject:@"miss1.mis"])
      return @"tg";
   return nil;
}

@interface FanMission : NSObject
@property(copy) NSString *folder;   // its folder's name in FMs/ (what fm= takes)
@property(copy) NSString *title;    // as shown
@property BOOL blackParade;
@end
@implementation FanMission
@end

// a file in a folder by its name in any case
static NSString *FileInAnyCase(NSString *dir, NSString *name)
{
   for (NSString *n in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
      if ([n caseInsensitiveCompare:name] == NSOrderedSame)
         return [dir stringByAppendingPathComponent:n];
   return nil;
}

// the fan missions in a game's FMs folder: a folder with a mission (.mis) in it
static NSArray<FanMission *> *FanMissions(NSString *gameDir)
{
   NSMutableArray *fms = [NSMutableArray array];
   if (!gameDir.length)
      return fms;
   NSString *root = [gameDir stringByAppendingPathComponent:@"FMs"];
   for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:root error:nil]) {
      if ([f hasPrefix:@"."])
         continue;
      NSString *dir = [root stringByAppendingPathComponent:f];
      BOOL isDir = NO;
      if (![[NSFileManager defaultManager] fileExistsAtPath:dir isDirectory:&isDir] || !isDir)
         continue;
      // (not the engine's rules for a safe FM name: no slashes, no "..")
      if ([f containsString:@".."] || [f containsString:@"\\"])
         continue;
      NSSet *names = LowerNames(dir);
      BOOL hasMission = NO;
      for (NSString *n in names)
         if ([n hasSuffix:@".mis"]) { hasMission = YES; break; }
      if (!hasMission)
         continue;
      FanMission *fm = [FanMission new];
      fm.folder = f;
      fm.title = f;
      // its name as its fm.ini gives it (NiceName=)
      NSString *ini = FileInAnyCase(dir, @"fm.ini");
      NSString *text = ini ? [NSString stringWithContentsOfFile:ini encoding:NSUTF8StringEncoding error:nil] : nil;
      if (!text && ini)
         text = [NSString stringWithContentsOfFile:ini encoding:NSWindowsCP1252StringEncoding error:nil];
      for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
         if ([line.lowercaseString hasPrefix:@"nicename="]) {
            NSString *nice = [[line substringFromIndex:9]
               stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if (nice.length)
               fm.title = nice;
         }
      }
      fm.blackParade = [names containsObject:@"tbpgam1.gam"] ||
                       [[f.lowercaseString stringByReplacingOccurrencesOfString:@" " withString:@""]
                          containsString:@"blackparade"];
      if (fm.blackParade && [fm.title isEqualToString:f])
         fm.title = @"The Black Parade";
      [fms addObject:fm];
   }
   [fms sortUsingComparator:^NSComparisonResult(FanMission *a, FanMission *b) {
      if (a.blackParade != b.blackParade)
         return a.blackParade ? NSOrderedAscending : NSOrderedDescending;
      return [a.title localizedCaseInsensitiveCompare:b.title];
   }];
   return fms;
}

// ---- the window ------------------------------------------------------------------

static NSArray<NSString *> *PresetNames(void) { return @[@"Ultra", @"High", @"Medium", @"Low"]; }
static NSString *PresetNote(NSString *p)
{
   NSDictionary *notes = @{
      @"Ultra": @"The screen's full resolution (MetalFX Balanced), two bounces of light, the most lights at once. The slowest.",
      @"High": @"1800p (MetalFX Performance), bounce light, god rays from every lamp. Recommended.",
      @"Medium": @"1440p (MetalFX Performance), one bounce of light, god rays from the moon only.",
      @"Low": @"1080p (MetalFX Performance), one bounce, no god rays, flat smoke, no mirror reflections. The fastest.",
      @"Custom": @"Your own settings, as set in the game (Cmd+, or F10).",
   };
   NSString *n = notes[p] ?: @"";
   if (![p isEqualToString:@"Custom"])
      n = [n stringByAppendingString:@" Dynamic resolution holds 60 fps."];
   return n;
}

@interface Launcher : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) NSPopUpButton *gamePopup, *missionPopup, *qualityPopup;
@property(strong) NSSegmentedControl *renderer;
@property(strong) NSTextField *qualityNote, *gameNote;
@property(strong) NSButton *playButton;
@property(strong) NSMutableDictionary<NSString *, NSString *> *games, *choices;
@property(strong) NSArray<FanMission *> *missions;
@property(strong) NSPanel *devPanel;
@property(strong) NSGridView *grid;
@property(strong) NSPopUpButton *glScalePopup, *glLightsPopup, *glVsyncPopup;
@property(strong) NSButton *glShadowsCheck, *glBumpCheck;
@property(strong) NSButton *devAsk, *fpsCheck;
@end

@implementation Launcher

- (NSString *)gamesPath { return [SupportDir() stringByAppendingPathComponent:@"games.conf"]; }
- (NSString *)choicesPath { return [SupportDir() stringByAppendingPathComponent:@"launcher.conf"]; }

- (NSString *)dirFor:(NSString *)gid
{
   NSString *d = self.games[[gid isEqualToString:@"t2"] ? @"T2_DIR" : @"TG_DIR"];
   BOOL isDir = NO;
   return d.length && [[NSFileManager defaultManager] fileExistsAtPath:d isDirectory:&isDir] && isDir ? d : nil;
}

- (NSString *)selectedGame { return self.gamePopup.selectedItem.representedObject; }

static NSTextField *Label(NSString *s)
{
   NSTextField *t = [NSTextField labelWithString:s];
   t.alignment = NSTextAlignmentRight;
   return t;
}
static NSTextField *Note(void)
{
   NSTextField *t = [NSTextField wrappingLabelWithString:@""];
   t.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
   t.textColor = NSColor.secondaryLabelColor;
   t.preferredMaxLayoutWidth = 330;
   return t;
}

// a pop-up of choices, each a title and the value written for it; the one
// nearest the value now chosen
static NSPopUpButton *Choices(NSArray<NSArray<NSString *> *> *items, NSString *now, NSString *fallback)
{
   NSPopUpButton *p = [NSPopUpButton new];
   for (NSArray *it in items) {
      [p addItemWithTitle:it[0]];
      p.lastItem.representedObject = it[1];
   }
   const double v = (now ?: fallback).doubleValue;
   NSInteger best = 0;
   for (NSInteger i = 0; i < (NSInteger)items.count; ++i)
      if (fabs([items[i][1] doubleValue] - v) < fabs([items[best][1] doubleValue] - v))
         best = i;
   [p selectItemAtIndex:best];
   return p;
}

// the OpenGL renderer's grid rows (shown with OpenGL chosen) and the ray tracer's (Quality)
enum { kRowQuality = 4, kRowGlFirst = 6, kRowGlLast = 9 };

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
   [self buildMenu];
   self.games = ReadConf(self.gamesPath);
   self.choices = ReadConf(self.choicesPath);

   NSString *engine = [SupportDir() stringByAppendingPathComponent:@"Engine/Thief RT.app/Contents/MacOS/thief-runner"];
   if (![[NSFileManager defaultManager] isExecutableFileAtPath:engine]) {
      NSAlert *a = [NSAlert new];
      a.messageText = @"Thief RT is not installed completely";
      a.informativeText = @"Open the Thief RT disk image and run \"Install Thief RT\" again.";
      [a runModal];
      [NSApp terminate:nil];
      return;
   }

   self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 300)
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                               backing:NSBackingStoreBuffered defer:NO];
   self.window.title = kTitle;
   self.window.delegate = self;

   // the heading: the icon and the name
   NSImageView *icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
   [icon.widthAnchor constraintEqualToConstant:64].active = YES;
   [icon.heightAnchor constraintEqualToConstant:64].active = YES;
   NSTextField *name = [NSTextField labelWithString:@"Thief RT"];
   name.font = [NSFont systemFontOfSize:24 weight:NSFontWeightSemibold];
   NSTextField *sub = [NSTextField labelWithString:@"Thief and Thief II with path-traced lighting"];
   sub.textColor = NSColor.secondaryLabelColor;
   NSStackView *titles = [NSStackView stackViewWithViews:@[name, sub]];
   titles.orientation = NSUserInterfaceLayoutOrientationVertical;
   titles.alignment = NSLayoutAttributeLeading;
   titles.spacing = 2;
   NSStackView *heading = [NSStackView stackViewWithViews:@[icon, titles]];
   heading.spacing = 14;

   self.gamePopup = [NSPopUpButton new];
   self.gamePopup.target = self;
   self.gamePopup.action = @selector(gameChanged:);
   self.gameNote = Note();
   self.missionPopup = [NSPopUpButton new];
   self.missionPopup.target = self;
   self.missionPopup.action = @selector(missionChanged:);
   self.renderer = [NSSegmentedControl segmentedControlWithLabels:@[@"Ray traced", @"OpenGL"]
                                                     trackingMode:NSSegmentSwitchTrackingSelectOne
                                                           target:self action:@selector(rendererChanged:)];
   self.qualityPopup = [NSPopUpButton new];
   self.qualityPopup.target = self;
   self.qualityPopup.action = @selector(qualityChanged:);
   self.qualityNote = Note();
   // the frame counter, for either renderer (the game's panels have it too: Cmd+, or F10)
   self.fpsCheck = [NSButton checkboxWithTitle:@"Show the frame rate (FPS)" target:nil action:nil];
   self.fpsCheck.state = [RtSetting(@"fps") intValue] != 0 || [GlSetting(@"fps") intValue] != 0
      ? NSControlStateValueOn : NSControlStateValueOff;

   // the OpenGL renderer's options (its panel in the game has them too: Cmd+, or F10)
   self.glScalePopup = Choices(@[@[@"100% (the screen's own)", @"1.00"], @[@"85%", @"0.85"], @[@"75%", @"0.75"],
                                 @[@"67%", @"0.67"], @[@"50%", @"0.50"]],
                               GlSetting(@"render_scale"), @"1.00");
   self.glLightsPopup = Choices(@[@[@"Off (the game's lightmaps)", @"0"], @[@"The nearest 8", @"8"],
                                  @[@"The nearest 16", @"16"], @[@"The nearest 24", @"24"], @[@"The nearest 32", @"32"]],
                                GlSetting(@"dynamic_lights"), @"16");
   self.glVsyncPopup = Choices(@[@[@"Adaptive", @"2"], @[@"On", @"1"], @[@"Off (unlocked, may tear)", @"0"]],
                               GlSetting(@"vsync"), @"2");
   self.glShadowsCheck = [NSButton checkboxWithTitle:@"Shadows from objects" target:nil action:nil];
   self.glShadowsCheck.state = [(GlSetting(@"light_shadows") ?: @"1") intValue] ? NSControlStateValueOn : NSControlStateValueOff;
   self.glBumpCheck = [NSButton checkboxWithTitle:@"Bump maps" target:nil action:nil];
   self.glBumpCheck.state = [(GlSetting(@"bump_maps") ?: @"1") intValue] ? NSControlStateValueOn : NSControlStateValueOff;
   NSStackView *glChecks = [NSStackView stackViewWithViews:@[self.glShadowsCheck, self.glBumpCheck]];
   glChecks.spacing = 16;

   NSGridView *grid = [NSGridView gridViewWithViews:@[
      @[Label(@"Game:"), self.gamePopup],
      @[NSGridCell.emptyContentView, self.gameNote],
      @[Label(@"Play:"), self.missionPopup],
      @[Label(@"Graphics:"), self.renderer],
      @[Label(@"Quality:"), self.qualityPopup],
      @[NSGridCell.emptyContentView, self.qualityNote],
      @[Label(@"Resolution:"), self.glScalePopup],
      @[Label(@"Real-time lights:"), self.glLightsPopup],
      @[NSGridCell.emptyContentView, glChecks],
      @[Label(@"V-sync:"), self.glVsyncPopup],
      @[NSGridCell.emptyContentView, self.fpsCheck],
   ]];
   self.grid = grid;
   grid.rowSpacing = 8;
   grid.columnSpacing = 10;
   [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
   for (NSInteger r = 0; r < grid.numberOfRows; ++r)
      [grid rowAtIndex:r].rowAlignment = NSGridRowAlignmentFirstBaseline;
   [self.gamePopup.widthAnchor constraintEqualToConstant:330].active = YES;
   [self.missionPopup.widthAnchor constraintEqualToConstant:330].active = YES;
   [self.qualityPopup.widthAnchor constraintEqualToConstant:200].active = YES;
   for (NSPopUpButton *p in @[self.glScalePopup, self.glLightsPopup, self.glVsyncPopup])
      [p.widthAnchor constraintEqualToConstant:240].active = YES;

   NSButton *dev = [NSButton buttonWithTitle:@"Developer Settings..." target:self action:@selector(showDev:)];
   NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:NSApp action:@selector(terminate:)];
   quit.keyEquivalent = @"\033";
   self.playButton = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
   self.playButton.keyEquivalent = @"\r";
   NSView *spacer = [NSView new];
   [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
   NSStackView *buttons = [NSStackView stackViewWithViews:@[dev, spacer, quit, self.playButton]];
   buttons.spacing = 10;

   NSStackView *all = [NSStackView stackViewWithViews:@[heading, grid, buttons]];
   all.orientation = NSUserInterfaceLayoutOrientationVertical;
   all.alignment = NSLayoutAttributeLeading;
   all.spacing = 18;
   all.edgeInsets = NSEdgeInsetsMake(20, 24, 20, 24);
   all.translatesAutoresizingMaskIntoConstraints = NO;
   [buttons.widthAnchor constraintEqualToAnchor:grid.widthAnchor].active = YES;
   NSView *content = self.window.contentView;
   [content addSubview:all];
   [NSLayoutConstraint activateConstraints:@[
      [all.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
      [all.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
      [all.topAnchor constraintEqualToAnchor:content.topAnchor],
      [all.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
   ]];

   [self fillGames];
   // the renderer: as last chosen (here, or in the game's Options)
   NSString *r = [[NSString stringWithContentsOfFile:[SupportDir() stringByAppendingPathComponent:@"renderer.txt"]
                                            encoding:NSUTF8StringEncoding error:nil]
                  stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
   self.renderer.selectedSegment = [r hasPrefix:@"gl"] ? 1 : 0;
   [self fillQuality];
   [self rendererChanged:nil];

   [self.window center];
   // no games yet (the installer could not look for them: macOS keeps an app on a disk
   // image out of Downloads): looked for now, by this app -- macOS asks, and lists it in
   // its privacy settings -- then the window
   if (![self dirFor:@"tg"] && ![self dirFor:@"t2"])
      [self findGames:nil];
   else
      [self showWindow];
}

- (void)showWindow
{
   [self.window makeKeyAndOrderFront:nil];
   [NSApp activateIgnoringOtherApps:YES];
}

// the installer's finding part (install.sh --setup, as setup.sh in Resources): the games,
// The Black Parade, the HD Mod, in its own dialogs; the window again when it is done
- (void)findGames:(id)sender
{
   NSString *script = [[NSBundle mainBundle] pathForResource:@"setup" ofType:@"sh"];
   if (!script) {
      [self showWindow];
      return;
   }
   [self.window orderOut:nil];
   NSTask *t = [NSTask new];
   t.executableURL = [NSURL fileURLWithPath:@"/bin/bash"];
   t.arguments = @[script, @"--setup"];
   t.terminationHandler = ^(NSTask *task) {
      dispatch_async(dispatch_get_main_queue(), ^{
         self.games = ReadConf(self.gamesPath);
         [self fillGames];
         [self showWindow];
      });
   };
   NSError *err = nil;
   if (![t launchAndReturnError:&err])
      [self showWindow];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

- (void)buildMenu
{
   NSMenu *bar = [NSMenu new];
   NSMenuItem *appItem = [NSMenuItem new];
   [bar addItem:appItem];
   NSMenu *appMenu = [NSMenu new];
   [appMenu addItemWithTitle:@"About Thief RT" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
   [appMenu addItem:NSMenuItem.separatorItem];
   [appMenu addItemWithTitle:@"Hide Thief RT" action:@selector(hide:) keyEquivalent:@"h"];
   [appMenu addItemWithTitle:@"Quit Thief RT" action:@selector(terminate:) keyEquivalent:@"q"];
   appItem.submenu = appMenu;
   NSMenuItem *fileItem = [NSMenuItem new];
   [bar addItem:fileItem];
   NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
   [fileMenu addItemWithTitle:@"Find Games Again..." action:@selector(findGames:) keyEquivalent:@"f"].target = self;
   [fileMenu addItemWithTitle:@"Choose a Game Folder..." action:@selector(chooseFolder:) keyEquivalent:@"o"].target = self;
   [fileMenu addItemWithTitle:@"Show the Logs" action:@selector(showLogs:) keyEquivalent:@"l"].target = self;
   [fileMenu addItem:NSMenuItem.separatorItem];
   [fileMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
   fileItem.submenu = fileMenu;
   NSApp.mainMenu = bar;
}

// ---- the game ----------------------------------------------------------------------

- (void)fillGames
{
   [self.gamePopup removeAllItems];
   NSArray *gs = @[@[@"tg", @"Thief Gold"], @[@"t2", @"Thief II: The Metal Age"]];
   for (NSArray *g in gs) {
      NSString *dir = [self dirFor:g[0]];
      NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:dir ? g[1] : [g[1] stringByAppendingString:@"  (not found)"]
                                                  action:nil keyEquivalent:@""];
      it.representedObject = g[0];
      [self.gamePopup.menu addItem:it];
   }
   [self.gamePopup.menu addItem:NSMenuItem.separatorItem];
   NSMenuItem *choose = [[NSMenuItem alloc] initWithTitle:@"Choose a Game Folder..." action:nil keyEquivalent:@""];
   choose.representedObject = @"choose";
   [self.gamePopup.menu addItem:choose];

   NSString *last = self.choices[@"GAME"];
   if (![self dirFor:last])
      last = [self dirFor:@"tg"] ? @"tg" : [self dirFor:@"t2"] ? @"t2" : @"tg";
   [self.gamePopup selectItemAtIndex:[last isEqualToString:@"t2"] ? 1 : 0];
   [self gameShown];
}

- (void)gameChanged:(id)sender
{
   if ([self.gamePopup.selectedItem.representedObject isEqualToString:@"choose"]) {
      [self chooseFolder:nil];
      return;
   }
   self.choices[@"GAME"] = self.selectedGame;
   [self gameShown];
}

- (void)gameShown
{
   NSString *dir = [self dirFor:self.selectedGame];
   self.gameNote.stringValue = dir ? [dir stringByAbbreviatingWithTildeInPath]
                                   : @"Not found on this Mac. Choose its folder (the one with cam.cfg in it): File > Choose a Game Folder.";
   [self fillMissions];
   self.playButton.enabled = dir != nil;
}

- (void)chooseFolder:(id)sender
{
   NSOpenPanel *p = [NSOpenPanel openPanel];
   p.canChooseDirectories = YES;
   p.canChooseFiles = NO;
   p.message = @"Choose the folder of Thief Gold or Thief II: the one with cam.cfg and the missions in it.";
   p.prompt = @"Choose";
   [p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
      if (r == NSModalResponseOK) {
         NSString *dir = p.URL.path;
         NSString *kind = GameKind(dir);
         if (!kind) {
            NSAlert *a = [NSAlert new];
            a.messageText = @"That is not a Thief game folder";
            a.informativeText = @"Choose the folder with cam.cfg, dark.gam and the missions (miss1.mis ...) in it.";
            [a beginSheetModalForWindow:self.window completionHandler:nil];
         } else {
            self.games[[kind isEqualToString:@"t2"] ? @"T2_DIR" : @"TG_DIR"] = dir;
            WriteConf(self.games, self.gamesPath);
            self.choices[@"GAME"] = kind;
         }
      }
      [self fillGames];
   }];
}

// ---- what to play ------------------------------------------------------------------

- (void)fillMissions
{
   NSString *gid = self.selectedGame;
   self.missions = FanMissions([self dirFor:gid]);
   [self.missionPopup removeAllItems];
   self.missionPopup.autoenablesItems = NO;
   NSMenuItem *orig = [[NSMenuItem alloc] initWithTitle:[gid isEqualToString:@"t2"] ? @"The Metal Age (the original campaign)"
                                                                                    : @"The Dark Project (the original campaign)"
                                                 action:nil keyEquivalent:@""];
   orig.representedObject = @"";
   [self.missionPopup.menu addItem:orig];
   if (self.missions.count) {
      [self.missionPopup.menu addItem:NSMenuItem.separatorItem];
      NSMenuItem *h = [[NSMenuItem alloc] initWithTitle:@"Fan missions" action:nil keyEquivalent:@""];
      h.enabled = NO;
      [self.missionPopup.menu addItem:h];
   }
   for (FanMission *fm in self.missions) {
      NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:fm.title action:nil keyEquivalent:@""];
      it.representedObject = fm.folder;
      it.indentationLevel = 1;
      if (![fm.title isEqualToString:fm.folder])
         it.toolTip = fm.folder;
      [self.missionPopup.menu addItem:it];
   }
   [self.missionPopup.menu addItem:NSMenuItem.separatorItem];
   NSMenuItem *open = [[NSMenuItem alloc] initWithTitle:@"Open the FMs Folder (to add fan missions)..." action:nil keyEquivalent:@""];
   open.representedObject = @"open-fms";
   [self.missionPopup.menu addItem:open];

   NSString *last = self.choices[[@"FM_" stringByAppendingString:gid.uppercaseString]] ?: @"";
   NSInteger i = [self.missionPopup indexOfItemWithRepresentedObject:last];
   [self.missionPopup selectItemAtIndex:i >= 0 ? i : 0];
}

- (void)missionChanged:(id)sender
{
   NSString *fm = self.missionPopup.selectedItem.representedObject;
   if ([fm isEqualToString:@"open-fms"]) {
      NSString *dir = [self dirFor:self.selectedGame];
      if (dir) {
         NSString *fms = [dir stringByAppendingPathComponent:@"FMs"];
         [[NSFileManager defaultManager] createDirectoryAtPath:fms withIntermediateDirectories:YES attributes:nil error:nil];
         [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:fms]];
         NSAlert *a = [NSAlert new];
         a.messageText = @"Fan missions go in the FMs folder";
         a.informativeText = @"Unpack a fan mission (its .zip or .7z) into a folder of its own in FMs, then choose it here. The list is read again when you pick the game.";
         [a beginSheetModalForWindow:self.window completionHandler:nil];
      }
      [self fillMissions];
      return;
   }
   self.choices[[@"FM_" stringByAppendingString:self.selectedGame.uppercaseString]] = fm ?: @"";
}

// ---- graphics ----------------------------------------------------------------------

- (void)fillQuality
{
   [self.qualityPopup removeAllItems];
   [self.qualityPopup addItemsWithTitles:PresetNames()];
   // the preset the game's settings are now (rt_settings.txt; none yet: High, the defaults)
   // (settings made before the presets, with no quality_preset yet: kept as they are, Custom,
   // until the game has said which preset they are)
   NSString *q = RtSetting(@"quality_preset");
   const BOOL haveSettings = [[NSFileManager defaultManager] fileExistsAtPath:RtSettingsPath()];
   int i = q ? q.intValue : haveSettings ? 0 : 3;   // 1 Low .. 4 Ultra, 0 Custom
   if (i == 0) {
      [self.qualityPopup.menu addItem:NSMenuItem.separatorItem];
      [self.qualityPopup addItemWithTitle:@"Custom"];
      [self.qualityPopup selectItemWithTitle:@"Custom"];
   } else {
      static NSString *const byNumber[] = {@"High", @"Low", @"Medium", @"High", @"Ultra"};
      [self.qualityPopup selectItemWithTitle:byNumber[MAX(0, MIN(i, 4))]];
   }
   [self qualityChanged:nil];
}

- (void)qualityChanged:(id)sender
{
   self.qualityNote.stringValue = self.renderer.selectedSegment == 1
      ? @"OpenGL: the game's own look with sharper textures, and real-time lights with shadows from torches and lamps. "
        @"For more speed, lower the resolution or the real-time lights. All of these are in the game too (Cmd+, or F10)."
      : PresetNote(self.qualityPopup.titleOfSelectedItem);
}

- (void)rendererChanged:(id)sender
{
   const BOOL gl = self.renderer.selectedSegment == 1;
   self.qualityPopup.enabled = !gl;
   [self.grid rowAtIndex:kRowQuality].hidden = gl;
   for (NSInteger r = kRowGlFirst; r <= kRowGlLast; ++r)
      [self.grid rowAtIndex:r].hidden = !gl;
   [self qualityChanged:nil];
   [self fitWindow];
}

// the window to its rows (more with OpenGL's options), its top kept where it is
- (void)fitWindow
{
   if (!self.window)
      return;
   [self.window.contentView layoutSubtreeIfNeeded];
   const NSSize fit = self.window.contentView.fittingSize;
   NSRect f = self.window.frame;
   const NSRect c = [self.window frameRectForContentRect:NSMakeRect(0, 0, fit.width, fit.height)];
   f.origin.y += f.size.height - c.size.height;
   f.size = c.size;
   [self.window setFrame:f display:YES animate:NO];
}

// the OpenGL options into gl_settings.txt (those changed)
- (void)saveGlOptions
{
   NSDictionary *v = @{
      @"render_scale": self.glScalePopup.selectedItem.representedObject ?: @"1.00",
      @"dynamic_lights": self.glLightsPopup.selectedItem.representedObject ?: @"16",
      @"vsync": self.glVsyncPopup.selectedItem.representedObject ?: @"2",
      @"light_shadows": self.glShadowsCheck.state == NSControlStateValueOn ? @"1" : @"0",
      @"bump_maps": self.glBumpCheck.state == NSControlStateValueOn ? @"1" : @"0",
   };
   for (NSString *k in v) {
      NSString *now = GlSetting(k);
      if (!now || fabs(now.doubleValue - [v[k] doubleValue]) > 0.001)
         SetGlSetting(k, v[k]);
   }
}

// ---- developer settings ----------------------------------------------------------

- (NSButton *)check:(NSString *)title on:(BOOL)on
{
   NSButton *b = [NSButton checkboxWithTitle:title target:nil action:nil];
   b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
   return b;
}

- (void)showDev:(id)sender
{
   NSPanel *p = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 440, 300)
                                           styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
   NSTextField *head = [NSTextField labelWithString:@"Developer Settings"];
   head.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
   NSTextField *about = Note();
   about.preferredMaxLayoutWidth = 390;
   about.stringValue = @"Every ray tracing setting -- the light, the materials, the finer and technical ones -- is in the game: press Cmd+, (or F10), then \"Developer settings\" at the end of the list. The Reset row there brings back the defaults (High).";
   self.devAsk = [self check:@"Stop at the engine's debug checks (asserts) and ask" on:[self.choices[@"ASSERT_ASK"] isEqualToString:@"1"]];
   NSButton *logs = [NSButton buttonWithTitle:@"Show the Logs" target:self action:@selector(showLogs:)];
   NSButton *settings = [NSButton buttonWithTitle:@"Show the Settings Files" target:self action:@selector(showSettings:)];
   NSStackView *links = [NSStackView stackViewWithViews:@[logs, settings]];
   NSButton *done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(closeDev:)];
   done.keyEquivalent = @"\r";
   NSView *spacer = [NSView new];
   [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
   NSStackView *bottom = [NSStackView stackViewWithViews:@[links, spacer, done]];
   NSStackView *all = [NSStackView stackViewWithViews:@[head, about, self.devAsk, bottom]];
   all.orientation = NSUserInterfaceLayoutOrientationVertical;
   all.alignment = NSLayoutAttributeLeading;
   all.spacing = 10;
   all.edgeInsets = NSEdgeInsetsMake(20, 24, 20, 24);
   all.translatesAutoresizingMaskIntoConstraints = NO;
   [bottom.widthAnchor constraintEqualToConstant:392].active = YES;
   [p.contentView addSubview:all];
   [NSLayoutConstraint activateConstraints:@[
      [all.leadingAnchor constraintEqualToAnchor:p.contentView.leadingAnchor],
      [all.trailingAnchor constraintEqualToAnchor:p.contentView.trailingAnchor],
      [all.topAnchor constraintEqualToAnchor:p.contentView.topAnchor],
      [all.bottomAnchor constraintEqualToAnchor:p.contentView.bottomAnchor],
   ]];
   self.devPanel = p;
   [self.window beginSheet:p completionHandler:nil];
}

- (void)closeDev:(id)sender
{
   self.choices[@"ASSERT_ASK"] = self.devAsk.state == NSControlStateValueOn ? @"1" : @"0";
   WriteConf(self.choices, self.choicesPath);
   [self.window endSheet:self.devPanel];
   self.devPanel = nil;
}

- (void)showLogs:(id)sender
{
   NSString *logs = [SupportDir() stringByAppendingPathComponent:@"logs"];
   [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
   [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:logs]];
}

- (void)showSettings:(id)sender
{
   [[NSFileManager defaultManager] createDirectoryAtPath:EngineSettingsDir() withIntermediateDirectories:YES attributes:nil error:nil];
   [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:EngineSettingsDir()]];
}

// ---- play ------------------------------------------------------------------------

- (void)play:(id)sender
{
   NSString *gid = self.selectedGame;
   NSString *dir = [self dirFor:gid];
   if (!dir)
      return;
   NSString *fm = self.missionPopup.selectedItem.representedObject;
   if ([fm isEqualToString:@"open-fms"])
      fm = @"";
   const BOOL gl = self.renderer.selectedSegment == 1;
   NSString *preset = gl || [self.qualityPopup.titleOfSelectedItem isEqualToString:@"Custom"]
                         ? @"" : self.qualityPopup.titleOfSelectedItem.lowercaseString;

   self.choices[@"GAME"] = gid;
   self.choices[[@"FM_" stringByAppendingString:gid.uppercaseString]] = fm ?: @"";
   WriteConf(self.choices, self.choicesPath);
   // the frame counter, for both renderers (as the box says; the game's panels change it too)
   NSString *fps = self.fpsCheck.state == NSControlStateValueOn ? @"1" : @"0";
   if (![RtSetting(@"fps") isEqualToString:fps])
      SetRtSetting(@"fps", fps);
   if (![GlSetting(@"fps") isEqualToString:fps])
      SetGlSetting(@"fps", fps);
   if (gl)
      [self saveGlOptions];
   [(gl ? @"gl\n" : @"rt\n") writeToFile:[SupportDir() stringByAppendingPathComponent:@"renderer.txt"]
                              atomically:YES encoding:NSUTF8StringEncoding error:nil];

   // this process becomes the game (launch.sh starts it, and says if it fails)
   NSString *script = [[NSBundle mainBundle] pathForResource:@"launch" ofType:@"sh"];
   NSArray *args = @[@"/bin/bash", script,
                     [@"game=" stringByAppendingString:gid],
                     [@"game_dir=" stringByAppendingString:dir],
                     [@"fm=" stringByAppendingString:fm ?: @""],
                     [@"renderer=" stringByAppendingString:gl ? @"gl" : @"rt"],
                     [@"preset=" stringByAppendingString:preset],
                     [@"assert_ask=" stringByAppendingString:self.choices[@"ASSERT_ASK"] ?: @"0"]];
   const char **argv = (const char **)calloc(args.count + 1, sizeof(char *));
   for (NSUInteger i = 0; i < args.count; ++i)
      argv[i] = strdup([args[i] fileSystemRepresentation]);
   [self.window orderOut:nil];
   fflush(NULL);
   execv("/bin/bash", (char *const *)argv);
   // (only if it could not)
   NSAlert *a = [NSAlert new];
   a.messageText = @"The game could not be started";
   a.informativeText = [NSString stringWithFormat:@"%s (%@)", strerror(errno), script];
   [a runModal];
   [self.window makeKeyAndOrderFront:nil];
}

@end

int main(int argc, const char *argv[])
{
   @autoreleasepool {
      NSApplication *app = [NSApplication sharedApplication];
      app.activationPolicy = NSApplicationActivationPolicyRegular;
      Launcher *l = [Launcher new];
      app.delegate = l;
      [app run];
   }
   return 0;
}
