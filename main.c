#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uv.h>


struct party {
	uv_timer_t timeout;
	uv_udp_t   receiver;
};

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
	rc = uv_udp_send(req, handle, &buf, 1, addr, on_send);
	assert(rc <= 0);
}

static int udp4_start(uv_udp_t *receiver, int port)
{
	struct sockaddr_in addr;
	int rc;

	rc = uv_ip4_addr("127.0.0.1", port, &addr);
	assert(rc == 0);

	rc = uv_udp_init(uv_default_loop(), receiver);
	if (rc != 0) {
		fprintf(stderr, "uv_udp_init: %s\n", uv_strerror(rc));
		return 1;
	}

	rc = uv_udp_bind(receiver, (const struct sockaddr *) &addr, 0);
	if (rc != 0) {
		fprintf(stderr, "uv_udp_bind: %s\n", uv_strerror(rc));
		return 1;
	}

	rc = uv_udp_recv_start(receiver, pool_alloc, on_recv);
	if (rc != 0) {
		fprintf(stderr, "uv_udp_recv_start: %s\n", uv_strerror(rc));
		return 1;
	}

	return 0;
}

//static void on_timeout(uv_timer_t *handle)
//{
// fprintf(stderr, "timeout\n");
//}

//rc = uv_timer_start(&timeout, on_timeout, 2000, 0);
//if (rc != 0)
// return 1;

int main(void)
{
	int rc;
	struct party party;

	rc = uv_timer_init(uv_default_loop(), &party.timeout);
	if (rc != 0)
		return rc;

	rc = udp4_start(&party.receiver, 8080);
	if (rc != 0)
		return rc;

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
	return 0;
}
