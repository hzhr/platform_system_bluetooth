/*
 * Copyright (C) 2008 The Android Open Source Project
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

#define LOG_TAG "bluedroid"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <bluedroid/bluetooth.h>

#ifndef HCI_DEV_ID
#define HCI_DEV_ID 0
#endif

#define HCID_START_DELAY_SEC   3
#define HCID_STOP_DELAY_USEC 500000

#define MIN(x,y) (((x)<(y))?(x):(y))


static int rfkill_id = -1;
static char *rfkill_state_path = NULL;


static int init_rfkill() {
    char path[64];
    char buf[16];
    int fd;
    int sz;
    int id;
    for (id = 0; ; id++) {
        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", id);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            LOGW("open(%s) failed: %s (%d)\n", path, strerror(errno), errno);
            return -1;
        }
        sz = read(fd, &buf, sizeof(buf));
        close(fd);
        if (sz >= 9 && memcmp(buf, "bluetooth", 9) == 0) {
            rfkill_id = id;
            break;
        }
    }

    asprintf(&rfkill_state_path, "/sys/class/rfkill/rfkill%d/state", rfkill_id);
    return 0;
}

static int check_bluetooth_power() {
    int sz;
    int fd = -1;
    int ret = -1;
    char buffer;

    if (rfkill_id == -1) {
        if (init_rfkill()) goto out;
    }

    fd = open(rfkill_state_path, O_RDONLY);
    if (fd < 0) {
        LOGE("open(%s) failed: %s (%d)", rfkill_state_path, strerror(errno),
             errno);
        goto out;
    }
    sz = read(fd, &buffer, 1);
    if (sz != 1) {
        LOGE("read(%s) failed: %s (%d)", rfkill_state_path, strerror(errno),
             errno);
        goto out;
    }

    switch (buffer) {
    case '1':
        ret = 1;
        break;
    case '0':
        ret = 0;
        break;
    }

out:
    if (fd >= 0) close(fd);
    return ret;
}

static int set_bluetooth_power(int on) {
    int sz;
    int fd = -1;
    int ret = -1;
    const char buffer = (on ? '1' : '0');

    if (rfkill_id == -1) {
        if (init_rfkill()) goto out;
    }

    fd = open(rfkill_state_path, O_WRONLY);
    if (fd < 0) {
        LOGE("open(%s) for write failed: %s (%d)", rfkill_state_path,
             strerror(errno), errno);
        goto out;
    }
    sz = write(fd, &buffer, 1);
    if (sz < 0) {
        LOGE("write(%s) failed: %s (%d)", rfkill_state_path, strerror(errno),
             errno);
        goto out;
    }
    ret = 0;

out:
    if (fd >= 0) close(fd);
    return ret;
}

static const char* m8se_path="/proc/m8_se";
static const char* m8fe_path="/proc/m8_fe";

extern int init_module(void *, unsigned long, const char *);
extern int delete_module(const char *, unsigned int);
extern void *load_file(const char *fn, unsigned *_sz); /* system/core/libcutils/load_file.c */

static int m8_checkse()
{
	static int detected = 0;
	static int m8_se = 0;

	if (!detected) {
		if (0 == access(m8se_path, F_OK))
		{
			m8_se = 1;
		}
		detected = 1;
	}

	return m8_se;
}

static int insmod(const char *filename, const char *args)
{
    void *module;
    unsigned int size;
    int ret;

    module = load_file(filename, &size);
    if (!module)
        return -1;

    ret = init_module(module, size, args);

    free(module);

    return ret;
}

static int rmmod(const char *modname)
{
    int ret = -1;
    int maxtry = 10;

    while (maxtry-- > 0) {
        ret = delete_module(modname, O_NONBLOCK | O_EXCL);
        if (ret < 0 && errno == EAGAIN)
            usleep(500000);
        else
            break;
    }

    if (ret != 0)
        LOGD("Unable to unload driver module \"%s\": %s\n",
             modname, strerror(errno));
    return ret;
}

#define DRIVER_MODULE_NAME	"btmrvl_sdio"
#define DRIVER_MODULE_PATH      "/system/lib/modules/btmrvl_sdio.ko"
#define DRIVER_MODULE_ARG          ""
#define DRIVER_MODULE_TAG		DRIVER_MODULE_NAME
#define MODULE_FILE				"/proc/modules"
#define DRIVER_LOADER_DELAY	1000000

static int check_driver_loaded() {
    char driver_status[PROPERTY_VALUE_MAX];
    FILE *proc;
    char line[sizeof(DRIVER_MODULE_TAG)+10];

    if ((proc = fopen(MODULE_FILE, "r")) == NULL) {
        LOGW("Could not open %s: %s", MODULE_FILE, strerror(errno));
        return 0;
    }
    while ((fgets(line, sizeof(line), proc)) != NULL) {
        if ((strncmp(line, DRIVER_MODULE_TAG, strlen(DRIVER_MODULE_TAG)) == 0) ||
			(strncmp(line, DRIVER_MODULE_TAG, strlen(DRIVER_MODULE_TAG)) == 0)) {
            fclose(proc);
            return 1;
        }
    }
    fclose(proc);
    return 0;
}

static int bt_unload_driver()
{
	int count = 20; /* wait at most 10 seconds for completion */
	int ret;

	LOGI("bt_unload_driver");

	if (m8_checkse()) {
		ret = rmmod(DRIVER_MODULE_NAME);
	} else {
		//ret = rmmod(DRIVER_MODULE_NAME_FE);
		ret = -1;
	}

	if (ret == 0) {
		while (count-- > 0) {
		    if (!check_driver_loaded())
			break;
		    usleep(500000);
		}
		if (count) {
			return 0;
		}
		return -1;
	} else
		return -1;
}

static int bt_load_driver()
{
	int count = 100; /* wait at most 20 seconds for completion */

	LOGI("bt_load_driver");

	if (check_driver_loaded()) {
		return 0;
	}

	if (m8_checkse()) {
		if (insmod(DRIVER_MODULE_PATH, DRIVER_MODULE_ARG) < 0)
			return -1;
	} else {
		//if (insmod(DRIVER_MODULE_PATH, DRIVER_MODULE_ARG) < 0)
		return -1;
	}

	usleep(DRIVER_LOADER_DELAY);
	sched_yield();

	while (count-- > 0) {
		if (check_driver_loaded())
			break;
		usleep(500000);
		if (count) {
			bt_unload_driver();
			return -1;
		}
	}

	return 0;
}

static inline int create_hci_sock() {
    int sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (sk < 0) {
        LOGE("Failed to create bluetooth hci socket: %s (%d)",
             strerror(errno), errno);
    }
    return sk;
}

int bt_enable() {
    LOGV(__FUNCTION__);

    int ret = -1;
    int hci_sock = -1;
    int attempt;

    if (set_bluetooth_power(1) < 0) goto out;

    if (bt_load_driver() < 0) {
        set_bluetooth_power(0);
        goto out;
    }

    LOGI("Starting hciattach daemon");
    if (property_set("ctl.start", "hciattach") < 0) {
        LOGE("Failed to start hciattach");
        bt_unload_driver();
        set_bluetooth_power(0);
        goto out;
    }

    // Try for 10 seconds, this can only succeed once hciattach has sent the
    // firmware and then turned on hci device via HCIUARTSETPROTO ioctl
    for (attempt = 1000; attempt > 0;  attempt--) {
        hci_sock = create_hci_sock();
        if (hci_sock < 0) goto out;

        if (!ioctl(hci_sock, HCIDEVUP, HCI_DEV_ID)) {
            break;
        }
        close(hci_sock);
        usleep(10000);  // 10 ms retry delay
    }
    if (attempt == 0) {
        LOGE("%s: Timeout waiting for HCI device to come up", __FUNCTION__);
        goto out;
    }

    LOGI("Starting bluetoothd deamon");
    if (property_set("ctl.start", "bluetoothd") < 0) {
        LOGE("Failed to start bluetoothd");
        goto out;
    }
    sleep(HCID_START_DELAY_SEC);

    ret = 0;

out:
    if (hci_sock >= 0) close(hci_sock);
    return ret;
}

int bt_disable() {
    LOGV(__FUNCTION__);

    int ret = -1;
    int hci_sock = -1;

    LOGI("Stopping bluetoothd deamon");
    if (property_set("ctl.stop", "bluetoothd") < 0) {
        LOGE("Error stopping bluetoothd");
        goto out;
    }
    usleep(HCID_STOP_DELAY_USEC);

    hci_sock = create_hci_sock();
    if (hci_sock < 0) goto err_out;
    ioctl(hci_sock, HCIDEVDOWN, HCI_DEV_ID);

    LOGI("Stopping hciattach deamon");
    if (property_set("ctl.stop", "hciattach") < 0) {
        LOGE("Error stopping hciattach");
        goto out;
    }

err_out:
    if (bt_unload_driver() < 0) {
        goto out;
    }

    if (set_bluetooth_power(0) < 0) {
        goto out;
    }
    ret = 0;

out:
    if (hci_sock >= 0) close(hci_sock);
    return ret;
}

int bt_is_enabled() {
    LOGV(__FUNCTION__);

    int hci_sock = -1;
    int ret = -1;
    struct hci_dev_info dev_info;


    // Check power first
    ret = check_bluetooth_power();
    if (ret == -1 || ret == 0) goto out;

    ret = -1;

    // Power is on, now check if the HCI interface is up
    hci_sock = create_hci_sock();
    if (hci_sock < 0) goto out;

    dev_info.dev_id = HCI_DEV_ID;
    if (ioctl(hci_sock, HCIGETDEVINFO, (void *)&dev_info) < 0) {
        ret = 0;
        goto out;
    }

    ret = hci_test_bit(HCI_UP, &dev_info.flags);

out:
    if (hci_sock >= 0) close(hci_sock);
    return ret;
}

int ba2str(const bdaddr_t *ba, char *str) {
    return sprintf(str, "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X",
                ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
}

int str2ba(const char *str, bdaddr_t *ba) {
    int i;
    for (i = 5; i >= 0; i--) {
        ba->b[i] = (uint8_t) strtoul(str, &str, 16);
        str++;
    }
    return 0;
}
