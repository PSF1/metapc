#ifndef _HDD_HOSTDISK_H_
#define _HDD_HOSTDISK_H_
/* Direct access to a physical disk of the host (a CompactFlash card in a USB
   reader, for example) so it can be used as an emulated hard disk.

   A host disk is named by its device path: \\.\PhysicalDriveN on Windows,
   /dev/... elsewhere. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOSTDISK_MAX 32

typedef struct hostdisk_info_t {
        char path[64];      /* \\.\PhysicalDrive2, /dev/sdb */
        char model[64];     /* as reported by the host, or by the card */
        uint64_t sectors;   /* 512-byte sectors */
        int removable;      /* USB / SD / removable media */
        int system;         /* holds the host's operating system: never usable */
        int usable;         /* may be opened as an emulated hard disk */
        /* Geometry to enter in the BIOS of a real machine. */
        int cyl, hpc, spt;
        int geometry_from_card; /* 1 = read from the card (IDENTIFY), 0 = calculated */
} hostdisk_info_t;

/* Non-zero if fn names a host device rather than an image file. */
int hostdisk_is_device_path(const char *fn);

/* Fills list with the host's disks. Returns how many. */
int hostdisk_enumerate(hostdisk_info_t *list, int max);

/* Queries one disk by path. Returns 0 on success. */
int hostdisk_query(const char *path, hostdisk_info_t *info);

/* The default ATA geometry for a disk of the given size, as a CompactFlash
   card reports it: what a BIOS set to "Auto" detects. */
void hostdisk_default_geometry(uint64_t sectors, int *cyl, int *hpc, int *spt);

/* Raw sector access, used by hdd_file.c. */
typedef struct hostdisk_t hostdisk_t;
hostdisk_t *hostdisk_open(const char *path, int read_only, char *err, int err_size);
void hostdisk_close(hostdisk_t *d);
uint64_t hostdisk_sectors(hostdisk_t *d);
int hostdisk_read(hostdisk_t *d, uint64_t sector, int count, void *buffer);
int hostdisk_write(hostdisk_t *d, uint64_t sector, int count, const void *buffer);

#ifdef __cplusplus
}
#endif

#endif /* _HDD_HOSTDISK_H_ */
