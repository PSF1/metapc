#ifndef _SOUND_DRIVES_H_
#define _SOUND_DRIVES_H_

/* Synthesised mechanical noise for floppy drives, hard disks and CD/DVD drives.

   All drivesnd_* hooks are safe to call when the drive sound device is not loaded;
   they do nothing. Except for the drivesnd_ui_* functions, they must be called
   from the emulation thread. */

#include "device.h"

extern device_t drivesnd_device;

/*Set from the machine configuration; adds drivesnd_device when non-zero*/
extern int drive_sounds_enabled;

/*Floppy drives. drive is the physical drive (0 or 1)*/
void drivesnd_fdd_motor(int drive, int on);
void drivesnd_fdd_seek(int drive, int track);

/*Hard disks. id is the hdc[] index of the drive*/
void drivesnd_hdd_attach(int id, int cylinders, uint64_t bytes);
void drivesnd_hdd_access(int id, int cylinder);

/*CD/DVD drive*/
void drivesnd_cd_attach(int max_speed, int is_dvd);
void drivesnd_cd_read(uint32_t lba);
void drivesnd_cd_audio(int playing);
void drivesnd_cd_tray(int open);

/*Media changed from the user interface. Safe to call from any thread*/
void drivesnd_ui_fdd_insert(int drive);
void drivesnd_ui_cd_change(void);

#endif /* _SOUND_DRIVES_H_ */
