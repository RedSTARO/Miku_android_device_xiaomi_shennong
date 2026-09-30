/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Apply the per-unit speaker protection calibration (CAL_R, i.e. the
 * measured R0) of the two CS35L43 amplifiers.
 *
 * On stock HyperOS this is done by audio.primary.pineapple.so
 * (audio_extn_cirrus_playback_init) through /odm/lib64/libcrussp.so:
 *  - cirrus_cal_fread() reads one native-endian (little-endian) u32 per
 *    channel from /mnt/vendor/persist/audio/crus_calr.bin; channel 0 is
 *    the "T" amplifier, channel 1 the "B" amplifier. A value of 0 in any
 *    channel makes the whole file invalid.
 *  - cirrus_cal_apply() writes the values as integers to the
 *    "T DSP Set CAL_R" and "B DSP Set CAL_R" mixer controls.
 * The file is only written by the calibration the factory test requests
 * through getParameters("cirrus_speaker_calib"), and only after
 * cirrus_cal_check() accepted every channel in the [8000, 12000] range.
 *
 * The cs35l43 driver caches the value and writes it to CAL_R_SEL of the
 * protection algorithm immediately if the DSP is running, and again on the
 * first playback after every DSP firmware (re)load. If the DSP is not
 * running yet the control returns -EPERM, but the value is still cached.
 *
 * Nothing is written unless every channel holds a value in the range the
 * stock calibration accepts; otherwise the amplifiers keep the default from
 * the tuning file, like on stock without a calibration file.
 */

#define LOG_TAG "crus_cal_apply"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <sound/asound.h>

#define CAL_FILE "/mnt/vendor/persist/audio/crus_calr.bin"
#define SND_DIR "/dev/snd"
#define MAX_CARDS 8

/* Range accepted by libcrussp's cirrus_cal_check() for both channels */
#define CAL_R_MIN 8000
#define CAL_R_MAX 12000

/*
 * Started before the ADSP boots so the value is in place before the audio
 * HAL can start the first stream, like the stock HAL does from adev_open().
 * Waiting happens in the background, nothing waits for this service.
 */
#define WAIT_TIMEOUT_S 180
#define WAIT_STEP_MS 100

static const char* const cal_ctls[] = {
        "T DSP Set CAL_R", /* channel 0 */
        "B DSP Set CAL_R", /* channel 1 */
};

#define NUM_CHANNELS (sizeof(cal_ctls) / sizeof(cal_ctls[0]))

static bool verbose;

static void log_msg(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void log_msg(int prio, const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    __android_log_vprint(prio, LOG_TAG, fmt, ap);
    va_end(ap);

    if (verbose) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
}

#define LOGE(...) log_msg(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) log_msg(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGI(...) log_msg(ANDROID_LOG_INFO, __VA_ARGS__)

/* Same conversion as the "Apply CAL_R Channel[%d]=%.2f ohm" log of stock */
static double cal_r_to_ohm(uint32_t cal_r) {
    return cal_r * 5.85714 / 8192.0;
}

static int read_calibration(const char* path, uint32_t* cal_r) {
    uint8_t buf[NUM_CHANNELS * sizeof(uint32_t)];
    struct stat st;
    ssize_t len;
    size_t i;
    int fd;

    fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) {
        if (errno == ENOENT)
            LOGW("%s does not exist, keeping the default calibration", path);
        else
            LOGE("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }

    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        LOGE("%s is not a regular file", path);
        close(fd);
        return -1;
    }

    if (st.st_size < (off_t)sizeof(buf)) {
        LOGE("%s is too short (%lld bytes, expected %zu)", path, (long long)st.st_size,
             sizeof(buf));
        close(fd);
        return -1;
    }

    /* Like stock, anything after the u32 of the last channel is ignored */
    if (st.st_size != (off_t)sizeof(buf))
        LOGW("%s has %lld bytes, expected %zu, using the first %zu", path, (long long)st.st_size,
             sizeof(buf), sizeof(buf));

    len = TEMP_FAILURE_RETRY(pread(fd, buf, sizeof(buf), 0));
    close(fd);
    if (len != (ssize_t)sizeof(buf)) {
        LOGE("Failed to read %s: %s", path, len < 0 ? strerror(errno) : "short read");
        return -1;
    }

    for (i = 0; i < NUM_CHANNELS; i++) {
        const uint8_t* p = &buf[i * sizeof(uint32_t)];

        cal_r[i] =
                (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (cal_r[i] < CAL_R_MIN || cal_r[i] > CAL_R_MAX) {
            LOGE("Channel[%zu] CAL_R = %u out of range [%d - %d], keeping the default "
                 "calibration on all channels",
                 i, cal_r[i], CAL_R_MIN, CAL_R_MAX);
            return -1;
        }
    }

    return 0;
}

/*
 * Returns the control device of the first card exposing every calibration
 * control, with their info filled in, or -1 if there is none (yet).
 */
static int find_card(const char* snd_dir, struct snd_ctl_elem_info* info, int* card_out) {
    char path[PATH_MAX];
    size_t i;
    int card, fd;

    for (card = 0; card < MAX_CARDS; card++) {
        snprintf(path, sizeof(path), "%s/controlC%d", snd_dir, card);
        fd = TEMP_FAILURE_RETRY(open(path, O_RDWR | O_CLOEXEC));
        if (fd < 0) continue;

        for (i = 0; i < NUM_CHANNELS; i++) {
            memset(&info[i], 0, sizeof(info[i]));
            info[i].id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
            snprintf((char*)info[i].id.name, sizeof(info[i].id.name), "%s", cal_ctls[i]);
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info[i]) < 0) break;
        }

        if (i == NUM_CHANNELS) {
            *card_out = card;
            return fd;
        }

        close(fd);
    }

    return -1;
}

static int check_ctl(const struct snd_ctl_elem_info* info, size_t ch, uint32_t cal_r) {
    if (info->type != SNDRV_CTL_ELEM_TYPE_INTEGER || info->count != 1 ||
        !(info->access & SNDRV_CTL_ELEM_ACCESS_WRITE)) {
        LOGE("'%s' has an unexpected type %d / count %u / access 0x%x", cal_ctls[ch], info->type,
             info->count, info->access);
        return -1;
    }

    if (info->access & SNDRV_CTL_ELEM_ACCESS_LOCK) {
        LOGE("'%s' is locked by pid %d", cal_ctls[ch], info->owner);
        return -1;
    }

    if ((long)cal_r < info->value.integer.min || (long)cal_r > info->value.integer.max) {
        LOGE("Channel[%zu] CAL_R = %u out of control range [%ld - %ld]", ch, cal_r,
             info->value.integer.min, info->value.integer.max);
        return -1;
    }

    return 0;
}

static int write_ctl(int fd, const struct snd_ctl_elem_info* info, size_t ch, uint32_t cal_r) {
    struct snd_ctl_elem_value val;

    memset(&val, 0, sizeof(val));
    val.id = info->id;
    val.value.integer.value[0] = cal_r;

    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) == 0) {
        LOGI("Apply CAL_R Channel[%zu]=%.2f ohm (%u)", ch, cal_r_to_ohm(cal_r), cal_r);
        return 0;
    }

    /* The driver caches the value before failing with -EPERM */
    if (errno == EPERM) {
        LOGI("Stored CAL_R Channel[%zu]=%.2f ohm (%u), applied on the first playback", ch,
             cal_r_to_ohm(cal_r), cal_r);
        return 0;
    }

    LOGE("Failed to write '%s' = %u: %s", cal_ctls[ch], cal_r, strerror(errno));
    return -1;
}

static void usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-n] [-v] [-f calr.bin] [-d snd_dir] [-t timeout_s]\n"
            "  -n  only parse and check the calibration file\n"
            "  -v  also log to stderr\n",
            prog);
}

int main(int argc, char** argv) {
    struct snd_ctl_elem_info info[NUM_CHANNELS];
    const char* cal_file = CAL_FILE;
    const char* snd_dir = SND_DIR;
    uint32_t cal_r[NUM_CHANNELS];
    long timeout_s = WAIT_TIMEOUT_S;
    bool dry_run = false;
    int card = -1, fd = -1, ret = 0;
    long waited_ms;
    size_t i;
    int opt;

    while ((opt = getopt(argc, argv, "nvf:d:t:")) != -1) {
        switch (opt) {
            case 'n':
                dry_run = true;
                verbose = true;
                break;
            case 'v':
                verbose = true;
                break;
            case 'f':
                cal_file = optarg;
                break;
            case 'd':
                snd_dir = optarg;
                break;
            case 't':
                timeout_s = strtol(optarg, NULL, 10);
                if (timeout_s < 0 || timeout_s > 3600) {
                    usage(argv[0]);
                    return 2;
                }
                break;
            default:
                usage(argv[0]);
                return 2;
        }
    }

    if (read_calibration(cal_file, cal_r) < 0) return dry_run ? 1 : 0;

    for (i = 0; i < NUM_CHANNELS; i++)
        LOGI("Channel[%zu] ('%s') CAL_R = %u (%.2f ohm)", i, cal_ctls[i], cal_r[i],
             cal_r_to_ohm(cal_r[i]));

    if (dry_run) return 0;

    for (waited_ms = 0;; waited_ms += WAIT_STEP_MS) {
        fd = find_card(snd_dir, info, &card);
        if (fd >= 0 || waited_ms >= timeout_s * 1000) break;

        struct timespec ts = {
                .tv_sec = WAIT_STEP_MS / 1000,
                .tv_nsec = (WAIT_STEP_MS % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }

    if (fd < 0) {
        LOGE("No sound card with the CS35L43 calibration controls after %ld s", timeout_s);
        return 1;
    }

    LOGI("Found the calibration controls on card %d after %ld ms", card, waited_ms);

    /* Either all channels are written or none */
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (check_ctl(&info[i], i, cal_r[i]) < 0) {
            close(fd);
            return 1;
        }
    }

    for (i = 0; i < NUM_CHANNELS; i++)
        if (write_ctl(fd, &info[i], i, cal_r[i]) < 0) ret = 1;

    close(fd);

    return ret;
}
