#include <ev.h>            // NOLINT(llvmlibc-restrict-system-libc-headers)
#include <math.h>          // NOLINT(llvmlibc-restrict-system-libc-headers)
#include <netinet/in.h>    // NOLINT(llvmlibc-restrict-system-libc-headers)
#include <sys/socket.h>    // NOLINT(llvmlibc-restrict-system-libc-headers)

#include "https_client.h"
#include "logging.h"
#include "options.h"

static size_t write_buffer(void *buf, size_t size, size_t nmemb, void *userp) {
  struct https_fetch_ctx *ctx = (struct https_fetch_ctx *)userp;
  char *new_buf = (char *)realloc(
      ctx->buf, ctx->buflen + size * nmemb + 1);
  if (new_buf == NULL) {
    ELOG("Out of memory!");
    return 0;
  }
  ctx->buf = new_buf;
  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  memcpy(&(ctx->buf[ctx->buflen]), buf, size * nmemb);
  ctx->buflen += size * nmemb;
  // We always expect to receive valid non-null ASCII but just to be safe...
  ctx->buf[ctx->buflen] = '\0';
  return size * nmemb;
}

static curl_socket_t opensocket_callback(void *clientp, curlsocktype purpose, struct curl_sockaddr *addr) {
  curl_socket_t sock = 0;

  sock=socket(addr->family, addr->socktype, addr->protocol);

  DLOG("curl opened socket: %d", (int)sock);

#if defined(IP_TOS)
  if (purpose != CURLSOCKTYPE_IPCXN) {
	return sock;
  }

  if (sock != -1) {
	if (addr->family == AF_INET) {
		(void)setsockopt(sock, IPPROTO_IP, IP_TOS, (int *)clientp, sizeof(int));
	}
#if defined(IPV6_TCLASS)
	else if (addr->family == AF_INET6) {
		(void)setsockopt(sock, IPPROTO_IPV6, IPV6_TCLASS, (int *)clientp, sizeof(int));
	}
#endif
  }
#endif

  return sock;
}

static int closesocket_callback(void __attribute__((unused)) *clientp, curl_socket_t item)
{
  DLOG("curl closed socket: %d", (int)item);
  return 0;
}

static void https_fetch_ctx_init(https_client_t *client,
                                 struct https_fetch_ctx *ctx, const char *url,
                                 const char* data, size_t datalen,
                                 struct curl_slist *resolv,
                                 https_response_cb cb, void *cb_data) {
  ctx->curl = curl_easy_init();
  ctx->header_list = NULL;
  ctx->cb = cb;
  ctx->cb_data = cb_data;
  ctx->buf = NULL;
  ctx->buflen = 0;
  ctx->next = client->fetches;
  client->fetches = ctx;

  CURLcode res = 0;
  if ((res = curl_easy_setopt(ctx->curl, CURLOPT_RESOLVE, resolv)) !=
      CURLE_OK) {
    FLOG("CURLOPT_RESOLV error: %s", curl_easy_strerror(res));
  }

  DLOG("Requesting HTTP/1.1: %d\n", client->opt->use_http_1_1);
  curl_easy_setopt(ctx->curl, CURLOPT_HTTP_VERSION,
                   client->opt->use_http_1_1 ?
                   CURL_HTTP_VERSION_1_1 :
                   CURL_HTTP_VERSION_2_0);
  if (logging_debug_enabled()) {
    curl_easy_setopt(ctx->curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(ctx->curl, CURLOPT_OPENSOCKETFUNCTION, opensocket_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_CLOSESOCKETFUNCTION, closesocket_callback);
  }
#if defined(IP_TOS)
  if (client->opt->dscp) {
    curl_easy_setopt(ctx->curl, CURLOPT_OPENSOCKETDATA, &client->opt->dscp);
    if (!logging_debug_enabled()) {
        curl_easy_setopt(ctx->curl, CURLOPT_OPENSOCKETFUNCTION, opensocket_callback);
      }
  }
#endif
  curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
  ctx->header_list = curl_slist_append(ctx->header_list, "Accept: application/dns-message");
  ctx->header_list = curl_slist_append(ctx->header_list, "Content-Type: application/dns-message");
  curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->header_list);
  curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDSIZE, datalen);
  curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, data);
  curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, &write_buffer);
  curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, ctx);
#ifdef CURLOPT_MAXAGE_CONN
  curl_easy_setopt(ctx->curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(ctx->curl, CURLOPT_TCP_KEEPIDLE, 50L);
  curl_easy_setopt(ctx->curl, CURLOPT_TCP_KEEPINTVL, 50L);
  curl_easy_setopt(ctx->curl, CURLOPT_MAXAGE_CONN, 300L);
#endif
  curl_easy_setopt(ctx->curl, CURLOPT_USERAGENT, "dns-to-https-proxy/0.2");
  curl_easy_setopt(ctx->curl, CURLOPT_NOSIGNAL, 0);
  curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, 10 /* seconds */);
  // We know Google supports this, so force it.
  curl_easy_setopt(ctx->curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  if (client->opt->curl_proxy) {
    DLOG("Using curl proxy: %s", client->opt->curl_proxy);
    if ((res = curl_easy_setopt(ctx->curl, CURLOPT_PROXY,
                                client->opt->curl_proxy)) != CURLE_OK) {
      FLOG("CURLOPT_PROXY error: %s", curl_easy_strerror(res));
    }
  }
  curl_multi_add_handle(client->curlm, ctx->curl);
}

static void https_fetch_ctx_cleanup(https_client_t *client,
                                    struct https_fetch_ctx *ctx) {
  struct https_fetch_ctx *last = NULL;
  struct https_fetch_ctx *cur = client->fetches;
  while (cur) {
    if (cur == ctx) {
      curl_multi_remove_handle(client->curlm, ctx->curl);
      if (client->opt->loglevel <= LOG_DEBUG) {
        CURLcode res = 0;
        long long_resp = 0;
        char *str_resp = NULL;
        if ((res = curl_easy_getinfo(
                ctx->curl, CURLINFO_EFFECTIVE_URL, &str_resp)) != CURLE_OK) {
          ELOG("CURLINFO_EFFECTIVE_URL: %s", curl_easy_strerror(res));
        } else {
          DLOG("CURLINFO_EFFECTIVE_URL: %s", str_resp);
        }
        if ((res = curl_easy_getinfo(
                ctx->curl, CURLINFO_REDIRECT_URL, &str_resp)) != CURLE_OK) {
          ELOG("CURLINFO_REDIRECT_URL: %s", curl_easy_strerror(res));
        } else if (str_resp != NULL) {
          DLOG("CURLINFO_REDIRECT_URL: %s", str_resp);
        }
        if ((res = curl_easy_getinfo(
                ctx->curl, CURLINFO_RESPONSE_CODE, &long_resp)) != CURLE_OK) {
          ELOG("CURLINFO_RESPONSE_CODE: %s", curl_easy_strerror(res));
        } else if (long_resp != 200) {
          DLOG("CURLINFO_RESPONSE_CODE: %d", long_resp);
        }
        if ((res = curl_easy_getinfo(
                ctx->curl, CURLINFO_SSL_VERIFYRESULT, &long_resp)) != CURLE_OK) {
          ELOG("CURLINFO_SSL_VERIFYRESULT: %s", curl_easy_strerror(res));
        } else if (long_resp != CURLE_OK) {
          ELOG("CURLINFO_SSL_VERIFYRESULT: %s", curl_easy_strerror(long_resp));
        }
        if ((res = curl_easy_getinfo(
                ctx->curl, CURLINFO_OS_ERRNO, &long_resp)) != CURLE_OK) {
          ELOG("CURLINFO_OS_ERRNO: %s", curl_easy_strerror(res));
        } else if (long_resp != 0) {
          ELOG("CURLINFO_OS_ERRNO: %d", long_resp);
        }
#ifdef CURLINFO_HTTP_VERSION
        if ((res = curl_easy_getinfo(
                ctx->curl, CURLINFO_HTTP_VERSION, &long_resp)) != CURLE_OK) {
          ELOG("CURLINFO_HTTP_VERSION: %s", curl_easy_strerror(res));
        } else {
          switch (long_resp) {
            case CURL_HTTP_VERSION_1_0:
              DLOG("CURLINFO_HTTP_VERSION: %s", "1.0");
              break;
            case CURL_HTTP_VERSION_1_1:
              DLOG("CURLINFO_HTTP_VERSION: %s", "1.1");
              break;
            case CURL_HTTP_VERSION_2_0:
              DLOG("CURLINFO_HTTP_VERSION: %s", "2");
              break;
            default:
              DLOG("CURLINFO_HTTP_VERSION: %d", long_resp);
          }
        }
#endif
#ifdef CURLINFO_PROTOCOL
        if ((res = curl_easy_getinfo(
                ctx->curl, CURLINFO_PROTOCOL, &long_resp)) != CURLE_OK) {
          ELOG("CURLINFO_PROTOCOL: %s", curl_easy_strerror(res));
        } else if (long_resp != CURLPROTO_HTTPS) {
          DLOG("CURLINFO_PROTOCOL: %d", long_resp);
        }
#endif

        double namelookup_time = NAN;
        double connect_time = NAN;
        double appconnect_time = NAN;
        double pretransfer_time = NAN;
        double starttransfer_time = NAN;
        double total_time = NAN;
        if (curl_easy_getinfo(ctx->curl,
                              CURLINFO_NAMELOOKUP_TIME, &namelookup_time) != CURLE_OK ||
            curl_easy_getinfo(ctx->curl,
                              CURLINFO_CONNECT_TIME, &connect_time) != CURLE_OK ||
            curl_easy_getinfo(ctx->curl,
                              CURLINFO_APPCONNECT_TIME, &appconnect_time) != CURLE_OK ||
            curl_easy_getinfo(ctx->curl,
                              CURLINFO_PRETRANSFER_TIME, &pretransfer_time) != CURLE_OK ||
            curl_easy_getinfo(ctx->curl,
                              CURLINFO_STARTTRANSFER_TIME, &starttransfer_time) != CURLE_OK ||
            curl_easy_getinfo(ctx->curl,
                              CURLINFO_TOTAL_TIME, &total_time) != CURLE_OK) {
          ELOG("Err getting timing");
        } else {
          DLOG("Times: %lf, %lf, %lf, %lf, %lf, %lf",
               namelookup_time, connect_time, appconnect_time, pretransfer_time,
               starttransfer_time, total_time);
        }

      }
      curl_easy_cleanup(ctx->curl);
      cur->cb(cur->cb_data, cur->buf, cur->buflen);
      curl_slist_free_all(cur->header_list);
      free(cur->buf);
      if (last) {
        last->next = cur->next;
      } else {
        client->fetches = cur->next;
      }
      free(cur);
      return;
    }
    last = cur;
    cur = cur->next;
  }
}

static void check_multi_info(https_client_t *c) {
  CURLMsg *msg = NULL;
  int msgs_left = 0;
  while ((msg = curl_multi_info_read(c->curlm, &msgs_left))) {
    if (msg->msg == CURLMSG_DONE) {
      struct https_fetch_ctx *n = c->fetches;
      while (n) {
        if (n->curl == msg->easy_handle) {
          https_fetch_ctx_cleanup(c, n);
          break;
        }
        n = n->next;
      }
    }
  }
}

static void sock_cb(struct ev_loop __attribute__((unused)) *loop,
                    struct ev_io *w, int revents) {
  https_client_t *c = (https_client_t *)w->data;
  if (c == NULL) {
    FLOG("c is NULL");
  }
  CURLMcode rc = curl_multi_socket_action(
      c->curlm, w->fd, (revents & EV_READ ? CURL_CSELECT_IN : 0) |
                       (revents & EV_WRITE ? CURL_CSELECT_OUT : 0),
      &c->still_running);
  if (rc != CURLM_OK) {
    FLOG("curl_multi_socket_action: %d", rc);
  }
  check_multi_info(c);
}

static void timer_cb(struct ev_loop __attribute__((unused)) *loop,
                     struct ev_timer *w, int __attribute__((unused)) revents) {
  https_client_t *c = (https_client_t *)w->data;
  CURLMcode rc = curl_multi_socket_action(c->curlm, CURL_SOCKET_TIMEOUT, 0,
                                          &c->still_running);
  if (rc != CURLM_OK) {
    FLOG("curl_multi_socket_action: %d", rc);
  }
  check_multi_info(c);
}

static int multi_sock_cb(CURL *curl, curl_socket_t sock, int what,
                         https_client_t *c, void __attribute__((unused)) *sockp) {
  if (!curl) {
    FLOG("Unexpected NULL pointer for CURL");
  }
  if (!c) {
    FLOG("Unexpected NULL pointer for https_client_t");
  }
  if (what == CURL_POLL_REMOVE) {
    ev_io_stop(c->loop, &c->fd[sock]);
    c->fd[sock].fd = 0;
    return 0;
  }
  if (c->fd[sock].fd) {
    ev_io_stop(c->loop, &c->fd[sock]);
  }
  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  ev_io_init(&c->fd[sock], sock_cb, sock,
             ((what & CURL_POLL_IN) ? EV_READ : 0) |
                 ((what & CURL_POLL_OUT) ? EV_WRITE : 0));
  c->fd[sock].data = c;
  ev_io_start(c->loop, &c->fd[sock]);
  return 0;
}

static int multi_timer_cb(CURLM __attribute__((unused)) *multi,
                          long timeout_ms, https_client_t *c) {
  ev_timer_stop(c->loop, &c->timer);
  if (timeout_ms > 0) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    ev_timer_init(&c->timer, timer_cb, timeout_ms / 1000.0, 0);
    ev_timer_start(c->loop, &c->timer);
  } else {
    timer_cb(c->loop, &c->timer, 0);
  }
  return 0;
}

void https_client_init(https_client_t *c, options_t *opt, struct ev_loop *loop) {
  int i = 0;

  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  memset(c, 0, sizeof(*c));
  c->loop = loop;
  c->curlm = curl_multi_init();
  c->fetches = NULL;
  c->timer.data = c;

  for (i = 0; i < FD_SETSIZE; i++) {
    c->fd[i].fd = 0;
  }

  c->opt = opt;

#if defined(CURLMOPT_PIPELINING) && defined(CURLPIPE_HTTP1) && \
  defined(CURLPIPE_MULTIPLEX)
  if (c->opt->use_http_1_1) {
    curl_multi_setopt(c->curlm, CURLMOPT_PIPELINING, CURLPIPE_HTTP1);
  } else {
    curl_multi_setopt(c->curlm, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
  }
#endif
  curl_multi_setopt(c->curlm, CURLMOPT_MAX_TOTAL_CONNECTIONS, 8);
  curl_multi_setopt(c->curlm, CURLMOPT_SOCKETDATA, c);
  curl_multi_setopt(c->curlm, CURLMOPT_SOCKETFUNCTION, multi_sock_cb);
  curl_multi_setopt(c->curlm, CURLMOPT_TIMERDATA, c);
  curl_multi_setopt(c->curlm, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
}

void https_client_fetch(https_client_t *c, const char *url,
                        const char* postdata, size_t postdata_len,
                        struct curl_slist *resolv, https_response_cb cb,
                        void *data) {
  struct https_fetch_ctx *new_ctx =
      (struct https_fetch_ctx *)calloc(1, sizeof(struct https_fetch_ctx));
  if (!new_ctx) {
    FLOG("Out of mem");
  }
  https_fetch_ctx_init(c, new_ctx, url, postdata, postdata_len, resolv, cb, data);
}

void https_client_reset(https_client_t *c) {
  options_t *opt = c->opt;
  struct ev_loop *loop = c->loop;
  https_client_cleanup(c);
  https_client_init(c, opt, loop);
}

void https_client_cleanup(https_client_t *c) {
  int i = 0;

  while (c->fetches) {
    https_fetch_ctx_cleanup(c, c->fetches);
  }

  for (i = 0; i < FD_SETSIZE; i++) {
    if (c->fd[i].fd) {
      ev_io_stop(c->loop, &c->fd[i]);
    }
  }
  ev_timer_stop(c->loop, &c->timer);
  curl_multi_cleanup(c->curlm);
}
