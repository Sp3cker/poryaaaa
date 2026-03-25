#include "native_file_dialog.h"

#if !defined(__APPLE__)

bool choose_midi_file_dialog(uintptr_t parentView, char *outPath, size_t outPathSize)
{
    (void)parentView;
    (void)outPath;
    (void)outPathSize;
    return false;
}

bool choose_directory_dialog(uintptr_t parentView, char *outPath, size_t outPathSize)
{
    (void)parentView;
    (void)outPath;
    (void)outPathSize;
    return false;
}

#endif
