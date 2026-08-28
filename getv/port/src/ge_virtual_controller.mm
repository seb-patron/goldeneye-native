// On-screen touch controls for iOS.
//
// Why this needs almost no code: GCVirtualController, once connected, registers itself as
// an ordinary GCController -- the exact same object model a physical MFi/Bluetooth pad
// uses. SDL2's iOS/tvOS joystick backend (src/joystick/iphoneos/SDL_mfijoystick.m) already
// listens for GCControllerDidConnectNotification and reads state through
// controller.extendedGamepad, with no branch anywhere that excludes a virtual controller
// from a physical one. port_input.c already reads pads purely through
// SDL_GameController*/SDL_IsGameController() -- it has no idea GCVirtualController exists,
// and does not need to: once this file connects one, it just looks like a second gamepad
// showed up. That is the whole integration.
//
// geControllerIsReal() (port_input.c) requires LEFTX/LEFTY/RIGHTX axes plus A/B/
// LEFTSHOULDER buttons before it will bind a pad to a port, so the configuration below
// includes those at minimum; the rest (X/Y, right shoulder, both triggers, the d-pad) are
// added so the game's other reads (Z-trigger, C-buttons, START) have something real to
// read rather than silently stuck at zero.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

/* Always included, not just inside the iOS branch below: this header's declarations are
 * wrapped in extern "C", and that linkage only applies to the stub definitions in the
 * #else branch (used by tvOS and desktop) if the compiler has already seen it declared
 * extern "C" by the time it reaches them. Getting this wrong compiles the stubs with
 * ordinary C++ (name-mangled) linkage instead -- verified by the exact failure: tvOS's
 * linker reported the mangled `__Z27gePortVirtualControllerInitv` as an unrelated extra
 * symbol while `_gePortVirtualControllerInit`, the plain C name ge_tvos_main.c's `extern`
 * declaration actually links against, came back undefined. */
#import "ge_virtual_controller.h"

#if defined(TARGET_OS_IOS) && TARGET_OS_IOS && !(defined(TARGET_OS_TV) && TARGET_OS_TV)

#import <GameController/GameController.h>
#import <UIKit/UIKit.h>

namespace {
GCVirtualController *g_virtualController;
}

void gePortVirtualControllerInit(void)
{
    if (g_virtualController != nil) return;
    if (@available(iOS 15.0, *)) {
        /* Do not overlay a touch pad on top of a real one -- if a physical controller is
         * already connected (Bluetooth pad, or a keyboard-less MFi controller paired
         * before launch), it is what the player wants to use. */
        if (GCController.controllers.count > 0) {
            printf("[getv][ios] physical game controller already connected -- skipping "
                   "the on-screen virtual controller\n");
            return;
        }

        /* GCInputDirectionPad is NOT included alongside GCInputLeftThumbstick: Apple's
         * GCVirtualController throws an uncaught NSInvalidArgumentException at creation
         * if both are requested together ("The Apple touch controller does not support
         * both of these elements simultaneously") -- an immediate, hard crash, not a
         * degraded fallback. Confirmed on a real iPhone 14 Pro: it fired the instant
         * gePortLauncherRun()'s successor (this function) ran, i.e. exactly when the
         * player tapped Start Mission. The thumbstick wins because port_input.c's own
         * geControllerIsReal() gate requires LEFTX/LEFTY/RIGHTX axes before it will
         * bind a pad to a port at all -- the d-pad alone would not even pass that
         * check, so the d-pad binding was never actually used here. */
        GCVirtualControllerConfiguration *config = [[GCVirtualControllerConfiguration alloc] init];
        config.elements = [NSSet setWithArray:@[
            GCInputLeftThumbstick, GCInputRightThumbstick,
            GCInputButtonA, GCInputButtonB, GCInputButtonX, GCInputButtonY,
            GCInputLeftShoulder, GCInputRightShoulder,
            GCInputLeftTrigger, GCInputRightTrigger,
        ]];

        g_virtualController = [[GCVirtualController alloc] initWithConfiguration:config];
        [g_virtualController connectWithReplyHandler:^(NSError * _Nullable error) {
            if (error != nil) {
                printf("[getv][ios] GCVirtualController connect failed: %s\n",
                       error.localizedDescription.UTF8String);
            } else {
                printf("[getv][ios] on-screen virtual controller connected\n");
            }
        }];
    } else {
        printf("[getv][ios] iOS < 15.0: no virtual controller support, and no way to "
               "reach this build (deployment target is 15.0) -- unreachable in practice\n");
    }
}

void gePortVirtualControllerShutdown(void)
{
    if (g_virtualController == nil) return;
    [g_virtualController disconnect];
    g_virtualController = nil;
}

/* Info.plist's UISupportedInterfaceOrientations and SDL_HINT_ORIENTATIONS only constrain
 * which orientations the app is ALLOWED to rotate into -- neither one makes an app that
 * launched in portrait actually rotate to landscape. Verified on the iOS Simulator: even
 * with both correctly restricting to landscape, and SDL_GetDisplayBounds() reporting the
 * right landscape-shaped point size once the launch-screen fix (UILaunchScreen: {} in
 * project_ios.yml) took the app out of legacy 480x320 compatibility mode, the physical
 * screen stayed in portrait and the landscape-shaped render got clipped into a corner of
 * it. This forces the rotation the same way games have worked around this for years. */
/* TEMPORARY: printf capture from this app is unreliable through every simctl/log
 * mechanism tried, same issue ge_launcher.cpp's diag block documents -- append to the
 * same file instead. Remove once the iOS orientation bug is closed. */
static void geDiagLog(NSString *line)
{
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *dir = [[paths.firstObject stringByAppendingPathComponent:@"goldeneyenative"]
                      stringByAppendingPathComponent:@"getv-diag"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *path = [dir stringByAppendingPathComponent:@"diag.txt"];
    NSString *withNewline = [line stringByAppendingString:@"\n"];
    NSFileHandle *fh = [NSFileHandle fileHandleForWritingAtPath:path];
    if (fh == nil) {
        [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
        fh = [NSFileHandle fileHandleForWritingAtPath:path];
    }
    [fh seekToEndOfFile];
    [fh writeData:[withNewline dataUsingEncoding:NSUTF8StringEncoding]];
    [fh closeFile];
}

void gePortForceLandscapeOrientation(void)
{
    geDiagLog([NSString stringWithFormat:@"orientation: idiom=%ld statusBarOrientation(deprecated)=%ld",
               (long)UIDevice.currentDevice.userInterfaceIdiom,
               (long)UIApplication.sharedApplication.statusBarOrientation]);

    if (@available(iOS 16.0, *)) {
        UIWindowScene *scene = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
            if ([s isKindOfClass:[UIWindowScene class]]) {
                scene = (UIWindowScene *)s;
                break;
            }
        }
        geDiagLog([NSString stringWithFormat:@"orientation: connectedScenes=%lu foundWindowScene=%d",
                   (unsigned long)UIApplication.sharedApplication.connectedScenes.count, scene != nil]);
        if (scene != nil) {
            geDiagLog([NSString stringWithFormat:@"orientation: scene.windows=%lu scene.interfaceOrientation=%ld "
                       "effectiveGeometry.interfaceOrientation=%ld",
                       (unsigned long)scene.windows.count, (long)scene.interfaceOrientation,
                       (long)scene.effectiveGeometry.interfaceOrientation]);
            UIWindowSceneGeometryPreferencesIOS *pref = [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
            [scene requestGeometryUpdateWithPreferences:pref errorHandler:^(NSError * _Nonnull error) {
                geDiagLog([NSString stringWithFormat:@"orientation: requestGeometryUpdateWithPreferences FAILED: %@",
                           error.localizedDescription]);
            }];
            for (UIWindow *w in scene.windows) {
                geDiagLog([NSString stringWithFormat:@"orientation: window rootVC=%@ supportedOrientations-before=%lu",
                           NSStringFromClass(w.rootViewController.class),
                           (unsigned long)w.rootViewController.supportedInterfaceOrientations]);
                [w.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
            }
        }
    } else {
        geDiagLog(@"orientation: iOS < 16, no UIWindowScene geometry API available");
    }
    /* The long-standing KVC trick, still the only thing that works pre-16 and kept here
     * even on 16+ as a second push -- it is harmless to call both. */
    if ([[UIDevice currentDevice] respondsToSelector:@selector(setValue:forKey:)]) {
        [[UIDevice currentDevice] setValue:@(UIInterfaceOrientationLandscapeLeft) forKey:@"orientation"];
    }
    [UIViewController attemptRotationToDeviceOrientation];
    geDiagLog([NSString stringWithFormat:@"orientation: after force, statusBarOrientation(deprecated)=%ld",
               (long)UIApplication.sharedApplication.statusBarOrientation]);
}

#else /* not iOS, or is tvOS */

void gePortVirtualControllerInit(void) {}
void gePortVirtualControllerShutdown(void) {}
void gePortForceLandscapeOrientation(void) {}

#endif
