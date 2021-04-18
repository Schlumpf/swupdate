/*
 * (C) Copyright 2020
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include "network_ipc.h"
#include "util.h"
#include "installer.h"

static pthread_mutex_t mymutex;

static char buf[16 * 1024];
static int fd = STDIN_FILENO;
static int end_status = EXIT_SUCCESS;
static pthread_cond_t cv_end = PTHREAD_COND_INITIALIZER;
/*
 * this is the callback to get a new chunk of the
 * image.
 * It is called by a thread generated by the library and
 * can block.
 */
static int readimage(char **p, int *size) {
	int ret;

	ret = read(fd, buf, sizeof(buf));

	*p = buf;

	*size = ret;

	return ret;
}

/*
 * this is called at the end reporting the status
 * of the upgrade and running any post-update actions
 * if successful
 */
static int endupdate(RECOVERY_STATUS status)
{
	end_status = (status == SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;

	INFO("Swupdate %s\n",
		status == FAILURE ? "*failed* !" :
			"was successful !");

	if (status == SUCCESS) {
		ipc_message msg;
		msg.data.procmsg.len = 0;
		if (ipc_postupdate(&msg) != 0 || msg.type != ACK) {
			end_status = EXIT_FAILURE;
		}
	}

	pthread_mutex_lock(&mymutex);
	pthread_cond_signal(&cv_end);
	pthread_mutex_unlock(&mymutex);

	return 0;
}

int install_from_file(const char *filename, bool check)
{
	int rc;
	int timeout_cnt = 3;

	if (filename && (fd = open(filename, O_RDONLY)) < 0) {
		fprintf(stderr, "Unable to open %s\n", filename);
		return EXIT_FAILURE;
	}

	/* May be set non-zero by end() function on failure */
	end_status = EXIT_SUCCESS;

	struct swupdate_request req;
	swupdate_prepare_req(&req);
	if (check)
		req.dry_run = RUN_DRYRUN;

	while (timeout_cnt > 0) {
		rc = swupdate_async_start(readimage, NULL,
					  endupdate, &req, sizeof(req));
		if (rc >= 0)
			break;
		timeout_cnt--;
		sleep(1);
	}

	/* return if we've hit an error scenario */
	if (rc < 0) {
		ERROR ("swupdate_async_start returns %d\n", rc);
		close(fd);
		return EXIT_FAILURE;
	}

	pthread_mutex_init(&mymutex, NULL);

	/* Now block */
	pthread_mutex_lock(&mymutex);
	pthread_cond_wait(&cv_end, &mymutex);
	pthread_mutex_unlock(&mymutex);

	if (filename)
		close(fd);

	return end_status;
}