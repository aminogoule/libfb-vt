#ifndef PROTO_H
#define PROTO_H

/*
 * proto -- the libfb-vt display protocol between the compositor (server) and
 * its clients (e.g. term). A tiny, Wayland-flavoured seed:
 *
 *   - one AF_UNIX / SOCK_STREAM socket at FBVT_SOCK_PATH;
 *   - every message is a fixed-size struct fbvt_msg header, optionally followed
 *     by paylen bytes of variable payload (only SET_TITLE uses it);
 *   - client pixel buffers are anonymous shared memory (shm_open(SHM_ANON)):
 *     the server allocates one per surface and hands the client its fd over the
 *     socket via SCM_RIGHTS (attached to the SURFACE_CREATED header). Both sides
 *     mmap the same pages, so a COMMIT is just "re-read my buffer, please".
 *
 * Everything is native-endian: server and clients share one host. This is a
 * trust-the-peer local protocol; there is no version negotiation beyond a
 * single byte and no bounds paranoia past what keeps the server alive.
 */

#include <stdint.h>
#include <stddef.h>

#define FBVT_SOCK_PATH      "/var/run/libfbvt.sock"
#define FBVT_PROTO_VERSION  1u
#define FBVT_MAX_PAYLOAD    256u   /* cap on variable payload (title text) */

enum fbvt_msg_type {
	FBVT_HELLO = 1,        /* C->S: arg[0]=version                          */
	FBVT_HELLO_ACK,        /* S->C: arg[0]=version                          */
	FBVT_CREATE_SURFACE,   /* C->S: arg[0]=w, arg[1]=h (client pixel size)  */
	FBVT_SURFACE_CREATED,  /* S->C: id; arg[0]=w arg[1]=h arg[2]=stride     */
	                       /*        + shm fd via SCM_RIGHTS                 */
	FBVT_COMMIT,           /* C->S: id; arg[0..3]=x,y,w,h damage rectangle  */
	FBVT_SET_TITLE,        /* C->S: id; payload = title text (<=PAYLOAD)    */
	FBVT_DESTROY_SURFACE,  /* C->S: id                                      */
	FBVT_INPUT_KEY,        /* S->C: id; arg[0]=byte (0..255) from keyboard  */
	FBVT_CLOSE             /* S->C: id; the compositor wants this surface   */
	                       /*        gone (e.g. on shutdown)                */
};

/* Fixed 28-byte message header. `id` is the surface id (0 when not applicable);
   `arg` carries the small scalar operands for the type; `paylen` is how many
   bytes of variable payload follow this header on the wire. */
struct fbvt_msg {
	uint32_t type;
	uint32_t id;
	int32_t  arg[4];
	uint32_t paylen;
};

/*
 * Send one message. `payload` (paylen bytes) may be NULL when m->paylen == 0.
 * Returns 0 on success, -1 on error (errno set). Writes the whole thing or
 * fails; short writes are looped internally.
 */
int fbvt_send(int fd, const struct fbvt_msg* m, const void* payload);

/*
 * Like fbvt_send but also passes an open file descriptor to the peer via
 * SCM_RIGHTS, attached to the header. Used for FBVT_SURFACE_CREATED (the shm
 * fd). m->paylen must be 0. Returns 0 / -1.
 */
int fbvt_send_fd(int fd, const struct fbvt_msg* m, int passfd);

/*
 * Receive exactly one message. The header is filled into *m; up to paycap
 * payload bytes are read into paybuf (any excess is drained and discarded).
 * If the peer passed an fd it is stored in *passfd, else *passfd = -1;
 * passfd may be NULL to ignore (a received fd is then closed).
 *
 * Returns  1  on a complete message,
 *          0  on orderly peer shutdown (EOF) before any bytes of a message,
 *         -1  on error (errno set; a truncated message reads as EIO).
 *
 * Blocking: call only when the fd is readable (e.g. after poll()). A peer that
 * sends a partial header and then stalls can block this call -- acceptable for
 * this local, trusted seed.
 */
int fbvt_recv(int fd, struct fbvt_msg* m, void* paybuf, size_t paycap,
              int* passfd);

#endif /* PROTO_H */
