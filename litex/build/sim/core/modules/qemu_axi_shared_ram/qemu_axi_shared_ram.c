#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <json-c/json.h>

#include "error.h"
#include "modules.h"

#define AXI_RESP_OKAY    0
#define AXI_RESP_SLVERR  2
#define AXI_BURST_FIXED  0
#define AXI_BURST_INCR   1

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

  clk_edge_state_t clk_edge;

  int fd;
  char path[512];
  uint64_t size;
  uint8_t *mem;

  int write_active;
  uint32_t wr_addr;
  uint8_t wr_len;
  uint8_t wr_size;
  uint8_t wr_burst;
  uint8_t wr_id;
  uint8_t wr_beat;
  uint8_t wr_resp;
  int b_valid;
  uint8_t b_resp;
  uint8_t b_id;

  int read_active;
  uint32_t rd_addr;
  uint8_t rd_len;
  uint8_t rd_size;
  uint8_t rd_burst;
  uint8_t rd_id;
  uint8_t rd_beat;
  int r_valid;
  uint32_t r_data;
  uint8_t r_resp;
  uint8_t r_id;
  uint8_t r_last;
};

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

  fprintf(stderr, "[qemu_axi_shared_ram] missing pad: %s\n", name);
  return RC_ERROR;
}

static uint64_t qemu_axi_shared_ram_parse_size(const char *value)
{
  char *end = NULL;
  uint64_t size;

  if (!value) {
    return 0;
  }

  errno = 0;
  size = strtoull(value, &end, 0);
  if (errno || end == value) {
    return 0;
  }

  if (!strcmp(end, "K") || !strcmp(end, "k")) {
    size *= 1024ULL;
  } else if (!strcmp(end, "M") || !strcmp(end, "m")) {
    size *= 1024ULL * 1024ULL;
  } else if (!strcmp(end, "G") || !strcmp(end, "g")) {
    size *= 1024ULL * 1024ULL * 1024ULL;
  } else if (*end != '\0') {
    return 0;
  }

  return size;
}

static int qemu_axi_shared_ram_parse_args(struct session_s *s, char *args)
{
  json_object *args_json = NULL;
  json_object *obj = NULL;

  snprintf(s->path, sizeof(s->path), "%s", "qemu-main-ram.bin");
  s->size = 0;

  if (!args) {
    return RC_OK;
  }

  args_json = json_tokener_parse(args);
  if (!args_json) {
    fprintf(stderr, "[qemu_axi_shared_ram] could not parse args: %s\n", args);
    return RC_JSERROR;
  }

  if (json_object_object_get_ex(args_json, "path", &obj)) {
    snprintf(s->path, sizeof(s->path), "%s", json_object_get_string(obj));
  }
  if (json_object_object_get_ex(args_json, "size", &obj)) {
    if (json_object_get_type(obj) == json_type_string) {
      s->size = qemu_axi_shared_ram_parse_size(json_object_get_string(obj));
    } else {
      s->size = (uint64_t)json_object_get_int64(obj);
    }
  }

  json_object_put(args_json);

  if (s->size == 0) {
    fprintf(stderr, "[qemu_axi_shared_ram] invalid size\n");
    return RC_INVARG;
  }

  return RC_OK;
}

static int qemu_axi_shared_ram_map(struct session_s *s)
{
  s->fd = open(s->path, O_RDWR | O_CREAT, 0666);
  if (s->fd < 0) {
    fprintf(stderr, "[qemu_axi_shared_ram] could not open %s: %s\n", s->path, strerror(errno));
    return RC_ERROR;
  }

  if (ftruncate(s->fd, (off_t)s->size) < 0) {
    fprintf(stderr, "[qemu_axi_shared_ram] could not size %s: %s\n", s->path, strerror(errno));
    close(s->fd);
    s->fd = -1;
    return RC_ERROR;
  }

  s->mem = mmap(NULL, (size_t)s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
  if (s->mem == MAP_FAILED) {
    s->mem = NULL;
    fprintf(stderr, "[qemu_axi_shared_ram] could not mmap %s: %s\n", s->path, strerror(errno));
    close(s->fd);
    s->fd = -1;
    return RC_ERROR;
  }

  printf("[qemu_axi_shared_ram] mapped %s (%llu bytes)\n",
    s->path, (unsigned long long)s->size);

  return RC_OK;
}

static int qemu_axi_shared_ram_start(void *b)
{
  (void)b;
  printf("[qemu_axi_shared_ram] loaded\n");
  return RC_OK;
}

static int qemu_axi_shared_ram_new(void **sess, char *args)
{
  int ret = RC_OK;
  struct session_s *s = NULL;

  if (!sess) {
    return RC_INVARG;
  }

  s = (struct session_s *)malloc(sizeof(struct session_s));
  if (!s) {
    return RC_NOENMEM;
  }
  memset(s, 0, sizeof(struct session_s));
  s->fd = -1;

  ret = qemu_axi_shared_ram_parse_args(s, args);
  if (ret != RC_OK) {
    goto out;
  }

  ret = qemu_axi_shared_ram_map(s);

out:
  *sess = (void *)s;
  return ret;
}

static int qemu_axi_shared_ram_close(void *sess)
{
  struct session_s *s = (struct session_s *)sess;

  if (!s) {
    return RC_OK;
  }
  if (s->mem) {
    munmap(s->mem, (size_t)s->size);
  }
  if (s->fd >= 0) {
    close(s->fd);
  }
  free(s);
  return RC_OK;
}

static int qemu_axi_shared_ram_add_pads(void *sess, struct pad_list_s *plist)
{
  int ret = RC_OK;
  struct session_s *s = (struct session_s *)sess;
  struct pad_s *pads;

  if (!sess || !plist) {
    return RC_INVARG;
  }

  pads = plist->pads;
  if (!strcmp(plist->name, "qemu_axi_shared_ram")) {
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
  } else if (!strcmp(plist->name, "sys_clk")) {
    ret |= litex_sim_module_pads_get(pads, "sys_clk", (void **)&s->sys_clk);
  }

  return ret;
}

static uint32_t qemu_axi_shared_ram_read32(struct session_s *s, uint64_t addr)
{
  return ((uint32_t)s->mem[addr + 0]) |
    ((uint32_t)s->mem[addr + 1] << 8) |
    ((uint32_t)s->mem[addr + 2] << 16) |
    ((uint32_t)s->mem[addr + 3] << 24);
}

static void qemu_axi_shared_ram_write32(struct session_s *s,
  uint64_t addr, uint32_t data, uint8_t strb)
{
  int i;

  for (i = 0; i < 4; i++) {
    if (strb & (1 << i)) {
      s->mem[addr + i] = (data >> (8 * i)) & 0xff;
    }
  }
}

static uint8_t qemu_axi_shared_ram_write(struct session_s *s,
  uint32_t addr, uint32_t data, uint8_t strb)
{
  uint64_t word_addr = (uint64_t)addr & ~3ULL;

  if (word_addr + 4 > s->size) {
    return AXI_RESP_SLVERR;
  }

  qemu_axi_shared_ram_write32(s, word_addr, data, strb);
  return AXI_RESP_OKAY;
}

static uint8_t qemu_axi_shared_ram_read(struct session_s *s, uint32_t addr, uint32_t *data)
{
  uint64_t word_addr = (uint64_t)addr & ~3ULL;

  if (word_addr + 4 > s->size) {
    *data = 0;
    return AXI_RESP_SLVERR;
  }

  *data = qemu_axi_shared_ram_read32(s, word_addr);
  return AXI_RESP_OKAY;
}

static uint32_t qemu_axi_shared_ram_next_addr(uint32_t addr, uint8_t size, uint8_t burst)
{
  uint32_t step = 1u << size;

  if (step > 4) {
    step = 4;
  }
  if (burst == AXI_BURST_FIXED) {
    return addr;
  }
  return addr + step;
}

static void qemu_axi_shared_ram_drive(struct session_s *s)
{
  *s->awready = !s->write_active && !s->b_valid;
  *s->wready  = s->write_active && !s->b_valid;
  *s->bvalid  = s->b_valid;
  *s->bresp   = s->b_resp;
  *s->bid     = s->b_id;
  *s->buser   = 0;
  *s->arready = !s->read_active && !s->r_valid;
  *s->rvalid  = s->r_valid;
  *s->rresp   = s->r_resp;
  *s->rdata   = s->r_data;
  *s->rid     = s->r_id;
  *s->rlast   = s->r_last;
  *s->ruser   = 0;
}

static void qemu_axi_shared_ram_start_read_beat(struct session_s *s)
{
  s->r_resp = qemu_axi_shared_ram_read(s, s->rd_addr, &s->r_data);
  s->r_id = s->rd_id;
  s->r_last = s->rd_beat >= s->rd_len;
  s->r_valid = 1;

  if (!s->r_last) {
    s->rd_beat++;
    s->rd_addr = qemu_axi_shared_ram_next_addr(s->rd_addr, s->rd_size, s->rd_burst);
  }
}

static int qemu_axi_shared_ram_tick(void *sess, uint64_t time_ps)
{
  struct session_s *s = (struct session_s *)sess;

  (void)time_ps;

  if (!s || !s->sys_clk || !clk_pos_edge(&s->clk_edge, *s->sys_clk)) {
    return RC_OK;
  }

  if (s->b_valid && *s->bready) {
    s->b_valid = 0;
  }

  if (s->r_valid && *s->rready) {
    if (s->r_last) {
      s->read_active = 0;
    }
    s->r_valid = 0;
  }

  if (!s->write_active && !s->b_valid && *s->awvalid) {
    s->write_active = 1;
    s->wr_addr  = *s->awaddr;
    s->wr_len   = *s->awlen;
    s->wr_size  = *s->awsize;
    s->wr_burst = *s->awburst;
    s->wr_id    = *s->awid;
    s->wr_beat  = 0;
    s->wr_resp  = AXI_RESP_OKAY;
  }

  if (s->write_active && !s->b_valid && *s->wvalid) {
    uint8_t resp = qemu_axi_shared_ram_write(s, s->wr_addr, *s->wdata, *s->wstrb);
    if (resp != AXI_RESP_OKAY) {
      s->wr_resp = resp;
    }
    if (*s->wlast || s->wr_beat >= s->wr_len) {
      s->b_resp = s->wr_resp;
      s->b_id = s->wr_id;
      s->b_valid = 1;
      s->write_active = 0;
    } else {
      s->wr_beat++;
      s->wr_addr = qemu_axi_shared_ram_next_addr(s->wr_addr, s->wr_size, s->wr_burst);
    }
  }

  if (!s->read_active && !s->r_valid && *s->arvalid) {
    s->read_active = 1;
    s->rd_addr  = *s->araddr;
    s->rd_len   = *s->arlen;
    s->rd_size  = *s->arsize;
    s->rd_burst = *s->arburst;
    s->rd_id    = *s->arid;
    s->rd_beat  = 0;
  }

  if (s->read_active && !s->r_valid) {
    qemu_axi_shared_ram_start_read_beat(s);
  }

  qemu_axi_shared_ram_drive(s);
  return RC_OK;
}

static struct ext_module_s ext_mod = {
  "qemu_axi_shared_ram",
  qemu_axi_shared_ram_start,
  qemu_axi_shared_ram_new,
  qemu_axi_shared_ram_add_pads,
  qemu_axi_shared_ram_close,
  qemu_axi_shared_ram_tick
};

int litex_sim_ext_module_init(int (*register_module)(struct ext_module_s *))
{
  return register_module(&ext_mod);
}
