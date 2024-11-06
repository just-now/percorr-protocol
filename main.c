#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <uv.h>
#include "sm.h"

#define min(a,b) (((a)<(b))?(a):(b))
#define impossible() assert(0);


static void sent_cb(uv_udp_send_t* req, int status);
static void on_timeout(uv_timer_t *timeout);
static void on_send(uv_udp_send_t *req, int status);

enum {
	CHUNK_SIZE = 4096,
};

enum msg_type {
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
	int chunks_total;
};

struct msg_restarted_reply {
	struct msg_base b;
	int restarted;
};

struct msg_blob {
	struct msg_base b;
	uint32_t chunk_index;
	uint32_t chunk_size;
	char blob[CHUNK_SIZE];
};

struct msg_blob_reply {
	struct msg_base b;
	int result;
	int known_index;
};

enum role {
	R_LEADER,
	R_FOLLOWER,
	R_NONE,
};

struct party {
	uv_udp_send_t  reply;
	uv_udp_send_t  request;
	uv_timer_t     timeout;
	uv_udp_t       server;

	enum role      role;
	struct sm      leader;
	struct sm      reply_sm;
	struct sm      request_sm;

	uint32_t       chunk_index;
	uint32_t       chunks_total;
	uint32_t       file_size;
	uint32_t       chunks_received;
	int            fd;
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

	leader->fd = open("main.c", O_RDONLY);
	assert(leader->fd > 0);
	size = lseek(leader->fd, 0, SEEK_END);
	assert(size > 0);
	leader->file_size = size;
	leader->chunks_total = size == 0 ? 0 : 1 + size / CHUNK_SIZE;
	printf("size=%ld total=%d\n", size, leader->chunks_total);
	size = lseek(leader->fd, 0, SEEK_SET);
	assert(size == 0);
}

static void leader_file_read_chunk(struct party *leader, struct msg_blob *blob)
{
	int remaining;

	blob->chunk_index = leader->chunk_index;
	remaining = leader->file_size - blob->chunk_index * CHUNK_SIZE;
	blob->chunk_size = MIN(CHUNK_SIZE, remaining);
	read(leader->fd, blob->blob, blob->chunk_size);
}

static int rpc_tick(struct party *leader)
{
	struct sockaddr_in    saddr;
	struct sm            *sm = &leader->leader;
	uv_buf_t              buf;
	int                   rc;

	switch(sm_state(sm)) {
	case L_RESTARTED_LOOP: {
		sm_init(&leader->request_sm, rpc_sm_invariant,
			NULL, rpc_sm_conf, R_INIT);
		sm_to_sm_obs(&leader->leader, &leader->request_sm);

		struct msg_restarted *message;
		message = malloc(sizeof(*message));
		assert(message != NULL);
		buf = uv_buf_init((char *) message, sizeof *message);
		message->b.type = MSG_RESTARTED;
		message->b.sm_id = leader->request_sm.id;
		message->b.sm_pid = sm_pid();
		uv_req_set_data((uv_req_t *) &leader->request, message);
		sm_move(&leader->request_sm, R_SENT);
		break;
	}
	case L_BLOB_LOOP: {
		sm_init(&leader->request_sm, rpc_sm_invariant,
			NULL, rpc_sm_conf, R_INIT);
		sm_to_sm_obs(&leader->leader, &leader->request_sm);

		struct msg_blob *message;
		message = malloc(sizeof(*message));
		assert(message != NULL);
		buf = uv_buf_init((char *) message, sizeof *message);
		message->b.type = MSG_BLOB;
		message->b.sm_id = leader->request_sm.id;
		message->b.sm_pid = sm_pid();
		leader_file_read_chunk(leader, message);
		uv_req_set_data((uv_req_t *) &leader->request, message);
		sm_move(&leader->request_sm, R_SENT);

		char buf[80];
		sprintf(buf, "%d", leader->chunk_index);
		sm_attr_obs(&leader->request_sm, "chunk_index", buf);

		sprintf(buf, "%d", leader->chunks_total);
		sm_attr_obs(&leader->request_sm, "chunks_total", buf);

		sprintf(buf, "%d", leader->file_size);
		sm_attr_obs(&leader->request_sm, "file_size", buf);

		sprintf(buf, "%d", leader->chunks_received);
		sm_attr_obs(&leader->request_sm, "chunks_received", buf);

		sprintf(buf, "%d", message->chunk_size);
		sm_attr_obs(&leader->request_sm, "chunk_size", buf);

		break;
	}
	case L_RESTARTED_REPLIED:
	case L_BLOB_REPLIED:
		rc = uv_timer_stop(&leader->timeout);
		assert(rc == 0);

		sm_move(&leader->request_sm, R_REPLIED);
		sm_fini(&leader->request_sm);
		return 0;
	case L_RESTARTED_SENT:
	case L_BLOB_SENT:
	default:
		impossible();
	}

	rc = uv_ip4_addr("127.0.0.1", 8080, &saddr);
	assert(rc == 0);

	rc = uv_timer_start(&leader->timeout, on_timeout, 1000, 0);
	assert(rc == 0);

	return uv_udp_send(&leader->request, &leader->server, &buf, 1,
			   (const struct sockaddr*) &saddr, sent_cb);
}

static void leader_tick(struct party *leader, enum trigger trigger,
			const uv_buf_t *message, int status)
{
	struct sm *sm = &leader->leader;
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
			sm_move(&leader->request_sm, R_TIMEOUT);
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
		assert(rc == 0);

		struct msg_restarted_reply *msg;
		msg = (struct msg_restarted_reply *) message->base;
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
	}
}

static void follower_tick(struct party *follower, const struct sockaddr *addr,
			  ssize_t nread, const uv_buf_t *message)
{
	int rc;
	uv_buf_t buf;
	struct msg_base *req;

	req = (struct msg_base *) message->base;
	printf("recv type=%x\n", req->type);

	sm_init(&follower->reply_sm, rpc_sm_invariant, NULL,
		rpc_sm_conf, R_INIT);
	sm_move(&follower->reply_sm, R_RECV);
	sm_from_to_obs("rpc", req->sm_pid, req->sm_id,
		       "rpc", sm_pid(), follower->reply_sm.id);

	switch(req->type) {
	case MSG_RESTARTED: {
		struct msg_restarted_reply *reply;
		reply = malloc(sizeof(*reply));
		assert(reply != NULL);
		uv_req_set_data((uv_req_t *) &follower->reply, reply);
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
		fd = open("file.x", O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
		assert(fd > 0);

		size = lseek(fd, blob->chunk_index * CHUNK_SIZE, SEEK_SET);
		assert(size >= 0);

		rc = write(fd, blob->blob, blob->chunk_size);
		assert(rc > 0);
		close(fd);

		/* Prepare message */
		reply = malloc(sizeof(*reply));
		assert(reply != NULL);
		uv_req_set_data((uv_req_t *) &follower->reply, reply);
		reply->b.type = MSG_BLOB_REPLY;
		reply->result = 0;
		reply->known_index = blob->chunk_index;
		buf = uv_buf_init((char *)reply, sizeof(*reply));
		break;
	}
	default:
		impossible();
	}

	rc = uv_udp_send(&follower->reply, &follower->server, &buf, 1,
			 addr, on_send);
	assert(rc == 0);

	sm_move(&follower->reply_sm, R_RECV_SENT);
	sm_fini(&follower->reply_sm);
}

// -----------------------------------------------------------------------------

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

static int udp4_start(uv_udp_t *server, int port)
{
	struct sockaddr_in addr;
	int rc;

	rc = uv_ip4_addr("127.0.0.1", port, &addr);
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
	struct party *p = container_of(req, struct party, request);
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
	struct party party = {};
	int raddr = 8080;

	if (argc < 2)
		return EINVAL;

	if (strcmp("leader", argv[1]) == 0)
		party.role = R_LEADER;
	else if (strcmp("follower", argv[1]) == 0)
		party.role = R_FOLLOWER;
	else
		return EINVAL;

	rc = uv_timer_init(uv_default_loop(), &party.timeout);
	if (rc != 0)
		return rc;

	if (party.role == R_LEADER) {
		rc = uv_timer_start(&party.timeout, on_timeout, 0, 0);
		assert(rc == 0);
		raddr = 8081;
		party.leader = (struct sm) { .name = "leader" };
		sm_init(&party.leader, leader_sm_invariant,
			NULL, leader_sm_conf, L_RESTARTED_LOOP);
		party.request_sm = (struct sm) { .name = "rpc" };
		sm_attr_obs(&party.leader, "port", "8081");
	} else {
		party.reply_sm = (struct sm) { .name = "rpc" };
	}

	rc = udp4_start(&party.server, raddr);
	if (rc != 0)
		return rc;

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
	return 0;
}
