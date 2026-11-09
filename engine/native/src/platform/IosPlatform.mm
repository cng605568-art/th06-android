// The iOS half of the app (platform/Platform.hpp): the player's settings from the Settings
// app (Settings.bundle, NSUserDefaults), the game folder in the app's Documents (shown by the
// Files app), the system fonts through Core Text for the game's text, the Taptic Engine for
// vibration, and SDL's screen framebuffer.

#include "platform/Platform.hpp"

#if defined(TOUHOU_IOS)

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace touhou {
namespace {

// ---- settings (Settings.bundle keys; unset keys take the launcher's defaults) -------------

int intSetting(NSString* key, int fallback) {
    id v = [[NSUserDefaults standardUserDefaults] objectForKey:key];
    return [v respondsToSelector:@selector(intValue)] ? [v intValue] : fallback;
}

bool boolSetting(NSString* key, bool fallback) {
    id v = [[NSUserDefaults standardUserDefaults] objectForKey:key];
    return [v respondsToSelector:@selector(boolValue)] ? [v boolValue] : fallback;
}

int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

std::string documentsDir() {
    NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* d = dirs.count > 0 ? dirs[0] : NSTemporaryDirectory();
    std::string s = d.UTF8String ? d.UTF8String : "";
    if (!s.empty() && s.back() != '/') s += '/';
    return s;
}

bool hasArchive(NSString* dir) {
    NSArray* names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
    for (NSString* n in names) {
        if ([[n.pathExtension lowercaseString] isEqualToString:@"dat"] && ![[n lowercaseString] hasPrefix:@"thbgm"]) return true;
    }
    return false;
}

// The folder below `dir` (depth levels at most) with the game's archive; nil if none.
NSString* findArchiveFolder(NSString* dir, int depth) {
    if (hasArchive(dir)) return dir;
    if (depth <= 0) return nil;
    NSFileManager* fm = [NSFileManager defaultManager];
    NSArray* names = [[fm contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)];
    for (NSString* n in names) {
        if ([n isEqualToString:@"Saves"] || [n hasPrefix:@"."]) continue;
        NSString* sub = [dir stringByAppendingPathComponent:n];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:sub isDirectory:&isDir] || !isDir) continue;
        if (NSString* found = findArchiveFolder(sub, depth - 1)) return found;
    }
    return nil;
}

// ---- text: Core Text into a band, laid out like the Android renderer -----------------------

struct Bitmap {
    CGContextRef ctx = nullptr;
    std::vector<std::uint8_t> pixels;  // premultiplied RGBA
    int width = 0, height = 0;

    Bitmap(int w, int h) : pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0), width(w), height(h) {
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        ctx = CGBitmapContextCreate(pixels.data(), static_cast<size_t>(w), static_cast<size_t>(h), 8, static_cast<size_t>(w) * 4, rgb,
                                    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(rgb);
        if (ctx) {
            CGContextSetShouldAntialias(ctx, true);
            CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
        }
    }
    ~Bitmap() {
        if (ctx) CGContextRelease(ctx);
    }
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;

    // Into the band as straight (not premultiplied) RGBA, top row first.
    void copyTo(Image& band) const {
        CGContextFlush(ctx);
        const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* s = pixels.data() + i * 4;
            std::uint8_t* d = band.rgba.data() + i * 4;
            const unsigned a = s[3];
            d[3] = static_cast<std::uint8_t>(a);
            for (int c = 0; c < 3; ++c) d[c] = a == 0 ? 0 : static_cast<std::uint8_t>(std::min(255u, (s[c] * 255u + a / 2) / a));
        }
    }
};

CGColorRef rgbColor(std::uint32_t rgb) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    const CGFloat c[4] = {((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, 1.0};
    CGColorRef color = CGColorCreate(space, c);
    CGColorSpaceRelease(space);
    return color;
}

// One line of text in `font` and `color` (a CTLine; release it).
CTLineRef makeLine(NSString* text, CTFontRef font, std::uint32_t rgb) {
    CGColorRef color = rgbColor(rgb);
    NSDictionary* attrs = @{(__bridge id)kCTFontAttributeName : (__bridge id)font,
                            (__bridge id)kCTForegroundColorAttributeName : (__bridge id)color};
    NSAttributedString* s = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)s);
    CGColorRelease(color);
    return line;
}

double lineWidth(CTLineRef line) { return CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr); }

bool isCjk(NSString* text) {
    for (NSUInteger i = 0; i < text.length; ++i) {
        const unichar c = [text characterAtIndex:i];
        if ((c >= 0x3001 && c <= 0x30ff) || (c >= 0x3400 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
            (c >= 0xff01 && c <= 0xff9f) || (c >= 0xac00 && c <= 0xd7af)) {
            return true;
        }
    }
    return false;
}

// A bold font of `size` points: Hiragino Sans W6 (Japanese) or a condensed bold sans (Latin
// translations), with a slant for italics.
CTFontRef makeFont(bool cjk, double size, bool italic) {
    NSString* name = cjk ? @"HiraginoSans-W6" : @"AvenirNextCondensed-DemiBold";
    CGAffineTransform m = italic ? CGAffineTransformMake(1, 0, 0.25, 1, 0, 0) : CGAffineTransformIdentity;
    CTFontRef f = CTFontCreateWithName((__bridge CFStringRef)name, size, &m);
    if (!f) {
        UIFont* sys = [UIFont boldSystemFontOfSize:size];
        f = CTFontCreateWithName((__bridge CFStringRef)sys.fontName, size, &m);
    }
    return f;
}

void drawLine(CGContextRef ctx, CTLineRef line, double x, double baselineFromTop, int height) {
    CGContextSetTextPosition(ctx, x, height - baselineFromTop);  // Core Graphics counts from the bottom
    CTLineDraw(line, ctx);
}

class IosTextRenderer final : public TextRenderer {
  public:
    // Shift-JIS: every byte is (F + 1) / 2 texels wide, each character centred in its cells,
    // a shadow 1.5 texels right and 1 down (as the Android renderer draws it).
    bool render(std::span<const std::uint8_t> sjis, const TextStyle& style, float startX, Image& band) override {
        if (sjis.empty() || band.empty()) return false;
        @autoreleasepool {
            Bitmap bmp(band.width, band.height);
            if (!bmp.ctx) return false;
            const double cell = (style.fontSize + 1) / 2.0;
            CTFontRef font = makeFont(true, cell * 2.0 * 0.92, false);
            const double baseline = band.height * 0.8;
            double x = startX;
            for (std::size_t i = 0; i < sjis.size();) {
                const int n = sjisCharBytes(sjis[i]) == 2 && i + 1 < sjis.size() ? 2 : 1;
                // Code page 932 (Windows Shift-JIS), as the game's files are written.
                static const NSStringEncoding cp932 = CFStringConvertEncodingToNSStringEncoding(kCFStringEncodingDOSJapanese);
                NSString* ch = [[NSString alloc] initWithBytes:sjis.data() + i length:static_cast<NSUInteger>(n) encoding:cp932];
                if (ch && [ch stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]].length > 0) {
                    CTLineRef shadow = makeLine(ch, font, style.shadow);
                    CTLineRef text = makeLine(ch, font, style.rgb);
                    const double w = lineWidth(text);
                    const double dx = x + std::max(0.0, (cell * n - w) / 2.0);
                    drawLine(bmp.ctx, shadow, dx + 1.5, baseline + 1.0, band.height);
                    drawLine(bmp.ctx, text, dx, baseline, band.height);
                    CFRelease(shadow);
                    CFRelease(text);
                }
                x += cell * n;
                i += static_cast<std::size_t>(n);
            }
            CFRelease(font);
            bmp.copyTo(band);
        }
        return true;
    }

    // Translations (UTF-8): aligned within [left, left + width) and narrowed to fit.
    float renderUtf8(std::string_view utf8, const TextStyle& style, float left, float width, TextAlign align,
                     Image& band) override {
        if (utf8.empty() || band.empty()) return 0.0f;
        float drawn = 0.0f;
        @autoreleasepool {
            NSString* text = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
            if (!text) return 0.0f;
            const bool cjk = isCjk(text);
            const double cell = (style.fontSize + 1) / 2.0;
            CTFontRef font = makeFont(cjk, cell * 2.0 * (cjk ? 0.92 : 0.95), style.italic);
            CTLineRef shadow = makeLine(text, font, style.shadow);
            CTLineRef line = makeLine(text, font, style.rgb);
            const double full = lineWidth(line);
            const double room = width;
            const double scale = full > room && full > 0 ? room / full : 1.0;
            drawn = static_cast<float>(std::min(full, room));
            const double x = align == TextAlign::Right ? left + room - drawn : align == TextAlign::Centre ? left + (room - drawn) / 2.0 : left;
            Bitmap bmp(band.width, band.height);
            if (bmp.ctx) {
                const double baseline = band.height * 0.8;
                CGContextSaveGState(bmp.ctx);
                CGContextTranslateCTM(bmp.ctx, x, 0);
                CGContextScaleCTM(bmp.ctx, scale, 1.0);
                drawLine(bmp.ctx, shadow, 1.5 / scale, baseline + 1.0, band.height);
                drawLine(bmp.ctx, line, 0.0, baseline, band.height);
                CGContextRestoreGState(bmp.ctx);
                bmp.copyTo(band);
            } else {
                drawn = 0.0f;
            }
            CFRelease(shadow);
            CFRelease(line);
            CFRelease(font);
        }
        return drawn;
    }

    float measureUtf8(std::string_view utf8, const TextStyle& style) override {
        if (utf8.empty()) return 0.0f;
        float w = 0.0f;
        @autoreleasepool {
            NSString* text = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
            if (!text) return 0.0f;
            const bool cjk = isCjk(text);
            const double cell = (style.fontSize + 1) / 2.0;
            CTFontRef font = makeFont(cjk, cell * 2.0 * (cjk ? 0.92 : 0.95), style.italic);
            CTLineRef line = makeLine(text, font, style.rgb);
            w = static_cast<float>(lineWidth(line));
            CFRelease(line);
            CFRelease(font);
        }
        return w;
    }
};

}  // namespace

// ---- platform/Platform.hpp ----------------------------------------------------------------

std::vector<int> platformIntArray(const char* name) {
    const std::string n = name ? name : "";
    if (n == "getGameOptions") {
        // {shot type 0-5, difficulty 0-5, stage 1-6, spare lives 0-4, practice, skip the title,
        //  English text, mischief bits} as the Android launcher sends them.
        int mischief = 0;
        if (boolSetting(@"mischief_lives", false)) mischief |= 1;
        if (boolSetting(@"mischief_bombs", false)) mischief |= 2;
        if (boolSetting(@"mischief_invincible", false)) mischief |= 4;
        if (boolSetting(@"mischief_power", false)) mischief |= 8;
        if (boolSetting(@"mischief_continues", false)) mischief |= 16;
        return {clampInt(intSetting(@"shot_type", 0), 0, 5), clampInt(intSetting(@"difficulty", 1), 0, 5),
                clampInt(intSetting(@"stage", 1), 1, 6), clampInt(intSetting(@"lives", 2), 0, 4),
                boolSetting(@"practice", false) ? 1 : 0, boolSetting(@"skip_title", false) ? 1 : 0,
                boolSetting(@"english_text", true) ? 1 : 0, mischief};
    }
    return {};
}

ControlOptions readPlatformControlOptions() {
    ControlOptions out;
    out.drag = intSetting(@"move_mode", 0) == 0;
    out.sensitivity = static_cast<float>(clampInt(intSetting(@"drag_speed", 150), 50, 400)) / 100.0f;
    out.buttonScale = static_cast<float>(clampInt(intSetting(@"button_size", 100), 50, 200)) / 100.0f;
    out.opacity = static_cast<float>(clampInt(intSetting(@"opacity", 100), 40, 100)) / 100.0f;
    out.leftHanded = boolSetting(@"left_handed", false);
    out.focusToggle = boolSetting(@"focus_toggle", false);
    out.autoFire = boolSetting(@"auto_fire", false);
    out.vibrate = boolSetting(@"vibrate", true);
    const int speed = intSetting(@"game_speed", 100);
    out.speedPercent = speed == 75 || speed == 50 ? speed : 100;
    out.showHitbox = boolSetting(@"show_hitbox", false);
    out.saveStates = boolSetting(@"save_states", false);
    return out;
}

DisplayOptions readPlatformDisplayOptions() {
    DisplayOptions out;
    out.portrait = intSetting(@"layout", 0) == 1;
    out.scaling = clampInt(intSetting(@"scaling", 0), 0, 2);
    out.scanlines = boolSetting(@"scanlines", false);
    return out;
}

void platformVibrate(int milliseconds) {
    // A miss (80 ms) is a heavy tap, a bomb (35 ms) a light one.
    UIImpactFeedbackGenerator* g = [[UIImpactFeedbackGenerator alloc]
        initWithStyle:milliseconds >= 60 ? UIImpactFeedbackStyleHeavy : UIImpactFeedbackStyleLight];
    [g prepare];
    [g impactOccurred];
}

std::unique_ptr<TextRenderer> makePlatformTextRenderer() { return std::make_unique<IosTextRenderer>(); }

// The game text on iOS comes from a thcrap folder next to the game files (th07 looks there).
std::vector<std::string> readPlatformGameTextStack() { return {}; }

std::string iosGameDataDir() {
    @autoreleasepool {
        const std::string docs = documentsDir();
        NSString* root = [NSString stringWithUTF8String:docs.c_str()];
        NSString* found = findArchiveFolder(root, 2);
        std::string s = found ? std::string(found.UTF8String) : docs;
        if (!s.empty() && s.back() != '/') s += '/';
        return s;
    }
}

std::string iosStorageDir() {
    @autoreleasepool {
        const std::string docs = documentsDir();
        const std::string saves = docs + "Saves/";
        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:saves.c_str()]
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        // A note for the Files app, so the player knows where the game files go.
        NSString* note = [NSString stringWithUTF8String:(docs + "Put your game folder here.txt").c_str()];
        if (![[NSFileManager defaultManager] fileExistsAtPath:note]) {
            NSString* text = @"Copy your own copy of the game here (the whole folder is fine): th07.dat and th07.exe "
                             @"(version 1.00b), and thbgm.dat for music. Keep a thcrap folder with the English patch "
                             @"next to them for English text. Saves, replays and logs are kept in the Saves folder.\n";
            [text writeToFile:note atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }
        return saves;
    }
}

unsigned iosScreenFramebuffer(void* sdlWindow) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (sdlWindow && SDL_GetWindowWMInfo(static_cast<SDL_Window*>(sdlWindow), &info) && info.subsystem == SDL_SYSWM_UIKIT) {
        return info.info.uikit.framebuffer;
    }
    return 0;
}

}  // namespace touhou

#endif
