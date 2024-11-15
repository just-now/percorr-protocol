#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <uv.h>
#include "sm.h"

// ============================================================================
// State machines and protocol.
// ============================================================================

#define assert_disk_io_never_fails(p)    assert((p))
#define assert_timer_never_fails_also(p) assert((p))

enum msg_type {
	MSG_CHUNK_SIZE = 4096,
	MSG_RESTARTED = 0x42,
	MSG_RESTARTED_REPLY,
	MSG_BLOB,
	MSG_BLOB_REPLY,
};

struct msg_base {
	int      type;
	uint64_t sm_id;
	uint32_t sm_pid;
};

struct msg_restarted {
	struct msg_base b;
	uint32_t        chunks_total;
};

struct msg_restarted_reply {
	struct msg_base b;
	uint32_t        restarted; /* boolean flag */
};

struct msg_blob {
	struct msg_base b;
	uint32_t        chunk_index;
	uint32_t        chunk_size;
	char            blob[MSG_CHUNK_SIZE];
};

struct msg_blob_reply {
	struct msg_base b;
	uint32_t        known_index;
	int             result;
};

enum role {
	R_LEADER,
	R_FOLLOWER,
	R_NONE,
};

struct party {
	uv_udp_send_t  sender;
	uv_timer_t     timeout;
	uv_udp_t       server;

	enum role      role;
	struct sm      rpc_sm;
	struct sm      sm;

	uint32_t       chunk_index;
	uint32_t       chunks_total;
	uint32_t       file_size;

	int            fd;
	const char    *fd_name;
	const char    *dst_ipaddr;
	int            dst_port;
};

enum trigger {
	T_TIMEOUT,
	T_MSGSENT,
	T_MSGRECV,
	T_AGAIN,
	T_NR,
};

enum leader_sm_states {
	L_RESTARTED_LOOP,
	L_RESTARTED_SENT,
	L_RESTARTED_REPLIED,
	L_BLOB_LOOP,
	L_BLOB_SENT,
	L_BLOB_REPLIED,
	L_NR,
};

static const struct sm_conf leader_sm_conf[L_NR] = {
        [L_RESTARTED_LOOP] = {
                .flags   = SM_INITIAL | SM_FINAL,
                .name    = "rest-loop",
                .allowed = BITS(L_RESTARTED_LOOP)
                         | BITS(L_RESTARTED_SENT),
        },
        [L_RESTARTED_SENT] = {
                .name    = "rest-sent",
                .allowed = BITS(L_RESTARTED_LOOP)
			 | BITS(L_RESTARTED_REPLIED),
        },
        [L_RESTARTED_REPLIED] = {
                .name    = "rest-replied",
                .allowed = BITS(L_BLOB_LOOP),
        },
        [L_BLOB_LOOP] = {
                .flags   = SM_INITIAL | SM_FINAL,
                .name    = "blob-loop",
                .allowed = BITS(L_BLOB_LOOP)
                         | BITS(L_BLOB_SENT),
        },
        [L_BLOB_SENT] = {
                .name    = "blob-sent",
                .allowed = BITS(L_BLOB_LOOP)
			 | BITS(L_BLOB_REPLIED),
        },
        [L_BLOB_REPLIED] = {
                .name    = "blob-replied",
                .allowed = BITS(L_RESTARTED_LOOP)
			 | BITS(L_BLOB_LOOP),
        },
};

enum rpc_sm_states {
	R_INIT,
	R_SENT,
	R_REPLIED,
	R_TIMEOUT,
	R_RECV,
	R_RECV_SENT,
	R_FAILED,
	R_NR,
};

static const struct sm_conf rpc_sm_conf[R_NR] = {
        [R_INIT] = {
                .name    = "init",
                .flags   = SM_INITIAL | SM_FINAL,
                .allowed = BITS(R_FAILED)
                         | BITS(R_SENT)
                         | BITS(R_RECV),
        },
	[R_FAILED] = {
                .name    = "failed",
                .flags   = SM_FINAL,
        },
        [R_SENT] = {
                .name    = "sent",
                .allowed = BITS(R_REPLIED)
			 | BITS(R_TIMEOUT)
			 | BITS(R_FAILED),
        },
        [R_REPLIED] = {
                .name    = "replied",
                .flags   = SM_FINAL,
        },
        [R_TIMEOUT] = {
                .name    = "timeout",
                .flags   = SM_FINAL,
        },
        [R_RECV] = {
                .name    = "recv",
                .allowed = BITS(R_RECV_SENT)
			 | BITS(R_FAILED),
        },
        [R_RECV_SENT] = {
                .name    = "recv-sent",
                .flags   = SM_FINAL,
        },
};

static void sent_cb(uv_udp_send_t* req, int status);
static void on_timeout(uv_timer_t *timeout);
static void on_send(uv_udp_send_t *req, int status);

static void *alloc(size_t size)
{
	void *ret = malloc(size);
	/* Assume allocation never fails */
	assert(ret != NULL);
	return ret;
}

static bool rpc_sm_invariant(const struct sm *sm, int prev_state)
{
        return true;
}

static bool leader_sm_invariant(const struct sm *sm, int prev_state)
{
        return true;
}

static void leader_file_open(struct party *leader)
{
	off_t size;

	/* NOTE: For our experiments, assume disk IO never fails. */
	leader->fd = open(leader->fd_name, O_RDONLY);
	assert_disk_io_never_fails(leader->fd > 0);
	size = lseek(leader->fd, 0, SEEK_END);
	assert_disk_io_never_fails(size > 0);
	leader->file_size = size;
	leader->chunks_total = size == 0 ? 0 : 1 + size / MSG_CHUNK_SIZE;
	fprintf(stderr, "size=%ld total=%d\n", size, leader->chunks_total);
	size = lseek(leader->fd, 0, SEEK_SET);
	assert_disk_io_never_fails(size == 0);
}

static void leader_file_read_chunk(struct party *leader, struct msg_blob *blob)
{
	ssize_t  rc;
	uint32_t remaining;

	blob->chunk_index = leader->chunk_index;
	remaining = leader->file_size - blob->chunk_index * MSG_CHUNK_SIZE;
	blob->chunk_size = MIN(MSG_CHUNK_SIZE, remaining);

	/* NOTE: For our experiments, assume disk IO never fails. */
	rc = read(leader->fd, blob->blob, blob->chunk_size);
	assert_disk_io_never_fails(rc >= 0);
}

static int rpc_tick(struct party *leader)
{
	struct sockaddr_in    saddr;
	struct sm            *rpc_sm = &leader->rpc_sm;
	struct sm            *sm = &leader->sm;
	uv_buf_t              buf;
	int                   rc;

	switch(sm_state(sm)) {
	case L_RESTARTED_LOOP: {
		struct msg_restarted *message;

		sm_init(rpc_sm, rpc_sm_invariant, NULL,	rpc_sm_conf, R_INIT);
		sm_to_sm_obs(&leader->sm, rpc_sm);

		message = alloc(sizeof(*message));
		buf = uv_buf_init((char *) message, sizeof *message);
		message->b.type = MSG_RESTARTED;
		message->b.sm_id = rpc_sm->id;
		message->b.sm_pid = sm_pid();
		message->chunks_total = leader->chunks_total;
		uv_req_set_data((uv_req_t *) &leader->sender, message);
		sm_move(rpc_sm, R_SENT);
		break;
	}
	case L_BLOB_LOOP: {
		struct msg_blob *message;

		sm_init(rpc_sm, rpc_sm_invariant, NULL, rpc_sm_conf, R_INIT);
		sm_to_sm_obs(&leader->sm, rpc_sm);

		message = alloc(sizeof(*message));
		buf = uv_buf_init((char *) message, sizeof *message);
		message->b.type = MSG_BLOB;
		message->b.sm_id = rpc_sm->id;
		message->b.sm_pid = sm_pid();
		leader_file_read_chunk(leader, message);
		uv_req_set_data((uv_req_t *) &leader->sender, message);
		sm_move(rpc_sm, R_SENT);

		sm_attr_obs_d(rpc_sm, "chunk_index", leader->chunk_index);
		sm_attr_obs_d(rpc_sm, "chunks_total", leader->chunks_total);
		sm_attr_obs_d(rpc_sm, "file_size", leader->file_size);
		sm_attr_obs_d(rpc_sm, "chunk_size", message->chunk_size);
		break;
	}
	case L_RESTARTED_REPLIED:
	case L_BLOB_REPLIED:
		rc = uv_timer_stop(&leader->timeout);
		assert(rc == 0);
		sm_move(rpc_sm, R_REPLIED);
		sm_fini(rpc_sm);
		return 0;
	case L_RESTARTED_SENT:
	case L_BLOB_SENT:
	default:
		IMPOSSIBLE("Unexpected request");
	}

	rc = uv_ip4_addr(leader->dst_ipaddr, leader->dst_port, &saddr);
	assert(rc == 0);

	rc = uv_timer_start(&leader->timeout, on_timeout, 1000, 0);
	assert_timer_never_fails_also(rc == 0);

	return uv_udp_send(&leader->sender, &leader->server, &buf, 1,
			   (const struct sockaddr*) &saddr, sent_cb);
}

static void leader_tick(struct party *leader, enum trigger trigger,
			const uv_buf_t *message, int status)
{
	struct sm *sm = &leader->sm;
	int rc;

again:
	switch(sm_state(sm)) {
	case L_RESTARTED_LOOP:
		if (trigger != T_TIMEOUT)
			break;

		leader_file_open(leader);
		rc = rpc_tick(leader);
		if (rc == 0) {
			sm_move(sm, L_RESTARTED_SENT);
			return;
		}
		break;
	case L_RESTARTED_SENT:
		if (trigger == T_TIMEOUT) {
			sm_move(sm, L_RESTARTED_LOOP);
			sm_move(&leader->rpc_sm, R_TIMEOUT);
			goto again;
		} else if (trigger == T_MSGSENT && status != 0) {
			break;
		} else if (trigger == T_MSGRECV) {
			sm_move(sm, L_RESTARTED_REPLIED);
			goto again;
		}
		break;
	case L_RESTARTED_REPLIED: {
		rc = rpc_tick(leader);
		 /* TODO: sender may fail, this needs to be handled. */
		assert(rc == 0);

		struct msg_restarted_reply *msg;
		msg = (struct msg_restarted_reply *) message->base;
		/* TODO: we may receive duplicates of unknown type,
		 * therefore the code around assert needs to be
		 * reworked out. */
		assert(msg->b.type == MSG_RESTARTED_REPLY);
		if (msg->restarted) {
			sm_move(sm, L_BLOB_LOOP);
			trigger = T_AGAIN;
			goto again;
		}
		sm_move(sm, L_RESTARTED_LOOP);
		break;
	}
	case L_BLOB_LOOP:
		rc = rpc_tick(leader);
		if (rc == 0) {
			sm_move(sm, L_BLOB_SENT);
			return;
		} else {
			/* TODO: retry sending message after certain timeout */
		}
		break;
	case L_BLOB_SENT:
		if (trigger == T_TIMEOUT) {
			sm_move(sm, L_BLOB_LOOP);
			goto again;
		} else if (trigger == T_MSGSENT && status != 0) {
			break;
		} else if (trigger == T_MSGRECV) {
			sm_move(sm, L_BLOB_REPLIED);
			goto again;
		}
		break;
	case L_BLOB_REPLIED: {
		rc = rpc_tick(leader);
		assert(rc == 0);

		struct msg_blob_reply *msg;
		msg = (struct msg_blob_reply *) message->base;
		assert(msg->b.type == MSG_BLOB_REPLY);

		leader->chunk_index++; /* TODO: assume all is good */
		if (leader->chunk_index < leader->chunks_total) {
			sm_move(sm, L_BLOB_LOOP);
			goto again;
		}

		break;
	}
	default:
		IMPOSSIBLE("Unexpected request");
	}
}

static void follower_tick(struct party *follower, const struct sockaddr *addr,
			  ssize_t nread, const uv_buf_t *message)
{
	int rc;
	uv_buf_t buf;
	struct msg_base *req;

	req = (struct msg_base *) message->base;
	fprintf(stderr, "recv type=%x\n", req->type);

	sm_init(&follower->rpc_sm, rpc_sm_invariant, NULL, rpc_sm_conf, R_INIT);
	sm_move(&follower->rpc_sm, R_RECV);
	sm_from_to_obs("rpc", req->sm_pid, req->sm_id,
		       "rpc", sm_pid(), follower->rpc_sm.id);

	switch(req->type) {
	case MSG_RESTARTED: {
		struct msg_restarted_reply *reply;
		reply = alloc(sizeof(*reply));
		uv_req_set_data((uv_req_t *) &follower->sender, reply);
		reply->b.type = MSG_RESTARTED_REPLY;
		reply->restarted = 1;
		buf = uv_buf_init((char *)reply, sizeof(*reply));
		break;
	}
	case MSG_BLOB: {
		struct msg_blob_reply *reply;
		struct msg_blob *blob;
		off_t size;
		int fd;

		/* TODO: check incoming message */
		blob = (struct msg_blob *) message->base;
		fd = open(follower->fd_name, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
		assert(fd > 0);

		size = lseek(fd, blob->chunk_index * MSG_CHUNK_SIZE, SEEK_SET);
		assert(size >= 0);

		rc = write(fd, blob->blob, blob->chunk_size);
		assert(rc > 0);
		close(fd);

		/* Prepare message */
		reply = alloc(sizeof(*reply));
		uv_req_set_data((uv_req_t *) &follower->sender, reply);
		reply->b.type = MSG_BLOB_REPLY;
		reply->result = 0;
		reply->known_index = blob->chunk_index;
		buf = uv_buf_init((char *)reply, sizeof(*reply));
		break;
	}
	default:
		IMPOSSIBLE("Unexpected request");
	}

	rc = uv_udp_send(&follower->sender, &follower->server,
			 &buf, 1, addr, on_send);
	/* TODO: In case of the link layer failure, wait 100ms and retry. */
	assert(rc == 0);

	sm_move(&follower->rpc_sm, R_RECV_SENT);
	sm_fini(&follower->rpc_sm);
}

// ============================================================================
// libuv-related hooks, callbacks and main.
// ============================================================================

static void pool_alloc(uv_handle_t *handle, size_t _suggested_sz, uv_buf_t *buf)
{
	static char slab[16 * 64 * 1024];

	buf->base = slab;
	buf->len = sizeof(slab);
}

static void on_send(uv_udp_send_t *req, int status)
{
	assert(req != NULL);
	assert(status == 0);
	free(uv_req_get_data((uv_req_t *) req));
}

static void on_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *rcvbuf,
                    const struct sockaddr *addr, unsigned flags)
{
	struct party *p = container_of(handle, struct party, server);

	if (nread == 0)
		return;

	if (p->role == R_LEADER) {
		leader_tick(p, T_MSGRECV, rcvbuf, 0);
		return;
	} else if (p->role == R_FOLLOWER) {
		assert(nread > 0);
		assert(addr->sa_family == AF_INET);
		follower_tick(p, addr, nread, rcvbuf);
	}
}

static int udp4_start(uv_udp_t *server, const char *src_addr, int port)
{
	struct sockaddr_in addr;
	int rc;

	rc = uv_ip4_addr(src_addr, port, &addr);
	assert(rc == 0);

	rc = uv_udp_init(uv_default_loop(), server);
	if (rc != 0) {
		fprintf(stderr, "uv_udp_init: %s\n", uv_strerror(rc));
		return 1;
	}

	rc = uv_udp_bind(server, (const struct sockaddr *) &addr, 0);
	if (rc != 0) {
		fprintf(stderr, "uv_udp_bind: %s\n", uv_strerror(rc));
		return 1;
	}

	rc = uv_udp_recv_start(server, pool_alloc, on_recv);
	if (rc != 0) {
		fprintf(stderr, "uv_udp_recv_start: %s\n", uv_strerror(rc));
		return 1;
	}

	return 0;
}

static void sent_cb(uv_udp_send_t* req, int status)
{
	assert(req != 0);
	struct party *p = container_of(req, struct party, sender);
	leader_tick(p, T_MSGSENT, NULL, status);
	free(uv_req_get_data((uv_req_t *) req));
}

static void on_timeout(uv_timer_t *timeout)
{
	struct party *p = container_of(timeout, struct party, timeout);
	leader_tick(p, T_TIMEOUT, NULL, 0);
}

int main(int argc, char **argv)
{
	int rc;
	int port;
	const char *src_addr;
	struct party party = {0};

	if (argc < 4)
		return EINVAL;

	if (strcmp("leader", argv[1]) == 0) {
		party.role = R_LEADER;
		party.fd_name = "main.c";
		src_addr = argv[2];
		party.dst_ipaddr = argv[3];
		party.dst_port = 8080;
		port = 8081;
	} else if (strcmp("follower", argv[1]) == 0) {
		party.role = R_FOLLOWER;
		party.fd_name = "file.x";
		src_addr = argv[2];
		port = 8080;
	} else
		return EINVAL;

	rc = uv_timer_init(uv_default_loop(), &party.timeout);
	if (rc != 0)
		return rc;

	if (party.role == R_LEADER) {
		rc = uv_timer_start(&party.timeout, on_timeout, 0, 0);
		assert(rc == 0);
		party.sm = (struct sm) { .name = "leader" };
		sm_init(&party.sm, leader_sm_invariant,	NULL, leader_sm_conf,
			L_RESTARTED_LOOP);
		sm_attr_obs_d(&party.sm, "port", port);
	}
	party.rpc_sm = (struct sm) { .name = "rpc" };

	rc = udp4_start(&party.server, src_addr, port);
	if (rc != 0)
		return rc;

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
	return 0;
}
