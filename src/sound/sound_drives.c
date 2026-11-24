/*Synthesised drive noise.

  Nothing here is sampled: every sound is built from white noise, band-pass filters,
  sine oscillators and banks of damped resonators (a cheap modal model of a drive
  chassis being struck).

  - Floppy drives: spindle motor, media rubbing in the jacket, head stepping
    (one strike per track), head load solenoid, disk insertion.
  - Hard disks: spindle spin-up and steady hum/whine, plus seeks. Stepper motor
    drives step once per cylinder with acceleration; voice coil drives make a
    "tick-buzz-tock" whose length and strength depend on the seek distance.
  - CD/DVD drives: spin-up/spin-down with speed depending on the drive, CLV speed
    change across the disc for slow drives, optical sled seeks, and the tray.

  Events are timestamped with the sample position inside the current sound buffer,
  so rapid steps and seeks keep their real spacing. Seeks that arrive while the
  mechanism is still moving are merged into one pending seek, which keeps the
  sound rate-limited to what a real mechanism could do.*/
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ibm.h"
#include "device.h"
#include "fdd.h"
#include "disc.h"
#include "hdd.h"
#include "sound.h"
#include "sound_drives.h"

#define DS_RATE 48000
#define DS_PI 3.14159265358979f
#define DS_TWO_PI 6.28318530717959f
#define DS_SCALE 8000.0f

#define MAX_MODES 5
#define MAX_HDD 7
#define MAX_TRAY 4

#define MS(x) ((uint64_t)((x) * (DS_RATE / 1000.0f)))

int drive_sounds_enabled = 0;

static volatile int ui_fdd_insert_count[2];
static volatile int ui_cd_change_count;

/* ---------------------------------------------------------------- DSP helpers */

static inline float frand(uint32_t *s) {
        *s ^= *s << 13;
        *s ^= *s >> 17;
        *s ^= *s << 5;
        return (float)(int32_t)*s * (1.0f / 2147483648.0f);
}

/*Chamberlin state variable filter, band-pass output*/
typedef struct {
        float f, q, lp, bp;
} svf_t;

static void svf_set(svf_t *s, float fc, float q) {
        if (fc > DS_RATE / 7)
                fc = DS_RATE / 7;
        s->f = 2.0f * sinf(DS_PI * fc / DS_RATE);
        s->q = 1.0f / q;
}

static inline float svf_bp(svf_t *s, float x) {
        float hp = x - s->lp - s->q * s->bp;
        s->bp += s->f * hp;
        s->lp += s->f * s->bp;
        return s->bp;
}

typedef struct {
        float f, tau, g;
} drv_mode_t;

/*A bank of damped resonators struck by impulses and short noise bursts*/
typedef struct {
        float b1[MAX_MODES], b2[MAX_MODES], g[MAX_MODES], y1[MAX_MODES], y2[MAX_MODES];
        int nmodes;
        float x;
        float burst, burst_k, burst_ratio, burst_direct;
        float lp, lp_a;
        int active;
        uint32_t seed;
} body_t;

static void body_init(body_t *b, const drv_mode_t *m, float burst_ms, float burst_ratio, float burst_lp_hz, float burst_direct,
                      uint32_t seed) {
        int c;

        memset(b, 0, sizeof(body_t));
        for (c = 0; c < MAX_MODES && m[c].f > 0; c++) {
                float w = DS_TWO_PI * m[c].f / DS_RATE;
                float rad = expf(-1.0f / (m[c].tau * DS_RATE));

                b->b1[c] = 2.0f * rad * cosf(w);
                b->b2[c] = -rad * rad;
                b->g[c] = m[c].g * sinf(w);
        }
        b->nmodes = c;
        b->burst_k = expf(-1.0f / (burst_ms * (DS_RATE / 1000.0f)));
        b->burst_ratio = burst_ratio;
        b->burst_direct = burst_direct;
        b->lp_a = 1.0f - expf(-DS_TWO_PI * burst_lp_hz / DS_RATE);
        b->seed = seed;
}

static void body_excite(body_t *b, float amp) {
        b->x += amp;
        b->burst += amp * b->burst_ratio;
        b->active = DS_RATE / 2;
}

static inline float body_run(body_t *b) {
        float in, out, nz;
        int c;

        if (!b->active)
                return 0.0f;

        nz = frand(&b->seed) * b->burst;
        b->burst *= b->burst_k;
        b->lp += b->lp_a * (nz - b->lp);

        /*Only the impulse drives the resonators. Feeding the noise burst into the low,
          slowly decaying modes amplifies it enormously during fast step trains.*/
        in = b->x;
        b->x = 0.0f;
        out = b->lp * b->burst_direct;
        for (c = 0; c < b->nmodes; c++) {
                float y = in * b->g[c] + b->b1[c] * b->y1[c] + b->b2[c] * b->y2[c];
                b->y2[c] = b->y1[c];
                b->y1[c] = y;
                out += y;
        }

        if (!--b->active) {
                memset(b->y1, 0, sizeof(b->y1));
                memset(b->y2, 0, sizeof(b->y2));
                b->burst = b->lp = 0.0f;
        }
        return out;
}

/*Spinning motor: rotation harmonics, electrical whine and air noise. speed is 0..1 of full speed*/
typedef struct {
        float speed, target;
        float up_rate, down_k;
        float rot_hz;
        float hum[3];
        float whine_mult, whine_amp;
        float air_fc, air_q, air_amp;
        float ph_rot, ph_whine;
        svf_t air;
        int air_upd;
        uint32_t seed;
} spindle_t;

static void spindle_init(spindle_t *s, float rpm, float up_s, float down_s, const float hum[3], float whine_mult, float whine_amp,
                         float air_fc, float air_amp, uint32_t seed) {
        memset(s, 0, sizeof(spindle_t));
        s->rot_hz = rpm / 60.0f;
        s->up_rate = 1.0f / (up_s * DS_RATE);
        s->down_k = 1.0f / (down_s * DS_RATE / 3.0f);
        memcpy(s->hum, hum, sizeof(s->hum));
        s->whine_mult = whine_mult;
        s->whine_amp = whine_amp;
        s->air_fc = air_fc;
        s->air_q = 0.7f;
        s->air_amp = air_amp;
        s->seed = seed;
}

static inline float spindle_run(spindle_t *s) {
        float f, out, sp;

        if (s->speed < s->target) {
                s->speed += s->up_rate;
                if (s->speed > s->target)
                        s->speed = s->target;
        } else if (s->speed > s->target) {
                s->speed -= (s->speed - s->target) * s->down_k + s->up_rate * 0.01f;
                if (s->speed < s->target)
                        s->speed = s->target;
        }
        sp = s->speed;
        if (sp < 0.0005f)
                return 0.0f;

        f = s->rot_hz * sp;
        s->ph_rot += f / DS_RATE;
        if (s->ph_rot >= 1.0f)
                s->ph_rot -= 1.0f;
        s->ph_whine += f * s->whine_mult / DS_RATE;
        if (s->ph_whine >= 1.0f)
                s->ph_whine -= floorf(s->ph_whine);

        out = (s->hum[0] * sinf(DS_TWO_PI * s->ph_rot) + s->hum[1] * sinf(2.0f * DS_TWO_PI * s->ph_rot) +
               s->hum[2] * sinf(3.0f * DS_TWO_PI * s->ph_rot)) *
              sp;
        out += s->whine_amp * sinf(DS_TWO_PI * s->ph_whine) * sp;

        if (!s->air_upd--) {
                svf_set(&s->air, s->air_fc * (0.25f + 0.75f * sp), s->air_q);
                s->air_upd = 256;
        }
        out += svf_bp(&s->air, frand(&s->seed)) * s->air_amp * sp * sp;

        return out;
}

/* ---------------------------------------------------------------- profiles */

typedef struct {
        float step_ms, step_amp;
        drv_mode_t modes[MAX_MODES];
        float burst_ms, burst_ratio, burst_lp_hz, burst_direct;
        float headload_amp;
        float spin_up_ms, spin_down_ms;
        float hum_hz, hum_amp;
        float motor_fc, motor_q, motor_amp;
        float media_fc, media_q, media_amp;
        float rot_mod;
        float insert_amp;
} fdd_profile_t;

static const fdd_profile_t fdd_profiles_525[] = {
        {/*Full-height, belt driven, head load solenoid (early 1980s)*/
         .step_ms = 6.0f, .step_amp = 0.4f,
         .modes = {{125, 0.03f, 0.5f}, {410, 0.025f, 0.5f}, {1150, 0.012f, 0.35f}, {2600, 0.006f, 0.25f}, {4300, 0.004f, 0.12f}},
         .burst_ms = 3.0f, .burst_ratio = 0.6f, .burst_lp_hz = 5000, .burst_direct = 0.3f,
         .headload_amp = 0.7f,
         .spin_up_ms = 300, .spin_down_ms = 700,
         .hum_hz = 50, .hum_amp = 0.05f,
         .motor_fc = 900, .motor_q = 1.5f, .motor_amp = 0.09f,
         .media_fc = 2500, .media_q = 0.8f, .media_amp = 0.06f,
         .rot_mod = 0.35f,
         .insert_amp = 0.7f},
        {/*Half-height, direct drive (late 1980s)*/
         .step_ms = 3.0f, .step_amp = 0.4f,
         .modes = {{260, 0.02f, 0.5f}, {700, 0.015f, 0.5f}, {1800, 0.008f, 0.35f}, {3500, 0.005f, 0.2f}, {5200, 0.003f, 0.1f}},
         .burst_ms = 2.0f, .burst_ratio = 0.5f, .burst_lp_hz = 6000, .burst_direct = 0.3f,
         .headload_amp = 0.35f,
         .spin_up_ms = 300, .spin_down_ms = 450,
         .hum_hz = 150, .hum_amp = 0.02f,
         .motor_fc = 1200, .motor_q = 1.0f, .motor_amp = 0.045f,
         .media_fc = 3000, .media_q = 0.8f, .media_amp = 0.055f,
         .rot_mod = 0.3f,
         .insert_amp = 0.5f},
};

static const fdd_profile_t fdd_profiles_35[] = {
        {/*Standard 3.5" drive*/
         .step_ms = 3.0f, .step_amp = 0.35f,
         .modes = {{520, 0.012f, 0.4f}, {1300, 0.01f, 0.55f}, {2900, 0.006f, 0.4f}, {4700, 0.004f, 0.25f}, {5800, 0.003f, 0.12f}},
         .burst_ms = 1.5f, .burst_ratio = 0.5f, .burst_lp_hz = 7000, .burst_direct = 0.35f,
         .headload_amp = 0.0f,
         .spin_up_ms = 200, .spin_down_ms = 300,
         .hum_hz = 100, .hum_amp = 0.015f,
         .motor_fc = 1500, .motor_q = 1.2f, .motor_amp = 0.03f,
         .media_fc = 3500, .media_q = 0.7f, .media_amp = 0.04f,
         .rot_mod = 0.25f,
         .insert_amp = 0.6f},
        {/*Quiet late 1990s 3.5" drive*/
         .step_ms = 3.0f, .step_amp = 0.22f,
         .modes = {{700, 0.008f, 0.35f}, {1700, 0.007f, 0.5f}, {3300, 0.005f, 0.35f}, {5200, 0.003f, 0.2f}},
         .burst_ms = 1.0f, .burst_ratio = 0.4f, .burst_lp_hz = 8000, .burst_direct = 0.3f,
         .headload_amp = 0.0f,
         .spin_up_ms = 150, .spin_down_ms = 250,
         .hum_hz = 120, .hum_amp = 0.008f,
         .motor_fc = 1800, .motor_q = 1.2f, .motor_amp = 0.018f,
         .media_fc = 4000, .media_q = 0.7f, .media_amp = 0.025f,
         .rot_mod = 0.2f,
         .insert_amp = 0.45f},
};

enum { HDD_PROF_AUTO = 0, HDD_PROF_STEPPER, HDD_PROF_VOICECOIL, HDD_PROF_5400, HDD_PROF_7200 };

typedef struct {
        float rpm, spin_up_s, spin_down_s;
        float hum[3];
        float whine_mult, whine_amp;
        float air_fc, air_amp;
        int stepper;
        float step_ms_start, step_ms_min, step_amp;
        float seek_ms_min, seek_ms_max;
        float seek_start_amp, seek_end_amp, track_amp;
        float buzz_fc, buzz_q, buzz_amp;
        drv_mode_t modes[MAX_MODES];
        float burst_ms, burst_ratio, burst_lp_hz, burst_direct;
        float unpark_amp;
} hdd_profile_t;

static const hdd_profile_t hdd_profiles[] = {
        [HDD_PROF_STEPPER] = {/*Full-height MFM drive with stepper motor positioner*/
                              .rpm = 3600, .spin_up_s = 8.0f, .spin_down_s = 10.0f,
                              .hum = {0.04f, 0.03f, 0.015f},
                              .whine_mult = 40, .whine_amp = 0.01f,
                              .air_fc = 800, .air_amp = 0.06f,
                              .stepper = 1, .step_ms_start = 3.0f, .step_ms_min = 0.6f, .step_amp = 0.18f,
                              .seek_end_amp = 0.4f, .track_amp = 0.18f,
                              .modes = {{150, 0.04f, 0.6f}, {380, 0.03f, 0.6f}, {950, 0.015f, 0.4f}, {2200, 0.008f, 0.25f},
                                        {4000, 0.004f, 0.1f}},
                              .burst_ms = 2.0f, .burst_ratio = 0.5f, .burst_lp_hz = 4000, .burst_direct = 0.25f,
                              .unpark_amp = 0.0f},
        [HDD_PROF_VOICECOIL] = {/*Voice coil, 3600 RPM (late 1980s / early 1990s)*/
                                .rpm = 3600, .spin_up_s = 6.0f, .spin_down_s = 8.0f,
                                .hum = {0.03f, 0.02f, 0.01f},
                                .whine_mult = 20, .whine_amp = 0.012f,
                                .air_fc = 1000, .air_amp = 0.05f,
                                .seek_ms_min = 3.0f, .seek_ms_max = 28.0f,
                                .seek_start_amp = 0.5f, .seek_end_amp = 0.6f, .track_amp = 0.12f,
                                .buzz_fc = 900, .buzz_q = 1.5f, .buzz_amp = 0.25f,
                                .modes = {{220, 0.03f, 0.5f}, {640, 0.02f, 0.6f}, {1500, 0.01f, 0.4f}, {3100, 0.006f, 0.25f},
                                          {4800, 0.004f, 0.12f}},
                                .burst_ms = 1.5f, .burst_ratio = 0.5f, .burst_lp_hz = 5000, .burst_direct = 0.25f,
                                .unpark_amp = 0.7f},
        [HDD_PROF_5400] = {/*5400 RPM (1990s)*/
                           .rpm = 5400, .spin_up_s = 5.0f, .spin_down_s = 7.0f,
                           .hum = {0.02f, 0.012f, 0.006f},
                           .whine_mult = 8, .whine_amp = 0.006f,
                           .air_fc = 1500, .air_amp = 0.035f,
                           .seek_ms_min = 2.0f, .seek_ms_max = 20.0f,
                           .seek_start_amp = 0.32f, .seek_end_amp = 0.4f, .track_amp = 0.07f,
                           .buzz_fc = 1600, .buzz_q = 1.2f, .buzz_amp = 0.15f,
                           .modes = {{400, 0.012f, 0.45f}, {1100, 0.01f, 0.6f}, {2400, 0.006f, 0.45f}, {4100, 0.004f, 0.3f},
                                     {5600, 0.003f, 0.15f}},
                           .burst_ms = 1.0f, .burst_ratio = 0.45f, .burst_lp_hz = 6000, .burst_direct = 0.25f,
                           .unpark_amp = 0.45f},
        [HDD_PROF_7200] = {/*7200 RPM (late 1990s / 2000s)*/
                           .rpm = 7200, .spin_up_s = 4.0f, .spin_down_s = 6.0f,
                           .hum = {0.015f, 0.01f, 0.005f},
                           .whine_mult = 6, .whine_amp = 0.008f,
                           .air_fc = 2000, .air_amp = 0.03f,
                           .seek_ms_min = 1.5f, .seek_ms_max = 16.0f,
                           .seek_start_amp = 0.28f, .seek_end_amp = 0.32f, .track_amp = 0.06f,
                           .buzz_fc = 2200, .buzz_q = 1.2f, .buzz_amp = 0.13f,
                           .modes = {{700, 0.008f, 0.4f}, {1700, 0.007f, 0.6f}, {3200, 0.005f, 0.45f}, {4800, 0.003f, 0.3f},
                                     {5800, 0.002f, 0.15f}},
                           .burst_ms = 0.8f, .burst_ratio = 0.45f, .burst_lp_hz = 7000, .burst_direct = 0.25f,
                           .unpark_amp = 0.35f},
};

enum { CD_PROF_AUTO = 0, CD_PROF_2X, CD_PROF_8X, CD_PROF_52X, CD_PROF_DVD };

typedef struct {
        float max_rpm, audio_rpm;
        int clv;
        float spin_up_s, spin_down_s, idle_s;
        float hum[3];
        float whine_mult, whine_amp;
        float air_fc, air_amp;
        float sled_ms_min, sled_ms_max, sled_fc, sled_amp;
        float tray_ms, tray_fc, tray_amp;
        float tray_clunk_amp, sled_stop_amp;
        drv_mode_t modes[MAX_MODES];
        uint32_t max_lba;
} cd_profile_t;

static const cd_profile_t cd_profiles[] = {
        [CD_PROF_2X] = {/*1x-2x CD-ROM, CLV*/
                        .max_rpm = 1000, .audio_rpm = 400, .clv = 1,
                        .spin_up_s = 0.6f, .spin_down_s = 1.5f, .idle_s = 30.0f,
                        .hum = {0.01f, 0.004f, 0.0f},
                        .whine_mult = 6, .whine_amp = 0.008f,
                        .air_fc = 1200, .air_amp = 0.012f,
                        .sled_ms_min = 40, .sled_ms_max = 300, .sled_fc = 600, .sled_amp = 0.12f,
                        .tray_ms = 1100, .tray_fc = 350, .tray_amp = 0.09f,
                        .tray_clunk_amp = 0.55f, .sled_stop_amp = 0.12f,
                        .modes = {{330, 0.02f, 0.4f}, {880, 0.012f, 0.5f}, {2100, 0.006f, 0.35f}, {3800, 0.004f, 0.2f}},
                        .max_lba = 333000},
        [CD_PROF_8X] = {/*4x-8x CD-ROM, CLV*/
                        .max_rpm = 4000, .audio_rpm = 400, .clv = 1,
                        .spin_up_s = 1.0f, .spin_down_s = 2.5f, .idle_s = 30.0f,
                        .hum = {0.02f, 0.008f, 0.003f},
                        .whine_mult = 6, .whine_amp = 0.012f,
                        .air_fc = 1800, .air_amp = 0.03f,
                        .sled_ms_min = 25, .sled_ms_max = 180, .sled_fc = 800, .sled_amp = 0.1f,
                        .tray_ms = 1000, .tray_fc = 380, .tray_amp = 0.09f,
                        .tray_clunk_amp = 0.55f, .sled_stop_amp = 0.1f,
                        .modes = {{350, 0.02f, 0.4f}, {900, 0.012f, 0.5f}, {2100, 0.006f, 0.35f}, {3800, 0.004f, 0.2f}},
                        .max_lba = 333000},
        [CD_PROF_52X] = {/*24x-52x CD-ROM, CAV*/
                         .max_rpm = 10000, .audio_rpm = 450, .clv = 0,
                         .spin_up_s = 2.0f, .spin_down_s = 4.0f, .idle_s = 20.0f,
                         .hum = {0.05f, 0.015f, 0.006f},
                         .whine_mult = 6, .whine_amp = 0.02f,
                         .air_fc = 2500, .air_amp = 0.09f,
                         .sled_ms_min = 15, .sled_ms_max = 100, .sled_fc = 1000, .sled_amp = 0.09f,
                         .tray_ms = 900, .tray_fc = 420, .tray_amp = 0.09f,
                         .tray_clunk_amp = 0.5f, .sled_stop_amp = 0.1f,
                         .modes = {{380, 0.018f, 0.4f}, {950, 0.01f, 0.5f}, {2300, 0.006f, 0.35f}, {4000, 0.004f, 0.2f}},
                         .max_lba = 333000},
        [CD_PROF_DVD] = {/*DVD-ROM, CAV*/
                         .max_rpm = 9000, .audio_rpm = 450, .clv = 0,
                         .spin_up_s = 1.8f, .spin_down_s = 4.0f, .idle_s = 20.0f,
                         .hum = {0.03f, 0.01f, 0.004f},
                         .whine_mult = 6, .whine_amp = 0.015f,
                         .air_fc = 2300, .air_amp = 0.07f,
                         .sled_ms_min = 15, .sled_ms_max = 120, .sled_fc = 1100, .sled_amp = 0.08f,
                         .tray_ms = 900, .tray_fc = 420, .tray_amp = 0.08f,
                         .tray_clunk_amp = 0.5f, .sled_stop_amp = 0.1f,
                         .modes = {{400, 0.018f, 0.4f}, {1000, 0.01f, 0.5f}, {2400, 0.006f, 0.35f}, {4200, 0.004f, 0.2f}},
                         .max_lba = 2300000},
};

/* ---------------------------------------------------------------- state */

typedef struct {
        int type; /*fdd_get_type(), 0 = no drive*/
        const fdd_profile_t *prof;
        body_t body;
        svf_t motor_bp, media_bp;
        int motor_on;
        float speed, up_rate, down_k;
        float ph_rot, ph_hum, rot_hz;
        int track, target, pending;
        uint64_t pend_t;
        int steps_left, dir;
        uint64_t next_step_t;
        uint32_t seed;
} fdd_snd_t;

typedef struct {
        int present;
        const hdd_profile_t *prof;
        int cylinders;
        spindle_t sp;
        body_t body;
        svf_t buzz;
        int cyl, target, pending;
        uint64_t pend_t, busy_until;
        int steps_left, steps_done, dir;
        uint64_t next_step_t;
        uint64_t buzz_start, buzz_end;
        float buzz_amp;
        uint64_t end_hit_t;
        float end_hit_amp;
        int unparked;
        uint32_t seed;
} hdd_snd_t;

typedef struct {
        int present;
        const cd_profile_t *prof;
        spindle_t sp;
        body_t body;
        svf_t sled_bp, tray_bp;
        int audio;
        uint64_t last_activity;
        uint32_t head_lba, target_lba;
        int pending;
        uint64_t pend_t, busy_until;
        uint64_t sled_start, sled_end;
        float sled_amp;
        struct {
                uint64_t start, end;
                int clunked;
        } tray[MAX_TRAY];
        int tray_open;
        uint64_t probe_t;
        float ph_tray;
        uint32_t seed;
} cd_snd_t;

typedef struct {
        uint64_t base;
        float gain_fdd, gain_hdd, gain_cd;
        int prof_525, prof_35, prof_hdd, prof_cd;
        fdd_snd_t fdd[2];
        hdd_snd_t hdd[MAX_HDD];
        cd_snd_t cd;
        int ui_fdd_seen[2], ui_cd_seen;
} drivesnd_t;

static drivesnd_t *ds = NULL;

static inline uint64_t ds_now(void) { return ds->base + (uint64_t)sound_pos_global; }

/* ---------------------------------------------------------------- floppy */

static void fdd_setup(fdd_snd_t *d, int drive, int type) {
        const fdd_profile_t *p;
        int track = d->track;

        memset(d, 0, sizeof(fdd_snd_t));
        d->type = type;
        d->track = d->target = track;
        if (!type)
                return;

        p = fdd_is_525(drive) ? &fdd_profiles_525[ds->prof_525] : &fdd_profiles_35[ds->prof_35];
        d->prof = p;
        d->seed = 0x1234567 + drive * 7919;
        body_init(&d->body, p->modes, p->burst_ms, p->burst_ratio, p->burst_lp_hz, p->burst_direct, d->seed ^ 0xabcdef);
        svf_set(&d->motor_bp, p->motor_fc, p->motor_q);
        svf_set(&d->media_bp, p->media_fc, p->media_q);
        d->up_rate = 1.0f / (p->spin_up_ms * (DS_RATE / 1000.0f));
        d->down_k = 3.0f / (p->spin_down_ms * (DS_RATE / 1000.0f));
        d->rot_hz = (type == 2) ? 6.0f : 5.0f; /*5.25" HD drives spin at 360 RPM*/
}

static inline float fdd_run(fdd_snd_t *d, int drive, uint64_t n) {
        const fdd_profile_t *p = d->prof;
        float out;

        if (!d->steps_left && d->pending && n >= d->pend_t) {
                d->pending = 0;
                d->steps_left = abs(d->target - d->track);
                d->dir = (d->target > d->track) ? 1 : -1;
                d->next_step_t = n;
        }
        if (d->steps_left && n >= d->next_step_t) {
                body_excite(&d->body, p->step_amp * (0.9f + 0.15f * frand(&d->seed)));
                d->track += d->dir;
                d->steps_left--;
                d->next_step_t = n + MS(p->step_ms);
        }

        out = body_run(&d->body);

        if (d->motor_on) {
                d->speed += d->up_rate;
                if (d->speed > 1.0f)
                        d->speed = 1.0f;
        } else if (d->speed > 0.0f) {
                d->speed -= d->speed * d->down_k + 0.00002f;
                if (d->speed < 0.0f)
                        d->speed = 0.0f;
        }

        if (d->speed > 0.0f) {
                float sp = d->speed, mod, hum;

                d->ph_rot += d->rot_hz * sp / DS_RATE;
                if (d->ph_rot >= 1.0f)
                        d->ph_rot -= 1.0f;
                d->ph_hum += p->hum_hz * sp / DS_RATE;
                if (d->ph_hum >= 1.0f)
                        d->ph_hum -= 1.0f;
                mod = 1.0f + p->rot_mod * sinf(DS_TWO_PI * d->ph_rot);
                hum = sinf(DS_TWO_PI * d->ph_hum) + 0.3f * sinf(2.0f * DS_TWO_PI * d->ph_hum);

                out += hum * p->hum_amp * sp;
                out += svf_bp(&d->motor_bp, frand(&d->seed)) * p->motor_amp * sp;
                if (!drive_empty[drive])
                        out += svf_bp(&d->media_bp, frand(&d->seed)) * p->media_amp * sp * mod;
        }

        return out;
}

void drivesnd_fdd_motor(int drive, int on) {
        fdd_snd_t *d;

        if (!ds || drive < 0 || drive > 1)
                return;
        d = &ds->fdd[drive];
        on = on ? 1 : 0;
        if (!d->type || d->motor_on == on)
                return;
        d->motor_on = on;
        if (d->prof->headload_amp > 0.0f)
                body_excite(&d->body, d->prof->headload_amp * (on ? 1.0f : 0.5f));
}

void drivesnd_fdd_seek(int drive, int track) {
        fdd_snd_t *d;

        if (!ds || drive < 0 || drive > 1)
                return;
        d = &ds->fdd[drive];
        if (!d->type || track == d->target)
                return;
        d->target = track;
        if (!d->pending) {
                d->pending = 1;
                d->pend_t = ds_now();
        }
}

void drivesnd_ui_fdd_insert(int drive) {
        if (drive >= 0 && drive < 2)
                ui_fdd_insert_count[drive]++;
}

/* ---------------------------------------------------------------- hard disk */

static int hdd_auto_profile(int cylinders, uint64_t bytes) {
        if (!strncmp(hdd_controller_name, "mfm", 3) || !strncmp(hdd_controller_name, "dtc", 3))
                return HDD_PROF_STEPPER;
        if (!strncmp(hdd_controller_name, "esdi", 4) || !strncmp(hdd_controller_name, "wd1007", 6))
                return HDD_PROF_VOICECOIL;
        if (bytes < 500ull * 1024 * 1024)
                return HDD_PROF_VOICECOIL;
        if (hdd_controller_current_is_scsi() ? bytes < 2048ull * 1024 * 1024 : bytes < 10240ull * 1024 * 1024)
                return HDD_PROF_5400;
        return HDD_PROF_7200;
}

void drivesnd_hdd_attach(int id, int cylinders, uint64_t bytes) {
        hdd_snd_t *h;
        const hdd_profile_t *p;
        int prof;

        if (!ds || id < 0 || id >= MAX_HDD)
                return;
        h = &ds->hdd[id];
        memset(h, 0, sizeof(hdd_snd_t));

        prof = ds->prof_hdd ? ds->prof_hdd : hdd_auto_profile(cylinders, bytes);
        p = &hdd_profiles[prof];
        h->present = 1;
        h->prof = p;
        h->cylinders = cylinders > 1 ? cylinders : 1;
        h->seed = 0x9e3779b9u * (id + 1);
        spindle_init(&h->sp, p->rpm, p->spin_up_s, p->spin_down_s, p->hum, p->whine_mult, p->whine_amp, p->air_fc, p->air_amp,
                     h->seed ^ 0x55aa55aa);
        h->sp.target = 1.0f;
        body_init(&h->body, p->modes, p->burst_ms, p->burst_ratio, p->burst_lp_hz, p->burst_direct, h->seed ^ 0x1f2e3d4c);
        svf_set(&h->buzz, p->buzz_fc ? p->buzz_fc : 1000, p->buzz_q ? p->buzz_q : 1.0f);
        h->unparked = 0;
}

static void hdd_start_seek(hdd_snd_t *h, uint64_t n) {
        const hdd_profile_t *p = h->prof;
        int dist = abs(h->target - h->cyl);
        float dn = sqrtf((float)dist / (float)h->cylinders);
        float r = 0.9f + 0.15f * frand(&h->seed);

        h->pending = 0;
        if (!dist)
                return;

        if (p->stepper) {
                h->steps_left = dist;
                h->steps_done = 0;
                h->dir = (h->target > h->cyl) ? 1 : -1;
                h->next_step_t = n;
                return;
        }

        if (dist <= 2) {
                body_excite(&h->body, p->track_amp * r);
                h->busy_until = n + MS(p->seek_ms_min * 0.5f);
        } else {
                uint64_t dur = MS(p->seek_ms_min + (p->seek_ms_max - p->seek_ms_min) * dn);

                body_excite(&h->body, p->seek_start_amp * (0.35f + 0.65f * dn) * r);
                h->buzz_start = n;
                h->buzz_end = n + dur;
                h->buzz_amp = p->buzz_amp * (0.3f + 0.7f * dn);
                h->end_hit_t = n + dur;
                h->end_hit_amp = p->seek_end_amp * (0.4f + 0.6f * dn) * r;
                h->busy_until = n + dur + MS(0.5f);
        }
        h->cyl = h->target;
}

static inline float hdd_run(hdd_snd_t *h, uint64_t n) {
        const hdd_profile_t *p = h->prof;
        float out;

        if (!h->unparked && h->sp.speed >= 0.97f) {
                /*Heads unlatch and the drive recalibrates once at speed*/
                h->unparked = 1;
                if (p->unpark_amp > 0.0f) {
                        body_excite(&h->body, p->unpark_amp);
                        h->target = h->cylinders / 2;
                        h->pending = 1;
                        h->pend_t = n + MS(120);
                }
        }

        if (p->stepper) {
                if (!h->steps_left && h->pending && n >= h->pend_t)
                        hdd_start_seek(h, n);
                if (h->steps_left && n >= h->next_step_t) {
                        int k = h->steps_done < h->steps_left ? h->steps_done : h->steps_left;
                        float interval = p->step_ms_start * powf(0.8f, (float)k);

                        if (interval < p->step_ms_min)
                                interval = p->step_ms_min;
                        body_excite(&h->body, p->step_amp * (0.85f + 0.2f * frand(&h->seed)));
                        h->cyl += h->dir;
                        h->steps_left--;
                        h->steps_done++;
                        h->next_step_t = n + MS(interval);
                        if (!h->steps_left) {
                                h->end_hit_t = n + MS(interval);
                                h->end_hit_amp = p->seek_end_amp * (h->steps_done > 1 ? 1.0f : 0.3f);
                        }
                }
        } else if (h->pending && n >= h->pend_t && n >= h->busy_until) {
                hdd_start_seek(h, n);
        }

        if (h->end_hit_t && n >= h->end_hit_t) {
                body_excite(&h->body, h->end_hit_amp);
                h->end_hit_t = 0;
        }

        out = spindle_run(&h->sp) + body_run(&h->body);

        if (h->buzz_end && n < h->buzz_end) {
                float frac = (float)(n - h->buzz_start) / (float)(h->buzz_end - h->buzz_start);

                out += svf_bp(&h->buzz, frand(&h->seed)) * h->buzz_amp * sinf(DS_PI * frac);
        } else if (h->buzz_end) {
                h->buzz_end = 0;
                h->buzz.lp = h->buzz.bp = 0.0f;
        }

        return out;
}

void drivesnd_hdd_access(int id, int cylinder) {
        hdd_snd_t *h;

        if (!ds || id < 0 || id >= MAX_HDD)
                return;
        h = &ds->hdd[id];
        if (!h->present)
                return;
        if (cylinder == h->target) {
                h->pending = 0;
                return;
        }
        h->target = cylinder;
        if (!h->pending) {
                h->pending = 1;
                h->pend_t = ds_now();
        }
}

/* ---------------------------------------------------------------- CD/DVD */

void drivesnd_cd_attach(int max_speed, int is_dvd) {
        cd_snd_t *c;
        const cd_profile_t *p;
        int prof;

        if (!ds)
                return;
        c = &ds->cd;
        memset(c, 0, sizeof(cd_snd_t));

        prof = ds->prof_cd;
        if (!prof) {
                if (is_dvd)
                        prof = CD_PROF_DVD;
                else if (max_speed <= 2)
                        prof = CD_PROF_2X;
                else if (max_speed <= 12)
                        prof = CD_PROF_8X;
                else
                        prof = CD_PROF_52X;
        }
        p = &cd_profiles[prof];
        c->present = 1;
        c->prof = p;
        c->seed = 0x2545f491u;
        spindle_init(&c->sp, p->max_rpm, p->spin_up_s, p->spin_down_s, p->hum, p->whine_mult, p->whine_amp, p->air_fc,
                     p->air_amp, c->seed ^ 0x13579bdf);
        body_init(&c->body, p->modes, 1.5f, 0.5f, 6000, 0.25f, c->seed ^ 0x2468ace0);
        svf_set(&c->sled_bp, p->sled_fc, 2.0f);
        svf_set(&c->tray_bp, p->tray_fc, 1.5f);

        /*Power-on: the drive spins up to read the table of contents*/
        c->probe_t = ds_now() + MS(500);
}

static float cd_speed_for(cd_snd_t *c, uint32_t lba) {
        const cd_profile_t *p = c->prof;
        float pos;

        if (!p->clv)
                return 1.0f;
        /*CLV: the disc turns about 2.5 times slower at the outer edge than at the hub*/
        pos = (float)lba / (float)p->max_lba;
        if (pos > 1.0f)
                pos = 1.0f;
        return 1.0f - 0.6f * pos;
}

static void cd_add_tray(cd_snd_t *c, uint64_t start) {
        int i;

        for (i = 0; i < MAX_TRAY; i++) {
                if (!c->tray[i].end) {
                        c->tray[i].start = start;
                        c->tray[i].end = start + MS(c->prof->tray_ms);
                        c->tray[i].clunked = 0;
                        return;
                }
        }
}

static inline float cd_run(cd_snd_t *c, uint64_t n) {
        const cd_profile_t *p = c->prof;
        float out;
        int i;

        if (c->probe_t && n >= c->probe_t) {
                c->probe_t = 0;
                c->last_activity = n;
                c->sp.target = cd_speed_for(c, 0);
        }

        if (c->audio)
                c->last_activity = n;
        else if (c->sp.target > 0.0f && n - c->last_activity > (uint64_t)(p->idle_s * DS_RATE))
                c->sp.target = 0.0f;

        if (c->pending && n >= c->pend_t && n >= c->busy_until) {
                uint32_t dist = (c->target_lba > c->head_lba) ? c->target_lba - c->head_lba : c->head_lba - c->target_lba;
                float dn = sqrtf((float)dist / (float)p->max_lba);
                uint64_t dur;

                if (dn > 1.0f)
                        dn = 1.0f;
                dur = MS(p->sled_ms_min + (p->sled_ms_max - p->sled_ms_min) * dn);
                c->pending = 0;
                c->sled_start = n;
                c->sled_end = n + dur;
                c->sled_amp = p->sled_amp * (0.5f + 0.5f * dn);
                c->busy_until = c->sled_end + MS(2);
                c->head_lba = c->target_lba;
        }

        out = spindle_run(&c->sp) + body_run(&c->body);

        if (c->sled_end) {
                if (n < c->sled_end) {
                        float frac = (float)(n - c->sled_start) / (float)(c->sled_end - c->sled_start);

                        out += svf_bp(&c->sled_bp, frand(&c->seed)) * c->sled_amp * sinf(DS_PI * frac);
                } else {
                        c->sled_end = 0;
                        body_excite(&c->body, p->sled_stop_amp);
                }
        }

        for (i = 0; i < MAX_TRAY; i++) {
                if (!c->tray[i].end || n < c->tray[i].start)
                        continue;
                if (n < c->tray[i].end) {
                        float frac = (float)(n - c->tray[i].start) / (float)(c->tray[i].end - c->tray[i].start);
                        float env = (frac < 0.05f) ? frac / 0.05f : ((frac > 0.9f) ? (1.0f - frac) / 0.1f : 1.0f);
                        float rattle = 0.7f + 0.3f * sinf(DS_TWO_PI * 28.0f * (float)(n - c->tray[i].start) / DS_RATE);

                        c->ph_tray += p->tray_fc * 0.5f / DS_RATE;
                        if (c->ph_tray >= 1.0f)
                                c->ph_tray -= 1.0f;
                        out += (svf_bp(&c->tray_bp, frand(&c->seed)) + 0.3f * sinf(DS_TWO_PI * c->ph_tray)) * p->tray_amp *
                               env * rattle;
                } else {
                        if (!c->tray[i].clunked)
                                body_excite(&c->body, p->tray_clunk_amp);
                        c->tray[i].end = 0;
                }
        }

        return out;
}

void drivesnd_cd_read(uint32_t lba) {
        cd_snd_t *c;
        uint32_t dist;

        if (!ds)
                return;
        c = &ds->cd;
        if (!c->present)
                return;

        c->last_activity = ds_now();
        c->sp.target = cd_speed_for(c, lba);

        dist = (lba > c->target_lba) ? lba - c->target_lba : c->target_lba - lba;
        if (dist > 300) {
                c->target_lba = lba;
                if (!c->pending) {
                        c->pending = 1;
                        c->pend_t = ds_now();
                }
        } else if (!c->pending) {
                c->target_lba = c->head_lba = lba;
        }
}

void drivesnd_cd_audio(int playing) {
        cd_snd_t *c;

        if (!ds)
                return;
        c = &ds->cd;
        if (!c->present)
                return;
        c->audio = playing;
        c->last_activity = ds_now();
        if (playing)
                c->sp.target = c->prof->audio_rpm / c->prof->max_rpm;
}

static void cd_tray(cd_snd_t *c, int open, uint64_t t) {
        if (open) {
                if (c->tray_open)
                        return;
                c->tray_open = 1;
                c->audio = 0;
                c->sp.target = 0.0f;
                c->probe_t = 0;
                cd_add_tray(c, t);
        } else {
                if (!c->tray_open)
                        return;
                c->tray_open = 0;
                cd_add_tray(c, t);
                c->probe_t = t + MS(c->prof->tray_ms + 400);
        }
}

void drivesnd_cd_tray(int open) {
        if (!ds || !ds->cd.present)
                return;
        cd_tray(&ds->cd, open, ds_now());
}

void drivesnd_ui_cd_change(void) { ui_cd_change_count++; }

/* ---------------------------------------------------------------- device */

static void drivesnd_poll_ui(uint64_t n) {
        int i, cnt;

        for (i = 0; i < 2; i++) {
                cnt = ui_fdd_insert_count[i];
                if (cnt != ds->ui_fdd_seen[i]) {
                        ds->ui_fdd_seen[i] = cnt;
                        if (ds->fdd[i].type)
                                body_excite(&ds->fdd[i].body, ds->fdd[i].prof->insert_amp);
                }
        }

        cnt = ui_cd_change_count;
        if (cnt != ds->ui_cd_seen) {
                ds->ui_cd_seen = cnt;
                if (ds->cd.present) {
                        cd_snd_t *c = &ds->cd;

                        c->tray_open = 0;
                        cd_tray(c, 1, n);
                        cd_tray(c, 0, n + MS(c->prof->tray_ms + 1200));
                }
        }
}

static void drivesnd_get_buffer(int32_t *buffer, int len, void *p) {
        int i, d;
        uint64_t n;

        /*Pick up floppy drive type changes*/
        for (d = 0; d < 2; d++) {
                int type = fdd_get_type(d);

                if (type != ds->fdd[d].type)
                        fdd_setup(&ds->fdd[d], d, type);
        }

        drivesnd_poll_ui(ds->base);

        for (i = 0, n = ds->base; i < len; i++, n++) {
                float f = 0.0f, h = 0.0f, c = 0.0f;
                int32_t s;

                for (d = 0; d < 2; d++) {
                        if (ds->fdd[d].type)
                                f += fdd_run(&ds->fdd[d], d, n);
                }
                for (d = 0; d < MAX_HDD; d++) {
                        if (ds->hdd[d].present)
                                h += hdd_run(&ds->hdd[d], n);
                }
                if (ds->cd.present)
                        c = cd_run(&ds->cd, n);

                s = (int32_t)((f * ds->gain_fdd + h * ds->gain_hdd + c * ds->gain_cd) * DS_SCALE);
                if (s > 32767)
                        s = 32767;
                else if (s < -32768)
                        s = -32768;
                buffer[i * 2] += s;
                buffer[i * 2 + 1] += s;
        }

        ds->base += len;
}

static void *drivesnd_init() {
        int d;

        ds = malloc(sizeof(drivesnd_t));
        memset(ds, 0, sizeof(drivesnd_t));

        ds->gain_fdd = device_get_config_int("fdd_volume") / 100.0f;
        ds->gain_hdd = device_get_config_int("hdd_volume") / 100.0f;
        ds->gain_cd = device_get_config_int("cd_volume") / 100.0f;
        ds->prof_525 = device_get_config_int("fdd_525_profile") ? 1 : 0;
        ds->prof_35 = device_get_config_int("fdd_35_profile") ? 1 : 0;
        ds->prof_hdd = device_get_config_int("hdd_profile");
        if (ds->prof_hdd < 0 || ds->prof_hdd > HDD_PROF_7200)
                ds->prof_hdd = HDD_PROF_AUTO;
        ds->prof_cd = device_get_config_int("cd_profile");
        if (ds->prof_cd < 0 || ds->prof_cd > CD_PROF_DVD)
                ds->prof_cd = CD_PROF_AUTO;

        for (d = 0; d < 2; d++) {
                ds->ui_fdd_seen[d] = ui_fdd_insert_count[d];
                fdd_setup(&ds->fdd[d], d, fdd_get_type(d));
        }
        ds->ui_cd_seen = ui_cd_change_count;

        sound_add_handler(drivesnd_get_buffer, ds);

        return ds;
}

static void drivesnd_close(void *p) {
        free(p);
        if (ds == p)
                ds = NULL;
}

#define VOLUME_SELECTION                                                                                                         \
        {                                                                                                                        \
                {.description = "Off", .value = 0}, {.description = "25%", .value = 25}, {.description = "50%", .value = 50},     \
                        {.description = "75%", .value = 75}, {.description = "100%", .value = 100},                              \
                        {.description = "150%", .value = 150}, {.description = "200%", .value = 200},                            \
        }

static device_config_t drivesnd_config[] = {
        {.name = "fdd_525_profile",
         .description = "5.25\" floppy drive",
         .type = CONFIG_SELECTION,
         .selection =
                 {
                         {.description = "Full-height, belt drive (early 1980s)", .value = 0},
                         {.description = "Half-height, direct drive (late 1980s)", .value = 1},
                 },
         .default_int = 0},
        {.name = "fdd_35_profile",
         .description = "3.5\" floppy drive",
         .type = CONFIG_SELECTION,
         .selection =
                 {
                         {.description = "Standard", .value = 0},
                         {.description = "Quiet (late 1990s)", .value = 1},
                 },
         .default_int = 0},
        {.name = "fdd_volume", .description = "Floppy drive volume", .type = CONFIG_SELECTION, .selection = VOLUME_SELECTION,
         .default_int = 100},
        {.name = "hdd_profile",
         .description = "Hard disk",
         .type = CONFIG_SELECTION,
         .selection =
                 {
                         {.description = "Automatic (from controller and size)", .value = HDD_PROF_AUTO},
                         {.description = "Stepper motor, 3600 RPM (early 1980s MFM)", .value = HDD_PROF_STEPPER},
                         {.description = "Voice coil, 3600 RPM (late 1980s)", .value = HDD_PROF_VOICECOIL},
                         {.description = "5400 RPM (1990s)", .value = HDD_PROF_5400},
                         {.description = "7200 RPM (late 1990s / 2000s)", .value = HDD_PROF_7200},
                 },
         .default_int = HDD_PROF_AUTO},
        {.name = "hdd_volume", .description = "Hard disk volume", .type = CONFIG_SELECTION, .selection = VOLUME_SELECTION,
         .default_int = 100},
        {.name = "cd_profile",
         .description = "CD/DVD drive",
         .type = CONFIG_SELECTION,
         .selection =
                 {
                         {.description = "Automatic (from drive model and speed)", .value = CD_PROF_AUTO},
                         {.description = "1x-2x CD-ROM", .value = CD_PROF_2X},
                         {.description = "4x-8x CD-ROM", .value = CD_PROF_8X},
                         {.description = "24x-52x CD-ROM", .value = CD_PROF_52X},
                         {.description = "DVD-ROM", .value = CD_PROF_DVD},
                 },
         .default_int = CD_PROF_AUTO},
        {.name = "cd_volume", .description = "CD/DVD drive volume", .type = CONFIG_SELECTION, .selection = VOLUME_SELECTION,
         .default_int = 100},
        {.type = -1}};

device_t drivesnd_device = {"Drive sounds", 0, drivesnd_init, drivesnd_close, NULL, NULL, NULL, NULL, drivesnd_config};
