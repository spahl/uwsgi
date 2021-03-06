#include "../../uwsgi.h"

extern struct uwsgi_server uwsgi;

#define MAX_SYSLOG_PKT 1024

ssize_t uwsgi_rsyslog_logger(struct uwsgi_logger *ul, char *message, size_t len) {

	char buf[MAX_SYSLOG_PKT];
	char ctime_storage[26];
	time_t current_time;
	int portn = 514;
	int rlen;

	if (!ul->configured) {

                if (!uwsgi.choosen_logger_arg) {
			uwsgi_log_safe("invalid rsyslog syntax\n");
			exit(1);
		}

                ul->fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (ul->fd < 0) {
			uwsgi_error_safe("socket()");
			exit(1);
		}

		uwsgi_socket_nb(ul->fd);

                char *comma = strchr(uwsgi.choosen_logger_arg, ',');
		if (comma) {
			ul->data = comma+1;
                	*comma = 0;
		}
		else {
			ul->data = uwsgi_concat2(uwsgi.hostname," uwsgi");
		}


                char *port = strchr(uwsgi.choosen_logger_arg, ':');
                if (port) {
			portn = atoi(port+1);
			*port = 0;
		}

		ul->addr_len = socket_to_in_addr(uwsgi.choosen_logger_arg, NULL, portn, &ul->addr.sa_in);

		if (port) *port = ':';
		if (comma) *comma = ',';

                ul->configured = 1;
        }


	current_time = time(NULL);

	// drop newline
	if (message[len-1] == '\n') len--;
	ctime_r(&current_time, ctime_storage);
 	rlen = snprintf(buf, MAX_SYSLOG_PKT, "<29>%.*s %s: %.*s", 15, ctime_storage+4, (char *) ul->data, (int) len, message);
	if (rlen > 0) {
		return sendto(ul->fd, buf, rlen, 0, (const struct sockaddr *) &ul->addr, ul->addr_len);
	}
	return -1;

}

void uwsgi_rsyslog_register() {
	uwsgi_register_logger("rsyslog", uwsgi_rsyslog_logger);
}

int uwsgi_rsyslog_init() {
	return 0;
}

struct uwsgi_plugin rsyslog_plugin = {

        .name = "rsyslog",
        .on_load = uwsgi_rsyslog_register,
	.init = uwsgi_rsyslog_init

};

