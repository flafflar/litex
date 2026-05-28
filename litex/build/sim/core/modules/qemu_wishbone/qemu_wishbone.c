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

#define QEMU_WB_REQ_MAGIC 0x3051584c /* "LXQ0" */
#define QEMU_WB_RSP_MAGIC 0x3052584c /* "LXR0" */
#define QEMU_WB_VERSION   1
#define QEMU_WB_REQ_SIZE  32
#define QEMU_WB_RSP_SIZE  32

enum qemu_wb_op {
  QEMU_WB_OP_READ  = 0,
  QEMU_WB_OP_WRITE = 1,
  QEMU_WB_OP_IRQ   = 2,
};

enum qemu_wb_status {
  QEMU_WB_STATUS_OK      = 0,
  QEMU_WB_STATUS_ERR     = 1,
  QEMU_WB_STATUS_BAD_REQ = 2,
};

struct qemu_wb_request_s {
  uint16_t op;
  uint32_t size;
  uint64_t addr;
  uint64_t data;
};

struct qemu_wb_txn_s {
  uint32_t adr;
  uint32_t dat_w;
  uint8_t sel;
  uint8_t bytes;
  uint8_t offset;
  uint8_t resp_shift;
};

struct session_s {
  uint32_t *adr;
  uint32_t *dat_w;
  uint32_t *dat_r;
  uint8_t  *sel;
  uint8_t  *cyc;
  uint8_t  *stb;
  uint8_t  *ack;
  uint8_t  *we;
  uint8_t  *cti;
  uint8_t  *bte;
  uint8_t  *err;
  uint8_t  *sys_clk;
  uint32_t *irq;
  uint8_t  *reset;

  clk_edge_state_t clk_edge;

  struct event *ev;
  int fd;
  int port;
  char bind[64];
  uint8_t rxbuf[QEMU_WB_REQ_SIZE];
  size_t rx_len;

  int req_valid;
  int active;
  int reset_latched;
  struct qemu_wb_request_s req;
  struct qemu_wb_txn_s txns[4];
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

  fprintf(stderr, "[qemu_wishbone] missing pad: %s\n", name);
  return RC_ERROR;
}

static int qemu_wishbone_parse_args(struct session_s *s, char *args)
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
    fprintf(stderr, "[qemu_wishbone] could not parse args: %s\n", args);
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

static void qemu_wishbone_drive_idle(struct session_s *s)
{
  if (!s->cyc) {
    return;
  }
  *s->cyc = 0;
  *s->stb = 0;
  *s->we  = 0;
  *s->sel = 0;
  *s->cti = 0;
  *s->bte = 0;
}

static uint32_t qemu_wishbone_irq(struct session_s *s)
{
  return s->irq ? *s->irq : 0;
}

static void qemu_wishbone_latch_reset(struct session_s *s)
{
  if (s->reset && *s->reset) {
    s->reset_latched = 1;
  }
}

static uint64_t qemu_wishbone_reset_status(struct session_s *s)
{
  uint64_t reset = s->reset_latched || (s->reset && *s->reset);

  s->reset_latched = 0;
  return reset;
}

static void qemu_wishbone_close_client(struct session_s *s)
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
  qemu_wishbone_drive_idle(s);
}

static int qemu_wishbone_send_response(struct session_s *s, uint16_t status, uint64_t data)
{
  uint8_t rsp[QEMU_WB_RSP_SIZE];
  size_t done = 0;

  if (s->fd < 0) {
    return RC_ERROR;
  }

  memset(rsp, 0, sizeof(rsp));
  wr_le32(rsp + 0, QEMU_WB_RSP_MAGIC);
  wr_le16(rsp + 4, QEMU_WB_VERSION);
  wr_le16(rsp + 6, status);
  wr_le32(rsp + 8, qemu_wishbone_irq(s));
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
    qemu_wishbone_close_client(s);
    return RC_ERROR;
  }

  return RC_OK;
}

static int qemu_wishbone_parse_request(struct session_s *s)
{
  uint32_t magic = rd_le32(s->rxbuf + 0);
  uint16_t version = rd_le16(s->rxbuf + 4);
  uint16_t op = rd_le16(s->rxbuf + 6);
  uint32_t size = rd_le32(s->rxbuf + 8);

  if (magic != QEMU_WB_REQ_MAGIC || version != QEMU_WB_VERSION) {
    return RC_ERROR;
  }

  if (op == QEMU_WB_OP_IRQ) {
    if (size != 0) {
      return RC_ERROR;
    }
  } else if ((op != QEMU_WB_OP_READ && op != QEMU_WB_OP_WRITE) ||
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

static void qemu_wishbone_read_handler(int fd, short event, void *arg)
{
  struct session_s *s = (struct session_s *)arg;

  (void)event;

  while (s->rx_len < QEMU_WB_REQ_SIZE && !s->req_valid) {
    ssize_t r = recv(fd, s->rxbuf + s->rx_len, QEMU_WB_REQ_SIZE - s->rx_len, 0);
    if (r > 0) {
      s->rx_len += (size_t)r;
      continue;
    }
    if (r == 0) {
      qemu_wishbone_close_client(s);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    qemu_wishbone_close_client(s);
    return;
  }

  if (s->rx_len == QEMU_WB_REQ_SIZE) {
    if (qemu_wishbone_parse_request(s) != RC_OK) {
      qemu_wishbone_send_response(s, QEMU_WB_STATUS_BAD_REQ, 0);
    }
    s->rx_len = 0;
  }
}

static void qemu_wishbone_accept_cb(struct evconnlistener *listener,
  evutil_socket_t fd, struct sockaddr *address, int socklen, void *ctx)
{
  struct session_s *s = (struct session_s *)ctx;

  (void)listener;
  (void)address;
  (void)socklen;

  qemu_wishbone_close_client(s);
  s->fd = fd;
  evutil_make_socket_nonblocking(fd);
  s->ev = event_new(base, fd, EV_READ | EV_PERSIST, qemu_wishbone_read_handler, s);
  event_add(s->ev, NULL);
  printf("[qemu_wishbone] client connected\n");
}

static void qemu_wishbone_accept_error_cb(struct evconnlistener *listener, void *ctx)
{
  struct event_base *event_base = evconnlistener_get_base(listener);

  (void)ctx;
  fprintf(stderr, "[qemu_wishbone] listener error\n");
  event_base_loopexit(event_base, NULL);
}

static int qemu_wishbone_start(void *b)
{
  base = (struct event_base *)b;
  printf("[qemu_wishbone] loaded (%p)\n", base);
  return RC_OK;
}

static int qemu_wishbone_new(void **sess, char *args)
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

  ret = qemu_wishbone_parse_args(s, args);
  if (ret != RC_OK) {
    goto out;
  }

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons((uint16_t)s->port);
  if (inet_pton(AF_INET, s->bind, &sin.sin_addr) != 1) {
    fprintf(stderr, "[qemu_wishbone] invalid bind address: %s\n", s->bind);
    ret = RC_ERROR;
    goto out;
  }

  listener = evconnlistener_new_bind(base, qemu_wishbone_accept_cb, s,
    LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
    (struct sockaddr *)&sin, sizeof(sin));
  if (!listener) {
    fprintf(stderr, "[qemu_wishbone] could not bind %s:%d\n", s->bind, s->port);
    ret = RC_ERROR;
    goto out;
  }
  evconnlistener_set_error_cb(listener, qemu_wishbone_accept_error_cb);
  printf("[qemu_wishbone] listening on %s:%d\n", s->bind, s->port);

out:
  *sess = (void *)s;
  return ret;
}

static int qemu_wishbone_add_pads(void *sess, struct pad_list_s *plist)
{
  int ret = RC_OK;
  struct session_s *s = (struct session_s *)sess;
  struct pad_s *pads;

  if (!sess || !plist) {
    return RC_INVARG;
  }

  pads = plist->pads;
  if (!strcmp(plist->name, "qemu_wishbone")) {
    ret |= litex_sim_module_pads_get(pads, "adr",   (void **)&s->adr);
    ret |= litex_sim_module_pads_get(pads, "dat_w", (void **)&s->dat_w);
    ret |= litex_sim_module_pads_get(pads, "dat_r", (void **)&s->dat_r);
    ret |= litex_sim_module_pads_get(pads, "sel",   (void **)&s->sel);
    ret |= litex_sim_module_pads_get(pads, "cyc",   (void **)&s->cyc);
    ret |= litex_sim_module_pads_get(pads, "stb",   (void **)&s->stb);
    ret |= litex_sim_module_pads_get(pads, "ack",   (void **)&s->ack);
    ret |= litex_sim_module_pads_get(pads, "we",    (void **)&s->we);
    ret |= litex_sim_module_pads_get(pads, "cti",   (void **)&s->cti);
    ret |= litex_sim_module_pads_get(pads, "bte",   (void **)&s->bte);
    ret |= litex_sim_module_pads_get(pads, "err",   (void **)&s->err);
  } else if (!strcmp(plist->name, "qemu_irq")) {
    ret |= litex_sim_module_pads_get(pads, "qemu_irq", (void **)&s->irq);
  } else if (!strcmp(plist->name, "qemu_reset")) {
    ret |= litex_sim_module_pads_get(pads, "qemu_reset", (void **)&s->reset);
  } else if (!strcmp(plist->name, "sys_clk")) {
    ret |= litex_sim_module_pads_get(pads, "sys_clk", (void **)&s->sys_clk);
  }

  return ret;
}

static int qemu_wishbone_build_txns(struct session_s *s)
{
  uint64_t addr = s->req.addr;
  uint64_t data = s->req.data;
  uint32_t remaining = s->req.size;
  uint8_t resp_shift = 0;

  s->txn_count = 0;
  s->txn_index = 0;
  s->resp_data = 0;

  while (remaining) {
    struct qemu_wb_txn_s *txn;
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
    txn->adr = addr >> 2;
    txn->bytes = bytes;
    txn->offset = offset;
    txn->resp_shift = resp_shift;
    txn->sel = ((1u << bytes) - 1u) << offset;

    for (i = 0; i < bytes; i++) {
      uint8_t byte = (data >> ((resp_shift + i) * 8)) & 0xff;
      txn->dat_w |= ((uint32_t)byte) << ((offset + i) * 8);
    }

    addr += bytes;
    remaining -= bytes;
    resp_shift += bytes;
  }

  return RC_OK;
}

static void qemu_wishbone_drive_txn(struct session_s *s)
{
  struct qemu_wb_txn_s *txn = &s->txns[s->txn_index];

  *s->adr   = txn->adr;
  *s->dat_w = txn->dat_w;
  *s->sel   = txn->sel;
  *s->we    = (s->req.op == QEMU_WB_OP_WRITE);
  *s->cti   = 0;
  *s->bte   = 0;
  *s->cyc   = 1;
  *s->stb   = 1;
}

static void qemu_wishbone_capture_read(struct session_s *s)
{
  struct qemu_wb_txn_s *txn = &s->txns[s->txn_index];
  uint32_t word = *s->dat_r;
  uint8_t i;

  for (i = 0; i < txn->bytes; i++) {
    uint8_t byte = (word >> ((txn->offset + i) * 8)) & 0xff;
    s->resp_data |= ((uint64_t)byte) << ((txn->resp_shift + i) * 8);
  }
}

static int qemu_wishbone_tick(void *sess, uint64_t time_ps)
{
  struct session_s *s = (struct session_s *)sess;

  (void)time_ps;

  if (!s || !s->sys_clk || !clk_pos_edge(&s->clk_edge, *s->sys_clk)) {
    return RC_OK;
  }
  qemu_wishbone_latch_reset(s);

  if (s->active && (*s->ack || *s->err)) {
    if (*s->err) {
      qemu_wishbone_send_response(s, QEMU_WB_STATUS_ERR, 0);
      s->active = 0;
      s->req_valid = 0;
    } else {
      if (s->req.op == QEMU_WB_OP_READ) {
        qemu_wishbone_capture_read(s);
      }
      s->txn_index++;
      if (s->txn_index >= s->txn_count) {
        qemu_wishbone_send_response(s, QEMU_WB_STATUS_OK, s->resp_data);
        s->active = 0;
        s->req_valid = 0;
      }
    }
  }

  if (!s->active && s->req_valid) {
    if (s->req.op == QEMU_WB_OP_IRQ) {
      qemu_wishbone_send_response(s, QEMU_WB_STATUS_OK, qemu_wishbone_reset_status(s));
      s->req_valid = 0;
    } else if (qemu_wishbone_build_txns(s) != RC_OK) {
      qemu_wishbone_send_response(s, QEMU_WB_STATUS_BAD_REQ, 0);
      s->req_valid = 0;
    } else {
      s->active = 1;
    }
  }

  if (s->active) {
    qemu_wishbone_drive_txn(s);
  } else {
    qemu_wishbone_drive_idle(s);
  }

  return RC_OK;
}

static struct ext_module_s ext_mod = {
  "qemu_wishbone",
  qemu_wishbone_start,
  qemu_wishbone_new,
  qemu_wishbone_add_pads,
  NULL,
  qemu_wishbone_tick
};

int litex_sim_ext_module_init(int (*register_module)(struct ext_module_s *))
{
  return register_module(&ext_mod);
}
