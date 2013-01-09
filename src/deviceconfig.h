#ifndef UMTS_DEVICECONFIG_H_
#define UMTS_DEVICECONFIG_H_

#include "umtsd.h"

/// ****************************
/// MODEM CONFIGURATION PROFILES
/// ****************************


/* Make sure that the correct ordering of this array is observed: First
 * specific devices, then generic per-vendor profiles and lastly generic
 * per-driver profiles.
 *
 * When autoselecting a profile from this list, the first entry that has
 * all of its conditions (vendor, device, driver) matched will be used.
 */
static struct umts_profile {
	char *name;
	uint16_t vendor;
	uint16_t device;
	char *driver;
	const struct umts_config cfg;
} profiles[] = {
	{
		.name   = "Ericsson F3705G",
		.vendor = 0x0bdb,
		.device = 0x1900,
		.cfg = {
			.ctlidx = 1,
			.datidx = 0,
			.modecmd = {
				[UMTS_MODE_AUTO] = "AT+CFUN=1\r",
				[UMTS_FORCE_UMTS] = "AT+CFUN=6\r",
				[UMTS_FORCE_GPRS] = "AT+CFUN=5\r",
			},
		},
	},

	{
		.name   = "Alcatel X060s",
		.vendor = 0x1bbb,
		.device = 0x0000,
		.cfg = {
			.ctlidx = 1,
			.datidx = 2,
			.modecmd = {
				[UMTS_MODE_AUTO] = "",
			},
		},
	},

	{
		.name = "Huawei K3520",
		.vendor = 0x12d1,
		.device = 0x1001,
		.cfg = {
			.ctlidx = 2,
			.datidx = 0,
			.modecmd = {
				[UMTS_MODE_AUTO] = "AT^SYSCFG=2,2,40000000,2,4\r",	// Set auto = prefer UMTS
				[UMTS_FORCE_UMTS] = "AT^SYSCFG=14,2,40000000,2,4\r",
				[UMTS_FORCE_GPRS] = "AT^SYSCFG=13,1,40000000,2,4\r",
				[UMTS_PREFER_UMTS] = "AT^SYSCFG=2,2,40000000,2,4\r",
				[UMTS_PREFER_GPRS] = "AT^SYSCFG=2,1,40000000,2,4\r",
			},
		},
	},
	{
		.name   = "Huawei E173",
		.vendor = 0x12d1,
		.device = 0x1433,
		.cfg = {
			.ctlidx = 2,
			.datidx = 0,
			.modecmd = {
				// These haven't been well-tested (just
				// copied from the Huawei generic
				// config). Seems that the device
				// doesn't get carrier after switching
				// from (force-)gprs to umts.
				[UMTS_MODE_AUTO] = "AT^SYSCFG=2,2,40000000,2,4\r",	// Set auto = prefer UMTS
				[UMTS_FORCE_UMTS] = "AT^SYSCFG=14,2,40000000,2,4\r",
				[UMTS_FORCE_GPRS] = "AT^SYSCFG=13,1,40000000,2,4\r",
				[UMTS_PREFER_UMTS] = "AT^SYSCFG=2,2,40000000,2,4\r",
				[UMTS_PREFER_GPRS] = "AT^SYSCFG=2,1,40000000,2,4\r",
			},
		},
	},


// VENDOR DEFAULT PROFILES
	{
		.name   = "Huawei generic",
		.vendor = 0x12d1,
		.cfg = {
			.ctlidx = 1,
			.datidx = 0,
			.modecmd = {
				[UMTS_MODE_AUTO] = "AT^SYSCFG=2,2,40000000,2,4\r",	// Set auto = prefer UMTS
				[UMTS_FORCE_UMTS] = "AT^SYSCFG=14,2,40000000,2,4\r",
				[UMTS_FORCE_GPRS] = "AT^SYSCFG=13,1,40000000,2,4\r",
				[UMTS_PREFER_UMTS] = "AT^SYSCFG=2,2,40000000,2,4\r",
				[UMTS_PREFER_GPRS] = "AT^SYSCFG=2,1,40000000,2,4\r",
			},
		},
	},
	{
		.name   = "ZTE generic",
		.vendor = 0x19d2,
		.cfg = {
			.ctlidx = 1,
			.datidx = 2,
			.modecmd = {
				[UMTS_MODE_AUTO] = "AT+ZSNT=0,0,0\r",
				[UMTS_FORCE_UMTS] = "AT+ZSNT=2,0,0\r",
				[UMTS_FORCE_GPRS] = "AT+ZSNT=1,0,0\r",
				[UMTS_PREFER_UMTS] = "AT+ZSNT=0,0,2\r",
				[UMTS_PREFER_GPRS] = "AT+ZSNT=0,0,1\r",
			},
		},
	},
// DRIVER PROFILES
	{
		.name   = "Option generic",
		.driver = "option",
		.cfg = {
				.ctlidx = 1,
				.datidx = 0,
				.modecmd = {
					[UMTS_MODE_AUTO] = "",
				},
		},
	},
	{
		.name   = "Sierra generic",
		.driver = "sierra",
		.cfg = {
				.ctlidx = 0,
				.datidx = 2,
				.modecmd = {
					[UMTS_MODE_AUTO] = "",
				},
		},
	},
	{
		.name   = "HSO generic",
		.driver = "hso",
		.cfg = {
			.ctlidx = 0,
			.datidx = 3,
			.modecmd = {
				[UMTS_MODE_AUTO] = "at_opsys=2,2\r",	// Set auto = prefer UMTS
				[UMTS_FORCE_UMTS] = "at_opsys=1,2\r",
				[UMTS_FORCE_GPRS] = "at_opsys=0,2\r",
				[UMTS_PREFER_UMTS] = "at_opsys=2,2\r",
				[UMTS_PREFER_GPRS] = "at_opsys=3,2\r",
			},
		},
	},
	{
		.name   = "CDC generic",
		.driver = "cdc_acm",
		.cfg = {
			/* These are just copied from the option generic
			 * profile */
			.ctlidx = 1,
			.datidx = 0,
			.modecmd = {
				[UMTS_MODE_AUTO] = "",
			},
		},
	},
	{
		.name   = "USB serial generic",
		.driver = "usbserial",
		.cfg = {
			.ctlidx = 0,
			.datidx = 2,
			.modecmd = {
				[UMTS_MODE_AUTO] = "",
			},
		},
	},
};

#endif /* UMTS_DEVICECONFIG_H_ */
