#include "dfu.h"

#include <zephyr/usb/usb_device.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dfu, LOG_LEVEL_INF);

void dfu_init(void)
{
	/* Confirm the running image so MCUboot won't roll back on next reset.
	 * After a DFU upload, MCUboot boots the new image in "test" mode; the
	 * app must call this to make it permanent. Safe to call repeatedly. */
	if (!boot_is_img_confirmed()) {
		int rc = boot_write_img_confirmed();
		if (rc) {
			LOG_ERR("Image confirm failed: %d", rc);
		} else {
			LOG_INF("Image confirmed");
		}
	}

	/* Start the USB device — this brings up the CDC-ACM virtual serial
	 * port that MCUmgr's UART transport listens on. */
	int rc = usb_enable(NULL);
	if (rc && rc != -EALREADY) {
		LOG_ERR("USB enable failed: %d", rc);
		return;
	}

	LOG_INF("DFU ready — connect USB and use 'mcumgr' to upload firmware");
}
