#ifndef _MEM_BIOS_H_
#define _MEM_BIOS_H_

#include <stdio.h>

int loadbios();

/* A BIOS image the user chose for a machine (for example one downloaded from The Retro Web),
   used instead of the model's own ROM set. It only applies while the machine uses the model
   it was chosen for, so switching to another model goes back to that model's own ROMs. */
extern char custom_bios_path[512];
extern char custom_bios_model[32];

/* Sets or clears (path NULL or empty) the custom BIOS for the model with that internal name. */
void custom_bios_set(const char *path, const char *model_internal_name);
/* Non-zero if the custom BIOS replaces the ROMs of this romset. */
int custom_bios_applies(int romset_id);
/* Non-zero if the custom BIOS replaces the ROMs of the current romset. */
int custom_bios_active(void);
/* Size in bytes of a BIOS image file, or -1 if it cannot be read. */
long custom_bios_file_size(const char *path);
/* Non-zero if a BIOS image of this size can be loaded as a custom BIOS. */
int custom_bios_size_supported(long size);

/* Opens the file that keeps a flash BIOS's writable blocks (ESCD, DMI, settings) between runs.
   rel_path is the model's own flash file under the ROM paths; a custom BIOS keeps its own file
   next to the machine's NVR instead, so it never mixes with the model's own BIOS. */
FILE *flash_fopen(char *rel_path, char *mode);

#endif /* _MEM_BIOS_H_ */
