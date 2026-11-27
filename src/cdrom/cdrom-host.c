/*Enumeration of the host's physical optical drives (internal or external/USB)*/

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if __APPLE__
#include <util.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ibm.h"
#include "ide.h"
#include "cdrom-ioctl.h"
#include "cdrom-host.h"

char cdrom_host_id[256];

/* Copy `len` bytes of `src`, trimming leading/trailing blanks. */
static void copy_trimmed(char *dst, int dst_len, const char *src, int len) {
        int start = 0, end = len;

        while (start < end && (isspace((unsigned char)src[start]) || !src[start]))
                start++;
        while (end > start && (isspace((unsigned char)src[end - 1]) || !src[end - 1]))
                end--;
        len = end - start;
        if (len >= dst_len)
                len = dst_len - 1;
        memcpy(dst, src + start, len);
        dst[len] = 0;
}

static void drive_init(cdrom_host_drive_t *d, int id) {
        memset(d, 0, sizeof(*d));
        d->id = id;
        d->external = CDROM_HOST_BUS_UNKNOWN;
}

#ifdef _WIN32
static const char *win_bus_name(int bus, int *external) {
        *external = CDROM_HOST_INTERNAL;
        switch (bus) {
        case BusTypeUsb:
                *external = CDROM_HOST_EXTERNAL;
                return "USB";
        case BusType1394:
                *external = CDROM_HOST_EXTERNAL;
                return "FireWire";
        case BusTypeSd:
                *external = CDROM_HOST_EXTERNAL;
                return "SD";
        case BusTypeAtapi:
                return "ATAPI";
        case BusTypeAta:
                return "ATA";
        case BusTypeSata:
                return "SATA";
        case BusTypeScsi:
                return "SCSI";
        case BusTypeSas:
                return "SAS";
        case BusTypeRAID:
                return "RAID";
        default:
                *external = CDROM_HOST_BUS_UNKNOWN;
                return "";
        }
}

int cdrom_host_query(int id, cdrom_host_drive_t *out) {
        char root[8];
        HANDLE h;

        if (id < 'A' || id > 'Z')
                return 0;
        snprintf(root, sizeof(root), "%c:\\", id);
        if (GetDriveTypeA(root) != DRIVE_CDROM)
                return 0;

        drive_init(out, id);
        snprintf(out->path, sizeof(out->path), "\\\\.\\%c:", id);
        snprintf(out->name, sizeof(out->name), "%c:", id);

        /* Access 0: querying the descriptor doesn't need read access and won't spin up the drive. */
        h = CreateFileA(out->path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
                STORAGE_PROPERTY_QUERY query;
                union {
                        STORAGE_DEVICE_DESCRIPTOR desc;
                        uint8_t raw[1024];
                } buf;
                DWORD ret = 0;

                memset(&query, 0, sizeof(query));
                query.PropertyId = StorageDeviceProperty;
                query.QueryType = PropertyStandardQuery;
                memset(&buf, 0, sizeof(buf));
                if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &buf, sizeof(buf), &ret, NULL) &&
                    ret >= offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength)) {
                        STORAGE_DEVICE_DESCRIPTOR *desc = &buf.desc;
                        const char *raw = (const char *)buf.raw;

                        if (desc->VendorIdOffset && desc->VendorIdOffset < ret)
                                copy_trimmed(out->vendor, sizeof(out->vendor), raw + desc->VendorIdOffset,
                                             strnlen(raw + desc->VendorIdOffset, ret - desc->VendorIdOffset));
                        if (desc->ProductIdOffset && desc->ProductIdOffset < ret)
                                copy_trimmed(out->product, sizeof(out->product), raw + desc->ProductIdOffset,
                                             strnlen(raw + desc->ProductIdOffset, ret - desc->ProductIdOffset));
                        if (desc->SerialNumberOffset && desc->SerialNumberOffset != (DWORD)-1 &&
                            desc->SerialNumberOffset < ret)
                                copy_trimmed(out->serial, sizeof(out->serial), raw + desc->SerialNumberOffset,
                                             strnlen(raw + desc->SerialNumberOffset, ret - desc->SerialNumberOffset));
                        snprintf(out->bus, sizeof(out->bus), "%s", win_bus_name(desc->BusType, &out->external));
                }
                CloseHandle(h);
        }
        return 1;
}

int cdrom_host_enumerate(cdrom_host_drive_t *out, int max) {
        DWORD mask = GetLogicalDrives();
        int n = 0;

        for (int c = 'A'; c <= 'Z' && n < max; c++) {
                if (!(mask & (1u << (c - 'A'))))
                        continue;
                if (cdrom_host_query(c, &out[n]))
                        n++;
        }
        return n;
}

#elif __linux__
static void read_sysfs(const char *path, char *buf, int len) {
        char tmp[256];
        FILE *f = fopen(path, "r");

        buf[0] = 0;
        if (!f)
                return;
        if (fgets(tmp, sizeof(tmp), f))
                copy_trimmed(buf, len, tmp, strlen(tmp));
        fclose(f);
}

static int linux_query_sr(const char *sr, cdrom_host_drive_t *out) {
        char path[PATH_MAX], real[PATH_MAX];
        struct stat st;

        snprintf(path, sizeof(path), "/dev/%s", sr);
        if (stat(path, &st))
                return 0;

        drive_init(out, 1);
        snprintf(out->path, sizeof(out->path), "%s", path);
        snprintf(out->name, sizeof(out->name), "%s", sr);

        snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", sr);
        read_sysfs(path, out->vendor, sizeof(out->vendor));
        snprintf(path, sizeof(path), "/sys/block/%s/device/model", sr);
        read_sysfs(path, out->product, sizeof(out->product));
        snprintf(path, sizeof(path), "/sys/block/%s/device/wwid", sr);
        read_sysfs(path, out->serial, sizeof(out->serial));

        snprintf(path, sizeof(path), "/sys/block/%s", sr);
        if (realpath(path, real)) {
                if (strstr(real, "/usb")) {
                        out->external = CDROM_HOST_EXTERNAL;
                        strcpy(out->bus, "USB");
                } else if (strstr(real, "/ata")) {
                        out->external = CDROM_HOST_INTERNAL;
                        strcpy(out->bus, "ATA");
                }
        }
        return 1;
}

int cdrom_host_enumerate(cdrom_host_drive_t *out, int max) {
        DIR *dir = opendir("/sys/block");
        struct dirent *ent;
        int n = 0;

        if (!dir)
                return 0;
        while ((ent = readdir(dir)) && n < max) {
                if (strncmp(ent->d_name, "sr", 2))
                        continue;
                if (linux_query_sr(ent->d_name, &out[n]))
                        n++;
        }
        closedir(dir);
        return n;
}

/* On Linux the drive is identified by cdrom_device_path; `id` is always 1. */
int cdrom_host_query(int id, cdrom_host_drive_t *out) {
        char want[PATH_MAX], real[PATH_MAX];
        cdrom_host_drive_t drives[CDROM_HOST_MAX_DRIVES];
        int n;

        if (id <= 0 || !realpath(cdrom_device_path, want))
                return 0;
        n = cdrom_host_enumerate(drives, CDROM_HOST_MAX_DRIVES);
        for (int c = 0; c < n; c++) {
                if (realpath(drives[c].path, real) && !strcmp(real, want)) {
                        *out = drives[c];
                        return 1;
                }
        }
        /* A device path not listed under /sys/block/sr* (set by hand): still usable, identity unknown. */
        drive_init(out, 1);
        snprintf(out->path, sizeof(out->path), "%s", cdrom_device_path);
        snprintf(out->name, sizeof(out->name), "%s", cdrom_device_path);
        return 1;
}

#elif __APPLE__
int cdrom_host_query(int id, cdrom_host_drive_t *out) {
        char s[80];
        int fd;

        if (id <= 0 || id >= 99)
                return 0;
        snprintf(s, sizeof(s), "disk%i", id);
        fd = opendev(s, O_RDONLY, 0, NULL);
        if (fd <= 0)
                return 0;
        close(fd);
        drive_init(out, id);
        snprintf(out->path, sizeof(out->path), "/dev/disk%i", id);
        snprintf(out->name, sizeof(out->name), "disk%i", id);
        return 1;
}

int cdrom_host_enumerate(cdrom_host_drive_t *out, int max) {
        int n = 0;

        for (int c = 1; c < 99 && n < max; c++) {
                if (cdrom_host_query(c, &out[n]))
                        n++;
        }
        return n;
}

#else
int cdrom_host_query(int id, cdrom_host_drive_t *out) { return 0; }
int cdrom_host_enumerate(cdrom_host_drive_t *out, int max) { return 0; }
#endif

void cdrom_host_identity(const cdrom_host_drive_t *drive, char *buf, int len) {
        if (!drive->vendor[0] && !drive->product[0] && !drive->serial[0])
                buf[0] = 0;
        else
                snprintf(buf, len, "%s|%s|%s", drive->vendor, drive->product, drive->serial);
}

int cdrom_host_find_by_identity(const char *identity) {
        cdrom_host_drive_t drives[CDROM_HOST_MAX_DRIVES];
        char id[256];
        int n;

        if (!identity || !identity[0])
                return 0;
        n = cdrom_host_enumerate(drives, CDROM_HOST_MAX_DRIVES);
        for (int c = 0; c < n; c++) {
                cdrom_host_identity(&drives[c], id, sizeof(id));
                if (!strcmp(id, identity)) {
#if __linux__
                        strncpy(cdrom_device_path, drives[c].path, sizeof(cdrom_device_path) - 1);
                        cdrom_device_path[sizeof(cdrom_device_path) - 1] = 0;
#endif
                        return drives[c].id;
                }
        }
        return 0;
}

void cdrom_host_describe(const cdrom_host_drive_t *drive, char *buf, int len) {
        char model[128];
        const char *where = "";

        if (drive->vendor[0] && drive->product[0])
                snprintf(model, sizeof(model), " %s %s", drive->vendor, drive->product);
        else if (drive->product[0])
                snprintf(model, sizeof(model), " %s", drive->product);
        else
                model[0] = 0;

        if (drive->external == CDROM_HOST_EXTERNAL)
                where = "external";
        else if (drive->external == CDROM_HOST_INTERNAL)
                where = "internal";

        if (where[0] && drive->bus[0])
                snprintf(buf, len, "%s%s (%s, %s)", drive->name, model, where, drive->bus);
        else if (where[0] || drive->bus[0])
                snprintf(buf, len, "%s%s (%s)", drive->name, model, where[0] ? where : drive->bus);
        else
                snprintf(buf, len, "%s%s", drive->name, model);
}

void cdrom_host_remember(int drive) {
        cdrom_host_drive_t d;

        if (drive > 0 && drive != CDROM_IMAGE && cdrom_host_query(drive, &d))
                cdrom_host_identity(&d, cdrom_host_id, sizeof(cdrom_host_id));
        else
                cdrom_host_id[0] = 0;
}

int cdrom_host_resolve(int drive) {
        cdrom_host_drive_t d;
        char id[256];
        int found;

        if (drive <= 0 || drive == CDROM_IMAGE)
                return drive;

        if (cdrom_host_query(drive, &d)) {
                cdrom_host_identity(&d, id, sizeof(id));
                /* Old config without identity, drive without identity, or same drive: keep it. */
                if (!cdrom_host_id[0] || !id[0] || !strcmp(id, cdrom_host_id)) {
                        if (!cdrom_host_id[0])
                                strcpy(cdrom_host_id, id);
                        return drive;
                }
        }

        found = cdrom_host_find_by_identity(cdrom_host_id);
        if (found) {
                pclog("Host CD-ROM drive '%s' moved, now using id %i\n", cdrom_host_id, found);
                return found;
        }

        pclog("Host CD-ROM drive %i ('%s') not attached, starting with an empty drive\n", drive, cdrom_host_id);
#if __linux__
        /* Keep the user's device path on Linux, it may simply have no medium / not be enumerable. */
        return drive;
#else
        return 0;
#endif
}
