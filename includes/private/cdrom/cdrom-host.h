#ifndef CDROM_HOST_H
#define CDROM_HOST_H

/* Enumeration of the host's physical optical drives (internal or external).

   `id` is the value stored in `cdrom_drive` when the drive is selected:
     - Windows: the drive letter ('D', 'E', ...)
     - macOS:   the /dev/diskN number
     - Linux:   1 (the device is identified by cdrom_device_path) */

#define CDROM_HOST_MAX_DRIVES 26

enum { CDROM_HOST_BUS_UNKNOWN = -1, CDROM_HOST_INTERNAL = 0, CDROM_HOST_EXTERNAL = 1 };

typedef struct cdrom_host_drive_t {
        int id;
        char path[256];    /* device path to open (\\.\D:, /dev/sr0, /dev/disk2) */
        char name[64];     /* short name shown to the user (D:, sr0, disk2) */
        char vendor[32];
        char product[64];
        char serial[64];
        char bus[16];      /* "USB", "SATA", "ATAPI", "SCSI", ... */
        int external;      /* CDROM_HOST_INTERNAL / CDROM_HOST_EXTERNAL / CDROM_HOST_BUS_UNKNOWN */
} cdrom_host_drive_t;

/* Fill `out` with up to `max` drives currently attached. Returns the number found. */
int cdrom_host_enumerate(cdrom_host_drive_t *out, int max);

/* Query a single drive by id. Returns 1 if present, 0 otherwise. */
int cdrom_host_query(int id, cdrom_host_drive_t *out);

/* Stable identity of a drive ("vendor|product|serial") that survives drive letter changes. */
void cdrom_host_identity(const cdrom_host_drive_t *drive, char *buf, int len);

/* Find a currently attached drive whose identity matches. Returns its id, or 0 if not found. */
int cdrom_host_find_by_identity(const char *identity);

/* Human readable description, e.g. "D: HL-DT-ST DVDRAM GU10N (external, USB)". */
void cdrom_host_describe(const cdrom_host_drive_t *drive, char *buf, int len);

/* Identity of the selected host drive, saved in the machine config as "cdrom_host_id". */
extern char cdrom_host_id[256];

/* Validate `cdrom_drive` against the attached drives at machine start.
   If the saved drive letter now belongs to another drive (or none), look the drive
   up by `cdrom_host_id`; if it's not attached, fall back to an empty drive.
   Returns the drive id to use (0 if none). */
int cdrom_host_resolve(int drive);

/* Remember the identity of the drive just selected (updates cdrom_host_id). */
void cdrom_host_remember(int drive);

#endif /* CDROM_HOST_H */
