/**
 *   udiald - UMTS connection manager
 *   Copyright (C) 2010 Steven Barth <steven@midlink.org>
 *   Copyright (C) 2010 John Crispin <blogic@openwrt.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <getopt.h>
#include <limits.h>

#include "udiald.h"
#include "config.h"

static volatile int signaled = 0;
static struct udiald_state state = {.uciname = "network", .networkname = "wan", .format = UDIALD_FORMAT_JSON};
int verbose = 0;

// UCI config section to use for global values
#define UCI_SECTION_GLOBAL "udiald"

static int udiald_usage(const char *app) {
	fprintf(stderr,
			"udiald - UMTS connection manager\n"
			"(c) 2010 Steven Barth, John Crispin\n\n"
			"Usage: %s [options] [params...]\n\n"
			"Command Options and Parameters:\n"
			"	-c, --connect			Connect using modem (default)\n"
			"	-s, --scan			Scan modem and reset state file\n"
			"	--probe				Like scan, but do more (debug) probing\n"
			"	-u, --unlock-pin		Same as scan but also try to unlock SIM\n"
			" 	-U, --unlock-puk <PUK> <PIN>	Reset PIN of locked SIM using PUK\n"
			"	-d, --dial			Dial (used internally)\n"
			"	-L, --list-profiles		List available configuration profiles\n"
			"	-l, --list-devices		Detect and list usable devices\n"
			"\nGlobal Options:\n"
			"	-n, --network-name <name>	Use given network name instead of \"wan\"\n"
			"	-v, --verbose			Increase verbosity (once = more info, twice = debug output)\n\n"
			"	-q, --quiet			Decrease verbosity (once = errors / warnings only, twice = no output)\n\n"
			"	-V, --vendor <vendor>		Only consider devices with the given vendor id (in hexadecimal)\n"
			"	-P, --product <productid>	Only consider devices with the given product id (in hexadecimal)\n"
			"	-D, --device-id <deviceid>	Only consider the device with the given id (as listed in sysfs,\n"
			"					e.g. 1.2-1)\n"
			"	-p, --profile <profilename>	Use the profile with the given name instead of autodetecting a\n"
			"					profile to use. Run with -L to get a list of valid profiles.\n"
			"       --pin <pin>                     Use the given pin, instead of loading it from the config file\n"
			"	--usable			Only consider devices that are usable (i.e., for which a\n"
			"					configuration profile is available). This is enabled by default\n"
			"					with --connect, but disabled by default with the listing options.\n"
			"Connect Options:\n"
			"	-t				Test state file for previous SIM-unlocking\n"
			"					errors before attempting to connect\n\n"
			"List options (valid for -L and -l):\n"
			"	-f, --format <format>		Sets the output format. Supported formats are \"json\" and \"id\".\n"
			"Return Codes:\n"
			"	0				OK\n"
			"	1				Syntax error\n"
			"	2				Internal error\n"
			"   	3				Terminated by signal\n"
			"	4				No usable modem found\n"
			"	5				Modem error\n"
			"	6				SIM error\n"
			"	7				SIM unlocking error (PIN failed etc.)\n"
			"	8				Dialing error\n"
			"	9				PPP auth error\n"
			"   	10				Generic PPP error\n"
			"   	11				Network error\n",
			app);
	return UDIALD_EINVAL;
}

static void udiald_catch_signal(int signal) {
	if (!signaled) signaled = signal;
}

// Signal safe cleanup function
static void udiald_cleanup_safe(int signal) {
	if (state.ctlfd > 0) {
		close(state.ctlfd);
		state.ctlfd = -1;
	}
	if (signal) {
		state.flags |= UDIALD_FLAG_SIGNALED;
	}
}

static void udiald_cleanup() {
	if (state.uci) {
		ucix_cleanup(state.uci);
		state.uci = NULL;
	}
	udiald_cleanup_safe(0);
}

static void udiald_exitcode(int code, const char *fmt, ...) {
	char buf[256];
	va_list ap;
	if (code && state.flags & UDIALD_FLAG_SIGNALED)
		code = UDIALD_ESIGNALED;
	if (code && code != UDIALD_ESIGNALED) {
		udiald_config_set_int(&state, "udiald_error_code", code);
		if (fmt) {
			va_start(ap, fmt);
			vsnprintf(buf, lengthof(buf), fmt, ap);
			va_end(ap);
			udiald_config_set(&state, "udiald_error_msg", buf);

			if (state.modem.device_id[0])
				syslog(LOG_CRIT, "%s: %s", state.modem.device_id, buf);
			else
				syslog(LOG_CRIT, "%s", buf);
		} else {
			udiald_config_revert(&state, "udiald_error_msg");
		}
	}
	if (state.app == UDIALD_APP_CONNECT) {
		if (code != UDIALD_OK)
			udiald_config_set(&state, "udiald_state", "error");
		else
			udiald_config_revert(&state, "udiald_state");
	}
	ucix_save(state.uci, state.uciname);
	exit(code);
}

static void sleep_seconds(int seconds) {
	const struct timespec ts = {.tv_sec = seconds};
	nanosleep(&ts, NULL);
}

/* Constants to return for long options withou a corresponding short
 * option. Long options with an equivalent short option just use the
 * short option char.
 * This list starts at UCHAR_MAX + 1, so we can be sure not to conflict
 * with any short option character.
 */
enum long_options {
	UDIALD_OPT_USABLE = UCHAR_MAX + 1,
	UDIALD_OPT_PROBE,
	UDIALD_OPT_PIN,
};

static struct option longopts[] = {
	{"connect", false, NULL, 'c'},
	{"scan", false, NULL, 's'},
	{"unlock-pin", false, NULL, 'u'},
	{"unlock-puk", false, NULL, 'U'},
	{"dial", false, NULL, 'd'},
	{"list-devices", false, NULL, 'l'},
	{"list-profiles", false, NULL, 'L'},
	{"list-profiles", false, NULL, 'L'},
	{"network-name", true, NULL, 'n'},
	{"verbose", false, NULL, 'v'},
	{"quiet", false, NULL, 'q'},
	{"vendor", true, NULL, 'V'},
	{"product", true, NULL, 'P'},
	{"device-id", true, NULL, 'D'},
	{"profile", true, NULL, 'p'},
	{"format", true, NULL, 'f'},
	{"usable", false, NULL, UDIALD_OPT_USABLE},
	{"probe", false, NULL, UDIALD_OPT_PROBE},
	{"pin", true, NULL, UDIALD_OPT_PIN},
	{0},
};

/**
 * Parse the commandline and return the selected app.
 */
static enum udiald_app udiald_parse_cmdline(struct udiald_state *state, int argc, char * const argv[]) {
	enum udiald_app app = UDIALD_APP_CONNECT;

	int s;
	while ((s = getopt_long(argc, argv, "csuUdn:vtlLV:P:D:p:fq", longopts, NULL)) != -1) {
		switch(s) {
			case 'c':
				app = UDIALD_APP_CONNECT;
				break;

			case 's':
				app = UDIALD_APP_SCAN;
				break;

			case UDIALD_OPT_PROBE:
				app = UDIALD_APP_PROBE;
				break;

			case 'u':
				app = UDIALD_APP_UNLOCK;
				break;

			case 'U':
				app = UDIALD_APP_PINPUK;
				break;

			case 'd':
				app = UDIALD_APP_DIAL;
				break;

			case 'l':
				app = UDIALD_APP_LIST_DEVICES;
				break;

			case 'L':
				app = UDIALD_APP_LIST_PROFILES;
				break;

			case 'n':
				strncpy(state->networkname, optarg, sizeof(state->networkname) - 1);
				break;

			case 'v':
				verbose++;
				break;

			case 'q':
				verbose--;
				break;

			case 't':
				state->flags |= UDIALD_FLAG_TESTSTATE;
				break;
			case 'V':
				if (udiald_util_parse_hex_word(optarg, &state->filter.vendor) != UDIALD_OK) {
					fprintf(stderr, "Failed to parse vendor id: \"%s\"\n", optarg);
					exit(UDIALD_EINVAL);
				}
				state->filter.flags |= UDIALD_FILTER_VENDOR;
				break;
			case 'P':
				if (udiald_util_parse_hex_word(optarg, &state->filter.device) != UDIALD_OK) {
					fprintf(stderr, "Failed to parse product id: \"%s\"\n", optarg);
					exit(UDIALD_EINVAL);
				}
				state->filter.flags |= UDIALD_FILTER_DEVICE;
				break;
			case 'D':
				state->filter.device_id = optarg;
				break;
			case 'p':
				state->filter.profile_name = strdup(optarg);
				break;
			case UDIALD_OPT_PIN:
				state->pin = strdup(optarg);
				break;
			case 'f':
				if (!strcmp(optarg, "json")) {
					state->format = UDIALD_FORMAT_JSON;
				} else if (!strcmp(optarg, "id")) {
					state->format = UDIALD_FORMAT_ID;
				} else {
					fprintf(stderr, "Invalid display format: %s\n", optarg);
					exit(UDIALD_EINVAL);
				}
				break;
			case UDIALD_OPT_USABLE:
				state->filter.flags |= UDIALD_FILTER_PROFILE;
				break;
			default:
				exit(udiald_usage(argv[0]));
		}
	}

	return app;
}

static void udiald_setup_syslog(struct udiald_state *state) {
	char *appname = "udiald";
	if (state->app == UDIALD_APP_DIAL)
		appname = "udiald-dialer";

	openlog(appname, LOG_PID | LOG_PERROR, LOG_USER);

	if (verbose > 1 ) // Log everything
		setlogmask(LOG_UPTO(LOG_DEBUG));
	else if (verbose == 1 )
		setlogmask(LOG_UPTO(LOG_INFO));
	else if (verbose == 0 )
		setlogmask(LOG_UPTO(LOG_NOTICE));
	else if (verbose == -1 )
		setlogmask(LOG_UPTO(LOG_WARNING));
	else if (verbose < -1 ) // Log nothing. We can't pass 0, so just
				// enable all non-relevant bits instead.
		setlogmask(INT_MAX & ~(LOG_UPTO(LOG_DEBUG)));
}

static void udiald_setup_uci(struct udiald_state *state) {
	// Prepare and initialize state
	if (!(state->uci = ucix_init(state->uciname, 1))) {
		exit(UDIALD_EINTERNAL);
	}
	ucix_add_section(state->uci, state->uciname, UCI_SECTION_GLOBAL, "udiald");
	/* Reset errno, when running udiald unprivileged, setting up uci
	 * might cause an ignored error, which could cloud debug
	 * attempts */
	errno = 0;
}

/**
 * Select the modem to use, depending on config or autodetection.
 */
void udiald_select_modem(struct udiald_state *state) {
	/* Only return a modem for which we have a valid configuration profile */
	state->filter.flags |= UDIALD_FILTER_PROFILE;

	/* Autodetect the first available modem (if any) */
	int e = udiald_modem_find_devices(state, &state->modem, NULL, NULL, &state->filter);
	if (e != UDIALD_OK) {
		udiald_exitcode(e, "No usable modem found");
	}
	char b[512] = {0};
	snprintf(b, sizeof(b), "%04x:%04x", state->modem.vendor, state->modem.device);
	syslog(LOG_NOTICE, "%s: Found %s modem %s", state->modem.device_id,
			state->modem.driver, b);
	udiald_config_set(state, "modem_id", b);
	udiald_config_set(state, "modem_driver", state->modem.driver);

	b[0] = '\0';
	// Writing modestrings
	const struct udiald_config *cfg = &state->modem.profile->cfg;
	for (size_t i = 0; i < UDIALD_NUM_MODES; ++i)
		if (cfg->modecmd[i]) {
			udiald_config_append(state, "modem_mode", udiald_modem_modestr(i));
			strncat(b, udiald_modem_modestr(i), sizeof(b) - strlen(b) - 2);
			strcat(b, " ");
		}
	syslog(LOG_NOTICE, "%s: Configuration profile supports modes: %s", state->modem.device_id, b);
}

/**
 * Open the control connection, storing the fd in state->ctlfd.
 */
static void udiald_open_control(struct udiald_state *state) {
	// Open control connection
	char ttypath[24];
	snprintf(ttypath, sizeof(ttypath), "/dev/%s", state->modem.ctl_tty);
	if ((state->ctlfd = udiald_tty_cloexec(udiald_tty_open(ttypath))) == -1) {
		udiald_exitcode(UDIALD_EMODEM, "Unable to open terminal");
	}
}

/**
 * Reset the modem through the control connection.
 */
static void udiald_modem_reset(struct udiald_state *state) {
	struct udiald_tty_read r;
	// Hangup modem, disable echoing
	tcflush(state->ctlfd, TCIFLUSH);
	udiald_tty_put(state->ctlfd, "ATE0\r");
	udiald_tty_get(state->ctlfd, &r, NULL, 2500);
	tcflush(state->ctlfd, TCIFLUSH);
}

/**
 * Query the modem for identification.
 */
static void udiald_identify(struct udiald_state *state) {
	struct udiald_tty_read r;
	char b[512];
	// Identify modem
	if (udiald_tty_put(state->ctlfd, "AT+CGMI;+CGMM\r") < 1
	|| udiald_tty_get(state->ctlfd, &r, NULL, 2500) != UDIALD_AT_OK
	|| r.lines < 3) {
		udiald_exitcode(UDIALD_EMODEM, "Unable to identify modem");
	}
	snprintf(b, sizeof(b), "%s %s", r.raw_lines[0], r.raw_lines[1]);
	syslog(LOG_NOTICE, "%s: Identified as %s", state->modem.device_id, b);
	udiald_config_set(state, "modem_name", b);
}

static void udiald_probe_cmd(struct udiald_state *state, const char *cmd, int timeout) {
	char b[512] = {0};
	struct udiald_tty_read r;
	syslog(LOG_NOTICE, "Sending %s", cmd);
	snprintf(b, sizeof(b) - 1, "%s\r", cmd);
	if (udiald_tty_put(state->ctlfd, b) < 1
	|| udiald_tty_get(state->ctlfd, &r, NULL, timeout) != UDIALD_AT_OK) {
		syslog(LOG_CRIT, "%s: %s failed (%s)", state->modem.device_id, cmd, udiald_tty_flatten_result(&r));
	} else {
		for (size_t i = 0; i < r.lines; ++i) {
			if (strstr(r.raw_lines[i], "IMEI"))
				syslog(LOG_NOTICE, "<IMEI censored by udiald>");
			else
				syslog(LOG_NOTICE, "%s", r.raw_lines[i]);
		}
	}
}

/**
 * Probe the modem for supported commands and features (intended as a
 * debug measure only).
 */
static void udiald_probe(struct udiald_state *state) {
        syslog(LOG_NOTICE, "Starting probe");
	// Diagnostic info
	udiald_probe_cmd(state, "ATI", 2500);
	// Manufacturer information
	udiald_probe_cmd(state, "AT+GMI", 2500);
	// Hardware version
	udiald_probe_cmd(state, "AT^HWVER", 2500);
	// Software version
	udiald_probe_cmd(state, "AT+CGMR", 2500);
	// Model info (Sierra only?)
	udiald_probe_cmd(state, "AT+GMM", 2500);
	// Revision info (Sierra only?)
	udiald_probe_cmd(state, "AT+GMR", 2500);
	// simlock status (e.g., single-operator lock)
	// Returns <status>,<tries left>,<operator>
	// <status> 1: locked 2: unlocked 3: locked forever
	udiald_probe_cmd(state, "AT^CARDLOCK?", 2500);
	// Capabilities
	udiald_probe_cmd(state, "AT+GCAP", 2500);
	// Current SIM card (SC) lock enabled state
	udiald_probe_cmd(state, "AT+CLCK=\"SC\",2", 2500);
	// Available locking facilities
	udiald_probe_cmd(state, "AT+CLCK=?", 2500);
	// Current functionality level
	udiald_probe_cmd(state, "AT+CFUN?", 2500);
	// Supported functionality levels
	udiald_probe_cmd(state, "AT+CFUN=?", 2500);
	// Current "PDP" context
	udiald_probe_cmd(state, "AT+CGDCONT?", 2500);
	// Available "PDP" contexts
	udiald_probe_cmd(state, "AT+CGDCONT=?", 2500);
	// Network attach status
	udiald_probe_cmd(state, "AT+CREG?", 2500);
	// GPRS attach status
	udiald_probe_cmd(state, "AT+CGREG?", 2500);
	// E-UTRAN EPS (LTE?) attach status
	udiald_probe_cmd(state, "AT+CEREG?", 2500);
	// Supported access technologies (GSM/UMTS/LTE) on Sierra
	// devices
	udiald_probe_cmd(state, "AT!SELRAT=?", 2500);
	// ZTE current mode (GSM/UMTS)
	udiald_probe_cmd(state, "AT+ZSNT?", 2500);
	// Huawei current mode (GSM/UMTS, legacy command)
	udiald_probe_cmd(state, "AT^SYSCFG?", 2500);
	// Huawei current mode (GSM/UMTS, new command)
	udiald_probe_cmd(state, "AT^SYSCFGEX?", 2500);
	// Huawei supported modes (GSM/UMTS, new command)
	udiald_probe_cmd(state, "AT^SYSCFGEX=?", 2500);
	// Huawei EVDO current mode (GSM/UMTS)
	udiald_probe_cmd(state, "AT^PREFMODE?", 2500);
	// Current network
	udiald_probe_cmd(state, "AT+COPS?", 2500);
	// Available networks (read using a longer timeout, this command
	// may take a while)
	udiald_probe_cmd(state, "AT+COPS=?", 45000);
        syslog(LOG_NOTICE, "Probe finished");
}

/**
 * Query the modem for its SIM status.
 */
static void udiald_check_sim(struct udiald_state *state) {
	struct udiald_tty_read r;
	// Getting SIM state
	tcflush(state->ctlfd, TCIFLUSH);
	if (udiald_tty_put(state->ctlfd, "AT+CPIN?\r") < 1
	|| udiald_tty_get(state->ctlfd, &r, "+CPIN: ", 2500) != UDIALD_AT_OK
	|| r.result_line == NULL) {
		syslog(LOG_CRIT, "%s: Unable to get SIM status (%s)", state->modem.device_id, udiald_tty_flatten_result(&r));
		udiald_config_set(state, "sim_state", "error");
		state->sim_state = -1;
		if (state->app != UDIALD_APP_PROBE)
			udiald_exitcode(UDIALD_ESIM, "Unable to get SIM status");
		return;
	}

	// Evaluate SIM state
	if (!strcmp(r.result_line, "+CPIN: READY")) {
		syslog(LOG_NOTICE, "%s: SIM card is ready", state->modem.device_id);
		udiald_config_set(state, "sim_state", "ready");
		state->sim_state = 0;
	} else if (!strcmp(r.result_line, "+CPIN: SIM PIN")) {
		syslog(LOG_NOTICE, "%s: SIM card requires pin", state->modem.device_id);
		udiald_config_set(state, "sim_state", "wantpin");
		state->sim_state = 1;
	} else if (!strcmp(r.result_line, "+CPIN: SIM PUK")) {
		syslog(LOG_WARNING, "%s: SIM requires PUK!", state->modem.device_id);
		udiald_config_set(state, "sim_state", "wantpuk");
		state->sim_state = 2;
	} else {
		udiald_config_set(state, "sim_state", "error");
		state->sim_state = -1;
		if (state->app != UDIALD_APP_PROBE)
			udiald_exitcode(UDIALD_ESIM, "Unknown SIM status (%s)", r.result_line);
		else
			syslog(LOG_CRIT, "%s: Unknown SIM status (%s)", state->modem.device_id, r.result_line);
	}
}

/**
 * Use the PUK code to reset the PIN.
 *
 * Can only be used when the device has locked itself down (due to
 * subsequent invalid PIN entries, for example).
 *
 * @param puk     The PUK to enter
 * @param pin     The new PIN code to set
 */
static void udiald_enter_puk(struct udiald_state *state, const char *puk, const char *pin) {
	// Reset PIN with PUK
	if (state->sim_state != 2)
		udiald_exitcode(UDIALD_ESIM, "Cannot use PUK - SIM not locked");

	// Prepare command
	char b[512] = {0};
	if (strpbrk(pin, "\"\r\n;") || strpbrk(puk, "\"\r\n;"))
		udiald_exitcode(UDIALD_EINVAL, "Invalid PIN or PUK");
	snprintf(b, sizeof(b), "AT+CPIN=\"%s\",\"%s\"\r", puk, pin);

	// Send command
	struct udiald_tty_read r;
	tcflush(state->ctlfd, TCIFLUSH);
	if (udiald_tty_put(state->ctlfd, b) >= 0
	&& udiald_tty_get(state->ctlfd, &r, NULL, 2500) == UDIALD_AT_OK) {
		syslog(LOG_NOTICE, "%s: PIN reset successful", state->modem.device_id);
		udiald_config_set(state, "sim_state", "ready");
		udiald_exitcode(UDIALD_OK, NULL);
	} else {
		udiald_exitcode(UDIALD_EUNLOCK, "Failed to reset PIN");
	}
}

/**
 * Unlock the device using the PIN.
 *
 * The pincode to use is taken from the configuration.
 */
static void udiald_enter_pin(struct udiald_state *state) {
	//Try unlocking with PIN
	char *pin = state->pin;
	if (!pin)
		pin = udiald_config_get(state, "udiald_pin");

	char b[512] = {0};
	if (!pin || !*pin) {
		if (state->app != UDIALD_APP_PROBE)
			udiald_exitcode(UDIALD_EUNLOCK, "No PIN configured");
		else
			syslog(LOG_CRIT, "%s: No PIN configured", state->modem.device_id);
		free(pin);
		return;
	}
	if (strpbrk(pin, "\"\r\n;")) {
		if (state->app != UDIALD_APP_PROBE)
			udiald_exitcode(UDIALD_EINVAL, "Invalid PIN configured (%s)", pin);
		else
			syslog(LOG_CRIT, "%s: Invalid PIN configured (%s)", state->modem.device_id, pin);
		free(pin);
		return;
	}

	const char *failed = ucix_get_option(state->uci, state->uciname, UCI_SECTION_GLOBAL, "failed_pin");
	if (failed && strcmp(pin, failed) == 0) {
		if (state->app != UDIALD_APP_PROBE)
			udiald_exitcode(UDIALD_ESIM, "Not retrying previously failed pin (%s)", failed);
		else
			syslog(LOG_CRIT, "%s: Not retrying previously failed PIN (%s)", state->modem.device_id, failed);
		free(pin);
		return;
	}
	udiald_config_revert(state, "failed_pin");

	snprintf(b, sizeof(b), "AT+CPIN=\"%s\"\r", pin);

	// Send command
	struct udiald_tty_read r;
	tcflush(state->ctlfd, TCIFLUSH);
	if (udiald_tty_put(state->ctlfd, b) < 0
	|| udiald_tty_get(state->ctlfd, &r, NULL, 2500) != UDIALD_AT_OK) {
		ucix_add_option(state->uci, state->uciname, UCI_SECTION_GLOBAL, "failed_pin", pin);
		if (state->app != UDIALD_APP_PROBE)
			udiald_exitcode(UDIALD_EUNLOCK, "PIN %s rejected (%s)", pin, udiald_tty_flatten_result(&r));
		else
			syslog(LOG_CRIT, "%s: PIN %s rejected (%s)", state->modem.device_id, pin, udiald_tty_flatten_result(&r));
		free(pin);
		return;
	}
	free(pin);

	syslog(LOG_NOTICE, "%s: PIN accepted", state->modem.device_id);
	udiald_config_set(state, "sim_state", "ready");

	// Wait a few seconds for the dongle to find a carrier.
	// Some dongles apparently do not send a NO CARRIER reply to the
	// dialing, but instead hang up directly after sending a CONNECT
	// reply (Alcatel X060S / 1bbb:0000 showed this problem).
	sleep_seconds(5);
}

/**
 * Query the device for supported capabilities.
 */
static void udiald_check_caps(struct udiald_state *state) {
	struct udiald_tty_read r;
	state->is_gsm = 0;
	if (udiald_tty_put(state->ctlfd, "AT+GCAP\r") >= 0
	&& udiald_tty_get(state->ctlfd, &r, "+GCAP: ", 2500) == UDIALD_AT_OK
	&& r.result_line) {
		if (strstr(r.result_line, "CGSM")) {
			state->is_gsm = 1;
			udiald_config_set(state, "modem_gsm", "1");
			syslog(LOG_NOTICE, "%s: Detected a GSM modem", state->modem.device_id);
		}
	}
}

/**
 * Set the device mode (GPRS/UMTS).
 *
 * The mode to set is taken from the configuration.
 */
static void udiald_set_mode(struct udiald_state *state) {
	struct udiald_tty_read r;
	char *m = udiald_config_get(state, "udiald_mode");
	enum udiald_mode mode = udiald_modem_modeval((m && *m) ? m : "auto");
	if (mode == -1 || !state->modem.profile->cfg.modecmd[mode]) {
		free(m);
		udiald_exitcode(UDIALD_EINVAL, "Unsupported mode (%s)", udiald_modem_modestr(mode));
	}
	tcflush(state->ctlfd, TCIFLUSH);
	if (state->modem.profile->cfg.modecmd[mode][0]
	&& (udiald_tty_put(state->ctlfd, state->modem.profile->cfg.modecmd[mode]) < 0
	|| udiald_tty_get(state->ctlfd, &r, NULL, 5000) != UDIALD_AT_OK)) {
		free(m);
		udiald_exitcode(UDIALD_EMODEM, "Failed to set mode %s (%s)",
			state->modem.device_id, udiald_modem_modestr(mode), udiald_tty_flatten_result(&r));
	}
	syslog(LOG_NOTICE, "%s: Mode set to %s", state->modem.device_id, udiald_modem_modestr(mode));
	free(m);
}

static void udiald_connect_status_mainloop(struct udiald_state *state) {
	int status = -1;
	int logsteps = 4;	// Report RSSI / BER to syslog every LOGSTEPS intervals
	char provider[64] = {0};
	struct udiald_tty_read r;

	// Set reporting format for AT+COPS? to 0 (long alphanumeric
	// format), for devices that default to reporting numeric
	// identifiers only. "3" means to leave actual network selection
	// parameters unchanged and only set the format.
	udiald_tty_put(state->ctlfd, "AT+COPS=3,0\r");
	if (udiald_tty_get(state->ctlfd, &r, NULL, 2500) != UDIALD_AT_OK)
		syslog(LOG_WARNING, "%s: Failed to set AT+COPS to long format\n", state->modem.device_id);

	// Main loop, wait for termination, measure signal strength
	while (!signaled) {
		// First run
		if (!++status) {
			udiald_config_set(state, "connected", "1");
			ucix_save(state->uci, state->uciname);
		} else {
			sleep_seconds(15);
			if (signaled) break;
		}

		// Query provider and RSSI / BER
		tcflush(state->ctlfd, TCIFLUSH);
/*		udiald_tty_put(state->ctlfd, "AT+CREG?\r");
		udiald_tty_get(state->ctlfd, b, sizeof(b), 2500);
		printf("%s:%s[%d]%s\n", __FILE__, __func__, __LINE__, b);
*/
		udiald_tty_put(state->ctlfd, "AT+COPS?;+CSQ\r");
		if (udiald_tty_get(state->ctlfd, &r, NULL, 2500) != UDIALD_AT_OK
		|| r.lines < 3)
			continue;

		char *saveptr;
		char *cops = r.raw_lines[0];
		char *csq = r.raw_lines[1];

		if (cops && (cops = strchr(cops, '"')) // +COPS: 0,0,"FONIC",2
		&& (cops = strtok_r(cops, "\"", &saveptr))
		&& strncmp(cops, provider, sizeof(provider) - 1)) {
			syslog(LOG_NOTICE, "%s: Provider is %s",
				state->modem.device_id, cops);
			udiald_config_revert(state, "provider");
			udiald_config_set(state, "provider", cops);
			strncpy(provider, cops, sizeof(provider) - 1);
		}

		if (csq && (csq = strtok_r(csq, " ,", &saveptr))
		&& (csq = strtok_r(NULL, " ,", &saveptr))) {	// +CSQ: 14,99
			// RSSI
			udiald_config_revert(state, "rssi");
			udiald_config_set(state, "rssi", csq);
			if ((status % logsteps) == 0)
				syslog(LOG_NOTICE, "%s: RSSI is %s",
					state->modem.device_id, csq);
		}
		ucix_save(state->uci, state->uciname);
	}
	syslog(LOG_NOTICE, "Received signal %d, disconnecting", signaled);
}

static void udiald_connect_finish(struct udiald_state *state) {
	udiald_config_revert(state, "pid");
	udiald_config_revert(state, "connected");
	udiald_config_revert(state, "provider");
	udiald_config_revert(state, "rssi");

	// Terminate active connection by hanging up and resetting
	udiald_tty_put(state->ctlfd, "ATH;&F\r");
	int status;
	if (waitpid(state->pppd, &status, WNOHANG) != state->pppd) {
		kill(state->pppd, SIGTERM);
		waitpid(state->pppd, &status, 0);
		udiald_exitcode(UDIALD_ESIGNALED, "Terminated by signal %i", signaled);
	}

	if (WIFSIGNALED(status) || WEXITSTATUS(status) == 5) {
		// pppd was termined externally, we won't treat this as an error
		udiald_exitcode(UDIALD_ESIGNALED, "pppd terminated");
	}

	switch (WEXITSTATUS(status)) {	// Exit codes from pppd (man pppd)
		case 7:
		case 16:
			udiald_exitcode(UDIALD_EMODEM, "pppd: modem error");

		case 8:
			udiald_exitcode(UDIALD_EDIAL, "pppd: dialing error");

		case 0:
		case 15:
			udiald_exitcode(UDIALD_ENETWORK, "ppd: terminated by network");

		case 19:
			udiald_exitcode(UDIALD_EAUTH, "pppd: invalid credentials");

		default:
			udiald_exitcode(UDIALD_EPPP, "pppd: other error (%i)", WEXITSTATUS(status));
	}
}

int main(int argc, char *const argv[]) {
	INIT_LIST_HEAD(&state.custom_profiles);

	state.app = udiald_parse_cmdline(&state, argc, argv);

	udiald_setup_syslog(&state);

	udiald_setup_uci(&state);

	/* Load additional profiles from uci */
	udiald_modem_load_profiles(&state);

	atexit(udiald_cleanup);

	//Setup signals
	struct sigaction sa = {
		.sa_handler = SIG_IGN,
	};
	sigaction(SIGPIPE, &sa, NULL);
	sa.sa_handler = udiald_cleanup_safe;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	// Dial only needs an active UCI context
	if (state.app == UDIALD_APP_DIAL)
		return udiald_dial_main(&state);

	if (state.app == UDIALD_APP_LIST_PROFILES)
		return udiald_modem_list_profiles(&state);

	if (state.app == UDIALD_APP_LIST_DEVICES)
		return udiald_modem_list_devices(&state, &state.filter);

	if (state.app == UDIALD_APP_CONNECT && state.flags & UDIALD_FLAG_TESTSTATE) {
		if (udiald_config_get_int(&state, "udiald_error", UDIALD_OK) == UDIALD_EUNLOCK) {
			syslog(LOG_CRIT, "Aborting due to previous SIM unlocking failure. "
			"Please check PIN and rescan device before reconnecting.");
			exit(UDIALD_EUNLOCK);
		}
	}

	// Reset state
	udiald_config_revert(&state, "modem_name");
	udiald_config_revert(&state, "modem_driver");
	udiald_config_revert(&state, "modem_id");
	udiald_config_revert(&state, "modem_mode");
	udiald_config_revert(&state, "modem_gsm");
	udiald_config_revert(&state, "sim_state");
	udiald_config_revert(&state, "udiald_error_code");
	udiald_config_revert(&state, "udiald_error_msg");

	if (state.app == UDIALD_APP_CONNECT) {
		udiald_config_set(&state, "udiald_state", "init");
		ucix_save(state.uci, state.uciname);
	}

	udiald_select_modem(&state);

	udiald_open_control(&state);

	udiald_modem_reset(&state);

	udiald_identify(&state);

	udiald_check_sim(&state);

	if (state.app == UDIALD_APP_SCAN) {
		udiald_exitcode(UDIALD_OK, NULL); // We are done here.
	} else if (state.app == UDIALD_APP_PINPUK) {
		// Need two arguments
		if (optind + 2 != argc) {
			udiald_exitcode(UDIALD_EINVAL, "Invalid arguments");
		}

		udiald_enter_puk(&state, argv[optind], argv[optind+1]);
	}

	if (state.sim_state == 1)
		udiald_enter_pin(&state);

	if (state.app == UDIALD_APP_UNLOCK)
		udiald_exitcode(UDIALD_OK, NULL); // We are done here.

	if (state.app == UDIALD_APP_PROBE) {
		udiald_probe(&state);
		udiald_exitcode(UDIALD_OK, NULL); // We are done here.
	}

	if (state.sim_state == 2)
		udiald_exitcode(UDIALD_EUNLOCK, "SIM locked - need PUK");

	udiald_check_caps(&state);
/*
	char b[512] = {0};
	// verbose provider info
	if (udiald_tty_put(state.ctlfd, "AT+CREG=2\r") < 1
	|| udiald_tty_get(state.ctlfd, b, sizeof(b), 2500) != UDIALD_AT_OK) {
		syslog(LOG_CRIT, "%s: failed to set verbose provider info (%s)", state.modem.device_id, b);
	}
*/

	// Setting network mode if GSM
	if (state.is_gsm) {
		udiald_set_mode(&state);
	} else {
		syslog(LOG_NOTICE, "%s: Skipped setting mode on non-GSM modem", state.modem.device_id);
	}

	// Save state
	udiald_config_set_int(&state, "pid", getpid());
	ucix_save(state.uci, state.uciname);

	// Block and unbind signals so they won't interfere
	sa.sa_handler = udiald_catch_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);

	if (state.app == UDIALD_APP_CONNECT) {
		udiald_config_set(&state, "udiald_state", "dial");
		ucix_save(state.uci, state.uciname);
	}

	// Start pppd to dial
	if (!(state.pppd = udiald_tty_pppd(&state)))
		udiald_exitcode(UDIALD_EINTERNAL, "pppd: Failed to start");

	udiald_connect_status_mainloop(&state);

	/* Clean up state and set exit code. Never returns. */
	udiald_connect_finish(&state);

	// This cannot happen
	return UDIALD_EINTERNAL;
}

