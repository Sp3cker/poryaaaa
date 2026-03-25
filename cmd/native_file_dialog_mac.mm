#include "native_file_dialog.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

static NSWindow *dialog_parent_window(uintptr_t parentView)
{
    if (!parentView)
        return nil;

    id obj = (__bridge id)(void *)parentView;
    if ([obj isKindOfClass:[NSWindow class]])
        return (NSWindow *)obj;
    if ([obj isKindOfClass:[NSView class]])
        return [(NSView *)obj window];
    return nil;
}

static bool run_open_panel(NSOpenPanel *panel,
                           uintptr_t parentView,
                           char *outPath,
                           size_t outPathSize)
{
    __block NSModalResponse response = NSModalResponseCancel;
    __block NSURL *selectedURL = nil;
    NSWindow *parentWindow = dialog_parent_window(parentView);

    if (parentWindow) {
        [panel beginSheetModalForWindow:parentWindow
                      completionHandler:^(NSModalResponse modalResponse) {
            response = modalResponse;
            if (modalResponse == NSModalResponseOK)
                selectedURL = [panel.URL copy];
            [NSApp stopModalWithCode:modalResponse];
        }];
        [NSApp runModalForWindow:parentWindow];
        [parentWindow endSheet:panel];
    } else {
        response = [panel runModal];
        if (response == NSModalResponseOK)
            selectedURL = [panel.URL copy];
    }

    if (response != NSModalResponseOK || !selectedURL)
        return false;

    const char *path = selectedURL.path.UTF8String;
    if (!path) {
        [selectedURL release];
        return false;
    }

    snprintf(outPath, outPathSize, "%s", path);
    [selectedURL release];
    return true;
}

bool choose_midi_file_dialog(uintptr_t parentView, char *outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
        return false;

    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if (@available(macOS 11.0, *)) {
            panel.allowedContentTypes = @[];
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            panel.allowedFileTypes = @[ @"mid", @"midi" ];
#pragma clang diagnostic pop
        }

        return run_open_panel(panel, parentView, outPath, outPathSize);
    }
}

bool choose_directory_dialog(uintptr_t parentView, char *outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
        return false;

    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;

        return run_open_panel(panel, parentView, outPath, outPathSize);
    }
}

#endif
