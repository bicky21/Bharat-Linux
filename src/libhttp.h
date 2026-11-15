#ifndef LIBHTTP_H
#define LIBHTTP_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * libhttp.h - fetching web pages: http:// and https://.
 *
 * Our own HTTP/1.1 client: addresses, redirects, chunked answers,
 * cookies, limits and time-outs. The encryption of https (TLS) is done
 * by BearSSL, a small, proven library (see THIRD-PARTY.md): writing
 * encryption ourselves would not be safe. Certificates are checked
 * against the authorities in /etc/ssl/certs/ca-certificates.crt.
 *
 * Used by the browser's fetcher (bharat-fetch) and by wget.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include <bearssl.h>

#define HTTP_MAX_BODY (16 * 1024 * 1024)
#define HTTP_TIMEOUT_MS 20000
#define CA_FILE "/etc/ssl/certs/ca-certificates.crt"

#include "liburl.h"

/* ====================================================================
 * trusted certificate authorities (for https)
 * ==================================================================== */

struct ta_list {
    br_x509_trust_anchor *ta;
    size_t count, cap;
};

static struct ta_list trust;

struct dn_buf { unsigned char *data; size_t len, cap; };

static void dn_collect(void *ctx, const void *buf, size_t len) {

    struct dn_buf *d = ctx;

    if (d->len + len > d->cap) {

        size_t cap = (d->len + len) * 2;
        unsigned char *bigger = realloc(d->data, cap);

        if (!bigger)
            return;

        d->data = bigger;
        d->cap = cap;
    }

    memcpy(d->data + d->len, buf, len);
    d->len += len;
}

static void add_anchor(const unsigned char *der, size_t len) {

    br_x509_decoder_context dc;
    struct dn_buf dn = {0};

    br_x509_decoder_init(&dc, dn_collect, &dn);
    br_x509_decoder_push(&dc, der, len);

    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);

    if (!pk || br_x509_decoder_last_error(&dc) != 0) {

        free(dn.data);
        return;
    }

    if (trust.count == trust.cap) {

        size_t cap = trust.cap ? trust.cap * 2 : 160;
        br_x509_trust_anchor *bigger = realloc(trust.ta, cap * sizeof(*bigger));

        if (!bigger) {

            free(dn.data);
            return;
        }

        trust.ta = bigger;
        trust.cap = cap;
    }

    br_x509_trust_anchor *ta = &trust.ta[trust.count];

    ta->dn.data = dn.data;
    ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;

    if (pk->key_type == BR_KEYTYPE_RSA) {

        ta->pkey.key_type = BR_KEYTYPE_RSA;
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.n = malloc(pk->key.rsa.nlen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
        ta->pkey.key.rsa.e = malloc(pk->key.rsa.elen);

        if (!ta->pkey.key.rsa.n || !ta->pkey.key.rsa.e)
            return;

        memcpy(ta->pkey.key.rsa.n, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(ta->pkey.key.rsa.e, pk->key.rsa.e, pk->key.rsa.elen);
    }
    else if (pk->key_type == BR_KEYTYPE_EC) {

        ta->pkey.key_type = BR_KEYTYPE_EC;
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
        ta->pkey.key.ec.q = malloc(pk->key.ec.qlen);

        if (!ta->pkey.key.ec.q)
            return;

        memcpy(ta->pkey.key.ec.q, pk->key.ec.q, pk->key.ec.qlen);
    }
    else {

        free(dn.data);
        return;
    }

    trust.count++;
}

static void der_collect(void *ctx, const void *buf, size_t len) {

    dn_collect(ctx, buf, len);
}

/* reads the PEM bundle once */
static int load_trust(void) {

    if (trust.count)
        return 0;

    /* SSL_CERT_FILE: another list, as with other Linux programs */
    const char *file = getenv("SSL_CERT_FILE");
    FILE *f = fopen(file && file[0] ? file : CA_FILE, "r");

    if (!f)
        return -1;

    br_pem_decoder_context pc;
    struct dn_buf der = {0};
    int in_cert = 0;
    char buf[4096];
    size_t n;

    br_pem_decoder_init(&pc);

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {

        const char *p = buf;

        while (n > 0) {

            size_t used = br_pem_decoder_push(&pc, p, n);

            p += used;
            n -= used;

            switch (br_pem_decoder_event(&pc)) {

                case BR_PEM_BEGIN_OBJ:
                    in_cert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE");
                    der.len = 0;
                    br_pem_decoder_setdest(&pc, in_cert ? der_collect : NULL, &der);
                    break;

                case BR_PEM_END_OBJ:
                    if (in_cert && der.len)
                        add_anchor(der.data, der.len);
                    in_cert = 0;
                    break;

                case BR_PEM_ERROR:
                    fclose(f);
                    free(der.data);
                    return trust.count ? 0 : -1;
            }
        }
    }

    fclose(f);
    free(der.data);

    return trust.count ? 0 : -1;
}

/* ====================================================================
 * connections: plain or TLS
 * ==================================================================== */

struct conn {
    int fd;
    int tls;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    /* read buffer for lines */
    unsigned char buf[16384];
    size_t have, at;
};

static int wait_fd(int fd, short ev) {

    struct pollfd p = {.fd = fd, .events = ev};

    return poll(&p, 1, HTTP_TIMEOUT_MS) > 0 ? 0 : -1;
}

static int sock_read(void *ctx, unsigned char *buf, size_t len) {

    int fd = *(int *)ctx;

    while (1) {

        if (wait_fd(fd, POLLIN) != 0)
            return -1;

        ssize_t n = read(fd, buf, len);

        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;

        return n <= 0 ? -1 : (int)n;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {

    int fd = *(int *)ctx;

    while (1) {

        if (wait_fd(fd, POLLOUT) != 0)
            return -1;

        ssize_t n = write(fd, buf, len);

        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;

        return n <= 0 ? -1 : (int)n;
    }
}

static int conn_open(struct conn *c, const struct url *u, char *err, size_t errsize) {

    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *res;
    int e = getaddrinfo(u->host, u->port, &hints, &res);

    memset(c, 0, offsetof(struct conn, iobuf));
    c->fd = -1;

    if (e) {

        snprintf(err, errsize, "%.80s: %s", u->host, e == EAI_NONAME ? "this name was not found (check the network)" : gai_strerror(e));
        return -1;
    }

    for (struct addrinfo *a = res; a && c->fd < 0; a = a->ai_next) {

        int s = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);

        if (s < 0)
            continue;

        if (connect(s, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) {

            close(s);
            continue;
        }

        int so = 0;
        socklen_t sl = sizeof(so);

        if (wait_fd(s, POLLOUT) != 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &so, &sl) != 0 || so) {

            close(s);
            continue;
        }

        c->fd = s;
    }

    freeaddrinfo(res);

    if (c->fd < 0) {

        snprintf(err, errsize, "cannot connect to %.80s", u->host);
        return -1;
    }

    if (!u->https)
        return 0;

    if (load_trust() != 0) {

        snprintf(err, errsize, "no trusted certificate list (%s)", CA_FILE);
        close(c->fd);
        return -1;
    }

    c->tls = 1;
    br_ssl_client_init_full(&c->sc, &c->xc, trust.ta, trust.count);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);

    if (!br_ssl_client_reset(&c->sc, u->host, 0)) {

        snprintf(err, errsize, "TLS start failed");
        close(c->fd);
        return -1;
    }

    br_sslio_init(&c->io, &c->sc.eng, sock_read, &c->fd, sock_write, &c->fd);

    return 0;
}

static void tls_error(struct conn *c, char *err, size_t errsize) {

    int e = br_ssl_engine_last_error(&c->sc.eng);

    if (e >= BR_ERR_X509_EXPIRED && e <= BR_ERR_X509_EXPIRED + 2)
        snprintf(err, errsize, "the site's certificate has expired or is not valid yet (check the clock)");
    else if (e == BR_ERR_X509_NOT_TRUSTED)
        snprintf(err, errsize, "the site's certificate is not from a trusted authority");
    else if (e == BR_ERR_X509_BAD_SERVER_NAME)
        snprintf(err, errsize, "the certificate belongs to another site");
    else
        snprintf(err, errsize, "secure connection failed (TLS error %d)", e);
}

static int conn_write(struct conn *c, const void *data, size_t len) {

    if (!c->tls) {

        const char *p = data;

        while (len) {

            int n = sock_write(&c->fd, (const unsigned char *)p, len);

            if (n < 0)
                return -1;

            p += n;
            len -= (size_t)n;
        }

        return 0;
    }

    if (br_sslio_write_all(&c->io, data, len) != 0 || br_sslio_flush(&c->io) != 0)
        return -1;

    return 0;
}

static int conn_fill(struct conn *c) {

    if (c->at < c->have)
        return 0;

    int n = c->tls ? br_sslio_read(&c->io, c->buf, sizeof(c->buf))
                   : sock_read(&c->fd, c->buf, sizeof(c->buf));

    if (n <= 0)
        return -1;

    c->have = (size_t)n;
    c->at = 0;

    return 0;
}

static int conn_line(struct conn *c, char *out, size_t size) {

    size_t k = 0;

    while (1) {

        if (conn_fill(c) != 0)
            return k ? 0 : -1;

        char ch = (char)c->buf[c->at++];

        if (ch == '\n')
            break;

        if (ch != '\r' && k + 1 < size)
            out[k++] = ch;
    }

    out[k] = 0;

    return 0;
}

static ssize_t conn_read(struct conn *c, void *out, size_t len) {

    if (conn_fill(c) != 0)
        return 0;

    size_t n = c->have - c->at;

    if (n > len)
        n = len;

    memcpy(out, c->buf + c->at, n);
    c->at += n;

    return (ssize_t)n;
}

static void conn_close(struct conn *c) {

    if (c->tls)
        br_sslio_close(&c->io);

    if (c->fd >= 0)
        close(c->fd);

    c->fd = -1;
}

/* ====================================================================
 * cookies (kept in memory: forgotten when the browser closes)
 * ==================================================================== */

#define MAX_COOKIES 300

struct cookie { char host[128], name[128], value[1024]; };

static struct cookie *cookies;
static int ncookies;

static void cookie_store(const char *host, const char *header) {

    char name[128], value[1024];
    const char *eq = strchr(header, '='), *semi = strchr(header, ';');

    if (!eq || (semi && semi < eq))
        return;

    snprintf(name, sizeof(name), "%.*s", (int)(eq - header < 127 ? eq - header : 127), header);
    snprintf(value, sizeof(value), "%.*s", (int)((semi ? (size_t)(semi - eq - 1) : strlen(eq + 1)) < 1023 ?
             (semi ? (size_t)(semi - eq - 1) : strlen(eq + 1)) : 1023), eq + 1);

    /* "Domain=.example.com" lets sub-sites share it */
    char domain[128];
    const char *d = strcasestr(header, "domain=");

    if (d) {

        d += 7;

        if (*d == '.')
            d++;

        snprintf(domain, sizeof(domain), "%.*s", (int)strcspn(d, "; "), d);
    }
    else
        snprintf(domain, sizeof(domain), "%.127s", host);

    if (!cookies)
        cookies = calloc(MAX_COOKIES, sizeof(struct cookie));

    if (!cookies)
        return;

    for (int i = 0; i < ncookies; i++) {

        if (!strcmp(cookies[i].host, domain) && !strcmp(cookies[i].name, name)) {

            snprintf(cookies[i].value, sizeof(cookies[i].value), "%s", value);
            return;
        }
    }

    if (ncookies < MAX_COOKIES) {

        snprintf(cookies[ncookies].host, sizeof(cookies[0].host), "%s", domain);
        snprintf(cookies[ncookies].name, sizeof(cookies[0].name), "%s", name);
        snprintf(cookies[ncookies].value, sizeof(cookies[0].value), "%s", value);
        ncookies++;
    }
}

static void cookie_header(const char *host, char *out, size_t size) {

    out[0] = 0;

    for (int i = 0; i < ncookies; i++) {

        size_t hl = strlen(host), dl = strlen(cookies[i].host);

        /* same site, or a sub-site of the cookie's domain */
        if (!(hl >= dl && !strcmp(host + hl - dl, cookies[i].host) && (hl == dl || host[hl - dl - 1] == '.')))
            continue;

        size_t used = strlen(out);

        snprintf(out + used, size - used, "%s%s=%s", used ? "; " : "Cookie: ", cookies[i].name, cookies[i].value);
    }

    if (out[0])
        strncat(out, "\r\n", size - strlen(out) - 1);
}

/* ====================================================================
 * one request
 * ==================================================================== */

struct http_result {
    int status;
    char final_url[4200];
    char content_type[128];
    char *body;
    size_t len;
    char error[200];
};

/*
 * GET (post_body NULL) or POST. Follows up to 8 redirects. With `save`,
 * the body goes into that file instead of memory (downloads).
 * Returns 0 when an answer arrived (any status), -1 with r->error set.
 */
static int http_fetch(const char *address, const char *post_body, const char *post_type,
                      FILE *save, struct http_result *r) {

    char current[4200];
    char method[8] = "GET";
    const char *body = post_body;

    memset(r, 0, sizeof(*r));
    snprintf(current, sizeof(current), "%s", address);

    if (body)
        snprintf(method, sizeof(method), "POST");

    for (int hop = 0; hop < 8; hop++) {

        struct url u;
        static struct conn c;

        if (url_parse(current, &u) != 0) {

            snprintf(r->error, sizeof(r->error), "this address is not understood");
            return -1;
        }

        url_text(&u, r->final_url, sizeof(r->final_url));

        if (conn_open(&c, &u, r->error, sizeof(r->error)) != 0)
            return -1;

        char cookie[4096], req[10000];
        int normal = !strcmp(u.port, u.https ? "443" : "80");

        cookie_header(u.host, cookie, sizeof(cookie));

        int n = snprintf(req, sizeof(req),
                         "%s %s HTTP/1.1\r\nHost: %s%s%s\r\n"
                         "User-Agent: Mozilla/5.0 (X11; Bharat Linux) BharatBrowser/0.1\r\n"
                         "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
                         "Accept-Language: en-IN,en;q=0.9,hi;q=0.8\r\n"
                         "Accept-Encoding: identity\r\n%s"
                         "Connection: close\r\n",
                         method, u.path, u.host, normal ? "" : ":", normal ? "" : u.port, cookie);

        if (body)
            n += snprintf(req + n, sizeof(req) - (size_t)n, "Content-Type: %s\r\nContent-Length: %zu\r\n",
                          post_type ? post_type : "application/x-www-form-urlencoded", strlen(body));

        n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");

        if (conn_write(&c, req, (size_t)n) != 0 || (body && conn_write(&c, body, strlen(body)) != 0)) {

            if (c.tls) tls_error(&c, r->error, sizeof(r->error));
            else snprintf(r->error, sizeof(r->error), "sending the request failed");

            conn_close(&c);
            return -1;
        }

        char line[8192], location[4200] = "";
        long long length = -1;
        int chunked = 0;

        if (conn_line(&c, line, sizeof(line)) != 0 || sscanf(line, "HTTP/%*s %d", &r->status) != 1) {

            if (c.tls) tls_error(&c, r->error, sizeof(r->error));
            else snprintf(r->error, sizeof(r->error), "no answer from %.80s", u.host);

            conn_close(&c);
            return -1;
        }

        r->content_type[0] = 0;

        while (conn_line(&c, line, sizeof(line)) == 0 && line[0]) {

            if (!strncasecmp(line, "Content-Length:", 15)) length = atoll(line + 15);
            else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(line, "chunked")) chunked = 1;
            else if (!strncasecmp(line, "Location:", 9))
                snprintf(location, sizeof(location), "%.4100s", line + 9 + strspn(line + 9, " "));
            else if (!strncasecmp(line, "Content-Type:", 13))
                snprintf(r->content_type, sizeof(r->content_type), "%.120s", line + 13 + strspn(line + 13, " "));
            else if (!strncasecmp(line, "Set-Cookie:", 11))
                cookie_store(u.host, line + 11 + strspn(line + 11, " "));
        }

        if (r->status >= 300 && r->status < 400 && location[0]) {

            conn_close(&c);

            char next[4200];

            if (url_resolve(current, location, next, sizeof(next)) != 0) {

                snprintf(r->error, sizeof(r->error), "a redirect to an address we cannot open");
                return -1;
            }

            snprintf(current, sizeof(current), "%s", next);

            /* after a POST, a 303 (or 301/302) continues with GET */
            if (r->status != 307 && r->status != 308) {

                body = NULL;
                snprintf(method, sizeof(method), "GET");
            }

            continue;
        }

        /* the body */
        size_t cap = 0;
        char buf[16384];

        r->body = NULL;
        r->len = 0;

        while (1) {

            long long want;

            if (chunked) {

                if (conn_line(&c, line, sizeof(line)) != 0)
                    break;

                want = strtoll(line, NULL, 16);

                if (want <= 0)
                    break;
            }
            else
                want = length >= 0 ? length - (long long)r->len : (long long)1 << 40;

            while (want > 0) {

                ssize_t got = conn_read(&c, buf, (size_t)(want < (long long)sizeof(buf) ? want : (long long)sizeof(buf)));

                if (got <= 0) {

                    want = -1;
                    break;
                }

                if (save) {

                    fwrite(buf, 1, (size_t)got, save);
                }
                else {

                    if (r->len + (size_t)got > HTTP_MAX_BODY) {

                        conn_close(&c);
                        snprintf(r->error, sizeof(r->error), "the page is too big (over 16 MB)");
                        return -1;
                    }

                    if (r->len + (size_t)got + 1 > cap) {

                        cap = (r->len + (size_t)got + 1) * 2;

                        char *bigger = realloc(r->body, cap);

                        if (!bigger) {

                            conn_close(&c);
                            snprintf(r->error, sizeof(r->error), "out of memory");
                            return -1;
                        }

                        r->body = bigger;
                    }

                    memcpy(r->body + r->len, buf, (size_t)got);
                }

                r->len += (size_t)got;
                want -= got;
            }

            if (chunked) {

                if (want < 0)
                    break;

                conn_line(&c, line, sizeof(line));     /* the end of the chunk */
                continue;
            }

            break;
        }

        if (r->body)
            r->body[r->len] = 0;

        conn_close(&c);

        return 0;
    }

    snprintf(r->error, sizeof(r->error), "too many redirects");

    return -1;
}

#endif
