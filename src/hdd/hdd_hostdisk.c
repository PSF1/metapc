/* Direct access to a physical disk of the host (a CompactFlash card in a USB
   reader, for example) so it can be used as an emulated hard disk. See
   hdd_hostdisk.h. */
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "hdd_hostdisk.h"

static void set_err(char *err, int err_size, const char *msg) {
        if (err && err_size > 0) {
                strncpy(err, msg, err_size - 1);
                err[err_size - 1] = 0;
        }
}

static void trim(char *s) {
        int len = strlen(s);
        while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == '\t'))
                s[--len] = 0;
        int start = 0;
        while (s[start] == ' ' || s[start] == '\t')
                start++;
        if (start)
                memmove(s, s + start, len - start + 1);
}

void hostdisk_default_geometry(uint64_t sectors, int *cyl, int *hpc, int *spt) {
        static const int spts[] = {63, 32, 16};

        /* Cards over 504 MB report 16 heads and 63 sectors, with as many
           cylinders as fit (at most 16383, the ATA limit). */
        if (sectors >= 1024 * 16 * 63) {
                uint64_t c = sectors / (16 * 63);
                *cyl = (int)(c > 16383 ? 16383 : c);
                *hpc = 16;
                *spt = 63;
                return;
        }
        /* Smaller cards report a geometry that covers them exactly; take the
           first one that keeps the cylinders within the 1024 a BIOS without
           LBA can reach. */
        for (int i = 0; i < (int)(sizeof(spts) / sizeof(spts[0])); i++) {
                for (int h = 1; h <= 16; h *= 2) {
                        uint64_t per_cyl = (uint64_t)h * spts[i];
                        if (sectors % per_cyl == 0 && sectors / per_cyl <= 1024) {
                                *cyl = (int)(sectors / per_cyl);
                                *hpc = h;
                                *spt = spts[i];
                                return;
                        }
                }
        }
        *hpc = 16;
        *spt = 63;
        *cyl = (int)(sectors / (16 * 63));
        if (*cyl == 0) {
                *hpc = 1;
                *spt = sectors < 63 ? (int)sectors : 63;
                *cyl = 1;
        }
}

#ifdef _WIN32
/* Takes the geometry and model from IDENTIFY DEVICE data when it makes sense. */
static void apply_identify(hostdisk_info_t *info, const uint16_t *w) {
        int c = w[1], h = w[3], s = w[6];

        if (c > 0 && c <= 16383 && h > 0 && h <= 16 && s > 0 && s <= 63 && (uint64_t)c * h * s <= info->sectors) {
                info->cyl = c;
                info->hpc = h;
                info->spt = s;
                info->geometry_from_card = 1;
        }
        char model[41];
        for (int i = 0; i < 20; i++) {
                model[i * 2] = w[27 + i] >> 8;
                model[i * 2 + 1] = w[27 + i] & 0xff;
        }
        model[40] = 0;
        trim(model);
        if (model[0] && (unsigned char)model[0] >= 0x20 && (unsigned char)model[0] < 0x7f) {
                strncpy(info->model, model, sizeof(info->model) - 1);
                info->model[sizeof(info->model) - 1] = 0;
        }
}

/* ------------------------------------------------------------------ Windows */
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

int hostdisk_is_device_path(const char *fn) {
        return fn && (!strncmp(fn, "\\\\.\\", 4) || !strncmp(fn, "//./", 4)) && strlen(fn) > 4;
}

static int disk_number(const char *path) {
        const char *p = path + 4;
        if (_strnicmp(p, "PhysicalDrive", 13))
                return -1;
        p += 13;
        if (*p < '0' || *p > '9')
                return -1;
        return atoi(p);
}

/* Disk numbers a volume (\\.\C: or \\?\Volume{...} without the trailing
   backslash) lives on, as a bit mask. */
static uint32_t volume_disks(HANDLE vol) {
        union {
                VOLUME_DISK_EXTENTS ext;
                char buf[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)];
        } u;
        DWORD ret;
        uint32_t mask = 0;

        if (!DeviceIoControl(vol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &u, sizeof(u), &ret, NULL))
                return 0;
        for (DWORD i = 0; i < u.ext.NumberOfDiskExtents; i++)
                if (u.ext.Extents[i].DiskNumber < 32)
                        mask |= 1u << u.ext.Extents[i].DiskNumber;
        return mask;
}

static uint32_t system_disks(void) {
        char windir[MAX_PATH];
        char vol[8];
        uint32_t mask;

        if (!GetSystemWindowsDirectoryA(windir, sizeof(windir)) || windir[1] != ':')
                return 1; /* be careful: assume disk 0 */
        snprintf(vol, sizeof(vol), "\\\\.\\%c:", windir[0]);
        HANDLE h = CreateFileA(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
                return 1;
        mask = volume_disks(h);
        CloseHandle(h);
        return mask ? mask : 1;
}

/* IDENTIFY DEVICE through SCSI/ATA Translation. Many USB card readers do not
   support it; then the geometry is calculated. */
static int identify_sat(HANDLE h, uint16_t *id) {
        struct {
                SCSI_PASS_THROUGH spt;
                unsigned char sense[32];
                uint16_t buf[256];
        } s;
        DWORD ret;

        memset(&s, 0, sizeof(s));
        s.spt.Length = sizeof(SCSI_PASS_THROUGH);
        s.spt.DataIn = SCSI_IOCTL_DATA_IN;
        s.spt.DataTransferLength = 512;
        s.spt.TimeOutValue = 3;
        s.spt.SenseInfoLength = sizeof(s.sense);
        s.spt.SenseInfoOffset = offsetof(__typeof__(s), sense);
        s.spt.DataBufferOffset = offsetof(__typeof__(s), buf);
        s.spt.CdbLength = 16;
        s.spt.Cdb[0] = 0x85;   /* ATA PASS-THROUGH (16) */
        s.spt.Cdb[1] = 4 << 1; /* PIO data-in */
        s.spt.Cdb[2] = 0x0e;   /* from device, count in sectors, in sector count field */
        s.spt.Cdb[6] = 1;
        s.spt.Cdb[14] = 0xec; /* IDENTIFY DEVICE */
        if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH, &s, sizeof(s), &s, sizeof(s), &ret, NULL) || s.spt.ScsiStatus != 0)
                return -1;
        /* Word 0 bit 15 clear = ATA device; a CF card reports 0x848a. */
        if ((s.buf[0] & 0x8000) && s.buf[0] != 0x848a)
                return -1;
        memcpy(id, s.buf, 512);
        return 0;
}

static int query_handle(HANDLE h, int number, uint32_t sys_mask, hostdisk_info_t *info) {
        GET_LENGTH_INFORMATION li;
        DWORD ret;

        memset(info, 0, sizeof(*info));
        snprintf(info->path, sizeof(info->path), "\\\\.\\PhysicalDrive%d", number);

        if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof(li), &ret, NULL) || li.Length.QuadPart < 512)
                return -1; /* no media */
        info->sectors = (uint64_t)li.Length.QuadPart / 512;

        STORAGE_PROPERTY_QUERY q;
        union {
                STORAGE_DEVICE_DESCRIPTOR d;
                char buf[1024];
        } u;
        memset(&q, 0, sizeof(q));
        memset(&u, 0, sizeof(u));
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &u, sizeof(u), &ret, NULL)) {
                char model[64] = "";
                if (u.d.VendorIdOffset && u.d.VendorIdOffset < sizeof(u))
                        snprintf(model, sizeof(model), "%s", u.buf + u.d.VendorIdOffset);
                trim(model);
                if (u.d.ProductIdOffset && u.d.ProductIdOffset < sizeof(u)) {
                        int len = strlen(model);
                        snprintf(model + len, sizeof(model) - len, "%s%s", len ? " " : "", u.buf + u.d.ProductIdOffset);
                }
                trim(model);
                strcpy(info->model, model);
                info->removable = u.d.RemovableMedia || u.d.BusType == BusTypeUsb || u.d.BusType == BusTypeSd ||
                                  u.d.BusType == BusTypeMmc || u.d.BusType == BusType1394;
        }
        if (!info->model[0])
                snprintf(info->model, sizeof(info->model), "Disk %d", number);

        info->system = number < 32 && (sys_mask & (1u << number));
        info->usable = info->removable && !info->system;

        hostdisk_default_geometry(info->sectors, &info->cyl, &info->hpc, &info->spt);
        if (info->usable) {
                uint16_t id[256];
                if (!identify_sat(h, id))
                        apply_identify(info, id);
        }
        return 0;
}

static HANDLE open_disk(int number, DWORD access) {
        char path[64];
        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", number);
        return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

int hostdisk_query(const char *path, hostdisk_info_t *info) {
        int number = hostdisk_is_device_path(path) ? disk_number(path) : -1;
        if (number < 0)
                return -1;
        HANDLE h = open_disk(number, GENERIC_READ | GENERIC_WRITE);
        if (h == INVALID_HANDLE_VALUE)
                h = open_disk(number, GENERIC_READ);
        if (h == INVALID_HANDLE_VALUE)
                return -1;
        int r = query_handle(h, number, system_disks(), info);
        CloseHandle(h);
        return r;
}

int hostdisk_enumerate(hostdisk_info_t *list, int max) {
        uint32_t sys_mask = system_disks();
        int n = 0;

        for (int i = 0; i < 32 && n < max; i++) {
                HANDLE h = open_disk(i, GENERIC_READ | GENERIC_WRITE);
                if (h == INVALID_HANDLE_VALUE)
                        h = open_disk(i, GENERIC_READ);
                if (h == INVALID_HANDLE_VALUE)
                        continue;
                if (!query_handle(h, i, sys_mask, &list[n]))
                        n++;
                CloseHandle(h);
        }
        return n;
}

#define MAX_LOCKED_VOLUMES 26

struct hostdisk_t {
        HANDLE h;
        uint64_t sectors;
        int read_only;
        HANDLE volumes[MAX_LOCKED_VOLUMES];
        int num_volumes;
};

/* Locks and dismounts every volume Windows has on the disk, so that writes
   to it are allowed and Windows does not touch it while the machine runs.
   The locks last until the handles are closed. */
static int lock_volumes(hostdisk_t *d, int number, char *err, int err_size) {
        WCHAR name[MAX_PATH];
        HANDLE find = FindFirstVolumeW(name, MAX_PATH);
        int ok = 1;

        if (find == INVALID_HANDLE_VALUE)
                return 1;
        do {
                size_t len = wcslen(name);
                if (len && name[len - 1] == L'\\')
                        name[len - 1] = 0;
                HANDLE vol = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                         0, NULL);
                if (vol == INVALID_HANDLE_VALUE)
                        continue;
                if (!(volume_disks(vol) & (1u << number))) {
                        CloseHandle(vol);
                        continue;
                }
                DWORD ret;
                if (!DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL)) {
                        CloseHandle(vol);
                        set_err(err, err_size, "A drive on this disk is in use by Windows (close any window or program using it)");
                        ok = 0;
                        break;
                }
                DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL);
                if (d->num_volumes < MAX_LOCKED_VOLUMES)
                        d->volumes[d->num_volumes++] = vol;
                else
                        CloseHandle(vol);
        } while (FindNextVolumeW(find, name, MAX_PATH));
        FindVolumeClose(find);
        return ok;
}

static void unlock_volumes(hostdisk_t *d) {
        for (int i = 0; i < d->num_volumes; i++)
                CloseHandle(d->volumes[i]);
        d->num_volumes = 0;
}

hostdisk_t *hostdisk_open(const char *path, int read_only, char *err, int err_size) {
        hostdisk_info_t info;
        int number = hostdisk_is_device_path(path) ? disk_number(path) : -1;

        if (number < 0 || number >= 32) {
                set_err(err, err_size, "Not a physical disk path");
                return NULL;
        }
        if (hostdisk_query(path, &info)) {
                set_err(err, err_size, "The disk is not present, has no card inserted, or MetaPC is not running as administrator");
                return NULL;
        }
        if (info.system) {
                set_err(err, err_size, "This disk holds Windows and cannot be used");
                return NULL;
        }
        if (!info.usable) {
                set_err(err, err_size, "Only removable disks (USB card readers, SD/CF cards) can be used");
                return NULL;
        }

        hostdisk_t *d = (hostdisk_t *)calloc(1, sizeof(hostdisk_t));
        if (!d)
                return NULL;
        d->read_only = read_only;
        d->sectors = info.sectors;
        if (!read_only && !lock_volumes(d, number, err, err_size)) {
                unlock_volumes(d);
                free(d);
                return NULL;
        }
        char dev[64];
        snprintf(dev, sizeof(dev), "\\\\.\\PhysicalDrive%d", number);
        d->h = CreateFileA(dev, read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE), FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
        if (d->h == INVALID_HANDLE_VALUE) {
                set_err(err, err_size, GetLastError() == ERROR_ACCESS_DENIED ? "Access denied: run MetaPC as administrator"
                                                                              : "Cannot open the disk");
                unlock_volumes(d);
                free(d);
                return NULL;
        }
        return d;
}

void hostdisk_close(hostdisk_t *d) {
        if (!d)
                return;
        if (!d->read_only) {
                DWORD ret;
                FlushFileBuffers(d->h);
                /* Let Windows read the new partition table. */
                DeviceIoControl(d->h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &ret, NULL);
        }
        CloseHandle(d->h);
        unlock_volumes(d);
        free(d);
}

static int seek_to(hostdisk_t *d, uint64_t sector) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)(sector * 512);
        return SetFilePointerEx(d->h, pos, NULL, FILE_BEGIN) ? 0 : -1;
}

int hostdisk_read(hostdisk_t *d, uint64_t sector, int count, void *buffer) {
        DWORD done;
        if (seek_to(d, sector) || !ReadFile(d->h, buffer, count * 512, &done, NULL) || done != (DWORD)count * 512)
                return -1;
        return 0;
}

int hostdisk_write(hostdisk_t *d, uint64_t sector, int count, const void *buffer) {
        DWORD done;
        if (d->read_only)
                return -1;
        if (seek_to(d, sector) || !WriteFile(d->h, buffer, count * 512, &done, NULL) || done != (DWORD)count * 512)
                return -1;
        return 0;
}

#else
/* ------------------------------------------------------------------ POSIX */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

int hostdisk_is_device_path(const char *fn) { return fn && !strncmp(fn, "/dev/", 5); }

static uint64_t fd_sectors(int fd) {
#ifdef BLKGETSIZE64
        uint64_t bytes;
        if (!ioctl(fd, BLKGETSIZE64, &bytes))
                return bytes / 512;
#endif
        off_t end = lseek(fd, 0, SEEK_END);
        return end > 0 ? (uint64_t)end / 512 : 0;
}

#ifdef __linux__
static int read_sys(const char *name, const char *attr, char *out, int size) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/block/%s/%s", name, attr);
        FILE *f = fopen(path, "r");
        if (!f)
                return -1;
        if (!fgets(out, size, f))
                out[0] = 0;
        fclose(f);
        trim(out);
        return 0;
}

/* The disk holds a file system mounted at / or /boot. */
static int is_system_disk(const char *name) {
        FILE *f = fopen("/proc/mounts", "r");
        char line[512], dev[256], mnt[256];
        int sys = 0;
        size_t len = strlen(name);

        if (!f)
                return 1;
        while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%255s %255s", dev, mnt) != 2)
                        continue;
                if (strncmp(dev, "/dev/", 5) || strncmp(dev + 5, name, len))
                        continue;
                if (!strcmp(mnt, "/") || !strcmp(mnt, "/boot") || !strcmp(mnt, "/boot/efi") || !strcmp(mnt, "/usr"))
                        sys = 1;
        }
        fclose(f);
        return sys;
}
#endif

int hostdisk_query(const char *path, hostdisk_info_t *info) {
        if (!hostdisk_is_device_path(path))
                return -1;
        memset(info, 0, sizeof(*info));
        snprintf(info->path, sizeof(info->path), "%s", path);

        int fd = open(path, O_RDONLY);
        if (fd < 0)
                return -1;
        info->sectors = fd_sectors(fd);
        close(fd);
        if (!info->sectors)
                return -1;

        const char *name = path + 5;
        snprintf(info->model, sizeof(info->model), "%s", name);
#ifdef __linux__
        char buf[128], vendor[64] = "", model[64] = "";
        if (!read_sys(name, "removable", buf, sizeof(buf)))
                info->removable = atoi(buf);
        char link[256], target[512];
        snprintf(link, sizeof(link), "/sys/block/%s", name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n > 0) {
                target[n] = 0;
                if (strstr(target, "/usb") || strstr(target, "/mmc"))
                        info->removable = 1;
        }
        read_sys(name, "device/vendor", vendor, sizeof(vendor));
        read_sys(name, "device/model", model, sizeof(model));
        if (vendor[0] || model[0]) {
                snprintf(info->model, sizeof(info->model), "%s%s%s", vendor, vendor[0] ? " " : "", model);
                trim(info->model);
        }
        info->system = is_system_disk(name);
#else
        info->removable = 1;
#endif
        info->usable = info->removable && !info->system;
        hostdisk_default_geometry(info->sectors, &info->cyl, &info->hpc, &info->spt);
        return 0;
}

int hostdisk_enumerate(hostdisk_info_t *list, int max) {
        int n = 0;
#ifdef __linux__
        DIR *dir = opendir("/sys/block");
        struct dirent *e;
        if (!dir)
                return 0;
        while ((e = readdir(dir)) != NULL && n < max) {
                if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3) ||
                    !strncmp(e->d_name, "zram", 4) || !strncmp(e->d_name, "dm-", 3) || !strncmp(e->d_name, "sr", 2))
                        continue;
                char path[64];
                snprintf(path, sizeof(path), "/dev/%s", e->d_name);
                if (!hostdisk_query(path, &list[n]))
                        n++;
        }
        closedir(dir);
#else
        (void)list;
        (void)max;
#endif
        return n;
}

struct hostdisk_t {
        int fd;
        uint64_t sectors;
        int read_only;
};

hostdisk_t *hostdisk_open(const char *path, int read_only, char *err, int err_size) {
        hostdisk_info_t info;

        if (hostdisk_query(path, &info)) {
                set_err(err, err_size, "The disk is not present or cannot be read (check the permissions of the device)");
                return NULL;
        }
        if (info.system) {
                set_err(err, err_size, "This disk holds the operating system and cannot be used");
                return NULL;
        }
        if (!info.usable) {
                set_err(err, err_size, "Only removable disks (USB card readers, SD/CF cards) can be used");
                return NULL;
        }
        /* O_EXCL on a block device fails while any of it is mounted. */
        int fd = open(path, (read_only ? O_RDONLY : O_RDWR) | O_EXCL);
        if (fd < 0) {
                set_err(err, err_size,
                        errno == EBUSY ? "The disk is mounted: unmount it first" : "Cannot open the disk (check its permissions)");
                return NULL;
        }
        hostdisk_t *d = (hostdisk_t *)calloc(1, sizeof(hostdisk_t));
        if (!d) {
                close(fd);
                return NULL;
        }
        d->fd = fd;
        d->sectors = info.sectors;
        d->read_only = read_only;
        return d;
}

void hostdisk_close(hostdisk_t *d) {
        if (!d)
                return;
        if (!d->read_only)
                fsync(d->fd);
        close(d->fd);
        free(d);
}

int hostdisk_read(hostdisk_t *d, uint64_t sector, int count, void *buffer) {
        ssize_t want = (ssize_t)count * 512;
        return pread(d->fd, buffer, want, (off_t)(sector * 512)) == want ? 0 : -1;
}

int hostdisk_write(hostdisk_t *d, uint64_t sector, int count, const void *buffer) {
        ssize_t want = (ssize_t)count * 512;
        if (d->read_only)
                return -1;
        return pwrite(d->fd, buffer, want, (off_t)(sector * 512)) == want ? 0 : -1;
}
#endif

uint64_t hostdisk_sectors(hostdisk_t *d) { return d ? d->sectors : 0; }
