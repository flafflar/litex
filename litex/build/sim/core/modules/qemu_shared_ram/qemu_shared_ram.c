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

  clk_edge_state_t clk_edge;

  int fd;
  char path[512];
  uint64_t size;
  uint8_t *mem;
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

  fprintf(stderr, "[qemu_shared_ram] missing pad: %s\n", name);
  return RC_ERROR;
}

static uint64_t qemu_shared_ram_parse_size(const char *value)
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

static int qemu_shared_ram_parse_args(struct session_s *s, char *args)
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
    fprintf(stderr, "[qemu_shared_ram] could not parse args: %s\n", args);
    return RC_JSERROR;
  }

  if (json_object_object_get_ex(args_json, "path", &obj)) {
    snprintf(s->path, sizeof(s->path), "%s", json_object_get_string(obj));
  }
  if (json_object_object_get_ex(args_json, "size", &obj)) {
    if (json_object_get_type(obj) == json_type_string) {
      s->size = qemu_shared_ram_parse_size(json_object_get_string(obj));
    } else {
      s->size = (uint64_t)json_object_get_int64(obj);
    }
  }

  json_object_put(args_json);

  if (s->size == 0) {
    fprintf(stderr, "[qemu_shared_ram] invalid size\n");
    return RC_INVARG;
  }

  return RC_OK;
}

static int qemu_shared_ram_map(struct session_s *s)
{
  s->fd = open(s->path, O_RDWR | O_CREAT, 0666);
  if (s->fd < 0) {
    fprintf(stderr, "[qemu_shared_ram] could not open %s: %s\n", s->path, strerror(errno));
    return RC_ERROR;
  }

  if (ftruncate(s->fd, (off_t)s->size) < 0) {
    fprintf(stderr, "[qemu_shared_ram] could not size %s: %s\n", s->path, strerror(errno));
    close(s->fd);
    s->fd = -1;
    return RC_ERROR;
  }

  s->mem = mmap(NULL, (size_t)s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
  if (s->mem == MAP_FAILED) {
    s->mem = NULL;
    fprintf(stderr, "[qemu_shared_ram] could not mmap %s: %s\n", s->path, strerror(errno));
    close(s->fd);
    s->fd = -1;
    return RC_ERROR;
  }

  printf("[qemu_shared_ram] mapped %s (%llu bytes)\n",
    s->path, (unsigned long long)s->size);

  return RC_OK;
}

static int qemu_shared_ram_start(void *b)
{
  (void)b;
  printf("[qemu_shared_ram] loaded\n");
  return RC_OK;
}

static int qemu_shared_ram_new(void **sess, char *args)
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

  ret = qemu_shared_ram_parse_args(s, args);
  if (ret != RC_OK) {
    goto out;
  }

  ret = qemu_shared_ram_map(s);

out:
  *sess = (void *)s;
  return ret;
}

static int qemu_shared_ram_close(void *sess)
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

static int qemu_shared_ram_add_pads(void *sess, struct pad_list_s *plist)
{
  int ret = RC_OK;
  struct session_s *s = (struct session_s *)sess;
  struct pad_s *pads;

  if (!sess || !plist) {
    return RC_INVARG;
  }

  pads = plist->pads;
  if (!strcmp(plist->name, "qemu_shared_ram")) {
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
  } else if (!strcmp(plist->name, "sys_clk")) {
    ret |= litex_sim_module_pads_get(pads, "sys_clk", (void **)&s->sys_clk);
  }

  return ret;
}

static uint32_t qemu_shared_ram_read32(struct session_s *s, uint64_t addr)
{
  return ((uint32_t)s->mem[addr + 0]) |
    ((uint32_t)s->mem[addr + 1] << 8) |
    ((uint32_t)s->mem[addr + 2] << 16) |
    ((uint32_t)s->mem[addr + 3] << 24);
}

static void qemu_shared_ram_write32(struct session_s *s, uint64_t addr, uint32_t data, uint8_t sel)
{
  int i;

  for (i = 0; i < 4; i++) {
    if (sel & (1 << i)) {
      s->mem[addr + i] = (data >> (8 * i)) & 0xff;
    }
  }
}

static int qemu_shared_ram_tick(void *sess, uint64_t time_ps)
{
  struct session_s *s = (struct session_s *)sess;
  uint64_t addr;

  (void)time_ps;

  if (!s || !s->sys_clk || !clk_pos_edge(&s->clk_edge, *s->sys_clk)) {
    return RC_OK;
  }

  *s->ack = 0;
  *s->err = 0;

  if (!*s->cyc || !*s->stb) {
    return RC_OK;
  }

  addr = ((uint64_t)*s->adr) << 2;
  if (addr + 4 > s->size) {
    *s->ack = 1;
    *s->err = 1;
    return RC_OK;
  }

  if (*s->we) {
    qemu_shared_ram_write32(s, addr, *s->dat_w, *s->sel);
  } else {
    *s->dat_r = qemu_shared_ram_read32(s, addr);
  }

  *s->ack = 1;
  return RC_OK;
}

static struct ext_module_s ext_mod = {
  "qemu_shared_ram",
  qemu_shared_ram_start,
  qemu_shared_ram_new,
  qemu_shared_ram_add_pads,
  qemu_shared_ram_close,
  qemu_shared_ram_tick
};

int litex_sim_ext_module_init(int (*register_module)(struct ext_module_s *))
{
  return register_module(&ext_mod);
}
