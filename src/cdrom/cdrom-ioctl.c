/*Win32 CD-ROM support via IOCTL*/

#include <stddef.h>
#include <windows.h>
#include <io.h>
#ifdef __MINGW64_VERSION_MAJOR
#include "ntddcdrm.h"
#include "ntddscsi.h"
#else
#include "ddk/ntddcdrm.h"
#include "ddk/ntddscsi.h"
#endif
#include "ibm.h"
#include "ide.h"
#include "cdrom-ioctl.h"

int cdrom_drive;
int old_cdrom_drive;

#ifndef __MINGW64_VERSION_MAJOR
typedef struct _CDROM_TOC_SESSION_DATA {
        UCHAR Length[2];
        UCHAR FirstCompleteSession;
        UCHAR LastCompleteSession;
        TRACK_DATA TrackData[1];
} CDROM_TOC_SESSION_DATA, *PCDROM_TOC_SESSION_DATA;
#endif

static ATAPI ioctl_atapi;

static uint32_t last_block = 0;
static uint32_t cdrom_capacity = 0;
static char ioctl_path[8];
void ioctl_close(void);
static HANDLE hIOCTL;
static CDROM_TOC toc;
static int tocvalid = 0;

#define MSFtoLBA(m, s, f) ((((m * 60) + s) * 75) + f)

enum { CD_STOPPED = 0, CD_PLAYING, CD_PAUSED };

static int ioctl_cd_state = CD_STOPPED;
static uint32_t ioctl_cd_pos = 0, ioctl_cd_end = 0;

#define BUF_SIZE 32768
static int16_t cd_buffer[BUF_SIZE];
static int cd_buflen = 0;
static HANDLE ioctl_open_handle(void) {
        return CreateFile(ioctl_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                          NULL);
}

void ioctl_audio_callback(int16_t *output, int len) {
        RAW_READ_INFO in;
        DWORD count;
        HANDLE h = INVALID_HANDLE_VALUE;

        //	return;
        //        pclog("Audio callback %08X %08X %i %i %i %04X %i\n", ioctl_cd_pos, ioctl_cd_end, ioctl_cd_state, cd_buflen, len,
        //        cd_buffer[4], GetTickCount());
        if (ioctl_cd_state != CD_PLAYING)
                return;
        while (cd_buflen < len) {
                if (ioctl_cd_pos < ioctl_cd_end) {
                        in.DiskOffset.QuadPart = (LONGLONG)(ioctl_cd_pos - 150) * 2048;
                        in.SectorCount = 1;
                        in.TrackMode = CDDA;
                        /* This runs on the sound thread: use a private handle, never hIOCTL. */
                        if (h == INVALID_HANDLE_VALUE)
                                h = ioctl_open_handle();
                        //        		pclog("Read to %i\n", cd_buflen);
                        if (!DeviceIoControl(h, IOCTL_CDROM_RAW_READ, &in, sizeof(in), &cd_buffer[cd_buflen], 2352, &count,
                                             NULL)) {
                                //                                pclog("DeviceIoControl returned false\n");
                                memset(&cd_buffer[cd_buflen], 0, (BUF_SIZE - cd_buflen) * 2);
                                ioctl_cd_state = CD_STOPPED;
                                cd_buflen = len;
                        } else {
                                //                                pclog("DeviceIoControl returned true\n");
                                ioctl_cd_pos++;
                                cd_buflen += (2352 / 2);
                        }
                } else {
                        memset(&cd_buffer[cd_buflen], 0, (BUF_SIZE - cd_buflen) * 2);
                        ioctl_cd_state = CD_STOPPED;
                        cd_buflen = len;
                }
        }
        if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        memcpy(output, cd_buffer, len * 2);
        //        for (c = 0; c < BUF_SIZE - len; c++)
        //            cd_buffer[c] = cd_buffer[c + cd_buflen];
        memcpy(&cd_buffer[0], &cd_buffer[len], (BUF_SIZE - len) * 2);
        cd_buflen -= len;
        //        pclog("Done %i\n", GetTickCount());
}

void ioctl_audio_stop() { ioctl_cd_state = CD_STOPPED; }

static int get_track_nr(uint32_t pos) {
        int c;
        int track = 0;

        if (!tocvalid)
                return 0;

        for (c = toc.FirstTrack; c < toc.LastTrack; c++) {
                uint32_t track_address = toc.TrackData[c].Address[3] + (toc.TrackData[c].Address[2] * 75) +
                                         (toc.TrackData[c].Address[1] * 75 * 60);

                if (track_address <= pos)
                        track = c;
        }
        return track;
}

static void ioctl_playaudio(uint32_t pos, uint32_t len, int ismsf) {
        if (!cdrom_drive)
                return;
        pclog("Play audio - %08X %08X %i\n", pos, len, ismsf);
        if (ismsf) {
                int m = (pos >> 16) & 0xff;
                int s = (pos >> 8) & 0xff;
                int f = pos & 0xff;
                pos = MSFtoLBA(m, s, f);
                m = (len >> 16) & 0xff;
                s = (len >> 8) & 0xff;
                f = len & 0xff;
                len = MSFtoLBA(m, s, f);
                pclog("MSF - pos = %08X len = %08X\n", pos, len);
        } else
                len += pos;
        ioctl_cd_pos = pos; // + 150;
        ioctl_cd_end = len; // + 150;
        ioctl_cd_state = CD_PLAYING;
        if (ioctl_cd_pos < 150)
                ioctl_cd_pos = 150;
        pclog("Audio start %08X %08X %i %i %i\n", ioctl_cd_pos, ioctl_cd_end, ioctl_cd_state, cd_buflen, len);
        /*        CDROM_PLAY_AUDIO_MSF msf;
                long size;
                BOOL b;
                if (ismsf)
                {
                        msf.StartingF=pos&0xFF;
                        msf.StartingS=(pos>>8)&0xFF;
                        msf.StartingM=(pos>>16)&0xFF;
                        msf.EndingF=len&0xFF;
                        msf.EndingS=(len>>8)&0xFF;
                        msf.EndingM=(len>>16)&0xFF;
                }
                else
                {
                        msf.StartingF=(uint8_t)(addr%75); addr/=75;
                        msf.StartingS=(uint8_t)(addr%60); addr/=60;
                        msf.StartingM=(uint8_t)(addr);
                        addr=pos+len+150;
                        msf.EndingF=(uint8_t)(addr%75); addr/=75;
                        msf.EndingS=(uint8_t)(addr%60); addr/=60;
                        msf.EndingM=(uint8_t)(addr);
                }
                ioctl_open(0);
                b = DeviceIoControl(hIOCTL,IOCTL_CDROM_PLAY_AUDIO_MSF,&msf,sizeof(msf),NULL,0,&size,NULL);
                pclog("DeviceIoControl returns %i\n", (int) b);
                ioctl_close();*/
}

static void ioctl_pause(void) {
        if (!cdrom_drive)
                return;
        if (ioctl_cd_state == CD_PLAYING)
                ioctl_cd_state = CD_PAUSED;
        //        ioctl_open(0);
        //        DeviceIoControl(hIOCTL,IOCTL_CDROM_PAUSE_AUDIO,NULL,0,NULL,0,&size,NULL);
        //        ioctl_close();
}

static void ioctl_resume(void) {
        if (!cdrom_drive)
                return;
        if (ioctl_cd_state == CD_PAUSED)
                ioctl_cd_state = CD_PLAYING;
        //        ioctl_open(0);
        //        DeviceIoControl(hIOCTL,IOCTL_CDROM_RESUME_AUDIO,NULL,0,NULL,0,&size,NULL);
        //        ioctl_close();
}

static void ioctl_stop(void) {
        if (!cdrom_drive)
                return;
        ioctl_cd_state = CD_STOPPED;
        //        ioctl_open(0);
        //        DeviceIoControl(hIOCTL,IOCTL_CDROM_STOP_AUDIO,NULL,0,NULL,0,&size,NULL);
        //        ioctl_close();
}

static void ioctl_seek(uint32_t pos) {
        if (!cdrom_drive)
                return;
        //       ioctl_cd_state = CD_STOPPED;
        pclog("Seek %08X\n", pos);
        ioctl_cd_pos = pos;
        ioctl_cd_state = CD_STOPPED;
        /*        pos+=150;
                CDROM_SEEK_AUDIO_MSF msf;
                msf.F=(uint8_t)(pos%75); pos/=75;
                msf.S=(uint8_t)(pos%60); pos/=60;
                msf.M=(uint8_t)(pos);
        //        pclog("Seek to %02i:%02i:%02i\n",msf.M,msf.S,msf.F);
                ioctl_open(0);
                DeviceIoControl(hIOCTL,IOCTL_CDROM_SEEK_AUDIO_MSF,&msf,sizeof(msf),NULL,0,&size,NULL);
                ioctl_close();*/
}

static int ioctl_ready(void) {
        long size;
        int temp;
        CDROM_TOC ltoc;
        //        pclog("Ready? %i\n",cdrom_drive);
        if (!cdrom_drive)
                return 0;
        ioctl_open(0);
        temp = DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC, NULL, 0, &ltoc, sizeof(ltoc), (LPDWORD)&size, NULL);
        ioctl_close();
        if (!temp)
                return 0;
        // pclog("ioctl_ready(): Drive opened successfully\n");
        // if ((cdrom_drive != old_cdrom_drive)) pclog("Drive has changed\n");
        if ((ltoc.TrackData[ltoc.LastTrack].Address[1] != toc.TrackData[toc.LastTrack].Address[1]) ||
            (ltoc.TrackData[ltoc.LastTrack].Address[2] != toc.TrackData[toc.LastTrack].Address[2]) ||
            (ltoc.TrackData[ltoc.LastTrack].Address[3] != toc.TrackData[toc.LastTrack].Address[3]) || !tocvalid ||
            (cdrom_drive != old_cdrom_drive)) {
                // pclog("ioctl_ready(): Disc or drive changed\n");
                ioctl_cd_state = CD_STOPPED;
                /*              pclog("Not ready %02X %02X %02X  %02X %02X %02X
                   %i\n",ltoc.TrackData[ltoc.LastTrack].Address[1],ltoc.TrackData[ltoc.LastTrack].Address[2],ltoc.TrackData[ltoc.LastTrack].Address[3],
                                                                                      toc.TrackData[ltoc.LastTrack].Address[1],
                   toc.TrackData[ltoc.LastTrack].Address[2], toc.TrackData[ltoc.LastTrack].Address[3],tocvalid);*/
                //        atapi_discchanged();
                /*                ioctl_open(0);
                                temp=DeviceIoControl(hIOCTL,IOCTL_CDROM_READ_TOC, NULL,0,&toc,sizeof(toc),&size,NULL);
                                ioctl_close();*/
                if (cdrom_drive != old_cdrom_drive)
                        old_cdrom_drive = cdrom_drive;
                return 1;
        }
        //        pclog("IOCTL says ready\n");
        //        pclog("ioctl_ready(): All is good\n");
        return 1;
}

/* Number of 2048-byte sectors on the disc. Uses the device length, since a leadout in MSF
   can't describe a disc longer than 255 minutes (i.e. any DVD). Falls back to the TOC leadout. */
static uint32_t ioctl_get_capacity(void) {
        GET_LENGTH_INFORMATION len_info;
        union {
                DISK_GEOMETRY_EX geom;
                uint8_t raw[256];
        } geom;
        CDROM_READ_TOC_EX toc_ex;
        CDROM_TOC lbtoc;
        DWORD size;
        LONGLONG bytes = 0;

        if (!cdrom_drive)
                return 0;

        ioctl_open(0);
        if (DeviceIoControl(hIOCTL, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &len_info, sizeof(len_info), &size, NULL))
                bytes = len_info.Length.QuadPart;
        else if (DeviceIoControl(hIOCTL, IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geom, sizeof(geom), &size, NULL))
                bytes = geom.geom.DiskSize.QuadPart;
        if (bytes <= 0) {
                /* TOC in LBA format, starting at the leadout (0xAA). */
                memset(&toc_ex, 0, sizeof(toc_ex));
                memset(&lbtoc, 0, sizeof(lbtoc));
                toc_ex.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
                toc_ex.Msf = 0;
                toc_ex.SessionTrack = 0xaa;
                if (DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC_EX, &toc_ex, sizeof(toc_ex), &lbtoc, sizeof(lbtoc), &size,
                                    NULL)) {
                        UCHAR *a = lbtoc.TrackData[0].Address;
                        bytes = (LONGLONG)(((uint32_t)a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]) * 2048;
                }
        }
        ioctl_close();
        return (uint32_t)(bytes / 2048);
}

static int ioctl_medium_changed(void) {
        long size;
        int temp;
        CDROM_TOC ltoc;
        if (!cdrom_drive)
                return 0; /* This will be handled by the not ready handler instead. */
        ioctl_open(0);
        temp = DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC, NULL, 0, &ltoc, sizeof(ltoc), (LPDWORD)&size, NULL);
        ioctl_close();
        if (!temp)
                return 0; /* Drive empty, a not ready handler matter, not disc change. */
        if (!tocvalid || (cdrom_drive != old_cdrom_drive)) {
                ioctl_cd_state = CD_STOPPED;
                toc = ltoc;
                tocvalid = 1;
                if (cdrom_drive != old_cdrom_drive)
                        old_cdrom_drive = cdrom_drive;
                cdrom_capacity = ioctl_get_capacity();
                return 0;
        }
        if ((ltoc.TrackData[ltoc.LastTrack].Address[1] != toc.TrackData[toc.LastTrack].Address[1]) ||
            (ltoc.TrackData[ltoc.LastTrack].Address[2] != toc.TrackData[toc.LastTrack].Address[2]) ||
            (ltoc.TrackData[ltoc.LastTrack].Address[3] != toc.TrackData[toc.LastTrack].Address[3])) {
                ioctl_cd_state = CD_STOPPED;
                toc = ltoc;
                cdrom_capacity = ioctl_get_capacity();
                return 1; /* TOC mismatches. */
        }
        return 0; /* None of the above, return 0. */
}

static uint8_t ioctl_getcurrentsubchannel(uint8_t *b, int msf) {
        CDROM_SUB_Q_DATA_FORMAT insub;
        SUB_Q_CHANNEL_DATA sub;
        long size;
        int pos = 0;
        if (!cdrom_drive)
                return 0;

        insub.Format = IOCTL_CDROM_CURRENT_POSITION;
        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_Q_CHANNEL, &insub, sizeof(insub), &sub, sizeof(sub), (LPDWORD)&size, NULL);
        ioctl_close();

        if (ioctl_cd_state == CD_PLAYING || ioctl_cd_state == CD_PAUSED) {
                uint32_t cdpos = ioctl_cd_pos;
                int track = get_track_nr(cdpos);
                uint32_t track_address = toc.TrackData[track].Address[3] + (toc.TrackData[track].Address[2] * 75) +
                                         (toc.TrackData[track].Address[1] * 75 * 60);

                b[pos++] = sub.CurrentPosition.Control;
                b[pos++] = track + 1;
                b[pos++] = sub.CurrentPosition.IndexNumber;

                if (msf) {
                        uint32_t dat = cdpos;
                        b[pos + 3] = (uint8_t)(dat % 75);
                        dat /= 75;
                        b[pos + 2] = (uint8_t)(dat % 60);
                        dat /= 60;
                        b[pos + 1] = (uint8_t)dat;
                        b[pos] = 0;
                        pos += 4;
                        dat = cdpos - track_address;
                        b[pos + 3] = (uint8_t)(dat % 75);
                        dat /= 75;
                        b[pos + 2] = (uint8_t)(dat % 60);
                        dat /= 60;
                        b[pos + 1] = (uint8_t)dat;
                        b[pos] = 0;
                        pos += 4;
                } else {
                        b[pos++] = (cdpos >> 24) & 0xff;
                        b[pos++] = (cdpos >> 16) & 0xff;
                        b[pos++] = (cdpos >> 8) & 0xff;
                        b[pos++] = cdpos & 0xff;
                        cdpos -= track_address;
                        b[pos++] = (cdpos >> 24) & 0xff;
                        b[pos++] = (cdpos >> 16) & 0xff;
                        b[pos++] = (cdpos >> 8) & 0xff;
                        b[pos++] = cdpos & 0xff;
                }

                if (ioctl_cd_state == CD_PLAYING)
                        return 0x11;
                return 0x12;
        }

        b[pos++] = sub.CurrentPosition.Control;
        b[pos++] = sub.CurrentPosition.TrackNumber;
        b[pos++] = sub.CurrentPosition.IndexNumber;

        if (msf) {
                int c;
                for (c = 0; c < 4; c++)
                        b[pos++] = sub.CurrentPosition.AbsoluteAddress[c];
                for (c = 0; c < 4; c++)
                        b[pos++] = sub.CurrentPosition.TrackRelativeAddress[c];
        } else {
                uint32_t temp = MSFtoLBA(sub.CurrentPosition.AbsoluteAddress[1], sub.CurrentPosition.AbsoluteAddress[2],
                                         sub.CurrentPosition.AbsoluteAddress[3]);
                b[pos++] = temp >> 24;
                b[pos++] = temp >> 16;
                b[pos++] = temp >> 8;
                b[pos++] = temp;
                temp = MSFtoLBA(sub.CurrentPosition.TrackRelativeAddress[1], sub.CurrentPosition.TrackRelativeAddress[2],
                                sub.CurrentPosition.TrackRelativeAddress[3]);
                b[pos++] = temp >> 24;
                b[pos++] = temp >> 16;
                b[pos++] = temp >> 8;
                b[pos++] = temp;
        }

        return 0x13;
}

static void ioctl_eject(void) {
        long size;
        if (!cdrom_drive)
                return;
        ioctl_cd_state = CD_STOPPED;
        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, (LPDWORD)&size, NULL);
        ioctl_close();
}

static void ioctl_load(void) {
        long size;
        if (!cdrom_drive)
                return;
        ioctl_cd_state = CD_STOPPED;
        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_STORAGE_LOAD_MEDIA, NULL, 0, NULL, 0, (LPDWORD)&size, NULL);
        ioctl_close();
        cdrom_capacity = ioctl_get_capacity();
}

#define IOCTL_DATA_BUFFER 8192
#define IOCTL_SPTI_MAX 65536

typedef struct {
        SCSI_PASS_THROUGH spt;
        ULONG filler;
        UCHAR sense[32];
        UCHAR data[IOCTL_SPTI_MAX];
} spti_buffered_t;

/* Send a CDB to the physical drive (buffered SPTI). See ATAPI.passthrough. */
static int ioctl_passthrough(const uint8_t *cdb, int cdb_len, uint8_t *buf, int *len, int data_in, uint8_t *sense) {
        spti_buffered_t *s;
        DWORD ret;
        int status = -1;
        int xfer = *len;

        if (!cdrom_drive || cdb_len > 16 || xfer < 0 || xfer > IOCTL_SPTI_MAX)
                return -1;
        s = calloc(1, sizeof(*s));
        if (!s)
                return -1;
        s->spt.Length = sizeof(SCSI_PASS_THROUGH);
        s->spt.CdbLength = cdb_len;
        s->spt.SenseInfoLength = 18;
        s->spt.DataIn = xfer ? (data_in ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT) : SCSI_IOCTL_DATA_UNSPECIFIED;
        s->spt.DataTransferLength = xfer;
        s->spt.TimeOutValue = 30;
        s->spt.DataBufferOffset = offsetof(spti_buffered_t, data);
        s->spt.SenseInfoOffset = offsetof(spti_buffered_t, sense);
        memcpy(s->spt.Cdb, cdb, cdb_len);
        if (!data_in && xfer)
                memcpy(s->data, buf, xfer);

        ioctl_open(0);
        if (hIOCTL && DeviceIoControl(hIOCTL, IOCTL_SCSI_PASS_THROUGH, s, sizeof(*s), s, sizeof(*s), &ret, NULL)) {
                status = s->spt.ScsiStatus;
                *len = (s->spt.DataTransferLength <= (ULONG)xfer) ? s->spt.DataTransferLength : xfer;
                if (data_in && *len)
                        memcpy(buf, s->data, *len);
                if (sense)
                        memcpy(sense, s->sense, 18);
        } else
                *len = 0;
        ioctl_close();
        free(s);
        return status;
}

/* CD or DVD, from the drive's current MMC profile; falls back to the disc size. */
static int ioctl_disc_type(void) {
        uint8_t cdb[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0}; /* GET CONFIGURATION, header only */
        uint8_t buf[8];
        int len = sizeof(buf);
        uint32_t capacity;

        if (!cdrom_drive)
                return CD_DISC_NONE;
        if (ioctl_passthrough(cdb, sizeof(cdb), buf, &len, 1, NULL) == 0 && len >= 8) {
                int profile = (buf[6] << 8) | buf[7];

                if (profile >= 0x08 && profile <= 0x0a)
                        return CD_DISC_CD;
                if (profile >= 0x10)
                        return CD_DISC_DVD; /* DVD-ROM/-R/-RW/+R/+RW/DL (and anything newer, read as DVD) */
                if (profile == 0)
                        return CD_DISC_NONE;
        }
        capacity = ioctl_get_capacity();
        if (!capacity)
                return CD_DISC_NONE;
        return (capacity > 450000) ? CD_DISC_DVD : CD_DISC_CD; /* 450000 sectors ~ a 100 minute CD */
}

typedef struct _SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER {
        SCSI_PASS_THROUGH_DIRECT spt;
        ULONG Filler;
        UCHAR SenseBuf[32];
} SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER;

static int ioctl_readsector(uint8_t *b, int sector, int count) {
        LARGE_INTEGER pos;
        BOOL status;
        long size = 0;

        if (!cdrom_drive)
                return -1;

        if (ioctl_cd_state == CD_PLAYING)
                return -1;

        /* ReadFile() past the end of the disc "succeeds" with 0 bytes: reject it here. */
        if (sector < 0 || (cdrom_capacity && (uint32_t)sector + count > cdrom_capacity))
                return -1;

        ioctl_cd_state = CD_STOPPED;
        pos.QuadPart = (LONGLONG)sector * 2048;
        ioctl_open(0);
        SetFilePointerEx(hIOCTL, pos, NULL, FILE_BEGIN);
        status = ReadFile(hIOCTL, b, count * 2048, (LPDWORD)&size, NULL);
        ioctl_close();

        /*If the read 'succeeded' but didn't read all required data, try again using SPTI*/
        if (status && size != count * 2048) {
                uint8_t cmd[12] = {0xbe, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
                SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER swb;
                DWORD returned;
                uint8_t *data_buffer; //[IOCTL_DATA_BUFFER];

                data_buffer = malloc(count * IOCTL_DATA_BUFFER);

                ioctl_open(0);

                cmd[9] |= 1 << 4; // userdata

                cmd[3] = (uint8_t)(sector >> 16);
                cmd[4] = (uint8_t)(sector >> 8);
                cmd[5] = (uint8_t)(sector >> 0);
                cmd[6] = (uint8_t)(count >> 16);
                cmd[7] = (uint8_t)(count >> 8);
                cmd[8] = (uint8_t)(count >> 0);

                memset(&swb, 0, sizeof(swb));
                memcpy(swb.spt.Cdb, cmd, 12);

                swb.spt.Length = sizeof(SCSI_PASS_THROUGH);
                swb.spt.CdbLength = 12;
                swb.spt.DataIn = SCSI_IOCTL_DATA_IN;
                swb.spt.DataTransferLength = count * IOCTL_DATA_BUFFER;
                swb.spt.DataBuffer = data_buffer;
                memset(data_buffer, 0, count * IOCTL_DATA_BUFFER);
                swb.spt.TimeOutValue = 80 * 60;
                swb.spt.SenseInfoOffset = (uintptr_t)&swb.SenseBuf - (uintptr_t)&swb; // offsetof(swb, SenseBuf);
                swb.spt.SenseInfoLength = 32;

                if (DeviceIoControl(hIOCTL, IOCTL_SCSI_PASS_THROUGH_DIRECT, &swb, sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER),
                                    &swb, sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER), &returned, NULL) &&
                    swb.spt.ScsiStatus == 0) {
                        memcpy(b, data_buffer, count * 2048);
                        status = 1;
                } else
                        status = 0;

                ioctl_close();

                free(data_buffer);
        }

        return !status;
}

static int is_track_audio(uint32_t pos) {
        int c;
        int control = 0;

        if (!tocvalid)
                return 0;

        for (c = 0; toc.TrackData[c].TrackNumber != 0xaa; c++) {
                uint32_t track_address = toc.TrackData[c].Address[3] + (toc.TrackData[c].Address[2] * 75) +
                                         (toc.TrackData[c].Address[1] * 75 * 60);

                if (toc.TrackData[c].TrackNumber >= toc.FirstTrack && toc.TrackData[c].TrackNumber <= toc.LastTrack &&
                    track_address >= pos) {
                        control = toc.TrackData[c].Control;
                        break;
                }
        }
        return (control & 4) ? 0 : 1;
}

static int ioctl_is_track_audio(uint32_t pos, int ismsf) {
        if (ismsf) {
                int m = (pos >> 16) & 0xff;
                int s = (pos >> 8) & 0xff;
                int f = pos & 0xff;
                pos = MSFtoLBA(m, s, f);
        }
        return is_track_audio(pos);
}

static void ioctl_readsector_raw(uint8_t *b, int sector) {
        long size;

        if (!cdrom_drive)
                return;
        if (ioctl_cd_state == CD_PLAYING)
                return;

        ioctl_cd_state = CD_STOPPED;
        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_CDROM_RAW_READ, NULL, 0, b, 1, (LPDWORD)&size, NULL);
        ioctl_close();
}

static int ioctl_readtoc(unsigned char *b, unsigned char starttrack, int msf, int maxlen, int single) {
        int len = 4;
        long size;
        int c, d;
        uint32_t temp;
        CDROM_READ_TOC_EX toc_ex;
        CDROM_TOC lba_toc;
        int lba_valid = 0;
        if (!cdrom_drive)
                return 0;
        ioctl_cd_state = CD_STOPPED;
        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC, NULL, 0, &toc, sizeof(toc), (LPDWORD)&size, NULL);
        if (!msf) {
                memset(&toc_ex, 0, sizeof(toc_ex));
                memset(&lba_toc, 0, sizeof(lba_toc));
                toc_ex.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
                toc_ex.Msf = 0;
                toc_ex.SessionTrack = toc.FirstTrack;
                lba_valid = DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC_EX, &toc_ex, sizeof(toc_ex), &lba_toc, sizeof(lba_toc),
                                            (LPDWORD)&size, NULL);
        }
        ioctl_close();
        tocvalid = 1;
        //        pclog("Read TOC done! %i\n",single);
        b[2] = toc.FirstTrack;
        b[3] = toc.LastTrack;
        d = 0;
        for (c = 0; c <= toc.LastTrack; c++) {
                if (toc.TrackData[c].TrackNumber >= starttrack) {
                        d = c;
                        break;
                }
        }
        last_block = 0;
        for (c = d; c <= toc.LastTrack; c++) {
                uint32_t address;
                if ((len + 8) > maxlen)
                        break;
                //                pclog("Len %i max %i Track %02X - %02X %02X %i %i %i %i
                //                %08X\n",len,maxlen,toc.TrackData[c].TrackNumber,toc.TrackData[c].Adr,toc.TrackData[c].Control,toc.TrackData[c].Address[0],toc.TrackData[c].Address[1],toc.TrackData[c].Address[2],toc.TrackData[c].Address[3],MSFtoLBA(toc.TrackData[c].Address[1],toc.TrackData[c].Address[2],toc.TrackData[c].Address[3]));
                b[len++] = 0; /*Reserved*/
                b[len++] = (toc.TrackData[c].Adr << 4) | toc.TrackData[c].Control;
                b[len++] = toc.TrackData[c].TrackNumber;
                b[len++] = 0; /*Reserved*/
                address = MSFtoLBA(toc.TrackData[c].Address[1], toc.TrackData[c].Address[2], toc.TrackData[c].Address[3]);
                if (address > last_block)
                        last_block = address;

                if (msf) {
                        b[len++] = toc.TrackData[c].Address[0];
                        b[len++] = toc.TrackData[c].Address[1];
                        b[len++] = toc.TrackData[c].Address[2];
                        b[len++] = toc.TrackData[c].Address[3];
                } else if (lba_valid) {
                        memcpy(&b[len], lba_toc.TrackData[c].Address, 4);
                        len += 4;
                } else {
                        temp = MSFtoLBA(toc.TrackData[c].Address[1], toc.TrackData[c].Address[2], toc.TrackData[c].Address[3]) -
                               150;
                        b[len++] = temp >> 24;
                        b[len++] = temp >> 16;
                        b[len++] = temp >> 8;
                        b[len++] = temp;
                }
                if (single)
                        break;
        }
        b[0] = (uint8_t)(((len - 2) >> 8) & 0xff);
        b[1] = (uint8_t)((len - 2) & 0xff);
        /*        pclog("Table of Contents (%i bytes) : \n",size);
                pclog("First track - %02X\n",toc.FirstTrack);
                pclog("Last  track - %02X\n",toc.LastTrack);
                for (c=0;c<=toc.LastTrack;c++)
                    pclog("Track %02X - number %02X control %02X adr %02X address %02X %02X %02X
           %02X\n",c,toc.TrackData[c].TrackNumber,toc.TrackData[c].Control,toc.TrackData[c].Adr,toc.TrackData[c].Address[0],toc.TrackData[c].Address[1],toc.TrackData[c].Address[2],toc.TrackData[c].Address[3]);
                for (c=0;c<=toc.LastTrack;c++)
                    pclog("Track %02X - number %02X control %02X adr %02X address
           %06X\n",c,toc.TrackData[c].TrackNumber,toc.TrackData[c].Control,toc.TrackData[c].Adr,MSFtoLBA(toc.TrackData[c].Address[1],toc.TrackData[c].Address[2],toc.TrackData[c].Address[3]));*/
        return len;
}

static int ioctl_readtoc_session(unsigned char *b, int msf, int maxlen) {
        int len = 4;
        int size;
        uint32_t temp;
        CDROM_READ_TOC_EX toc_ex;
        CDROM_TOC_SESSION_DATA toc;
        if (!cdrom_drive)
                return 0;
        ioctl_cd_state = CD_STOPPED;
        memset(&toc_ex, 0, sizeof(toc_ex));
        memset(&toc, 0, sizeof(toc));
        toc_ex.Format = CDROM_READ_TOC_EX_FORMAT_SESSION;
        toc_ex.Msf = msf;
        toc_ex.SessionTrack = 0;
        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC_EX, &toc_ex, sizeof(toc_ex), &toc, sizeof(toc), (PDWORD)&size, NULL);
        ioctl_close();
        //        pclog("Read TOC session - %i %02X %02X %i %i %02X %02X
        //        %02X\n",size,toc.Length[0],toc.Length[1],toc.FirstCompleteSession,toc.LastCompleteSession,toc.TrackData[0].Adr,toc.TrackData[0].Control,toc.TrackData[0].TrackNumber);
        b[2] = toc.FirstCompleteSession;
        b[3] = toc.LastCompleteSession;
        b[len++] = 0; /*Reserved*/
        b[len++] = (toc.TrackData[0].Adr << 4) | toc.TrackData[0].Control;
        b[len++] = toc.TrackData[0].TrackNumber;
        b[len++] = 0; /*Reserved*/
        if (msf) {
                b[len++] = toc.TrackData[0].Address[0];
                b[len++] = toc.TrackData[0].Address[1];
                b[len++] = toc.TrackData[0].Address[2];
                b[len++] = toc.TrackData[0].Address[3];
        } else {
                temp = MSFtoLBA(toc.TrackData[0].Address[1], toc.TrackData[0].Address[2], toc.TrackData[0].Address[3]);
                b[len++] = temp >> 24;
                b[len++] = temp >> 16;
                b[len++] = temp >> 8;
                b[len++] = temp;
        }

        return len;
}

static int ioctl_readtoc_raw(unsigned char *b, int maxlen) {
        int len = 4;
        int size;
        int i;
        CDROM_READ_TOC_EX toc_ex;
        CDROM_TOC_FULL_TOC_DATA toc;
        if (!cdrom_drive)
                return 0;
        ioctl_cd_state = CD_STOPPED;
        memset(&toc_ex, 0, sizeof(toc_ex));
        memset(&toc, 0, sizeof(toc));
        toc_ex.Format = CDROM_READ_TOC_EX_FORMAT_FULL_TOC;
        toc_ex.Msf = 1;
        toc_ex.SessionTrack = 0;
        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC_EX, &toc_ex, sizeof(toc_ex), &toc, sizeof(toc), (PDWORD)&size, NULL);
        ioctl_close();
        //        pclog("Read TOC session - %i %02X %02X %i %i %02X %02X
        //        %02X\n",size,toc.Length[0],toc.Length[1],toc.FirstCompleteSession,toc.LastCompleteSession,toc.TrackData[0].Adr,toc.TrackData[0].Control,toc.TrackData[0].TrackNumber);
        b[2] = toc.FirstCompleteSession;
        b[3] = toc.LastCompleteSession;

        size -= sizeof(CDROM_TOC_FULL_TOC_DATA);
        size /= sizeof(toc.Descriptors[0]);

        for (i = 0; i <= size; i++) {
                b[len++] = toc.Descriptors[i].SessionNumber;
                b[len++] = (toc.Descriptors[i].Adr << 4) | toc.Descriptors[i].Control;
                b[len++] = 0;
                b[len++] = toc.Descriptors[i].Reserved1; /*Reserved*/
                b[len++] = toc.Descriptors[i].MsfExtra[0];
                b[len++] = toc.Descriptors[i].MsfExtra[1];
                b[len++] = toc.Descriptors[i].MsfExtra[2];
                b[len++] = toc.Descriptors[i].Zero;
                b[len++] = toc.Descriptors[i].Msf[0];
                b[len++] = toc.Descriptors[i].Msf[1];
                b[len++] = toc.Descriptors[i].Msf[2];
        }

        return len;
}

static uint32_t ioctl_size() { return cdrom_capacity; }

static int ioctl_status() {
        if (!(ioctl_ready()) && (cdrom_drive <= 0))
                return CD_STATUS_EMPTY;

        switch (ioctl_cd_state) {
        case CD_PLAYING:
                return CD_STATUS_PLAYING;
        case CD_PAUSED:
                return CD_STATUS_PAUSED;
        case CD_STOPPED:
                return CD_STATUS_STOPPED;
        }
        return CD_STATUS_STOPPED;
}

void ioctl_reset() {
        CDROM_TOC ltoc;
        long size;

        if (!cdrom_drive) {
                tocvalid = 0;
                return;
        }

        ioctl_open(0);
        DeviceIoControl(hIOCTL, IOCTL_CDROM_READ_TOC, NULL, 0, &ltoc, sizeof(ltoc), (LPDWORD)&size, NULL);
        ioctl_close();

        toc = ltoc;
        tocvalid = 1;
}

void ioctl_set_drive(char d) {
        ioctl_close();

        sprintf(ioctl_path, "\\\\.\\%c:", d);
        pclog("Path is %s\n", ioctl_path);
        tocvalid = 0;
        atapi = &ioctl_atapi;

        ioctl_open(d);
}

int ioctl_open(char d) {
        if (hIOCTL)
                ioctl_close();
        hIOCTL = ioctl_open_handle();
        /* Drive unplugged or no access: leave hIOCTL NULL so DeviceIoControl() fails and the
           guest just sees "no medium". */
        if (hIOCTL == INVALID_HANDLE_VALUE)
                hIOCTL = NULL;
        return 0;
}

void ioctl_close(void) {
        if (hIOCTL) {
                CloseHandle(hIOCTL);
                hIOCTL = NULL;
        }
}

static void ioctl_exit(void) {
        ioctl_stop();
        tocvalid = 0;
}

static ATAPI ioctl_atapi = {ioctl_ready,
                            ioctl_medium_changed,
                            ioctl_readtoc,
                            ioctl_readtoc_session,
                            ioctl_readtoc_raw,
                            ioctl_getcurrentsubchannel,
                            ioctl_readsector,
                            ioctl_readsector_raw,
                            ioctl_playaudio,
                            ioctl_seek,
                            ioctl_load,
                            ioctl_eject,
                            ioctl_pause,
                            ioctl_resume,
                            ioctl_size,
                            ioctl_status,
                            ioctl_is_track_audio,
                            ioctl_stop,
                            ioctl_exit,
                            ioctl_disc_type,
                            ioctl_passthrough};
