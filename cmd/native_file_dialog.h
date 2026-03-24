#ifndef NATIVE_FILE_DIALOG_H
#define NATIVE_FILE_DIALOG_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool choose_midi_file_dialog(char *outPath, size_t outPathSize);

#ifdef __cplusplus
}
#endif

#endif
