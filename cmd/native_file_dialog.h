#ifndef NATIVE_FILE_DIALOG_H
#define NATIVE_FILE_DIALOG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool choose_midi_file_dialog(uintptr_t parentView, char *outPath, size_t outPathSize);
bool choose_directory_dialog(uintptr_t parentView, char *outPath, size_t outPathSize);

#ifdef __cplusplus
}
#endif

#endif
