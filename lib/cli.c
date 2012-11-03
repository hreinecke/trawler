/*
 * cli.c
 *
 * CLI command handling
 * Copyright(c) 2012 Hannes Reinecke <hare@suse.de>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/un.h>
#include <linux/netlink.h>

#include "logging.h"
#include "cli.h"

#define LOG_AREA "cli"

int cli_send_command(int cli_cmd, char *filename, int src_fd)
{
	struct sockaddr_un sun, local;
	socklen_t addrlen;
	struct msghdr smsg;
	char *cred_msg;
	int cred_msglen;
	struct cmsghdr *cmsg;
	struct ucred *cred;
	struct iovec iov;
	int cli_sock, feature_on = 1;
	char buf[1024];
	char cmd[1024];
	int buflen;
	char status;

	cli_sock = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (cli_sock < 0) {
		err("cannot open cli socket");
		return 3;
	}
	memset(&local, 0x00, sizeof(struct sockaddr_un));
	local.sun_family = AF_LOCAL;
	sprintf(&local.sun_path[1], "/org/kernel/trawler/dredger/%d", getpid());
	addrlen = offsetof(struct sockaddr_un, sun_path) +
		strlen(local.sun_path + 1) + 1;
	if (bind(cli_sock, (struct sockaddr *) &local, addrlen) < 0) {
		err("bind to local cli address failed, error %d", errno);
		return 4;
	}
	setsockopt(cli_sock, SOL_SOCKET, SO_PASSCRED,
		   &feature_on, sizeof(feature_on));
	memset(&sun, 0x00, sizeof(struct sockaddr_un));
	sun.sun_family = AF_LOCAL;
	strcpy(&sun.sun_path[1], "/org/kernel/trawler/dredger");
	addrlen = offsetof(struct sockaddr_un, sun_path) +
		strlen(sun.sun_path + 1) + 1;
	memset(&iov, 0, sizeof(iov));
	cmd[0] = cli_cmd;
	strcpy(cmd + 1, filename);
	iov.iov_base = cmd;
	iov.iov_len = strlen(filename) + 2;

	cred_msglen = CMSG_SPACE(sizeof(struct ucred));
	if (src_fd >= 0)
		cred_msglen += CMSG_SPACE(sizeof(int));
	cred_msg = malloc(cred_msglen);
	memset(&smsg, 0x00, sizeof(struct msghdr));
	smsg.msg_name = &sun;
	smsg.msg_namelen = addrlen;
	smsg.msg_iov = &iov;
	smsg.msg_iovlen = 1;
	smsg.msg_control = cred_msg;
	smsg.msg_controllen = cred_msglen;
	memset(cred_msg, 0, cred_msglen);

	cmsg = CMSG_FIRSTHDR(&smsg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;

	cred = (struct ucred *)CMSG_DATA(cmsg);
	cred->pid = getpid();
	cred->uid = getuid();
	cred->gid = getgid();

	if (src_fd >= 0) {
		cmsg = CMSG_NXTHDR(&smsg, cmsg);
		if (!cmsg) {
			err("sendmsg failed, not enough message headers");
			free(cred_msg);
			return 6;
		}
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		*(int *)CMSG_DATA(cmsg) = src_fd;
	}
	err("send msg '%d' fd '%d' filename '%s'",
	     cli_cmd, src_fd, filename);
	if (sendmsg(cli_sock, &smsg, 0) < 0) {
		if (errno == ECONNREFUSED) {
			err("sendmsg failed, md_monitor is not running");
		} else {
			err("sendmsg failed, error %d", errno);
		}
		free(cred_msg);
		return 5;
	}

	memset(buf, 0x00, sizeof(buf));
	iov.iov_base = buf;
	iov.iov_len = 1024;
	buflen = recvmsg(cli_sock, &smsg, 0);
	if (buflen < 0) {
		err("recvmsg failed, error %d", errno);
		status = errno;
	} else if (buflen < 1) {
		/* command ok */
		status = 0;
	} else if (buflen < 2) {
		/* Status message */
		status = buf[0];
		err("CLI message failed: %s", strerror(status));
	} else {
		printf("%s\n", buf);
		status = 0;
	}
	free(cred_msg);
	close(cli_sock);
	return status;
}

int cli_command(enum cli_commands cli_cmd, char *filename)
{
	int src_fd = -1, ret;
	struct flock lock;

	if (filename && cli_cmd == CLI_MIGRATE) {
		src_fd = open(filename, O_RDWR);
		if (src_fd < 0) {
			err("Cannot open source file '%s', error %d",
			    filename, errno);
			return errno;
		}
		info("Locking file '%s'", filename);
		lock.l_type = F_WRLCK;
		lock.l_start = 0;
		lock.l_whence = SEEK_SET;
		lock.l_len = 0;
		ret = fcntl(src_fd, F_SETLK, &lock);
		if (ret) {
			err("Cannot lock source file '%s', error %d",
			    filename, errno);
			close(src_fd);
			return errno;
		}
	}
	ret = cli_send_command(cli_cmd, filename, src_fd);
	if (src_fd >= 0)
		close(src_fd);
	return ret;
}
