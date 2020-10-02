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

#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <string.h>
#include <syslog.h>
#include "udiald.h"
#include "config.h"

static const char *ttyresstr[] = {
	[UDIALD_AT_OK] = "OK",
	[UDIALD_AT_CONNECT] = "CONNECT",
	[UDIALD_AT_ERROR] = "ERROR",
	[UDIALD_AT_CMEERROR] = "+CME ERROR",
	[UDIALD_AT_NODIALTONE] = "NO DIALTONE",
	[UDIALD_AT_BUSY] = "BUSY",
	[UDIALD_AT_NOCARRIER] = "NO CARRIER",
	// This one seems to be Huawei-specific:
	[UDIALD_AT_NOT_SUPPORTED] = "COMMAND NOT SUPPORT",
};

int udiald_tty_open(const char *tty) {
	struct termios tio;
	int fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) return -1;

	tcgetattr(fd, &tio);
	tio.c_cflag |= CREAD;
	tio.c_cflag |= CS8;
	tio.c_iflag |= IGNPAR;
	tio.c_lflag &= ~(ICANON);
	tio.c_lflag &= ~(ECHO);
	tio.c_lflag &= ~(ECHOE);
	tio.c_lflag &= ~(ISIG);
	tio.c_cc[VMIN]=1;
	tio.c_cc[VTIME]=0;
	tcsetattr(fd, TCSANOW, &tio);

	return fd;
}

// "Flatten" a udiald_tty_result, so it becomes a single string that can
// be stored in uci or logged.
//
// The resulting pointer points to a buffer insided the passed
// udiald_tty_read so it does not need to be freed and is valid until
// the next call to this function with the same udiald_tty_read.
const char *udiald_tty_flatten_result(struct udiald_tty_read *r) {
	char *flat = r->flat_buf;
	char *end = flat + lengthof(r->flat_buf) - 1;

	for (size_t i = 0; i < r->lines; ++i) {
		char *in = r->raw_lines[i];
		if (flat != end) *flat++ = '"';
		while (*in)
			if (flat != end) *flat++ = *in++;
		if (flat != end) *flat++ = '"';
		if (i != r->lines - 1) {
			if (flat != end) *flat++ = ',';
			if (flat != end) *flat++ = ' ';
		}
		if (flat == end)
			break;
	}
	*flat = '\0';
	return r->flat_buf;
}

int udiald_tty_put(int fd, const char *cmd) {
	if (verbose >= 2)
		syslog(LOG_DEBUG, "Writing: %s", cmd);
	if (write(fd, cmd, strlen(cmd)) != strlen(cmd))
		return -1;
	return strlen(cmd);
}

// Retrieve answer from modem
enum udiald_atres udiald_tty_get(int fd, struct udiald_tty_read *r, const char *result_prefix, int timeout) {
	struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLERR | POLLHUP};

	int err;
	char *c = r->raw_buf;
	size_t rem = lengthof(r->raw_buf);
	r->lines = 0;
	r->result_line = NULL;
	bool in_newline = true;

	// Modems are evil, they might not send the complete answer when doing
	// a read, so we read until we get a known AT status code (see top)
	while (rem > 0) {
		err = poll(&pfd, 1, timeout);
		if (err == 0) {
			syslog(LOG_ERR, "Poll timed out");
			errno = ETIMEDOUT;
			return -1;
		}
		if (err < 0) {
			syslog(LOG_ERR, "Poll failed: %s", strerror(errno));
			return -1;
		}

		ssize_t rxed;
		while ((rxed = read(fd, c, 1)) > 0) {
			if (c[0] == '\r' || c[0] == '\n') {
				if (!in_newline) {
					// Found the end of the current line,
					// process it.
					in_newline = true;
					// Replace the newline with a
					// nul-termination
					c[0] = '\0';

					char *start = r->raw_lines[r->lines];

					syslog(LOG_DEBUG, "Read: %s", start);

					if (start[0] == '^') {
						// Async reply, pretend the line was
						// never there
						c = start;
						continue;
					}

					++r->lines;

					// See if the current line starts with the
					// given prefix
					if (!r->result_line && result_prefix && !strncmp(start, result_prefix, strlen(result_prefix)))
					    r->result_line = start;

					// Compare with known AT status codes (array at the very top)
					for (size_t i = 0; i < lengthof(ttyresstr); ++i)
						if (!strncmp(start, ttyresstr[i], strlen(ttyresstr[i])))
							return i;

			    } else {
				    // Continuing the current newline, don't
				    // include this character in the output.
				    rxed = 0;
			    }
			} else if (in_newline) {
				// We were in a newline, but now found a
				// non-newline character. Start a new line.

				if (r->lines == lengthof(r->raw_lines) - 1) {
					syslog(LOG_ERR, "No complete response received within %zu lines", lengthof(r->raw_lines));
					errno = ERANGE;
					return -1;
				}

				// Remember the start of the line
				r->raw_lines[r->lines] = c;

				in_newline = false;
			} else {
				// Regular character, halfway a line. Do
				// nothing special.
			}

			rem -= rxed;
			c += rxed;
		}
		if (rxed == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
			syslog(LOG_ERR, "Read failed: %s", strerror(errno));
			return -1;
		}

	}

	syslog(LOG_ERR, "No complete response received within %zu bytes", lengthof(r->raw_buf));
	errno = ERANGE;
	return -1;
}

int udiald_tty_cloexec(int fd) {
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	return fd;
}

pid_t udiald_tty_pppd(struct udiald_state *state) {
	char cpath[18 + sizeof(state->networkname) + sizeof(pid_t) * 3];
	snprintf(cpath, sizeof(cpath), "/tmp/udiald-pppd-%s-%d", state->networkname, getpid());
	if (unlink(cpath) < 0 && errno != ENOENT) {
		syslog(LOG_CRIT, "%s: Failed to clean up existing ppp config file: %s",
				state->modem.device_id, strerror(errno));
		return 0;
	}

	// Create config file
	FILE *fp;
	int cfd = open(cpath, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (cfd < 0) {
		syslog(LOG_CRIT, "%s: Failed to create ppp config file: %s",
				state->modem.device_id, strerror(errno));
		return 0;
	}

	if (!(fp = fdopen(cfd, "w"))) {
		syslog(LOG_CRIT, "%s: Failed to create FILE* for ppp config file: %s",
				state->modem.device_id, strerror(errno));
		close(cfd);
		return 0;
	}

	char buf[256];

	fputs("/dev/", fp);
	fputs(state->modem.dat_tty, fp);
	fputs("\n460800\ncrtscts\nlock\n"
		"noauth\nnoipdefault\nnovj\nnodetach\n", fp);

	char *ifname;
	if ((ifname = udiald_config_get(state, "ifname")) && *ifname) {
		fputs("ifname \"", fp);
		fputs(ifname, fp);
		fputs("\"\n", fp);
	}

	// We need to pass ourselve as connect script so get our path from /proc
	memcpy(buf, "connect \"", 9);
	ssize_t l = readlink("/proc/self/exe", buf + 9, sizeof(buf) - 10);
	/* Pass on relevant options */
	char *verbose_opts = (verbose == 0 ? "" : verbose == 1 ? " -v" : " -v -v");
	snprintf(buf + 9 + l, sizeof(buf) - 9 - l, " -d -n%s -D%s -p%s %s\"\n", state->networkname, state->modem.device_id, state->modem.profile->name, verbose_opts);
	fputs(buf, fp);
	printf("%s", buf);

	// Set linkname and ipparam
	fprintf(fp, "linkname \"%s\"\nipparam \"%s\"\n", state->networkname, state->networkname);

	// UCI to pppd-cfg
	int val;
	if ((val = udiald_config_get_int(state, "defaultroute", 1)) != 0) {
		fputs("defaultroute\n", fp);
	}
	if ((val = udiald_config_get_int(state, "replacedefaultroute", 0)) != 0) {
		fputs("replacedefaultroute\n", fp);
	}
	if ((val = udiald_config_get_int(state, "usepeerdns", 1)) != 0) {
		fputs("usepeerdns\n", fp);
	}
	if ((val = udiald_config_get_int(state, "persist", 1)) != 0) {
		fputs("persist\n", fp);
	}
	if ((val = udiald_config_get_int(state, "unit", -1)) > 0) {
		fprintf(fp, "unit %i\n", val);
	}
	if ((val = udiald_config_get_int(state, "maxfail", 1)) >= 0) {
		fprintf(fp, "maxfail %i\n", val);
	}
	if ((val = udiald_config_get_int(state, "holdoff", 0)) >= 0) {
		fprintf(fp, "holdoff %i\n", val);
	}
	if ((val = udiald_config_get_int(state, "udiald_mtu", -1)) > 0) {
		fprintf(fp, "mtu %i\nmru %i\n", val, val);
	}
	if ((val = udiald_config_get_int(state, "noremoteip", 1)) > 0) {
		fprintf(fp, "noremoteip\n");
	}

	fprintf(fp, "lcp-echo-failure 12\n");

	char *s;
	s = udiald_config_get(state, "udiald_user");
	fprintf(fp, "user \"%s\"\n", (s && *s && !strpbrk(s, "\"\r\n")) ? s : "");
	free(s);

	s = udiald_config_get(state, "udiald_pass");
	fprintf(fp, "password \"%s\"\n", (s && *s && !strpbrk(s, "\"\r\n")) ? s : "");
	free(s);

	if (verbose) /* Log to stderr (as well as syslog) */
		fputs("logfd 2\n", fp);

	if (verbose >= 2) /* Include extra debug info */
		fputs("debug\n", fp);

	// Additional parameters
	struct list_head opts = LIST_HEAD_INIT(opts);
	udiald_config_get_list(state, "udiald_pppdopt", &opts);
	struct ucilist *p, *p2;
	list_for_each_entry_safe(p, p2, &opts, list) {
		fputs(p->val, fp);
		fputc('\n', fp);
		list_del(&p->list);
		free(p->val);
		free(p);
	}
	fclose(fp);

	char *const argv[] = {"/usr/sbin/pppd", "file", cpath, NULL};
	pid_t pid = vfork();
	if (pid == 0) {
		execv(argv[0], argv);
		syslog(LOG_CRIT, "%s: Failed to exec %s: %s",
				state->modem.device_id, argv[0], strerror(errno));
		_exit(128);
	} else if (pid == -1) {
		syslog(LOG_CRIT, "%s: Failed to fork for pppd: %s",
				state->modem.device_id, strerror(errno));
		return 0;
	} else {
		return pid;
	}
}
