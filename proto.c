/*
 * proto.c -- wire helpers for the libfb-vt display protocol (see proto.h).
 *
 * Two things need care over an AF_UNIX stream socket:
 *   - framing: read()/write() are byte streams, so we loop until the whole
 *     fixed header (and any payload) has moved;
 *   - fd passing: a descriptor rides in ancillary data (SCM_RIGHTS) attached to
 *     the header bytes. We capture it with recvmsg while reading those bytes.
 */

#include "proto.h"

#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* write exactly n bytes, looping over short writes; 0 ok, -1 errno set */
static int write_full(int fd, const void* buf, size_t n) {
	const char* p = buf;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (w == 0) {
			errno = EIO;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/*
 * Read exactly n bytes into buf using recvmsg, so that any SCM_RIGHTS fd that
 * the peer attached to this range is captured. On the first call for a message
 * *passfd (when non-NULL) is -1; if a descriptor arrives it is stored there.
 * Returns 1 on success, 0 on EOF before the first byte, -1 on error/truncation.
 */
static int read_full_cmsg(int fd, void* buf, size_t n, int* passfd) {
	char*  p   = buf;
	size_t got = 0;

	union {
		struct cmsghdr align;
		char           buf[CMSG_SPACE(sizeof(int))];
	} cmsgu;

	while (got < n) {
		struct msghdr msg;
		struct iovec  iov;
		ssize_t       r;

		iov.iov_base = p + got;
		iov.iov_len  = n - got;

		memset(&msg, 0, sizeof(msg));
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;
		msg.msg_control    = cmsgu.buf;
		msg.msg_controllen = sizeof(cmsgu.buf);

		r = recvmsg(fd, &msg, 0);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)                     /* peer closed */
			return got == 0 ? 0 : (errno = EIO, -1);

		/* harvest a passed fd, if any */
		{
			struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
			for (; c != NULL; c = CMSG_NXTHDR(&msg, c)) {
				if (c->cmsg_level == SOL_SOCKET &&
				    c->cmsg_type  == SCM_RIGHTS &&
				    c->cmsg_len   == CMSG_LEN(sizeof(int))) {
					int rfd;
					memcpy(&rfd, CMSG_DATA(c), sizeof(rfd));
					if (passfd != NULL && *passfd < 0)
						*passfd = rfd;
					else
						close(rfd);   /* unwanted / duplicate */
				}
			}
		}
		got += (size_t)r;
	}
	return 1;
}

int fbvt_send(int fd, const struct fbvt_msg* m, const void* payload) {
	if (write_full(fd, m, sizeof(*m)) != 0)
		return -1;
	if (m->paylen) {
		if (payload == NULL) {
			errno = EINVAL;
			return -1;
		}
		if (write_full(fd, payload, m->paylen) != 0)
			return -1;
	}
	return 0;
}

int fbvt_send_fd(int fd, const struct fbvt_msg* m, int passfd) {
	struct msghdr msg;
	struct iovec  iov;
	union {
		struct cmsghdr align;
		char           buf[CMSG_SPACE(sizeof(int))];
	} cmsgu;
	struct cmsghdr* c;

	if (m->paylen) {                     /* fd messages carry no payload */
		errno = EINVAL;
		return -1;
	}

	iov.iov_base = (void*)m;
	iov.iov_len  = sizeof(*m);

	memset(&cmsgu, 0, sizeof(cmsgu));
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
	msg.msg_control    = cmsgu.buf;
	msg.msg_controllen = CMSG_SPACE(sizeof(int));

	c = CMSG_FIRSTHDR(&msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type  = SCM_RIGHTS;
	c->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &passfd, sizeof(passfd));

	for (;;) {
		ssize_t w = sendmsg(fd, &msg, 0);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		/* sendmsg on a stream socket either sends the whole control message
		   with at least one data byte, or fails; a short data count here would
		   desync the fd from the header, so treat it as fatal. */
		if ((size_t)w != sizeof(*m)) {
			errno = EIO;
			return -1;
		}
		return 0;
	}
}

int fbvt_recv(int fd, struct fbvt_msg* m, void* paybuf, size_t paycap,
              int* passfd) {
	int rc;

	if (passfd != NULL)
		*passfd = -1;

	rc = read_full_cmsg(fd, m, sizeof(*m), passfd);
	if (rc <= 0)
		return rc;                       /* 0 = clean EOF, -1 = error */

	if (m->paylen > FBVT_MAX_PAYLOAD) {  /* refuse absurd payloads */
		errno = EMSGSIZE;
		return -1;
	}
	if (m->paylen) {
		size_t take = m->paylen < paycap ? m->paylen : paycap;
		size_t drop = m->paylen - take;

		if (take && read_full_cmsg(fd, paybuf, take, NULL) <= 0)
			return -1;
		while (drop) {                   /* discard payload past paycap */
			char scratch[64];
			size_t n = drop < sizeof(scratch) ? drop : sizeof(scratch);
			if (read_full_cmsg(fd, scratch, n, NULL) <= 0)
				return -1;
			drop -= n;
		}
	}
	return 1;
}
