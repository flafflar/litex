#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>
#include <json-c/json.h>

#include "error.h"
#include "modules.h"

#define QEMU_AXI_REQ_MAGIC 0x3051584c /* "LXQ0" */
#define QEMU_AXI_RSP_MAGIC 0x3052584c /* "LXR0" */
#define QEMU_AXI_VERSION   1
#define QEMU_AXI_REQ_SIZE  32
#define QEMU_AXI_RSP_SIZE  32
#define QEMU_AXI_RESP_OKAY 0
#define QEMU_AXI_SIZE_32B  2
#define QEMU_AXI_BURST_INCR 1

enum qemu_axi_op {
  QEMU_AXI_OP_READ  = 0,
  QEMU_AXI_OP_WRITE = 1,
  QEMU_AXI_OP_IRQ   = 2,
};

enum qemu_axi_status {
  QEMU_AXI_STATUS_OK      = 0,
  QEMU_AXI_STATUS_ERR     = 1,
  QEMU_AXI_STATUS_BAD_REQ = 2,
};

struct qemu_axi_request_s {
  uint16_t op;
  uint32_t size;
  uint64_t addr;
  uint64_t data;
};

struct qemu_axi_txn_s {
  uint32_t addr;
  uint32_t data;
  uint8_t strb;
  uint8_t bytes;
  uint8_t offset;
  uint8_t resp_shift;
};

struct session_s {
  uint8_t  *awvalid;
  uint8_t  *awready;
  uint32_t *awaddr;
  uint8_t  *awburst;
  uint8_t  *awlen;
  uint8_t  *awsize;
  uint8_t  *awlock;
  uint8_t  *awprot;
  uint8_t  *awcache;
  uint8_t  *awqos;
  uint8_t  *awregion;
  uint8_t  *awid;
  uint8_t  *awuser;
  uint8_t  *wvalid;
  uint8_t  *wready;
  uint8_t  *wlast;
  uint32_t *wdata;
  uint8_t  *wstrb;
  uint8_t  *wuser;
  uint8_t  *bvalid;
  uint8_t  *bready;
  uint8_t  *bresp;
  uint8_t  *bid;
  uint8_t  *buser;
  uint8_t  *arvalid;
  uint8_t  *arready;
  uint32_t *araddr;
  uint8_t  *arburst;
  uint8_t  *arlen;
  uint8_t  *arsize;
  uint8_t  *arlock;
  uint8_t  *arprot;
  uint8_t  *arcache;
  uint8_t  *arqos;
  uint8_t  *arregion;
  uint8_t  *arid;
  uint8_t  *aruser;
  uint8_t  *rvalid;
  uint8_t  *rready;
  uint8_t  *rlast;
  uint8_t  *rresp;
  uint32_t *rdata;
  uint8_t  *rid;
  uint8_t  *ruser;
  uint8_t  *sys_clk;
  uint32_t *irq;

  clk_edge_state_t clk_edge;

  struct event *ev;
  int fd;
  int port;
  char bind[64];
  uint8_t rxbuf[QEMU_AXI_REQ_SIZE];
  size_t rx_len;

  int req_valid;
  int active;
  int aw_done;
  int w_done;
  int b_seen;
  int ar_done;
  int r_seen;
  uint8_t b_resp;
  uint8_t r_resp;
  struct qemu_axi_request_s req;
  struct qemu_axi_txn_s txns[4];
  int txn_count;
  int txn_index;
  uint64_t resp_data;
};

static struct event_base *base;

static uint16_t rd_le16(const uint8_t *p)
{
  return ((uint16_t)p[0]) | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
  return ((uint32_t)p[0]) |
    ((uint32_t)p[1] << 8) |
    ((uint32_t)p[2] << 16) |
    ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p)
{
  return ((uint64_t)rd_le32(p)) | ((uint64_t)rd_le32(p + 4) << 32);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
}

static void wr_le32(uint8_t *p, uint32_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}

static void wr_le64(uint8_t *p, uint64_t v)
{
  wr_le32(p, v & 0xffffffffu);
  wr_le32(p + 4, v >> 32);
}

static int litex_sim_module_pads_get(struct pad_s *pads, char *name, void **signal)
{
  int i;

  if (!pads || !name || !signal) {
    return RC_INVARG;
  }

  *signal = NULL;
  for (i = 0; pads[i].name; i++) {
    if (!strcmp(pads[i].name, name)) {
      *signal = pads[i].signal;
      return RC_OK;
    }
  }

  fprintf(stderr, "[qemu_axi] missing pad: %s\n", name);
  return RC_ERROR;
}

static int qemu_axi_parse_args(struct session_s *s, char *args)
{
  json_object *args_json = NULL;
  json_object *obj = NULL;

  s->port = 1235;
  snprintf(s->bind, sizeof(s->bind), "%s", "127.0.0.1");

  if (!args) {
    return RC_OK;
  }

  args_json = json_tokener_parse(args);
  if (!args_json) {
    fprintf(stderr, "[qemu_axi] could not parse args: %s\n", args);
    return RC_JSERROR;
  }

  if (json_object_object_get_ex(args_json, "port", &obj)) {
    s->port = json_object_get_int(obj);
  }
  if (json_object_object_get_ex(args_json, "bind", &obj)) {
    snprintf(s->bind, sizeof(s->bind), "%s", json_object_get_string(obj));
  }

  json_object_put(args_json);
  return RC_OK;
}

static void qemu_axi_drive_idle(struct session_s *s)
{
  if (!s->awvalid) {
    return;
  }
  *s->awvalid = 0;
  *s->awaddr  = 0;
  *s->awburst = 0;
  *s->awlen   = 0;
  *s->awsize  = 0;
  *s->awlock  = 0;
  *s->awprot  = 0;
  *s->awcache = 0;
  *s->awqos   = 0;
  *s->awregion = 0;
  *s->awid    = 0;
  *s->awuser  = 0;
  *s->wvalid  = 0;
  *s->wlast   = 0;
  *s->wdata   = 0;
  *s->wstrb   = 0;
  *s->wuser   = 0;
  *s->bready  = 0;
  *s->arvalid = 0;
  *s->araddr  = 0;
  *s->arburst = 0;
  *s->arlen   = 0;
  *s->arsize  = 0;
  *s->arlock  = 0;
  *s->arprot  = 0;
  *s->arcache = 0;
  *s->arqos   = 0;
  *s->arregion = 0;
  *s->arid    = 0;
  *s->aruser  = 0;
  *s->rready  = 0;
}

static uint32_t qemu_axi_irq(struct session_s *s)
{
  return s->irq ? *s->irq : 0;
}

static void qemu_axi_close_client(struct session_s *s)
{
  if (s->ev) {
    event_del(s->ev);
    event_free(s->ev);
    s->ev = NULL;
  }
  if (s->fd >= 0) {
    close(s->fd);
    s->fd = -1;
  }
  s->rx_len = 0;
  s->req_valid = 0;
  s->active = 0;
  qemu_axi_drive_idle(s);
}

static int qemu_axi_send_response(struct session_s *s, uint16_t status, uint64_t data)
{
  uint8_t rsp[QEMU_AXI_RSP_SIZE];
  size_t done = 0;

  if (s->fd < 0) {
    return RC_ERROR;
  }

  memset(rsp, 0, sizeof(rsp));
  wr_le32(rsp + 0, QEMU_AXI_RSP_MAGIC);
  wr_le16(rsp + 4, QEMU_AXI_VERSION);
  wr_le16(rsp + 6, status);
  wr_le32(rsp + 8, qemu_axi_irq(s));
  wr_le64(rsp + 16, data);

  while (done < sizeof(rsp)) {
    ssize_t r = send(s->fd, rsp + done, sizeof(rsp) - done, 0);
    if (r > 0) {
      done += (size_t)r;
      continue;
    }
    if (r < 0 && (errno == EINTR)) {
      continue;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return RC_OK;
    }
    qemu_axi_close_client(s);
    return RC_ERROR;
  }

  return RC_OK;
}

static int qemu_axi_parse_request(struct session_s *s)
{
  uint32_t magic = rd_le32(s->rxbuf + 0);
  uint16_t version = rd_le16(s->rxbuf + 4);
  uint16_t op = rd_le16(s->rxbuf + 6);
  uint32_t size = rd_le32(s->rxbuf + 8);

  if (magic != QEMU_AXI_REQ_MAGIC || version != QEMU_AXI_VERSION) {
    return RC_ERROR;
  }

  if (op == QEMU_AXI_OP_IRQ) {
    if (size != 0) {
      return RC_ERROR;
    }
  } else if ((op != QEMU_AXI_OP_READ && op != QEMU_AXI_OP_WRITE) ||
             (size != 1 && size != 2 && size != 4 && size != 8)) {
    return RC_ERROR;
  }

  s->req.op   = op;
  s->req.size = size;
  s->req.addr = rd_le64(s->rxbuf + 16);
  s->req.data = rd_le64(s->rxbuf + 24);
  s->req_valid = 1;
  return RC_OK;
}

static void qemu_axi_read_handler(int fd, short event, void *arg)
{
  struct session_s *s = (struct session_s *)arg;

  (void)event;

  while (s->rx_len < QEMU_AXI_REQ_SIZE && !s->req_valid) {
    ssize_t r = recv(fd, s->rxbuf + s->rx_len, QEMU_AXI_REQ_SIZE - s->rx_len, 0);
    if (r > 0) {
      s->rx_len += (size_t)r;
      continue;
    }
    if (r == 0) {
      qemu_axi_close_client(s);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    qemu_axi_close_client(s);
    return;
  }

  if (s->rx_len == QEMU_AXI_REQ_SIZE) {
    if (qemu_axi_parse_request(s) != RC_OK) {
      qemu_axi_send_response(s, QEMU_AXI_STATUS_BAD_REQ, 0);
    }
    s->rx_len = 0;
  }
}

static void qemu_axi_accept_cb(struct evconnlistener *listener,
  evutil_socket_t fd, struct sockaddr *address, int socklen, void *ctx)
{
  struct session_s *s = (struct session_s *)ctx;

  (void)listener;
  (void)address;
  (void)socklen;

  qemu_axi_close_client(s);
  s->fd = fd;
  evutil_make_socket_nonblocking(fd);
  s->ev = event_new(base, fd, EV_READ | EV_PERSIST, qemu_axi_read_handler, s);
  event_add(s->ev, NULL);
  printf("[qemu_axi] client connected\n");
}

static void qemu_axi_accept_error_cb(struct evconnlistener *listener, void *ctx)
{
  struct event_base *event_base = evconnlistener_get_base(listener);

  (void)ctx;
  fprintf(stderr, "[qemu_axi] listener error\n");
  event_base_loopexit(event_base, NULL);
}

static int qemu_axi_start(void *b)
{
  base = (struct event_base *)b;
  printf("[qemu_axi] loaded (%p)\n", base);
  return RC_OK;
}

static int qemu_axi_new(void **sess, char *args)
{
  int ret = RC_OK;
  struct session_s *s = NULL;
  struct evconnlistener *listener = NULL;
  struct sockaddr_in sin;

  if (!sess) {
    return RC_INVARG;
  }

  s = (struct session_s *)malloc(sizeof(struct session_s));
  if (!s) {
    return RC_NOENMEM;
  }
  memset(s, 0, sizeof(struct session_s));
  s->fd = -1;

  ret = qemu_axi_parse_args(s, args);
  if (ret != RC_OK) {
    goto out;
  }

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons((uint16_t)s->port);
  if (inet_pton(AF_INET, s->bind, &sin.sin_addr) != 1) {
    fprintf(stderr, "[qemu_axi] invalid bind address: %s\n", s->bind);
    ret = RC_ERROR;
    goto out;
  }

  listener = evconnlistener_new_bind(base, qemu_axi_accept_cb, s,
    LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
    (struct sockaddr *)&sin, sizeof(sin));
  if (!listener) {
    fprintf(stderr, "[qemu_axi] could not bind %s:%d\n", s->bind, s->port);
    ret = RC_ERROR;
    goto out;
  }
  evconnlistener_set_error_cb(listener, qemu_axi_accept_error_cb);
  printf("[qemu_axi] listening on %s:%d\n", s->bind, s->port);

out:
  *sess = (void *)s;
  return ret;
}

static int qemu_axi_add_pads(void *sess, struct pad_list_s *plist)
{
  int ret = RC_OK;
  struct session_s *s = (struct session_s *)sess;
  struct pad_s *pads;

  if (!sess || !plist) {
    return RC_INVARG;
  }

  pads = plist->pads;
  if (!strcmp(plist->name, "qemu_axi")) {
    ret |= litex_sim_module_pads_get(pads, "awvalid",  (void **)&s->awvalid);
    ret |= litex_sim_module_pads_get(pads, "awready",  (void **)&s->awready);
    ret |= litex_sim_module_pads_get(pads, "awaddr",   (void **)&s->awaddr);
    ret |= litex_sim_module_pads_get(pads, "awburst",  (void **)&s->awburst);
    ret |= litex_sim_module_pads_get(pads, "awlen",    (void **)&s->awlen);
    ret |= litex_sim_module_pads_get(pads, "awsize",   (void **)&s->awsize);
    ret |= litex_sim_module_pads_get(pads, "awlock",   (void **)&s->awlock);
    ret |= litex_sim_module_pads_get(pads, "awprot",   (void **)&s->awprot);
    ret |= litex_sim_module_pads_get(pads, "awcache",  (void **)&s->awcache);
    ret |= litex_sim_module_pads_get(pads, "awqos",    (void **)&s->awqos);
    ret |= litex_sim_module_pads_get(pads, "awregion", (void **)&s->awregion);
    ret |= litex_sim_module_pads_get(pads, "awid",     (void **)&s->awid);
    ret |= litex_sim_module_pads_get(pads, "awuser",   (void **)&s->awuser);
    ret |= litex_sim_module_pads_get(pads, "wvalid",   (void **)&s->wvalid);
    ret |= litex_sim_module_pads_get(pads, "wready",   (void **)&s->wready);
    ret |= litex_sim_module_pads_get(pads, "wlast",    (void **)&s->wlast);
    ret |= litex_sim_module_pads_get(pads, "wdata",    (void **)&s->wdata);
    ret |= litex_sim_module_pads_get(pads, "wstrb",    (void **)&s->wstrb);
    ret |= litex_sim_module_pads_get(pads, "wuser",    (void **)&s->wuser);
    ret |= litex_sim_module_pads_get(pads, "bvalid",   (void **)&s->bvalid);
    ret |= litex_sim_module_pads_get(pads, "bready",   (void **)&s->bready);
    ret |= litex_sim_module_pads_get(pads, "bresp",    (void **)&s->bresp);
    ret |= litex_sim_module_pads_get(pads, "bid",      (void **)&s->bid);
    ret |= litex_sim_module_pads_get(pads, "buser",    (void **)&s->buser);
    ret |= litex_sim_module_pads_get(pads, "arvalid",  (void **)&s->arvalid);
    ret |= litex_sim_module_pads_get(pads, "arready",  (void **)&s->arready);
    ret |= litex_sim_module_pads_get(pads, "araddr",   (void **)&s->araddr);
    ret |= litex_sim_module_pads_get(pads, "arburst",  (void **)&s->arburst);
    ret |= litex_sim_module_pads_get(pads, "arlen",    (void **)&s->arlen);
    ret |= litex_sim_module_pads_get(pads, "arsize",   (void **)&s->arsize);
    ret |= litex_sim_module_pads_get(pads, "arlock",   (void **)&s->arlock);
    ret |= litex_sim_module_pads_get(pads, "arprot",   (void **)&s->arprot);
    ret |= litex_sim_module_pads_get(pads, "arcache",  (void **)&s->arcache);
    ret |= litex_sim_module_pads_get(pads, "arqos",    (void **)&s->arqos);
    ret |= litex_sim_module_pads_get(pads, "arregion", (void **)&s->arregion);
    ret |= litex_sim_module_pads_get(pads, "arid",     (void **)&s->arid);
    ret |= litex_sim_module_pads_get(pads, "aruser",   (void **)&s->aruser);
    ret |= litex_sim_module_pads_get(pads, "rvalid",   (void **)&s->rvalid);
    ret |= litex_sim_module_pads_get(pads, "rready",   (void **)&s->rready);
    ret |= litex_sim_module_pads_get(pads, "rlast",    (void **)&s->rlast);
    ret |= litex_sim_module_pads_get(pads, "rresp",    (void **)&s->rresp);
    ret |= litex_sim_module_pads_get(pads, "rdata",    (void **)&s->rdata);
    ret |= litex_sim_module_pads_get(pads, "rid",      (void **)&s->rid);
    ret |= litex_sim_module_pads_get(pads, "ruser",    (void **)&s->ruser);
  } else if (!strcmp(plist->name, "qemu_irq")) {
    ret |= litex_sim_module_pads_get(pads, "qemu_irq", (void **)&s->irq);
  } else if (!strcmp(plist->name, "sys_clk")) {
    ret |= litex_sim_module_pads_get(pads, "sys_clk", (void **)&s->sys_clk);
  }

  return ret;
}

static int qemu_axi_build_txns(struct session_s *s)
{
  uint64_t addr = s->req.addr;
  uint64_t data = s->req.data;
  uint32_t remaining = s->req.size;
  uint8_t resp_shift = 0;

  s->txn_count = 0;
  s->txn_index = 0;
  s->resp_data = 0;

  while (remaining) {
    struct qemu_axi_txn_s *txn;
    uint8_t offset = addr & 0x3;
    uint8_t bytes = 4 - offset;
    uint8_t i;

    if (bytes > remaining) {
      bytes = remaining;
    }
    if (s->txn_count >= (int)(sizeof(s->txns) / sizeof(s->txns[0]))) {
      return RC_ERROR;
    }

    txn = &s->txns[s->txn_count++];
    memset(txn, 0, sizeof(*txn));
    txn->addr = addr;
    txn->bytes = bytes;
    txn->offset = offset;
    txn->resp_shift = resp_shift;
    txn->strb = ((1u << bytes) - 1u) << offset;

    for (i = 0; i < bytes; i++) {
      uint8_t byte = (data >> ((resp_shift + i) * 8)) & 0xff;
      txn->data |= ((uint32_t)byte) << ((offset + i) * 8);
    }

    addr += bytes;
    remaining -= bytes;
    resp_shift += bytes;
  }

  return RC_OK;
}

static void qemu_axi_reset_txn_state(struct session_s *s)
{
  s->aw_done = 0;
  s->w_done = 0;
  s->b_seen = 0;
  s->ar_done = 0;
  s->r_seen = 0;
  s->b_resp = 0;
  s->r_resp = 0;
}

static void qemu_axi_capture_read(struct session_s *s)
{
  struct qemu_axi_txn_s *txn = &s->txns[s->txn_index];
  uint32_t word = *s->rdata;
  uint8_t i;

  for (i = 0; i < txn->bytes; i++) {
    uint8_t byte = (word >> ((txn->offset + i) * 8)) & 0xff;
    s->resp_data |= ((uint64_t)byte) << ((txn->resp_shift + i) * 8);
  }
}

static void qemu_axi_finish_txn(struct session_s *s, uint8_t resp)
{
  if (resp != QEMU_AXI_RESP_OKAY) {
    qemu_axi_send_response(s, QEMU_AXI_STATUS_ERR, 0);
    s->active = 0;
    s->req_valid = 0;
    return;
  }

  s->txn_index++;
  if (s->txn_index >= s->txn_count) {
    qemu_axi_send_response(s, QEMU_AXI_STATUS_OK, s->resp_data);
    s->active = 0;
    s->req_valid = 0;
  } else {
    qemu_axi_reset_txn_state(s);
  }
}

static void qemu_axi_drive_txn(struct session_s *s)
{
  struct qemu_axi_txn_s *txn = &s->txns[s->txn_index];

  if (s->req.op == QEMU_AXI_OP_WRITE) {
    *s->awvalid = !s->aw_done;
    *s->awaddr  = txn->addr;
    *s->awburst = QEMU_AXI_BURST_INCR;
    *s->awlen   = 0;
    *s->awsize  = QEMU_AXI_SIZE_32B;
    *s->awlock  = 0;
    *s->awprot  = 0;
    *s->awcache = 0x3;
    *s->awqos   = 0;
    *s->awregion = 0;
    *s->awid    = 0;
    *s->awuser  = 0;
    *s->wvalid  = !s->w_done;
    *s->wlast   = 1;
    *s->wdata   = txn->data;
    *s->wstrb   = txn->strb;
    *s->wuser   = 0;
    *s->bready  = s->b_seen;
    *s->arvalid = 0;
    *s->rready  = 0;
  } else {
    *s->awvalid = 0;
    *s->wvalid  = 0;
    *s->bready  = 0;
    *s->arvalid = !s->ar_done;
    *s->araddr  = txn->addr;
    *s->arburst = QEMU_AXI_BURST_INCR;
    *s->arlen   = 0;
    *s->arsize  = QEMU_AXI_SIZE_32B;
    *s->arlock  = 0;
    *s->arprot  = 0;
    *s->arcache = 0x3;
    *s->arqos   = 0;
    *s->arregion = 0;
    *s->arid    = 0;
    *s->aruser  = 0;
    *s->rready  = s->r_seen;
  }
}

static int qemu_axi_tick(void *sess, uint64_t time_ps)
{
  struct session_s *s = (struct session_s *)sess;

  (void)time_ps;

  if (!s || !s->sys_clk || !clk_pos_edge(&s->clk_edge, *s->sys_clk)) {
    return RC_OK;
  }

  if (s->active && s->req.op == QEMU_AXI_OP_WRITE) {
    int aw_valid = !s->aw_done;
    int w_valid  = (s->aw_done || (aw_valid && *s->awready)) && !s->w_done;
    if (s->b_seen) {
      qemu_axi_finish_txn(s, s->b_resp);
    } else {
      if (aw_valid && *s->awready) {
        s->aw_done = 1;
      }
      if (w_valid && *s->wready) {
        s->w_done = 1;
      }
      if (*s->bvalid) {
        s->b_resp = *s->bresp;
        s->b_seen = 1;
      }
    }
  }

  if (s->active && s->req.op == QEMU_AXI_OP_READ) {
    int ar_valid = !s->ar_done;
    if (s->r_seen) {
      qemu_axi_finish_txn(s, s->r_resp);
    } else {
      if (ar_valid && *s->arready) {
        s->ar_done = 1;
      }
      if (*s->rvalid) {
        qemu_axi_capture_read(s);
        s->r_resp = *s->rresp;
        s->r_seen = 1;
      }
    }
  }

  if (!s->active && s->req_valid) {
    if (s->req.op == QEMU_AXI_OP_IRQ) {
      qemu_axi_send_response(s, QEMU_AXI_STATUS_OK, 0);
      s->req_valid = 0;
    } else if (qemu_axi_build_txns(s) != RC_OK) {
      qemu_axi_send_response(s, QEMU_AXI_STATUS_BAD_REQ, 0);
      s->req_valid = 0;
    } else {
      qemu_axi_reset_txn_state(s);
      s->active = 1;
    }
  }

  if (s->active) {
    qemu_axi_drive_txn(s);
  } else {
    qemu_axi_drive_idle(s);
  }

  return RC_OK;
}

static struct ext_module_s ext_mod = {
  "qemu_axi",
  qemu_axi_start,
  qemu_axi_new,
  qemu_axi_add_pads,
  NULL,
  qemu_axi_tick
};

int litex_sim_ext_module_init(int (*register_module)(struct ext_module_s *))
{
  return register_module(&ext_mod);
}
