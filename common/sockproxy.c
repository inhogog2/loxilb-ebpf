/*
 * Copyright (c) 2024 NetLOX Inc
 *
 * SPDX short identifier: BSD-3-Clause
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <fcntl.h>

#include <locale.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <netdb.h>
#include <poll.h>
#include <bpf.h>
#include <ifaddrs.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/types.h>
#include <linux/tls.h>
#include <linux/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "log.h"
#include "common_pdi.h"
#include "llb_dpapi.h"
#include "notify.h"
#include "uthash.h"
#include "picohttpparser.h"
#include "sockproxy.h"

#define PROXY_NUM_BURST_RX 1024
#define PROXY_MAX_THREADS 4

#define PROXY_SSL_FNAME_SZ 128
#define PROXY_SSL_CERT_DIR "/opt/loxilb/cert"
#define PROXY_SSL_CA_DIR "/etc/ssl/certs"

typedef struct proxy_ep_val {
  int ep_cfd;
  int ep_num;
} proxy_ep_val_t;

typedef struct proxy_ep_sel {
  proxy_ep_val_t ep_cfds[MAX_PROXY_EP];
  int n_eps;
} proxy_ep_sel_t;

typedef struct proxy_epstat {
  uint64_t nrb;
  uint64_t nrp;
  uint64_t ntb;
  uint64_t ntp;
} proxy_epstat_t;

struct proxy_epval {
  char host_url[256];
  uint32_t _id;
  int main_fd;
  int n_eps;
  int ep_sel;
  int select;
  proxy_ent_t eps[MAX_PROXY_EP];
  proxy_epstat_t ep_stats[MAX_PROXY_EP];
  UT_hash_handle hh;
};
typedef struct proxy_epval proxy_epval_t;

struct proxy_val {
  int proxy_mode;
  int main_fd;
  int have_ssl;
  int have_epssl;
  int sched_free;
  void *ssl_ctx;
  void *ssl_epctx;
  uint32_t nfds;
  struct proxy_epval *ephash;
  struct proxy_fd_ent *fdlist;
};
typedef struct proxy_val proxy_val_t;

typedef struct proxy_map_ent {
  struct proxy_ent key;
  struct proxy_val val;
  struct proxy_map_ent *next;
} proxy_map_ent_t;

#define PROXY_START_MAPFD 500
#define PROXY_MAX_MAPFD 200
#define PROXY_MAPFD_ALLOC_RETRIES 100
#define PROXY_MAPFD_RETRIES 5

typedef struct proxy_mapfd {
  uint16_t start;
  uint16_t end;
  uint16_t next;
} proxy_mapfd_t;

typedef struct proxy_struct {
  pthread_rwlock_t lock;
  pthread_t pthr;
  proxy_map_ent_t *head;
  sockmap_cb_t sockmap_cb;
  void *ns;
  proxy_mapfd_t mapfd[PROXY_MAX_THREADS];
} proxy_struct_t;

typedef struct llb_sockmap_key smap_key_t;

static proxy_struct_t *proxy_struct;

#ifdef HAVE_PROXY_MAPFD
static int
fd_in_use(int fd)
{
  return (fcntl(fd, F_GETFD) != -1) || (errno != EBADF);
}

static int
get_random_fd_range(int r1, int r2)
{
   return r1 + rand() / (RAND_MAX / (r2 - r1 + 1) + 1);
}

static int
get_mapped_proxy_fd(int fd, int check_slot)
{
  proxy_mapfd_t *mep;
  int dfd, retry;
  pid_t tid;

  if (check_slot) {
    if (notify_check_slot(proxy_struct->ns, fd)) {
      return fd;
    }
  }

  tid = gettid() % PROXY_MAX_THREADS;
  mep = &proxy_struct->mapfd[tid];

  if (mep->next < mep->start ||
      mep->next >= mep->end) {
    mep->next = mep->start;
  }

  mep->next = get_random_fd_range(mep->start, mep->end);

  for (retry = 0; retry < PROXY_MAPFD_ALLOC_RETRIES; retry++) {
    mep->next++;
    if (fd_in_use(mep->next)) {
      continue;
    }
    dfd = mep->next;
    break;
  }

  if (retry >= PROXY_MAPFD_ALLOC_RETRIES) {
    log_error("mapfd (%d) find failed", fd);
    return fd;
  }

  if (dup2(fd, dfd) < 0) {
    log_error("mapfd (%d) dup2 failed", fd);
    return fd;
  }

  close(fd);
  return dfd;
}
#else
static int
get_mapped_proxy_fd(int fd, int check_slot)
{
  return fd;
}
#endif

static void
pfe_ent_accouting(proxy_fd_ent_t *pfe, uint64_t bc, int txdir)
{
  proxy_epval_t *epv = pfe->epv;
  int n = pfe->ep_num;
  if (!txdir) {
    pfe->nrb += bc;
    pfe->nrp++;
    if (epv && n >= 0 && n < MAX_PROXY_EP) {
      epv->ep_stats[n].nrb += bc;
      epv->ep_stats[n].nrp++;
    }
  } else {
    pfe->ntb += bc;
    pfe->ntp++;
    if (epv && n >= 0 && n < MAX_PROXY_EP) {
      epv->ep_stats[n].ntb += bc;
      epv->ep_stats[n].ntp++;
    }
  }
}

static bool
cmp_proxy_ent(proxy_ent_t *e1, proxy_ent_t *e2)
{
  if (e1->xip == e2->xip &&
      e1->xport == e2->xport &&
      e1->protocol == e2->protocol) {
    return true;
  }
  return false;
}

#if 0
static bool
cmp_proxy_val(proxy_val_t *v1, proxy_val_t *v2)
{
  int i;
  for (i = 0; i < MAX_PROXY_EP; i++) {
    if (!cmp_proxy_ent(&v1->eps[i], &v2->eps[i])) {
      return false;
    }
  }
  return true;
}
#endif

static int
proxy_add_xmitcache(proxy_fd_ent_t *ent, uint8_t *cache, size_t len)
{
  struct proxy_cache *new;
  struct proxy_cache *curr;
  struct proxy_cache **prev;

  new  = calloc(1, sizeof(struct proxy_cache)+len);
  assert(new);
  new->cache = new->data;
  memcpy(new->cache, cache, len);
  new->off = 0;;
  new->len = len;

  if (ent->cache_head == NULL) {
    notify_add_ent(proxy_struct->ns, ent->fd,
        NOTI_TYPE_IN|NOTI_TYPE_OUT|NOTI_TYPE_HUP, ent);
  }

  PROXY_ENT_CLOCK(ent);

  curr = ent->cache_head;
  prev = &ent->cache_head;

  while (curr) {
    prev = &curr->next;
    curr = curr->next;
  }

  if (prev) {
    *prev = new;
  }

  PROXY_ENT_CUNLOCK(ent);

  return 0;
}

#define HAVE_PROXY_DEBUG
#ifdef HAVE_PROXY_DEBUG
static void
proxy_log(const char *str, smap_key_t *key)
{
  char ab1[INET6_ADDRSTRLEN];
  char ab2[INET6_ADDRSTRLEN];

  inet_ntop(AF_INET, (struct in_addr *)&key->dip, ab1, INET_ADDRSTRLEN);
  inet_ntop(AF_INET, (struct in_addr *)&key->sip, ab2, INET_ADDRSTRLEN);
  log_trace("%s %s:%u -> %s:%u", str,
            ab1, ntohs((key->dport >> 16)), ab2, ntohs(key->sport >> 16));
}
#else
#define proxy_log(arg1, arg2)
#endif

static void
proxy_log_always(const char *str, smap_key_t *key)
{
  char ab1[INET6_ADDRSTRLEN];
  char ab2[INET6_ADDRSTRLEN];

  inet_ntop(AF_INET, (struct in_addr *)&key->dip, ab1, INET_ADDRSTRLEN);
  inet_ntop(AF_INET, (struct in_addr *)&key->sip, ab2, INET_ADDRSTRLEN);
  log_debug("%s %s:%u -> %s:%u", str,
            ab1, ntohs((key->dport >> 16)), ab2, ntohs(key->sport >> 16));
}

static void
proxy_destroy_xmitcache(proxy_fd_ent_t *ent)
{
  struct proxy_cache *curr = ent->cache_head;
  struct proxy_cache *next;

  while (curr) {
    next = curr->next;
    free(curr);
    curr = next;
  }
  ent->cache_head = NULL;
}

static void __attribute__((unused))
proxy_list_xmitcache(proxy_fd_ent_t *ent)
{
  int i = 0;
  struct proxy_cache *curr = ent->cache_head;

  while (curr) {
    curr = curr->next;
    i++;
  }
}

static int
proxy_xmit_cache(proxy_fd_ent_t *ent)
{
  struct proxy_cache *curr;
  struct proxy_cache *tmp = NULL;
  int rstev = 0;
  int n = 0;

  PROXY_ENT_CLOCK(ent);

  curr = ent->cache_head;
  if (ent->cache_head != NULL) {
    rstev = 1;
  }

  while (curr) {
    if (!ent->ssl) {
      n = send(ent->fd, (uint8_t *)(curr->cache) + curr->off, curr->len, MSG_DONTWAIT|MSG_NOSIGNAL);
      if (n <= 0) {
        /* errno == EAGAIN || errno == EWOULDBLOCK */
        //log_debug("Failed to send cache");
        PROXY_ENT_CUNLOCK(ent);
        return -1;
      }
      if (n != curr->len) {
        curr->off += n;
        curr->len -= n;
        pfe_ent_accouting(ent, n, 1);
        continue;
      }
    } else {
      n = SSL_write(ent->ssl, (uint8_t *)(curr->cache) + curr->off, curr->len);
      if (n <= 0) {
        switch (SSL_get_error(ent->ssl, n)) {
        case SSL_ERROR_NONE:
          PROXY_ENT_CUNLOCK(ent);
          return 0;
        case SSL_ERROR_WANT_WRITE:
          PROXY_ENT_CUNLOCK(ent);
          //log_trace("ssl-want-wr %s",
          //    ERR_error_string(ERR_get_error(), NULL));
          notify_add_ent(proxy_struct->ns, ent->fd,
            NOTI_TYPE_IN|NOTI_TYPE_HUP|NOTI_TYPE_OUT, ent);
          return -1;
        case SSL_ERROR_WANT_READ:
          PROXY_ENT_CUNLOCK(ent);
          //log_trace("ssl-want-rd %s",
          //    ERR_error_string(ERR_get_error(), NULL));
          return -1;
        case SSL_ERROR_SYSCALL:
        case SSL_ERROR_SSL:
          PROXY_ENT_CUNLOCK(ent);
          log_trace("ssl-err-sys/call %s",
              ERR_error_string(ERR_get_error(), NULL));
          ent->ssl_err = 1;
          return -1;
        case SSL_ERROR_ZERO_RETURN:
          log_trace("ssl-wr-zero-ret %s",
              ERR_error_string(ERR_get_error(), NULL));
        default:
          //log_trace("ssl-err-ret %s",
          //    ERR_error_string(ERR_get_error(), NULL));
          SSL_shutdown(ent->ssl);
          PROXY_ENT_CUNLOCK(ent);
          return -1;
        }
      }
    }

    tmp = curr;

    curr = curr->next;
    ent->cache_head = curr;

    if (tmp)
      free(tmp);
  }

  ent->cache_head = NULL;
  PROXY_ENT_CUNLOCK(ent);

  if (rstev) {
    notify_add_ent(proxy_struct->ns, ent->fd,
          NOTI_TYPE_IN|NOTI_TYPE_HUP, ent);
  }

  return 0;
}

static int
proxy_try_epxmit(proxy_fd_ent_t *ent, void *msg, size_t len, int sel)
{
  int n;
  proxy_fd_ent_t *rfd_ent = NULL;

  if (ent->rfd_ent[sel]) {
    rfd_ent = ent->rfd_ent[sel];
  }

  if (rfd_ent) {
    PROXY_ENT_LOCK(rfd_ent);
    n = proxy_xmit_cache(rfd_ent);
    if (n < 0) {
      proxy_add_xmitcache(rfd_ent, msg, len);
      PROXY_ENT_UNLOCK(rfd_ent);
      return 0;
    }

    if (!rfd_ent->ssl) {
      n = send(ent->rfd[sel], msg, len, MSG_DONTWAIT|MSG_NOSIGNAL);
    } else {
      n = SSL_write(rfd_ent->ssl, msg, len);
      if (n <= 0) {
        int ssl_err;
        switch ((ssl_err = SSL_get_error(rfd_ent->ssl, n))) {
          case SSL_ERROR_WANT_WRITE:
            log_trace("ssl-want-wr %s",
              ERR_error_string(ERR_get_error(), NULL));
            if (!sel) proxy_add_xmitcache(rfd_ent, msg, len);
            notify_add_ent(proxy_struct->ns, rfd_ent->fd,
              NOTI_TYPE_IN|NOTI_TYPE_HUP|NOTI_TYPE_OUT, rfd_ent);
            PROXY_ENT_UNLOCK(rfd_ent);
            return 0;
          case SSL_ERROR_WANT_READ:
            log_trace("ssl-want-rd %s",
              ERR_error_string(ERR_get_error(), NULL));
            if (!sel) proxy_add_xmitcache(rfd_ent, msg, len);
            PROXY_ENT_UNLOCK(rfd_ent);
            return 0;
          case SSL_ERROR_SSL:
          case SSL_ERROR_SYSCALL:
            log_trace("ssl-err-sys/call %s",
                ERR_error_string(ERR_get_error(), NULL));
          default:
            if (ssl_err != SSL_ERROR_SSL && ssl_err != SSL_ERROR_SYSCALL) {
              SSL_shutdown(rfd_ent->ssl);
            } else {
              rfd_ent->ssl_err = 1;
            }
            if (rfd_ent->odir) {
              shutdown(ent->fd, SHUT_RDWR);
            } else {
              shutdown(rfd_ent->fd, SHUT_RDWR);
            }
            PROXY_ENT_UNLOCK(rfd_ent);
            return -1;
        }
      }
    }
    if (n != len) {
      if (n > 0) {
        pfe_ent_accouting(rfd_ent, n, 1);
        if (!sel) proxy_add_xmitcache(rfd_ent, (uint8_t *)(msg) + n, len - n);
        PROXY_ENT_UNLOCK(rfd_ent);
        return 0;
      } else /*if (n <= 0)*/ {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          if (!sel) proxy_add_xmitcache(rfd_ent, msg, len);
          PROXY_ENT_UNLOCK(rfd_ent);
          return 0;
        }
        PROXY_ENT_UNLOCK(rfd_ent);
        return -1;
      }
    }

    pfe_ent_accouting(rfd_ent, n, 1);
    PROXY_ENT_UNLOCK(rfd_ent);
  }

  return 0;
}

static int
proxy_skmap_key_from_fd(int fd, smap_key_t *skmap_key, int *protocol)
{
  struct sockaddr_in sin_addr;
  socklen_t sin_len;
  socklen_t optsize = sizeof(int);

  if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, protocol, &optsize)) {
    log_error("getsockopt failed %s\n", strerror(errno));
    return -1;
  }

  sin_len = sizeof(struct sockaddr);
  if (getsockname(fd, (struct sockaddr*)&sin_addr, &sin_len)) {
    log_error("getsockname failed %s\n", strerror(errno));
    return -1;
  }
  skmap_key->sip = sin_addr.sin_addr.s_addr;
  skmap_key->sport = sin_addr.sin_port << 16;

  if (getpeername(fd, (struct sockaddr*)&sin_addr, &sin_len)) {
    log_error("getpeername failed %s\n", strerror(errno));
    return -1;
  }
  skmap_key->dip = sin_addr.sin_addr.s_addr;
  skmap_key->dport = sin_addr.sin_port << 16;

  return 0;
}

#ifdef HAVE_SOCKMAP_KTLS
static int
proxy_sock_init_ktls(int fd)
{
  int so_buf = 6553500;
  int err;
  struct tls12_crypto_info_aes_gcm_128 tls_tx = { 0 };
  struct tls12_crypto_info_aes_gcm_128 tls_rx = { 0 };

  tls_tx.info.version = TLS_1_2_VERSION;
  tls_tx.info.cipher_type = TLS_CIPHER_AES_GCM_128;

  tls_rx.info.version = TLS_1_2_VERSION;
  tls_rx.info.cipher_type = TLS_CIPHER_AES_GCM_128;

  err = setsockopt(fd, 6, TCP_ULP, "tls", sizeof("tls"));
  if (err) {
    log_error("setsockopt: TCP_ULP failed error %d\n", err);
    return -EINVAL;
  }

  err = setsockopt(fd, SOL_TLS, TLS_TX, (void *)&tls_tx, sizeof(tls_tx));
  if (err) {
    log_error("setsockopt: TLS_TX failed error %d\n", err);
    return -EINVAL;
  }

  err = setsockopt(fd, SOL_TLS, TLS_RX, (void *)&tls_rx, sizeof(tls_rx));
  if (err) {
    log_error("setsockopt: TLS_RX failed error %d\n", err);
    return -EINVAL;
  }

  err = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &so_buf, sizeof(so_buf));
  if (err) {
    log_error("setsockopt: SO_SNDBUF failed error %d\n", err);
    return -EINVAL;
  }

  err = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &so_buf, sizeof(so_buf));
  if (err) {
    log_error("setsockopt: SO_RCVBUF failed error %d\n", err);
    return -EINVAL;
  }

  return 0;
}
#endif

static void
proxy_sock_setnb(int fd)
{
  int rc, flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    flags = 0;
  }

  rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (rc == -1) {
    assert(0);
  }
}

static void
proxy_sock_setnodelay(int fd)
{
  int flag = 1;
  int rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                    (char *) &flag, sizeof(int));
  if (rc == -1) {
    log_error("setsockopt: failed to set tcp nodelay");
  }
}

static void
proxy_sock_set_opts(int fd, uint8_t protocol)
{
  proxy_sock_setnb(fd);

  switch (protocol) {
  case IPPROTO_TCP:
    proxy_sock_setnodelay(fd);
    break;
  default:
    break;
  }
}

static int
proxy_server_setup(int fd, uint32_t server, uint16_t port, uint8_t protocol)
{
  struct sockaddr_in addr;
  int rc, on = 1, flags;

#ifdef HAVE_SCTP_STREAM_CONF 
  struct sctp_initmsg im;
  if (protocol == IPPROTO_SCTP) {
    memset(&im, 0, sizeof(im));
    im.sinit_num_ostreams = 1;
    im.sinit_max_instreams = 1;
    im.sinit_max_attempts = 4;
    rc = setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, &im, sizeof(im));
    if (rc < 0) {
      close(fd);
      return -1;
    }
  }
#endif

  rc = setsockopt(fd, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
  if (rc < 0) {
    close(fd);
    return -1;
  }

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    flags = 0;
  }

  rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (rc == -1) {
    assert(0);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = port;
  addr.sin_addr.s_addr = server;
  rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    log_error("bind failed %s", strerror(errno));
    close(fd);
    return -1; 
  }

  rc = listen(fd, 32);
  if (rc < 0) {
    log_error("listen failed %s", strerror(errno));
    close(fd);
    return -1;
  }

  log_info("sock-proxy setup done");
  return 0;
}

static int
proxy_ssl_connect(int fd, void *ssl)
{
  int to = 10;
  int err;
  int ssl_err;
  int sret;
  struct pollfd pfds = { 0 };

  assert(ssl);
  SSL_set_fd(ssl, fd);

  pfds.fd = fd;

  while (to--) {
    err = SSL_connect(ssl);
    if (err == 1) {
      break;
    }

    ssl_err = SSL_get_error(ssl, err);
    if (ssl_err == SSL_ERROR_WANT_READ) {
      pfds.events = POLLIN;
      sret = poll(&pfds, 1, 500);
      if (sret == -1) {
        return -1;
      }
    } else if (ssl_err == SSL_ERROR_WANT_WRITE) {
      pfds.events = POLLOUT;
      sret = poll(&pfds, 1, 500);
      if (sret == -1) {
        return -1;
      }
    } else {
      log_error("Unable to ssl-connect %s",
        ERR_error_string(ERR_get_error(), NULL));

      return -1;
    }
  }

  return 0;
}

static int
proxy_setup_ep_connect(uint32_t epip, uint16_t epport, uint8_t protocol,
                       void *ssl_ctx, void **ssl)
{
  int fd, rc;
  struct sockaddr_in epaddr;
  struct pollfd pfds = { 0 };

  memset(&epaddr, 0, sizeof(epaddr));
  epaddr.sin_family = AF_INET;
  epaddr.sin_port = epport;
  epaddr.sin_addr.s_addr = epip;

  fd = socket(AF_INET, SOCK_STREAM, protocol);
  if (fd < 0) {
    return -1;
  }

  fd = get_mapped_proxy_fd(fd, 1);

  proxy_sock_set_opts(fd, protocol);

  if (connect(fd, (struct sockaddr*)&epaddr, sizeof(epaddr)) < 0) {
    if (errno != EINPROGRESS) {
      log_error("connect failed %s:%u", inet_ntoa(*(struct in_addr *)(&epip)), ntohs(epport));
      close(fd);
      return -1;
    }

    pfds.fd = fd;
    pfds.events = POLLOUT|POLLERR;

    rc = poll(&pfds, 1, 500);
    if (rc < 0) {
      log_error("connect poll %s:%u(%s)", inet_ntoa(*(struct in_addr *)(&epip)), ntohs(epport), strerror(errno));
      close(fd);
      return -1;
    }

    if (rc == 0) {
      log_error("connect %s:%u(timedout)", inet_ntoa(*(struct in_addr *)(&epip)), ntohs(epport));
      close(fd);
      return -1;
    }

    if (pfds.revents & POLLERR) {
      log_error("connect %s:%u(errors)", inet_ntoa(*(struct in_addr *)(&epip)), ntohs(epport));
      close(fd);
      return -1;
    }
  }

  if (ssl_ctx) {
    void *nssl = SSL_new(ssl_ctx);
    assert(nssl);
    if (proxy_ssl_connect(fd, nssl)) {
      log_error("ssl-connect %s:%u(failed)", inet_ntoa(*(struct in_addr *)(&epip)), ntohs(epport));
      close(fd);
      SSL_free(nssl);
      return -1;
    }

    *ssl = nssl;
  }

  return fd;
}

static int
proxy_setup_ep__(uint32_t xip, uint16_t xport, uint8_t protocol,
                 const char *host_str, proxy_ep_sel_t *ep_sel,
                 proxy_epval_t **epv, int *seltype, uint32_t *rid,
                 void *ssl_ctx, void **ssl)
{
  int sel = 0;
  uint32_t epip;
  uint16_t epport;
  uint8_t epprotocol;
  proxy_ent_t ent = { 0 };
  struct proxy_epval *tepval;
  proxy_map_ent_t *node = proxy_struct->head;

  ent.xip = xip;
  ent.xport = xport;
  ent.protocol = protocol;

  while (node) {
    if (cmp_proxy_ent(&node->key, &ent)) {
      if (node->val.proxy_mode == PROXY_MODE_DFL) {
        if (host_str == NULL) {
          tepval = node->val.ephash;
        } else {
          HASH_FIND_STR(node->val.ephash, host_str, tepval);
        }

        if (tepval == NULL) break;
        sel = tepval->ep_sel % tepval->n_eps;
        if (sel >= MAX_PROXY_EP) break;

        epip = tepval->eps[sel].xip;
        epport = tepval->eps[sel].xport;
        epprotocol = tepval->eps[sel].protocol;
        tepval->ep_sel++;
        ep_sel->ep_cfds[0].ep_cfd = proxy_setup_ep_connect(epip, epport, (uint8_t)epprotocol,
                                                           ssl_ctx, ssl);
        if (ep_sel->ep_cfds[0].ep_cfd < 0) {
          return -1;
        }

        *seltype = 0;
        *rid = tepval->_id;
        *epv = tepval;
        ep_sel->ep_cfds[0].ep_num = sel;
        ep_sel->n_eps = 1;

        return 0;
      } else if (node->val.proxy_mode == PROXY_MODE_ALL) {
        int ep = 0;

        tepval = node->val.ephash;
        if (tepval == NULL) break;

        /* Do not support for this mode */
        assert(ssl_ctx == NULL);

        for (ep = 0; ep < tepval->n_eps; ep++) {
          epip = tepval->eps[ep].xip;
          epport = tepval->eps[ep].xport;
          epprotocol = tepval->eps[ep].protocol;
          ep_sel->ep_cfds[sel].ep_cfd = proxy_setup_ep_connect(epip, epport, (uint8_t)epprotocol, 
                                                               NULL, NULL);
          if (ep_sel->ep_cfds[sel].ep_cfd > 0) {
            ep_sel->ep_cfds[sel].ep_num = sel;
            sel++;
          }
        }

        *rid = tepval->_id;
        *epv = tepval;
        if (sel) {
          ep_sel->n_eps = sel;
          *seltype = tepval->select;
          return 0;
        }
        return -1;
      }
    }
    node = node->next;
  }

  return -1;
}

static int
proxy_sock_init(uint32_t IP, uint16_t port, uint8_t protocol)
{
  int listen_sd;

  switch (protocol) {
  case IPPROTO_TCP:
  case IPPROTO_SCTP:
    listen_sd = socket(AF_INET, SOCK_STREAM, protocol);
    break;
  default:
    return -1;
  }

  if (listen_sd > 0) {
    if (!proxy_server_setup(listen_sd, IP, port, protocol)) {
      return listen_sd;
    }
    close(listen_sd); 
  }

  return -1;
}

static void *
proxy_run(void *arg)
{
  notify_start(proxy_struct->ns);
  return NULL;
}

int
proxy_find_ep(uint32_t xip, uint16_t xport, uint8_t protocol, 
              uint32_t *epip, uint16_t *epport, uint8_t *epprotocol)
{
#if 0
  int sel = 0;
  proxy_ent_t ent = { 0 };
  proxy_map_ent_t *node = proxy_struct->head;
   
  ent.xip = xip;
  ent.xport = xport;
  ent.protocol = protocol;

  PROXY_LOCK();

  while (node) {

    if (cmp_proxy_ent(&node->key, &ent)) {
      if (!node->val.n_eps) {
        PROXY_UNLOCK();
        return -1;
      }
      sel = node->val.ep_sel % node->val.n_eps;
      if (sel >= MAX_PROXY_EP) break;
      *epip = node->val.eps[sel].xip; 
      *epport = node->val.eps[sel].xport; 
      *epprotocol = node->val.eps[sel].protocol;
      node->val.ep_sel++;
      PROXY_UNLOCK();
      return 0;
    }
    node = node->next;
  }

  PROXY_UNLOCK();
#endif
  return -1;
}

static void
proxy_free_fd_ctx(proxy_fd_ent_t *pfe)
{
  if (pfe->used <= 0) {
    free(pfe);
  }
}

static void
proxy_try_free_fd_ctx(proxy_fd_ent_t *pfe)
{
  pfe->used--;
  proxy_free_fd_ctx(pfe);
}

static int
proxy_delete_entry__(proxy_ent_t *ent, proxy_arg_t *arg, int *mfd,
                     void **ssl_ctx, void **ssl_epctx)
{
  struct proxy_map_ent *prev = NULL;
  struct proxy_map_ent *node;
  proxy_epval_t *tepval;
  int epcount = 0;

  node = proxy_struct->head;

  while (node) {

    if (cmp_proxy_ent(&node->key, ent)) {
      break;
    }
    prev = node;
    node = node->next;
  }

  if (node) {

    HASH_FIND_STR(node->val.ephash, arg->host_url, tepval);
    if (tepval == NULL) {
      return -EINVAL;
    }

    HASH_DEL(node->val.ephash, tepval);

    epcount = HASH_COUNT(node->val.ephash);
    if (epcount > 0) {
      log_info("sockproxy: %s:%u (%s) deleted", inet_ntoa(*(struct in_addr *)&ent->xip),
                ntohs(ent->xport), arg->host_url);
      return 0;
    }

    if (prev) {
      prev->next = node->next;
    } else {
      proxy_struct->head = node->next;
    }

    if (node->val.main_fd > 0) {
      *mfd = node->val.main_fd;
    }
    if (node->val.ssl_ctx) {
      *ssl_ctx = node->val.ssl_ctx;
    }
    if (node->val.ssl_epctx) {
      *ssl_epctx = node->val.ssl_epctx;
    }

    /* This node is freed after cleanup in proxy_pdestroy() */
    //free(node);
  } else {
    log_info("sockproxy : %s:%u delete failed", inet_ntoa(*(struct in_addr *)&ent->xip), ntohs(ent->xport));
    return -EINVAL;
  }

  log_info("sockproxy: %s:%u (%s) deleted", inet_ntoa(*(struct in_addr *)&ent->xip),
                ntohs(ent->xport), arg->host_url);

  return 0;
}

SSL_CTX *
proxy_server_ssl_ctx_init(void)
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    method = TLS_server_method();

    ctx = SSL_CTX_new(method);
    if (!ctx) {
      log_error("sockproxy: ssl-ctx creation failed");
      return NULL;
    }

    return ctx;
}

int
proxy_ssl_cfg_opts(SSL_CTX *ctx, const char *site_path, int mtls_en)
{
  char fpath[512];

  if (mtls_en) {
    sprintf(fpath, "%s", PROXY_SSL_CA_DIR);
    if (SSL_CTX_load_verify_locations(ctx, NULL, fpath) <= 0) {
      log_error("Unable to set verify locations %s",
        ERR_error_string(ERR_get_error(), NULL));
      return -EINVAL;
    }
  }

  sprintf(fpath, "%s/%s/%s", PROXY_SSL_CERT_DIR, site_path?:"", "server.crt");
  if (site_path && !access(fpath, F_OK)) {
    if (SSL_CTX_use_certificate_file(ctx, fpath, SSL_FILETYPE_PEM) <= 0) {
      log_error("sockproxy: cert (%s) load failed", fpath);
      return -EINVAL;
    }

    sprintf(fpath, "%s/%s/%s", PROXY_SSL_CERT_DIR, site_path, "server.key");
    if (SSL_CTX_use_PrivateKey_file(ctx, fpath, SSL_FILETYPE_PEM) <= 0 ) {
      log_error("sockproxy: privkey (%s) load failed", fpath);
      return -EINVAL;
    }
  } else {
    sprintf(fpath, "%s/%s", PROXY_SSL_CERT_DIR, "server.crt");
    if (SSL_CTX_use_certificate_file(ctx, fpath, SSL_FILETYPE_PEM) <= 0) {
      log_error("sockproxy: cert (%s) load failed", fpath);
      return -EINVAL;
    }

    sprintf(fpath, "%s/%s", PROXY_SSL_CERT_DIR, "server.key");
    if (SSL_CTX_use_PrivateKey_file(ctx, fpath, SSL_FILETYPE_PEM) <= 0 ) {
      log_error("sockproxy: privkey (%s) load failed", fpath);
      return -EINVAL;
    }
  }

  if (!SSL_CTX_check_private_key(ctx)) {
    log_error("sockproxy: privkey mismatch with public certificate");
    return -EINVAL;
  }

  if (mtls_en) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT|
      SSL_VERIFY_CLIENT_ONCE, 0);
  }

#if 0
  if (!SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF)) {
    log_error("sockproxy: SSL_OP_IGNORE_UNEXPECTED_EOF failed");
    return -EINVAL;
  }
#endif

  if (!SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER)) {
    log_error("sockproxy: SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER failed");
    return -EINVAL;
  }

  return 0;
}

SSL_CTX *
proxy_client_ssl_ctx_init(void)
{
  const SSL_METHOD *method;
  SSL_CTX *ctx;

  method = TLS_client_method();
  ctx = SSL_CTX_new(method);
  if (!ctx) {
    log_error("sockproxy: ssl-ctx creation failed");
    return NULL;
  }

  return ctx;
}

int
proxy_add_entry(proxy_ent_t *new_ent, proxy_arg_t *arg)
{
  int lsd;
  void *ssl_ctx = NULL;
  void *ssl_epctx = NULL;
  proxy_map_ent_t *node;
  proxy_epval_t *tepval;
  proxy_map_ent_t *ent = proxy_struct->head;
  proxy_fd_ent_t *fd_ctx;

  PROXY_LOCK();

  while (ent) {
    if (cmp_proxy_ent(&ent->key, new_ent)) {
      proxy_epval_t *eval = NULL;
      HASH_FIND_STR(ent->val.ephash, arg->host_url, tepval);
      if (eval != NULL) {
        PROXY_UNLOCK();
        log_info("sockproxy : %s:%u exists",
          inet_ntoa(*(struct in_addr *)&ent->key.xip), ntohs(ent->key.xport));
        return -EEXIST;
      } else {
        tepval = calloc(1, sizeof(*tepval));
        assert(tepval);
        tepval->n_eps = arg->n_eps;
        tepval->_id = arg->_id;
        tepval->select = arg->select;
        strncpy(tepval->host_url, arg->host_url, sizeof(tepval->host_url) - 1);
        memcpy(tepval->eps, arg->eps, sizeof(arg->eps));
        HASH_ADD_STR(ent->val.ephash, host_url, tepval);
        log_info("sockproxy : %s:%u (%s) added %s",
          inet_ntoa(*(struct in_addr *)&ent->key.xip), ntohs(ent->key.xport),
          arg->host_url ? arg->host_url: "",
          ent->val.ssl_ctx ? "ssl-en":"ssl-dis");
        PROXY_UNLOCK();
        return 0;
      }
    }
    ent = ent->next;
  }

  node = calloc(1, sizeof(*node));
  if (node == NULL) {
    PROXY_UNLOCK();
    return -ENOMEM;
  }

  memcpy(&node->key, new_ent, sizeof(proxy_ent_t));
  node->val.main_fd = -1;
  node->val.have_ssl = arg->have_ssl;

  if (arg->have_ssl) {
    ssl_ctx = proxy_server_ssl_ctx_init();
    assert(ssl_ctx);
    if (proxy_ssl_cfg_opts(ssl_ctx,
          strcmp(arg->host_url, "") ? arg->host_url : NULL, 0)) {
      PROXY_UNLOCK();
      return -EINVAL;
    }
  }

  if (arg->have_epssl) {
    ssl_epctx = proxy_client_ssl_ctx_init();
    assert(ssl_epctx);
    if (proxy_ssl_cfg_opts(ssl_epctx, NULL, 0)) {
      if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
        ssl_ctx = NULL;
      }
      PROXY_UNLOCK();
      return -EINVAL;
    }
  }

  lsd = proxy_sock_init(node->key.xip, node->key.xport, node->key.protocol);
  if (lsd <= 0) {
    log_error("sockproxy : %s:%u sock-init failed",
        inet_ntoa(*(struct in_addr *)&node->key.xip), ntohs(node->key.xport));
    if (ssl_epctx) {
      SSL_CTX_free(ssl_epctx);
      ssl_epctx = NULL;
    }
    if (ssl_ctx) {
      SSL_CTX_free(ssl_ctx);
      ssl_ctx = NULL;
    }
    PROXY_UNLOCK();
    return -1; 
  }

  node->val.main_fd = lsd;
  node->val.ssl_ctx = ssl_ctx;
  node->val.ssl_epctx = ssl_epctx;
  node->val.proxy_mode = arg->proxy_mode;
  fd_ctx = calloc(1, sizeof(*fd_ctx));
  assert(fd_ctx);

  node->val.fdlist = fd_ctx;
  node->val.nfds++;
  fd_ctx->head = node;
  fd_ctx->stype = PROXY_SOCK_LISTEN;
  fd_ctx->fd = lsd;
  fd_ctx->seltype = arg->select;
  if (notify_add_ent(proxy_struct->ns, lsd, NOTI_TYPE_IN|NOTI_TYPE_HUP, fd_ctx)) {
    log_error("sockproxy : %s:%u notify failed",
        inet_ntoa(*(struct in_addr *)&node->key.xip), ntohs(node->key.xport));
    PROXY_UNLOCK();
    close(lsd);
    if (node->val.ssl_ctx) {
      SSL_CTX_free(node->val.ssl_ctx);
      node->val.ssl_ctx = NULL;
    }
    return -1; 
  }
  fd_ctx->used++;

  tepval = calloc(1, sizeof(*tepval));
  assert(tepval);
  tepval->n_eps = arg->n_eps;
  tepval->_id = arg->_id;
  tepval->select = arg->select;
  strncpy(tepval->host_url, arg->host_url, sizeof(tepval->host_url) - 1);
  tepval->host_url[sizeof(tepval->host_url) - 1] = '\0';
  memcpy(tepval->eps, arg->eps, sizeof(arg->eps));
  HASH_ADD_STR(node->val.ephash, host_url, tepval);

  node->next = proxy_struct->head;
  proxy_struct->head = node;

  HASH_FIND_STR(node->val.ephash, arg->host_url, tepval);
  if (tepval == NULL) {
    assert(0);
  }

  PROXY_UNLOCK();

  log_info("sockproxy : %s:%u (%s) added %s",
    inet_ntoa(*(struct in_addr *)&node->key.xip), ntohs(node->key.xport),
    arg->host_url ? arg->host_url: "",
    node->val.ssl_ctx ? "ssl-en":"ssl-dis");
  
  return 0;
}

int
proxy_delete_entry(proxy_ent_t *ent, proxy_arg_t *arg)
{
  int ret = 0, fd = 0;
  void *ssl_ctx = NULL;
  void *ssl_epctx = NULL;

  PROXY_LOCK();
  ret = proxy_delete_entry__(ent, arg, &fd, &ssl_ctx, &ssl_epctx);
  PROXY_UNLOCK();

  if (fd > 0) {
    notify_delete_ent(proxy_struct->ns, fd, 0);
    close(fd);
  }

  return ret;
}

static int
proxy_ct_from_fd(int fd, struct dp_ct_key *ctk, int odir)
{
  struct sockaddr_in sin_addr;
  struct sockaddr_in sin_addr2;
  socklen_t sin_len;
  socklen_t optsize = sizeof(int);
  int protocol;

  if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &optsize)) {
    return -1;
  }

  ctk->l4proto = (uint8_t)protocol;

  sin_len = sizeof(struct sockaddr);
  if (getsockname(fd, (struct sockaddr*)&sin_addr, &sin_len)) {
    return -1;
  }

  if (getpeername(fd, (struct sockaddr*)&sin_addr2, &sin_len)) {
    return -1;
  }

  if (odir) {
    ctk->saddr[0] = sin_addr.sin_addr.s_addr;
    ctk->sport = sin_addr.sin_port;
    ctk->daddr[0]= sin_addr2.sin_addr.s_addr;
    ctk->dport = sin_addr2.sin_port;
  } else {
    ctk->saddr[0] = sin_addr2.sin_addr.s_addr;
    ctk->sport = sin_addr2.sin_port;
    ctk->daddr[0]= sin_addr.sin_addr.s_addr;
    ctk->dport = sin_addr.sin_port;
  }

  return 0;
}

static void
proxy_ct_dump(const char *str, struct dp_ct_key *ctk)
{
  char ab1[INET6_ADDRSTRLEN];
  char ab2[INET6_ADDRSTRLEN];

  inet_ntop(AF_INET, (struct in_addr *)&ctk->daddr[0], ab1, INET_ADDRSTRLEN);
  inet_ntop(AF_INET, (struct in_addr *)&ctk->saddr[0], ab2, INET_ADDRSTRLEN);
  log_debug("%s %s:%u -> %s:%u:%d", str,
            ab1, ntohs(ctk->dport), ab2, ntohs(ctk->sport), ctk->l4proto);
}

void
proxy_dump_entry(proxy_info_cb_t cb)
{
  proxy_map_ent_t *node;
  proxy_fd_ent_t *fd_ent;
  struct dp_proxy_ct_ent pct;
  int i = 0;
  int j = 0;

  PROXY_LOCK();
  node = proxy_struct->head;
  while (node) {
    fd_ent = node->val.fdlist;
    while (fd_ent) {
      if (fd_ent->odir == 0) {
        memset(&pct, 0, sizeof(pct));
        pct.rid = fd_ent->_id;
        if (!proxy_ct_from_fd(fd_ent->fd, &pct.ct_in, 0)) {
          pct.st_in.bytes = fd_ent->ntb;
          pct.st_in.bytes += fd_ent->nrb;
          pct.st_in.packets = fd_ent->ntp;
          pct.st_in.packets += fd_ent->nrp;

          if (!cb) proxy_ct_dump("dir", &pct.ct_in);
          for (j = 0; j < fd_ent->n_rfd; j++) {
            if (!proxy_ct_from_fd(fd_ent->rfd[j], &pct.ct_out, 1)) {
              if (!cb) proxy_ct_dump("rdir", &pct.ct_out);
              pct.st_out.bytes = fd_ent->rfd_ent[j]->ntb;
              pct.st_out.bytes += fd_ent->rfd_ent[j]->nrb;
              pct.st_out.packets = fd_ent->rfd_ent[j]->ntp;
              pct.st_out.packets += fd_ent->rfd_ent[j]->nrp;
              if (cb) {
                cb(&pct);
              }
            }
          }
        }
      }
      //printf("proxy_dump_entry\n");
      fd_ent = fd_ent->next;
    }
    node = node->next;
    i++;
  }
  PROXY_UNLOCK();
}

void
proxy_get_entry_stats(uint32_t id, int epid, uint64_t *p, uint64_t *b)
{
  proxy_map_ent_t *node;
  proxy_epval_t *epv;
  int i = 0;

  *p = 0;
  *b = 0;

  PROXY_LOCK();
  node = proxy_struct->head;
  while (node) {
    for (epv = node->val.ephash; epv; epv = epv->hh.next) {
      if (epid >=0 && epid < MAX_PROXY_EP) {
        *b = epv->ep_stats[epid].ntb;
        *p = epv->ep_stats[epid].ntp;
      }
    }
    node = node->next;
    i++;
  }
  PROXY_UNLOCK();
}

int
proxy_selftests()
{
  proxy_ent_t key = { 0 };
  proxy_arg_t arg = { 0 };
  proxy_ent_t key2 = { 0 };
  int n = 0;

  key.xip = inet_addr("172.17.0.2");
  key.xport = htons(22222);

  arg.eps[0].xip = inet_addr("127.0.0.1");
  arg.eps[0].xport = htons(33333);
  arg.n_eps = 1;
  proxy_add_entry(&key, &arg);

  key2.xip = inet_addr("127.0.0.2");
  key2.xport = htons(22222);
  proxy_add_entry(&key2, &arg);
  proxy_dump_entry(NULL);

  proxy_delete_entry(&key2, &arg);
  proxy_dump_entry(NULL);

  while(0) {
    sleep(1);
    n++;
    if (n > 10) {
      proxy_delete_entry(&key, &arg);
    }
  }

  return 0;
}

static void
proxy_reset_fd_list(proxy_map_ent_t *ent, void *match_pfe)
{
  proxy_fd_ent_t *fd_ent;
  proxy_fd_ent_t *pfd_ent = NULL;

  if (!ent) return;

  fd_ent = ent->val.fdlist;

  if (match_pfe == NULL) {
    while (fd_ent) {
      fd_ent->head = NULL;
      fd_ent = fd_ent->next;
      ent->val.nfds--;
    }
    ent->val.fdlist = NULL;
  } else {
    while (fd_ent) {
      if (fd_ent == match_pfe) {
        if (pfd_ent) {
          pfd_ent->next = fd_ent->next;
        } else {
          ent->val.fdlist = fd_ent->next;
        }
        ent->val.nfds--;
        break;
      }
      pfd_ent = fd_ent;
      fd_ent = fd_ent->next;
    }
  }
}

static void
proxy_release_fd_ctx(proxy_fd_ent_t *fd_ent, int reset)
{
  proxy_destroy_xmitcache(fd_ent);

  if (fd_ent->ssl) {
    if (!fd_ent->ssl_err)
      SSL_shutdown(fd_ent->ssl);
  }

  if (fd_ent->fd > 0) {
    shutdown(fd_ent->fd, SHUT_RDWR);

    if (reset) {
      log_trace("sockproxy fd %d reset", fd_ent->fd);
      proxy_reset_fd_list(fd_ent->head, fd_ent);
      close(fd_ent->fd);
      fd_ent->fd = -1;
      if (fd_ent->ssl) {
        SSL_free(fd_ent->ssl);
        fd_ent->ssl = NULL;
      }
    }
  } else {
    assert(0);
  }
}

static void
proxy_release_rfd_ctx(proxy_fd_ent_t *pfe)
{
  proxy_fd_ent_t *fd_ent;
  int n = 0;

  for (int i = 0; n < pfe->n_rfd && i < MAX_PROXY_EP; i++) {
    fd_ent = pfe->rfd_ent[i];
    if (fd_ent) {
      PROXY_ENT_LOCK(fd_ent);
      log_trace("sockproxy rfd %d release", fd_ent->fd);
      proxy_release_fd_ctx(fd_ent, 0);
      notify_delete_ent(proxy_struct->ns, fd_ent->fd, 1);
      pfe->rfd_ent[i] = NULL;
      if (!pfe->odir) {
        for (int j = 0; j < fd_ent->n_rfd; j++) {
          fd_ent->rfd_ent[j] = NULL;
        }
        fd_ent->n_rfd = 0;
      } else {
        for (int j = 0; j < fd_ent->n_rfd; j++) {
          if (fd_ent->rfd_ent[j] == pfe) {
            fd_ent->rfd_ent[j] = NULL;
            fd_ent->n_rfd--;
          }
        }
      }
      PROXY_ENT_UNLOCK(fd_ent);
      n++;
    }
    pfe->rfd[i] = -1;
  }
  pfe->n_rfd = 0;
}

static void
proxy_pdestroy(void *priv)
{
  proxy_map_ent_t *ent;
  proxy_fd_ent_t *pfe = priv;
  proxy_fd_ent_t *fd_ent;
  int is_listener = 0;

  assert(pfe);

  PROXY_LOCK();
  if (pfe) {
    PROXY_ENT_LOCK(pfe);
    ent = pfe->head;
    if (!ent) {
      assert(0);
      PROXY_ENT_UNLOCK(pfe);
      proxy_try_free_fd_ctx(pfe);
      PROXY_UNLOCK();
      return;
    }

    if (pfe->fd == ent->val.main_fd) {
      is_listener = 1;
      fd_ent = ent->val.fdlist;
      while (fd_ent) {
        if (fd_ent->odir == 0) {
          proxy_release_rfd_ctx(fd_ent);
          if (fd_ent->fd != ent->val.main_fd) {
            proxy_release_fd_ctx(fd_ent, 0);
          }
        }
        fd_ent = fd_ent->next;
      }
    }

    if (!is_listener) {
      proxy_release_rfd_ctx(pfe);
    }
    proxy_release_fd_ctx(pfe, 1);
    PROXY_ENT_UNLOCK(pfe);
    proxy_try_free_fd_ctx(pfe);

    if (is_listener) {
      ent->val.sched_free = 1;
    }

    if (ent->val.sched_free && ent->val.fdlist == NULL) {
      log_info("sockproxy: %s:%u ent freed",
              inet_ntoa(*(struct in_addr *)&ent->key.xip),
              ntohs(ent->key.xport));
      if (ent->val.ssl_ctx)
        SSL_CTX_free(ent->val.ssl_ctx);
      if (ent->val.ssl_epctx)
        SSL_CTX_free(ent->val.ssl_epctx);
      free(ent);
    }
  }
  PROXY_UNLOCK();
}

static void
proxy_destroy_eps(int sfd, proxy_ep_sel_t *ep_sel)
{
  int i = 0;
  for (i = 0; i < ep_sel->n_eps; i++) {
    if (ep_sel->ep_cfds[i].ep_cfd > 0) {
      ep_sel->ep_cfds[i].ep_cfd = -1;
      ep_sel->ep_cfds[i].ep_num = -1;
    }
  }
}

#define PROXY_SEL_EP_DROP  -1
#define PROXY_SEL_EP_BC    1
#define PROXY_SEL_EP_UC    0

static int
proxy_select_ep(proxy_fd_ent_t *pfe, void *inbuf, size_t insz, int *ep)
{
  *ep = 0;

  switch (pfe->seltype) {
  case PROXY_SEL_N2:
  default:
    if (pfe->n_rfd > 1) {
      *ep = pfe->lsel % pfe->n_rfd;
      pfe->lsel++;
    }
    break;
  }

  return PROXY_SEL_EP_UC;
}

static int
proxy_multiplexor(proxy_fd_ent_t *pfe, void *inbuf, size_t insz)
{
  int epret;
  int ep = 0;

  epret = proxy_select_ep(pfe, inbuf, insz,  &ep);
  if (epret == PROXY_SEL_EP_DROP) {
      return -1;
  } else if (epret == PROXY_SEL_EP_BC) {
    for (int i = 0; i < pfe->n_rfd; i++) {
      proxy_try_epxmit(pfe, inbuf, insz, i);
    }
  } else {
    proxy_try_epxmit(pfe, inbuf, insz, ep);
  }
  return 0;
}

static int
proxy_sock_read(proxy_fd_ent_t *pfe, int fd, void *buf, size_t len)
{
  if (!pfe->ssl) {
    return recv(fd, buf, len, MSG_DONTWAIT);
  } else {
    return SSL_read(pfe->ssl, buf, len);
  }
}

static int
proxy_sock_read_err(proxy_fd_ent_t *pfe, int rval)
{
  if (!pfe->ssl) {
    if (rval <= 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        //log_error("pollin : failed %d", rval);
        shutdown(pfe->fd, SHUT_RDWR);
        return -1;
      }
      return 1;
    }
    return 0;
  } else {
    if (rval > 0) return 0;
    switch (SSL_get_error(pfe->ssl, rval)) {
      case SSL_ERROR_NONE:
        return 0;
      case SSL_ERROR_SSL:
      case SSL_ERROR_SYSCALL:
        log_trace("ssl-syscall-failed %s",
          ERR_error_string(ERR_get_error(), NULL));
        pfe->ssl_err = 1;
        shutdown(pfe->fd, SHUT_RDWR);
        return -1;
      case SSL_ERROR_WANT_READ:
        log_trace("ssl-want-rd %s",
          ERR_error_string(ERR_get_error(), NULL));
        return 1;
      case SSL_ERROR_WANT_WRITE:
        log_trace("ssl-want-wr %s",
          ERR_error_string(ERR_get_error(), NULL));
        notify_add_ent(proxy_struct->ns, pfe->fd,
              NOTI_TYPE_IN|NOTI_TYPE_HUP|NOTI_TYPE_OUT, pfe);
        return 1;
      case SSL_ERROR_ZERO_RETURN:
      default:
        //log_trace("ssl-err %s",
        //  ERR_error_string(ERR_get_error(), NULL));
        SSL_shutdown(pfe->ssl);
        shutdown(pfe->fd, SHUT_RDWR);
        return -1;
    }
  }

  /* Not reached */
  return -1;
}

static int
proxy_ssl_accept(void *ssl, int fd)
{
  struct pollfd pfds = { 0 };
  int n_try = 0;
  int sel_rc;
  int ssl_rc;

  pfds.fd = fd;

  for (n_try = 0; n_try < 10; n_try++) {
    if ((ssl_rc = SSL_accept(ssl)) > 0) {
      return 0;
    }

    if (ssl_rc == 0) {
      return -1;
    }

    sel_rc = 0;
    switch (SSL_get_error(ssl, ssl_rc)) {
      case SSL_ERROR_WANT_READ:
        log_trace("ssl-accept want-read %s",
          ERR_error_string(ERR_get_error(), NULL));
        pfds.events = POLLIN;
        sel_rc = poll(&pfds, 1, 100);
        break;
      case SSL_ERROR_WANT_WRITE:
        log_trace("ssl-accept want-write %s",
          ERR_error_string(ERR_get_error(), NULL));
        pfds.events = POLLOUT;
        sel_rc = poll(&pfds, 1, 100);
        break;
      default:
        log_error("ssl-accept failed %s",
          ERR_error_string(ERR_get_error(), NULL));
        SSL_shutdown(ssl);
        return -1;
    }
    if (sel_rc < 0) {
      return -1;
    }
  }

  return -1;
}

static int
setup_proxy_path(smap_key_t *key, smap_key_t *rkey, proxy_fd_ent_t *pfe, const char *flt_url)
{
  proxy_ep_sel_t ep_sel = { 0 };
  int j, n_eps = 0, seltype = 0;
  int epprotocol, protocol;
  uint32_t rid = 0;
  proxy_fd_ent_t *npfe1 = pfe;
  proxy_fd_ent_t *npfe2 = NULL;
  proxy_epval_t *tepval = NULL;
  proxy_map_ent_t *ent;
  void *ssl = NULL;
  int retry = 0;

  ent = pfe->head;
  assert(ent);

  if (proxy_skmap_key_from_fd(pfe->fd, key, &protocol)) {
    log_error("skmap key from fd failed");
    return -1;
  }

  memset(&ep_sel, 0, sizeof(ep_sel));
  if (proxy_setup_ep__(key->sip, key->sport >> 16, (uint8_t)(protocol),
                       flt_url, &ep_sel, &tepval, &seltype, &rid, ent->val.ssl_epctx, &ssl)) {
    proxy_log_always("no endpoint", key);
    proxy_destroy_eps(pfe->fd, &ep_sel);
    shutdown(pfe->fd, SHUT_RDWR);
    return -1;
  }

  n_eps = ep_sel.n_eps;

  for (j = 0; j < n_eps; j++) {
    int ep_cfd = ep_sel.ep_cfds[j].ep_cfd;
    int ep_num = ep_sel.ep_cfds[j].ep_num;
    if (ep_cfd < 0) {
      assert(0);
    }

    if (proxy_skmap_key_from_fd(ep_cfd, rkey, &epprotocol)) {
      log_error("skmap key from ep_cfd failed");
      proxy_destroy_eps(pfe->fd, &ep_sel);
      if (ssl) {
        SSL_shutdown(ssl);
      }
      shutdown(pfe->fd, SHUT_RDWR);
      return -1;
    }

    proxy_log("connected", rkey);
    log_trace("rfd = %d", ep_cfd);

    if (protocol == IPPROTO_TCP && epprotocol == IPPROTO_TCP && n_eps == 1) {
      if (proxy_struct->sockmap_cb) {
        proxy_struct->sockmap_cb(rkey, pfe->fd, 1);
        proxy_struct->sockmap_cb(key, ep_cfd, 1);
      }
#ifdef HAVE_SOCKMAP_KTLS
      if (proxy_sock_init_ktls(new_sd)) {
        log_error("tls failed");
        proxy_destroy_eps(pfe->fd, &ep_sel);
        if (ssl) {
          SSL_shutdown(ssl);
        }
        shutdown(pfe->fd, SHUT_RDWR);
        return -1;
      }
#endif
    }

    npfe2 = calloc(1, sizeof(*npfe2));
    assert(npfe2);
    npfe2->stype = PROXY_SOCK_ACTIVE;
    npfe2->fd = ep_cfd;
    npfe2->rfd[0] = npfe1->fd;
    npfe2->rfd_ent[0] = npfe1;
    npfe2->seltype = seltype;
    npfe2->ep_num = ep_num;
    npfe2->odir = 1;
    npfe2->_id = rid;
    npfe2->epv = tepval;
    npfe2->n_rfd++;
    npfe2->head = ent;
    npfe2->ssl = ssl;

    PROXY_LOCK();
    npfe2->next = ent->val.fdlist;
    ent->val.fdlist = npfe2;
    ent->val.nfds++;
    PROXY_UNLOCK();

    npfe1->_id = rid;
    npfe1->rfd[npfe1->n_rfd] = ep_cfd;
    npfe1->rfd_ent[npfe1->n_rfd] = npfe2;
    npfe1->n_rfd++;

    for (retry = 0; retry < PROXY_MAPFD_RETRIES; retry++) {
      if (notify_add_ent(proxy_struct->ns, ep_cfd,
          NOTI_TYPE_IN|NOTI_TYPE_HUP, npfe2) == 0)  {
        break;
      }
      //log_debug("failed to add epcfd %d retry", ep_cfd);
      ep_cfd = get_mapped_proxy_fd(ep_cfd, 0);
      npfe2->fd = ep_cfd;
      if (npfe2->ssl) {
        SSL_set_fd(npfe2->ssl, ep_cfd);
      }
    }
    npfe2->used++;

    if (retry >= PROXY_MAPFD_RETRIES) {
      proxy_destroy_eps(pfe->fd, &ep_sel);
      proxy_release_fd_ctx(npfe2, 0);
      if (npfe2->ssl) {
        SSL_shutdown(npfe2->ssl);
        SSL_free(npfe2->ssl);
      }
      free(npfe2);
      shutdown(pfe->fd, SHUT_RDWR);
      log_error("failed to add epcfd %d", ep_cfd);
      return -1;
    }
  }
  return 0;
}

int
handle_on_message_complete(llhttp_t* parser)
{
  llhttp_settings_t *settings = parser->settings;
  proxy_fd_ent_t *pfe;

  pfe = settings->uarg;
  assert(pfe);

  pfe->http_pok = 1;

#ifdef HAVE_PROXY_EXTRA_DEBUG
	log_debug("http completed %p!\n", settings->uarg);
#endif
	return 0;
}

int
handle_header_name(llhttp_t *parser, const char *at, size_t length)
{
  char str[256];
  llhttp_settings_t *settings = parser->settings;
  proxy_fd_ent_t *pfe;

  pfe = settings->uarg;
  assert(pfe);

  if (length >= sizeof(str)-1) {
    return 0;
  }
  strncpy(str, at, length);
  str[length] = '\0';

  if (strncasecmp("Host", str, length)) {
    return 0;
  }

  pfe->http_hok = 1;

#ifdef HAVE_PROXY_EXTRA_DEBUG
	fprintf(stdout, "header name rcvd %s!\n", str);
#endif

	return 0;
}

int
handle_header_val(llhttp_t *parser, const char *at, size_t length)
{
  llhttp_settings_t *settings = parser->settings;
  proxy_fd_ent_t *pfe;

  pfe = settings->uarg;
  assert(pfe);

  if (!pfe->http_hok) {
    return 0;
  }

  if (pfe->http_hvok) {
    //pfe->http_pok = 1;
    return 0;
  }

  if (length >= sizeof(pfe->host_url)-1) {
    return 0;
  }

  pfe->http_hvok = 1;
  strncpy(pfe->host_url, at, length);
  pfe->host_url[length] = '\0';

#ifdef HAVE_PROXY_EXTRA_DEBUG
	log_debug("Header val rcvd %s!\n", pfe->host_url);
#endif

	return 0;
}

#ifdef HAVE_PROXY_EXTRA_DEBUG
int
handle_url(llhttp_t *parser, const char *at, size_t length)
{
  char str[256];

  strncpy(str, at, length);
  str[length] = '\0';
	log_debug("url val rcvd %s!\n", str);
	return 0;
}
#endif

static int
proxy_notifier(int fd, notify_type_t type, void *priv)
{
  struct llb_sockmap_key key = { 0 };
  struct llb_sockmap_key rkey = { 0 };
  proxy_ep_sel_t ep_sel = { 0 };
  int j;
  int protocol, retry;
  proxy_fd_ent_t *pfe = priv;
  proxy_fd_ent_t *npfe1 = NULL;
  proxy_map_ent_t *ent;
  SSL *ssl = NULL;

  if (!priv) {
    return 0;
  }

  if (pfe->fd <= 0) {
    return 0;
  }

  ent = pfe->head;
  if (ent->val.sched_free) {
    return 0;
  }

  //log_trace("notify fd = %d(%d) type 0x%x", fd, pfe->fd, type);

restart:
  while (type) {
    if (type & NOTI_TYPE_IN) {
      type &= ~NOTI_TYPE_IN;

      if (pfe->stype == PROXY_SOCK_LISTEN) {
        int new_sd = accept(fd, NULL, NULL);
        if (new_sd < 0) {
          if (errno != EWOULDBLOCK) {
            log_error("accept failed\n");
          }
          continue;
        }

        new_sd = get_mapped_proxy_fd(new_sd, 1);

        if (proxy_skmap_key_from_fd(new_sd, &key, &protocol)) {
          log_error("skmap key from fd failed");
          if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
          }
          close(new_sd);
          continue;
        }

        proxy_sock_set_opts(new_sd, protocol);

        if (ent->val.ssl_ctx) {
          ssl = SSL_new(ent->val.ssl_ctx);
          assert(ssl);
          SSL_set_fd(ssl, new_sd);
          if (proxy_ssl_accept(ssl, new_sd) < 0) {
            SSL_free(ssl);
            close(new_sd);
            continue;
          }
        }

        proxy_log("new accept()", &key);
        log_trace("newfd = %d", new_sd);

        npfe1 = calloc(1, sizeof(*npfe1));
        assert(npfe1);
        npfe1->stype = PROXY_SOCK_ACTIVE;
        npfe1->fd = new_sd;
        npfe1->seltype = pfe->seltype;
        npfe1->ep_num = -1;
        npfe1->head = ent;
        npfe1->ssl = ssl;

        llhttp_settings_init(&npfe1->settings);
	      npfe1->settings.on_message_complete = handle_on_message_complete;
	      npfe1->settings.on_header_field = handle_header_name;
	      npfe1->settings.on_header_value = handle_header_val;
#ifdef HAVE_PROXY_EXTRA_DEBUG
	      npfe1->settings.on_url = handle_url;
#endif
	      npfe1->settings.uarg = npfe1;
        llhttp_init(&npfe1->parser, HTTP_BOTH, &npfe1->settings);

        for (retry = 0; retry < PROXY_MAPFD_RETRIES; retry++) {
          if (notify_add_ent(proxy_struct->ns, new_sd,
                  NOTI_TYPE_IN|NOTI_TYPE_HUP, npfe1) == 0)  {
            break;
          }
          //log_debug("failed to add new_sd %d - retry", new_sd);
          new_sd = get_mapped_proxy_fd(new_sd, 0);
          npfe1->fd = new_sd;
          if (npfe1->ssl) {
            SSL_set_fd(npfe1->ssl, new_sd);
          }
        }
        npfe1->used++;

        if (retry >= PROXY_MAPFD_RETRIES) {
          proxy_destroy_eps(new_sd, &ep_sel);
          proxy_release_fd_ctx(npfe1, 0);
          // Context to be defer freed
          free(npfe1);
          log_error("failed to add new_sd %d", new_sd);
          continue;
        }

        if (pfe->seltype == PROXY_SEL_N2 || protocol == IPPROTO_SCTP) {
          if (setup_proxy_path(&key, &rkey, npfe1, NULL)) {
            log_error("proxy setup failed %d - proto %d(sel %d)", fd, protocol, pfe->seltype);
            goto restart;
          }
        }

        PROXY_LOCK();
        npfe1->next = ent->val.fdlist;
        ent->val.fdlist = npfe1;
        ent->val.nfds++;
        PROXY_UNLOCK();
      } else if (pfe->stype == PROXY_SOCK_ACTIVE) {
        for (j = 0; j < PROXY_NUM_BURST_RX; j++) {
          int sret;
          int rc = proxy_sock_read(pfe, fd, pfe->rcvbuf + pfe->rcv_off, SP_SOCK_MSG_LEN - pfe->rcv_off);
          if ((sret = proxy_sock_read_err(pfe, rc))) {
            goto restart;
          }
          if (!pfe->odir) {
            const char *phurl = "";

            if (pfe->rfd[0] <= 0) {
              pfe->http_pok = 0;
              pfe->http_hok = 0;
              pfe->http_hvok = 0;

              enum llhttp_errno err = llhttp_execute(&pfe->parser,
                                      (char *)(pfe->rcvbuf + pfe->rcv_off), pfe->rcv_off+rc);
              if (err == HPE_OK) {
                pfe->rcv_off = 0;
                if (pfe->http_pok) {
                  if (pfe->http_hvok) {
                    phurl = pfe->host_url;
                  } else {
                    phurl = NULL;
                  }
                } else {
                  pfe->rcv_off += rc;
                  log_error("partial-rd %d", fd);
                  goto restart;
                }
              } else {
                log_debug("http parse error: %s %s", llhttp_errno_name(err), pfe->parser.reason);
                pfe->rcv_off = 0;
                llhttp_init(&pfe->parser, HTTP_BOTH, &pfe->settings);
                phurl = NULL;
              }

              if (setup_proxy_path(&key, &rkey, pfe, phurl)) {
                log_trace("proxy setup failed %d", fd);
                goto restart;
              }
            }
          }

          PROXY_ENT_LOCK(pfe);
          pfe_ent_accouting(pfe, rc, 0);
          PROXY_ENT_UNLOCK(pfe);

          if (proxy_multiplexor(pfe, pfe->rcvbuf, rc)) {
            goto restart;
          }
        }
      }
    } else if (type & NOTI_TYPE_OUT) {
      type &= ~NOTI_TYPE_OUT;
      if (pfe->stype == PROXY_SOCK_ACTIVE) {
        PROXY_ENT_LOCK(pfe);
        proxy_xmit_cache(pfe);
        PROXY_ENT_UNLOCK(pfe);
      }
    } else {
      /* Unhandled */
      return 0;
    }
  }
  return 0;
}

int
proxy_main(sockmap_cb_t sockmap_cb)
{
  int startfd = PROXY_START_MAPFD;
  notify_cbs_t cbs = { 0 };
  cbs.notify = proxy_notifier;
  cbs.pdestroy = proxy_pdestroy;

  proxy_struct = calloc(sizeof(proxy_struct_t), 1);
  if (proxy_struct == NULL) {
    assert(0);
  }
  proxy_struct->sockmap_cb = sockmap_cb;
  proxy_struct->ns = notify_ctx_new(&cbs, PROXY_MAX_THREADS);
  assert(proxy_struct->ns);

  for (int i = 0; i < PROXY_MAX_THREADS; i++) {
    proxy_struct->mapfd[i].start = startfd;
    proxy_struct->mapfd[i].next = proxy_struct->mapfd[i].start;
    proxy_struct->mapfd[i].end  = proxy_struct->mapfd[i].start + PROXY_MAX_MAPFD;
    startfd += PROXY_MAX_MAPFD + PROXY_MAPFD_ALLOC_RETRIES;
  }

  SSL_library_init();

  pthread_create(&proxy_struct->pthr, NULL, proxy_run, NULL);

  return 0;
}
