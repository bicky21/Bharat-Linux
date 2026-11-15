/*
 * netd - the network service (started by init, runs as root).
 *
 *  - switches on "lo" (127.0.0.1, the computer talking to itself)
 *  - finds wired network cards (also USB tethering from a phone) and
 *    connects them by themselves: it asks the network for an address
 *    with DHCP, like every Linux system does
 *  - notices cables being plugged in and out, and cards appearing later
 *  - keeps the address (renews the DHCP lease in time)
 *  - writes the name servers (DNS) to /etc/resolv.conf
 *  - tells everyone how things stand in /run/network/state
 *  - takes requests on /run/netd.sock: status, renew, on IF, off IF.
 *    root and the person at the desktop may ask (SO_PEERCRED).
 *
 * /etc/network.conf can fix a card's settings:
 *     eth0 static 192.168.1.20/24 192.168.1.1 1.1.1.1
 *     eth1 off
 * (anything not listed uses DHCP).
 *
 * DHCP is spoken with "packet" sockets: before a card has an address,
 * normal sockets cannot be used on it. The program sleeps in poll()
 * until a packet, a cable change or a timer needs it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "libuser.h"

#define MAX_IF 8
#define SOCKET_PATH "/run/netd.sock"
#define STATE_FILE "/run/network/state"

enum { S_OFF, S_NOCABLE, S_DISCOVER, S_REQUEST, S_BOUND, S_STATIC, S_FAILED };

static const char *state_names[] = {"off", "no-cable", "connecting", "connecting", "connected", "connected", "no-answer"};

struct iface {
    int used;
    char name[IFNAMSIZ];
    int index;
    unsigned char mac[6];
    int state;
    int user_off;              /* switched off by a person */
    int fixed;                 /* 0 dhcp, 1 static, 2 off (network.conf) */
    int fd;                    /* packet socket while talking DHCP */
    uint32_t xid;
    int tries;
    long long next;            /* ms: when to act next (resend, renew) */
    uint32_t offered, server;
    uint32_t addr, mask, gateway, dns[3];
    uint32_t lease;            /* seconds */
    long long bound_at;
};

static struct iface ifs[MAX_IF];
static int ctl = -1;           /* ioctl socket */

static long long now_ms(void) {

    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void ip_text(uint32_t a, char *out) {

    struct in_addr in = {a};

    inet_ntop(AF_INET, &in, out, 16);
}

static int prefix_len(uint32_t mask) {

    return __builtin_popcount(ntohl(mask));
}

/* ====================================================================
 * telling the kernel: up/down, address, route, name servers
 * ==================================================================== */

static void set_flags(const char *name, int up) {

    struct ifreq r;

    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, IFNAMSIZ, "%s", name);

    if (ioctl(ctl, SIOCGIFFLAGS, &r) != 0)
        return;

    if (up)
        r.ifr_flags |= IFF_UP;
    else
        r.ifr_flags &= ~IFF_UP;

    ioctl(ctl, SIOCSIFFLAGS, &r);
}

static void set_addr(const char *name, uint32_t addr, uint32_t mask) {

    struct ifreq r;
    struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;

    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, IFNAMSIZ, "%s", name);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = addr;
    ioctl(ctl, SIOCSIFADDR, &r);

    if (addr) {

        sin->sin_addr.s_addr = mask;
        ioctl(ctl, SIOCSIFNETMASK, &r);
    }
}

static void default_route(const char *name, uint32_t gateway, int add) {

    struct rtentry rt;
    struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
    struct sockaddr_in *gw = (struct sockaddr_in *)&rt.rt_gateway;
    struct sockaddr_in *mask = (struct sockaddr_in *)&rt.rt_genmask;
    char dev[IFNAMSIZ];

    memset(&rt, 0, sizeof(rt));
    snprintf(dev, sizeof(dev), "%s", name);
    dst->sin_family = gw->sin_family = mask->sin_family = AF_INET;
    gw->sin_addr.s_addr = gateway;
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    rt.rt_dev = dev;
    rt.rt_metric = 1;

    ioctl(ctl, add ? SIOCADDRT : SIOCDELRT, &rt);
}

static void write_resolv_conf(void) {

    FILE *f = fopen("/etc/resolv.conf.new", "w");

    if (!f)
        return;

    fprintf(f, "# Written by netd from the network's settings (DHCP or /etc/network.conf)\n");

    int any = 0;

    for (int i = 0; i < MAX_IF; i++) {

        if (!ifs[i].used || (ifs[i].state != S_BOUND && ifs[i].state != S_STATIC))
            continue;

        for (int k = 0; k < 3 && ifs[i].dns[k]; k++) {

            char t[16];

            ip_text(ifs[i].dns[k], t);
            fprintf(f, "nameserver %s\n", t);
            any = 1;
        }
    }

    fclose(f);

    if (any) {

        chmod("/etc/resolv.conf.new", 0644);
        rename("/etc/resolv.conf.new", "/etc/resolv.conf");
    }
    else
        unlink("/etc/resolv.conf.new");
}

/* /run/network/state: one line per card, for the desktop and commands */
static void write_state(void) {

    mkdir("/run/network", 0755);

    FILE *f = fopen(STATE_FILE ".new", "w");

    if (!f)
        return;

    for (int i = 0; i < MAX_IF; i++) {

        struct iface *n = &ifs[i];

        if (!n->used)
            continue;

        char a[16] = "-", g[16] = "-", d[16] = "-";

        if (n->state == S_BOUND || n->state == S_STATIC) {

            ip_text(n->addr, a);
            ip_text(n->gateway, g);

            if (n->dns[0])
                ip_text(n->dns[0], d);

            sprintf(a + strlen(a), "/%d", prefix_len(n->mask));
        }

        fprintf(f, "%s %s %s %s %s %s\n", n->name, state_names[n->state], a, g, d,
                n->state == S_STATIC ? "static" : "dhcp");
    }

    fclose(f);
    chmod(STATE_FILE ".new", 0644);
    rename(STATE_FILE ".new", STATE_FILE);
}

static void forget_address(struct iface *n) {

    if (n->addr) {

        if (n->gateway)
            default_route(n->name, n->gateway, 0);

        set_addr(n->name, 0, 0);
    }

    n->addr = n->mask = n->gateway = 0;
    memset(n->dns, 0, sizeof(n->dns));
}

static void apply(struct iface *n) {

    set_addr(n->name, n->addr, n->mask);

    if (n->gateway)
        default_route(n->name, n->gateway, 1);

    write_resolv_conf();
}

/* ====================================================================
 * DHCP
 * ==================================================================== */

struct dhcp {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t cookie;
    uint8_t options[312];
} __attribute__((packed));

struct packet {
    struct iphdr ip;
    struct udphdr udp;
    struct dhcp dhcp;
} __attribute__((packed));

static uint16_t checksum(const void *data, size_t len) {

    const uint8_t *p = data;
    uint32_t sum = 0;

    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);

    if (len & 1)
        sum += (uint32_t)(p[len - 1] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return htons((uint16_t)~sum);
}

static int open_packet_socket(struct iface *n) {

    int fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_IP));
    struct sockaddr_ll a = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP), .sll_ifindex = n->index};

    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {

        if (fd >= 0)
            close(fd);

        return -1;
    }

    return fd;
}

static void send_dhcp(struct iface *n, int type) {

    struct packet p;
    uint8_t *o = p.dhcp.options;
    char host[64] = "";

    memset(&p, 0, sizeof(p));
    gethostname(host, sizeof(host) - 1);

    p.dhcp.op = 1;
    p.dhcp.htype = 1;
    p.dhcp.hlen = 6;
    p.dhcp.xid = n->xid;
    p.dhcp.flags = htons(0x8000);          /* please answer to everyone: we have no address yet */
    memcpy(p.dhcp.chaddr, n->mac, 6);
    p.dhcp.cookie = htonl(0x63825363);

    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;          /* message type */

    if (type == 3 && n->state == S_REQUEST) {

        *o++ = 50; *o++ = 4; memcpy(o, &n->offered, 4); o += 4;   /* the address we want */
        *o++ = 54; *o++ = 4; memcpy(o, &n->server, 4); o += 4;    /* from this server */
    }
    else if (type == 3) {

        p.dhcp.ciaddr = n->addr;                         /* renewing what we have */
    }

    size_t hl = strlen(host);

    if (hl && hl < 60) {

        *o++ = 12; *o++ = (uint8_t)hl; memcpy(o, host, hl); o += hl;   /* our name */
    }

    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;     /* we want: mask, router, DNS, lease */
    *o++ = 255;

    size_t dhcp_len = sizeof(p.dhcp);

    p.udp.source = htons(68);
    p.udp.dest = htons(67);
    p.udp.len = htons((uint16_t)(sizeof(p.udp) + dhcp_len));

    p.ip.version = 4;
    p.ip.ihl = 5;
    p.ip.ttl = 64;
    p.ip.protocol = IPPROTO_UDP;
    p.ip.saddr = 0;
    p.ip.daddr = INADDR_BROADCAST;
    p.ip.tot_len = htons((uint16_t)sizeof(p));
    p.ip.check = checksum(&p.ip, sizeof(p.ip));

    struct sockaddr_ll a = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP),
                            .sll_ifindex = n->index, .sll_halen = 6};

    memset(a.sll_addr, 0xFF, 6);
    sendto(n->fd, &p, sizeof(p), 0, (struct sockaddr *)&a, sizeof(a));
}

static void start_dhcp(struct iface *n) {

    if (n->fd < 0)
        n->fd = open_packet_socket(n);

    n->state = S_DISCOVER;
    n->tries = 0;
    n->xid = (uint32_t)random();
    n->next = now_ms();
}

static void stop_dhcp(struct iface *n) {

    if (n->fd >= 0)
        close(n->fd);

    n->fd = -1;
}

/* the value of an option in an answer */
static const uint8_t *option(const struct dhcp *d, size_t len, int code, int *olen) {

    const uint8_t *o = d->options, *end = (const uint8_t *)d + len;

    while (o < end && *o != 255) {

        if (*o == 0) {

            o++;
            continue;
        }

        if (o + 2 > end || o + 2 + o[1] > end)
            break;

        if (*o == code) {

            *olen = o[1];
            return o + 2;
        }

        o += 2 + o[1];
    }

    return NULL;
}

static void got_packet(struct iface *n) {

    struct packet p;
    ssize_t len = recv(n->fd, &p, sizeof(p), 0);

    if (len < (ssize_t)(sizeof(p.ip) + sizeof(p.udp) + 240) || p.ip.protocol != IPPROTO_UDP ||
        p.udp.dest != htons(68) || p.dhcp.op != 2 || p.dhcp.xid != n->xid ||
        memcmp(p.dhcp.chaddr, n->mac, 6) != 0)
        return;

    size_t dlen = (size_t)len - sizeof(p.ip) - sizeof(p.udp);
    int ol;
    const uint8_t *t = option(&p.dhcp, dlen, 53, &ol);

    if (!t)
        return;

    if (*t == 2 && n->state == S_DISCOVER) {          /* OFFER */

        const uint8_t *s = option(&p.dhcp, dlen, 54, &ol);

        n->offered = p.dhcp.yiaddr;
        n->server = s && ol == 4 ? *(const uint32_t *)s : 0;
        n->state = S_REQUEST;
        n->tries = 0;
        n->next = now_ms();
        return;
    }

    if (*t == 6) {                                     /* NAK: start over */

        forget_address(n);
        start_dhcp(n);
        write_state();
        return;
    }

    if (*t == 5 && (n->state == S_REQUEST || n->state == S_BOUND)) {  /* ACK */

        uint32_t old_addr = n->addr, old_gw = n->gateway;
        const uint8_t *v;

        n->addr = p.dhcp.yiaddr;
        n->mask = (v = option(&p.dhcp, dlen, 1, &ol)) && ol == 4 ? *(const uint32_t *)v : htonl(0xFFFFFF00);
        n->gateway = (v = option(&p.dhcp, dlen, 3, &ol)) && ol >= 4 ? *(const uint32_t *)v : 0;
        n->lease = (v = option(&p.dhcp, dlen, 51, &ol)) && ol == 4 ? ntohl(*(const uint32_t *)v) : 3600;
        memset(n->dns, 0, sizeof(n->dns));

        if ((v = option(&p.dhcp, dlen, 6, &ol)) != NULL)
            for (int k = 0; k < 3 && (k + 1) * 4 <= ol; k++)
                memcpy(&n->dns[k], v + 4 * k, 4);

        if (n->lease < 60)
            n->lease = 60;

        n->state = S_BOUND;      /* before apply(): resolv.conf lists bound cards */

        if (old_addr != n->addr || old_gw != n->gateway) {

            if (old_addr) {

                if (old_gw)
                    default_route(n->name, old_gw, 0);
            }

            apply(n);
        }
        else
            write_resolv_conf();

        n->state = S_BOUND;
        n->bound_at = now_ms();
        n->next = n->bound_at + (long long)n->lease * 500;   /* renew at half time */
        n->tries = 0;
        write_state();

        /* not needed until the renewal: a packet socket sees every packet */
        stop_dhcp(n);
    }
}

/* resends and renewals */
static void timer(struct iface *n) {

    long long t = now_ms();

    if (n->next == 0 || t < n->next)
        return;

    if (n->state == S_DISCOVER || n->state == S_REQUEST) {

        /* give up after a while, but try again later (cable to a switch that starts slowly) */
        if (n->tries >= 6) {

            n->state = S_FAILED;
            n->next = t + 60 * 1000;
            write_state();
            return;
        }

        send_dhcp(n, n->state == S_DISCOVER ? 1 : 3);
        n->tries++;
        n->next = t + (1000LL << (n->tries < 4 ? n->tries : 4));   /* 2, 4, 8, 16 s */

        /* a REQUEST without an answer: ask from the start */
        if (n->state == S_REQUEST && n->tries >= 3) {

            n->state = S_DISCOVER;
            n->tries = 0;
        }
        return;
    }

    if (n->state == S_FAILED) {

        start_dhcp(n);
        write_state();
        return;
    }

    if (n->state == S_BOUND) {

        /* the lease ran out without a renewal: start over */
        if (t > n->bound_at + (long long)n->lease * 1000) {

            forget_address(n);
            start_dhcp(n);
            write_state();
            return;
        }

        if (n->fd < 0)
            n->fd = open_packet_socket(n);

        send_dhcp(n, 3);
        n->next = t + 30 * 1000;
    }
}

/* ====================================================================
 * cards
 * ==================================================================== */

static int carrier(const char *name) {

    char path[96], c = '0';

    snprintf(path, sizeof(path), "/sys/class/net/%.40s/carrier", name);

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {

        if (read(fd, &c, 1) != 1)
            c = '0';

        close(fd);
    }

    return c == '1';
}

/* /etc/network.conf: fixed settings for one card */
static void read_config(struct iface *n) {

    FILE *f = fopen("/etc/network.conf", "r");
    char line[256];

    n->fixed = 0;

    while (f && fgets(line, sizeof(line), f)) {

        char name[32], how[16], addr[40] = "", gw[20] = "", dns[20] = "";

        if (line[0] == '#' || sscanf(line, "%31s %15s %39s %19s %19s", name, how, addr, gw, dns) < 2 ||
            strcmp(name, n->name))
            continue;

        if (!strcmp(how, "off"))
            n->fixed = 2;
        else if (!strcmp(how, "static")) {

            char *slash = strchr(addr, '/');
            int bits = slash ? atoi(slash + 1) : 24;

            if (slash)
                *slash = 0;

            struct in_addr a, g, d;

            if (inet_pton(AF_INET, addr, &a) == 1) {

                n->fixed = 1;
                n->addr = a.s_addr;
                n->mask = bits <= 0 ? 0 : htonl(0xFFFFFFFFu << (32 - (bits > 32 ? 32 : bits)));
                n->gateway = inet_pton(AF_INET, gw, &g) == 1 ? g.s_addr : 0;
                n->dns[0] = inet_pton(AF_INET, dns, &d) == 1 ? d.s_addr : 0;
            }
        }
    }

    if (f)
        fclose(f);
}

/* what a card should do now */
static void decide(struct iface *n) {

    int was = n->state;

    if (n->user_off || n->fixed == 2) {

        stop_dhcp(n);
        forget_address(n);
        set_flags(n->name, 0);
        n->state = S_OFF;
    }
    else {

        set_flags(n->name, 1);

        if (!carrier(n->name)) {

            if (n->state != S_NOCABLE) {

                stop_dhcp(n);
                forget_address(n);
                n->state = S_NOCABLE;
            }
        }
        else if (n->fixed == 1) {

            if (n->state != S_STATIC) {

                stop_dhcp(n);
                n->state = S_STATIC;
                apply(n);
            }
        }
        else if (n->state == S_OFF || n->state == S_NOCABLE) {

            start_dhcp(n);
        }
    }

    if (was != n->state)
        write_state();
}

static int is_wired(const char *name) {

    char path[96];
    int type = 0;

    snprintf(path, sizeof(path), "/sys/class/net/%.40s/type", name);

    FILE *f = fopen(path, "r");

    if (f) {

        if (fscanf(f, "%d", &type) != 1)
            type = 0;

        fclose(f);
    }

    /* Ethernet (1), not Wi-Fi, not a virtual bridge or tunnel */
    snprintf(path, sizeof(path), "/sys/class/net/%.40s/wireless", name);

    if (type != 1 || access(path, F_OK) == 0)
        return 0;

    snprintf(path, sizeof(path), "/sys/class/net/%.40s/device", name);

    return access(path, F_OK) == 0;
}

static void find_cards(void) {

    DIR *d = opendir("/sys/class/net");
    struct dirent *e;

    while (d && (e = readdir(d)) != NULL) {

        if (e->d_name[0] == '.')
            continue;

        if (!strcmp(e->d_name, "lo")) {

            set_flags("lo", 1);
            set_addr("lo", htonl(INADDR_LOOPBACK), htonl(0xFF000000));
            continue;
        }

        if (!is_wired(e->d_name))
            continue;

        int known = 0, slot = -1;

        for (int i = 0; i < MAX_IF; i++) {

            if (ifs[i].used && !strcmp(ifs[i].name, e->d_name))
                known = 1;
            else if (!ifs[i].used && slot < 0)
                slot = i;
        }

        if (known || slot < 0)
            continue;

        struct iface *n = &ifs[slot];
        struct ifreq r;

        memset(n, 0, sizeof(*n));
        n->used = 1;
        n->fd = -1;
        n->state = S_NOCABLE;
        snprintf(n->name, sizeof(n->name), "%.15s", e->d_name);
        n->index = (int)if_nametoindex(n->name);

        memset(&r, 0, sizeof(r));
        snprintf(r.ifr_name, IFNAMSIZ, "%s", n->name);

        if (ioctl(ctl, SIOCGIFHWADDR, &r) == 0)
            memcpy(n->mac, r.ifr_hwaddr.sa_data, 6);

        read_config(n);
        set_flags(n->name, 1);
        n->state = S_OFF;          /* decide() takes it from here */
        decide(n);
        write_state();
    }

    if (d)
        closedir(d);

    /* cards that were removed (USB tethering unplugged) */
    for (int i = 0; i < MAX_IF; i++) {

        char path[96];

        snprintf(path, sizeof(path), "/sys/class/net/%.40s", ifs[i].name);

        if (ifs[i].used && access(path, F_OK) != 0) {

            stop_dhcp(&ifs[i]);
            ifs[i].used = 0;
            write_resolv_conf();
            write_state();
        }
    }
}

/* ====================================================================
 * requests
 * ==================================================================== */

static int may_change(uid_t uid) {

    if (uid == 0)
        return 1;

    char who[64] = "";
    FILE *f = fopen("/run/session-user", "r");
    struct bharat_user u;

    if (f) {

        if (fgets(who, sizeof(who), f))
            who[strcspn(who, "\n")] = 0;

        fclose(f);
    }

    return who[0] && find_user(who, (uid_t)-1, &u) && u.uid == uid;
}

static struct iface *by_name(const char *name) {

    for (int i = 0; i < MAX_IF; i++)
        if (ifs[i].used && (!name[0] || !strcmp(ifs[i].name, name)))
            return &ifs[i];

    return NULL;
}

static void answer(int server) {

    int c = accept4(server, NULL, NULL, SOCK_CLOEXEC);

    if (c < 0)
        return;

    struct ucred who;
    socklen_t len = sizeof(who);
    char req[128] = "", reply[512] = "";

    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &who, &len) != 0) {

        close(c);
        return;
    }

    struct pollfd p = {.fd = c, .events = POLLIN};
    ssize_t n = poll(&p, 1, 2000) > 0 ? read(c, req, sizeof(req) - 1) : -1;

    if (n > 0) {

        char cmd[16] = "", name[IFNAMSIZ] = "";

        req[n] = 0;
        sscanf(req, "%15s %15s", cmd, name);

        struct iface *f = by_name(name);

        if (!strcmp(cmd, "status")) {

            snprintf(reply, sizeof(reply), "ok\n");
        }
        else if (!may_change(who.uid)) {

            snprintf(reply, sizeof(reply), "error only root or the person at the desktop may change the network\n");
        }
        else if (!f) {

            snprintf(reply, sizeof(reply), "error no wired network card%s%s\n", name[0] ? " called " : "", name);
        }
        else if (!strcmp(cmd, "renew")) {

            for (int i = 0; i < MAX_IF; i++) {

                struct iface *k = &ifs[i];

                if (!k->used || (name[0] && k != f))
                    continue;

                if (k->state == S_BOUND)
                    k->next = now_ms();
                else if (k->state == S_FAILED || k->state == S_DISCOVER || k->state == S_REQUEST)
                    start_dhcp(k);
            }

            snprintf(reply, sizeof(reply), "ok asking the network again\n");
        }
        else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {

            for (int i = 0; i < MAX_IF; i++) {

                struct iface *k = &ifs[i];

                if (k->used && (!name[0] || k == f)) {

                    k->user_off = !strcmp(cmd, "off");
                    decide(k);
                }
            }

            write_resolv_conf();
            snprintf(reply, sizeof(reply), "ok network %s\n", cmd);
        }
        else
            snprintf(reply, sizeof(reply), "error unknown request\n");

        if (write(c, reply, strlen(reply)) < 0) {
            /* the asker went away */
        }
    }

    close(c);
}

static int open_server(void) {

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};

    snprintf(a.sun_path, sizeof(a.sun_path), "%s", SOCKET_PATH);
    unlink(SOCKET_PATH);

    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {

        if (s >= 0)
            close(s);

        return -1;
    }

    chmod(SOCKET_PATH, 0666);

    return s;
}

/* ====================================================================
 * main loop
 * ==================================================================== */

int main(void) {

    if (getuid() != 0) {

        fprintf(stderr, "netd must run as root (init starts it)\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    srandom((unsigned)(time(NULL) ^ getpid()));

    ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    /* the kernel tells us when cards and cables come and go */
    int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK};

    if (nl >= 0 && bind(nl, (struct sockaddr *)&sa, sizeof(sa)) != 0) {

        close(nl);
        nl = -1;
    }

    int server = open_server();

    find_cards();
    write_state();

    while (1) {

        struct pollfd p[2 + MAX_IF];
        int np = 0;
        long long t = now_ms(), wait = 60 * 1000;

        p[np++] = (struct pollfd){.fd = nl, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = server, .events = POLLIN};

        for (int i = 0; i < MAX_IF; i++) {

            struct iface *n = &ifs[i];

            p[np++] = (struct pollfd){.fd = n->used ? n->fd : -1, .events = POLLIN};

            if (n->used && n->next && n->next - t < wait)
                wait = n->next - t;
        }

        if (wait < 0)
            wait = 0;

        poll(p, (nfds_t)np, (int)wait);

        if (p[0].revents & POLLIN) {

            char buf[8192];

            while (recv(nl, buf, sizeof(buf), 0) > 0)
                ;

            /* a card or a cable changed: look again */
            find_cards();

            for (int i = 0; i < MAX_IF; i++)
                if (ifs[i].used)
                    decide(&ifs[i]);
        }

        if (p[1].revents & POLLIN)
            answer(server);

        for (int i = 0; i < MAX_IF; i++) {

            if (!ifs[i].used)
                continue;

            if (p[2 + i].revents & POLLIN)
                got_packet(&ifs[i]);

            timer(&ifs[i]);
        }
    }
}
