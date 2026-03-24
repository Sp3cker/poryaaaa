#include "native_file_dialog.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

bool choose_midi_file_dialog(char *outPath, size_t outPathSize)
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

        if ([panel runModal] != NSModalResponseOK)
            return false;

        NSURL *url = panel.URL;
        if (!url)
            return false;

        const char *path = url.path.UTF8String;
        if (!path)
            return false;

        snprintf(outPath, outPathSize, "%s", path);
        return true;
    }
}

#endif
