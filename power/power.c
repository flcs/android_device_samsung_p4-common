/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>

#define LOG_TAG "P3PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define CPU0_SCALINGMAXFREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define CPU1_SCALINGMAXFREQ_PATH "/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq"

#define TOUCH_SUSPEND_PATH "/sys/bus/i2c/drivers/sec_touch/4-004c/mxt1386/suspended"
#define MPU3050_SUSPEND_PATH "/sys/bus/i2c/drivers/mpu3050/0-0068/mpu3050/suspended"

#define LOW_POWER_MAX_FREQ "456000"
#define LOW_POWER_MIN_FREQ "150000"
#define NORMAL_MAX_FREQ "1000000"

#define MAX_BUF_SZ  10

/* initialize to something safe */
static char screen_off_max_freq[MAX_BUF_SZ] = "456000";
static char scaling_max_freq[MAX_BUF_SZ] = "1000000";
static char normal_max_freq[MAX_BUF_SZ] = "1000000";

static bool low_power_mode = false;

struct p3_power_module {
    struct power_module base;
    pthread_mutex_t lock;
};

int sysfs_read(const char *path, char *buf, ssize_t size)
{
    int fd;
    ssize_t len;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    do {
        len = read(fd, buf, size);
    } while (len < 0 && errno == EINTR);

    close(fd);

    if (len >= 0 && len < size)
        memset(buf+len, 0, size - len);

    return len;
}

static int sysfs_write(char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return -1;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void store_max_freq(char* max_freq)
{
    int len;
    char buf[MAX_BUF_SZ];

    /* read the current scaling max freq */
    len = sysfs_read(CPU0_SCALINGMAXFREQ_PATH, buf, sizeof(buf));

    /* make sure it's not the screen off freq, if the "on"
     * call is skipped (can happen if you press the power
     * button repeatedly) we might have read it. We should
     * skip it if that's the case
     */
    if (len != -1 && strncmp(buf, screen_off_max_freq,
            strlen(screen_off_max_freq)) != 0
            && !low_power_mode)
        memcpy(max_freq, buf, sizeof(buf));
}

static void p3_power_init( __attribute__((unused)) struct power_module *module)
{
    struct p3_power_module *p3 =
            (struct p3_power_module *) module;

    store_max_freq(scaling_max_freq);
    ALOGI("%s: stored scaling_max_freq = %s", __FUNCTION__, scaling_max_freq);
}

static void p3_power_set_interactive( __attribute__((unused)) struct power_module *module, int on)
{
    /*
     * Lower maximum frequency when screen is off.  CPU 0 and 1 share a
     * cpufreq policy.
     */
    if (!on) {
        store_max_freq(scaling_max_freq);

        sysfs_write(CPU0_SCALINGMAXFREQ_PATH, screen_off_max_freq);
        sysfs_write(CPU1_SCALINGMAXFREQ_PATH, screen_off_max_freq);

        sysfs_write(TOUCH_SUSPEND_PATH, "1");
        sysfs_write(MPU3050_SUSPEND_PATH, "1");
    } else if (low_power_mode) {
        store_max_freq(scaling_max_freq);

        sysfs_write(CPU0_SCALINGMAXFREQ_PATH, LOW_POWER_MAX_FREQ);
        sysfs_write(CPU1_SCALINGMAXFREQ_PATH, LOW_POWER_MAX_FREQ);
    } else {
        sysfs_write(CPU0_SCALINGMAXFREQ_PATH, scaling_max_freq);
        sysfs_write(CPU1_SCALINGMAXFREQ_PATH, scaling_max_freq);

        sysfs_write(TOUCH_SUSPEND_PATH, "0");
        sysfs_write(MPU3050_SUSPEND_PATH, "0");
    }
}

static void p3_power_hint(struct power_module *module, power_hint_t hint,
                            void *data)
{
    struct p3_power_module *p3 =
            (struct p3_power_module *) module;

    switch (hint) {
    case POWER_HINT_VSYNC:
        break;
#if 0
    case POWER_HINT_INTERACTION:
        break;
#endif
    case POWER_HINT_LOW_POWER:
        pthread_mutex_lock(&p3->lock);
        if (data) {
            store_max_freq(normal_max_freq);

            low_power_mode = true;
            sysfs_write(CPU0_SCALINGMAXFREQ_PATH, LOW_POWER_MAX_FREQ);
            sysfs_write(CPU1_SCALINGMAXFREQ_PATH, LOW_POWER_MAX_FREQ);
        } else {
            low_power_mode = false;
            sysfs_write(CPU0_SCALINGMAXFREQ_PATH, normal_max_freq);
            sysfs_write(CPU1_SCALINGMAXFREQ_PATH, normal_max_freq);
        }
        pthread_mutex_unlock(&p3->lock);
        break;

    case POWER_HINT_LAUNCH:
        ALOGV("POWER_HINT_LAUNCH\n");
        break;
    default:
        break;
    }
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct p3_power_module HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = POWER_MODULE_API_VERSION_0_2,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = POWER_HARDWARE_MODULE_ID,
            .name = "P3 Power HAL",
            .author = "The Android Open Source Project",
            .methods = &power_module_methods,
        },
        .init = p3_power_init,
        .setInteractive = p3_power_set_interactive,
        .powerHint = p3_power_hint,
    },

    .lock = PTHREAD_MUTEX_INITIALIZER,
};
