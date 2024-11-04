#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>

#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))

enum role {
	R_SENDER,
	R_RECVER,
	R_NONE,
};

struct party {
	uv_udp_send_t      message;
	uv_timer_t         timeout;
	uv_udp_t           server;
	enum role          role;
};

static void sender_tick(struct party *sender)
{
}

static void recver_tick(struct party *recver)
{
}

// -----------------------------------------------------------------------------

static void pool_alloc(uv_handle_t *handle, size_t _suggested_sz, uv_buf_t *buf)
{
	static char slab[16 * 64 * 1024];

	buf->base = slab;
	buf->len = sizeof(slab);
}

static uv_udp_send_t *req_alloc(void)
{
	uv_udp_send_t *req;

	req = malloc(sizeof(*req));
	assert(req != NULL);
	return req;
}

static void on_send(uv_udp_send_t *req, int status)
{
	assert(req != NULL);
	assert(status == 0);
	free(req);
}

static void on_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *rcvbuf,
                    const struct sockaddr *addr, unsigned flags)
{
	uv_udp_send_t *req;
	uv_buf_t       buf;
	int            rc;

	if (nread == 0)
		return; /* Everything OK, but nothing read. */

	assert(nread > 0);
	assert(addr->sa_family == AF_INET);

	req = req_alloc();
	assert(req != NULL);

	buf = uv_buf_init(rcvbuf->base, nread);
	printf("recv: %s\n", rcvbuf->base);
	rc = uv_udp_send(req, handle, &buf, 1, addr, on_send);
	assert(rc <= 0);
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

static void send_cb(uv_udp_send_t* req, int status)
{
	assert(req != 0);
	assert(status == 0);
}

static void on_timeout(uv_timer_t *timeout)
{
	struct party *p = container_of(timeout, struct party, timeout);
	uv_buf_t buf;
	int rc;
	static char *dummy = "dummy";
	struct sockaddr_in saddr;

	fprintf(stderr, "timeout\n");

	rc = uv_ip4_addr("127.0.0.1", 8080, &saddr);
	assert(rc == 0);

	buf = uv_buf_init(dummy, 6);
	rc = uv_udp_send(&p->message, &p->server, &buf, 1,
			 (const struct sockaddr*) &saddr, send_cb);
	assert(rc == 0);
}

// -----------------------------------------------------------------------------

int main(int argc, char **argv)
{
	int rc;
	struct party party;
	int raddr = 8080;

	if (argc < 2)
		return EINVAL;

	if (strcmp("sender", argv[1]) == 0)
		party.role = R_SENDER;
	else if (strcmp("recver", argv[1]) == 0)
		party.role = R_RECVER;
	else
		return EINVAL;

	printf("role=%d\n", party.role);

	rc = uv_timer_init(uv_default_loop(), &party.timeout);
	if (rc != 0)
		return rc;

	if (party.role == R_SENDER) {
		rc = uv_timer_start(&party.timeout, on_timeout, 2000, 0);
		assert(rc == 0);
		raddr = 8081;
	}

	rc = udp4_start(&party.server, raddr);
	if (rc != 0)
		return rc;

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
	return 0;
}
