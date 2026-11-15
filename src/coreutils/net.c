/*
 * Network commands: ip, ifconfig, ping, host, nslookup, ss, netstat, network.
 * (wget is its own program, src/net/wget.c: it needs TLS for https.)
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "coreutils.h"

static void read_line(const char *path, char *out, size_t size) {

    FILE *f = fopen(path, "r");

    out[0] = 0;

    if (f) {

        if (fgets(out, (int)size, f))
            out[strcspn(out, "\n")] = 0;

        fclose(f);
    }
}

static long long read_count(const char *dev, const char *what) {

    char path[128], text[32];

    snprintf(path, sizeof(path), "/sys/class/net/%.40s/statistics/%s", dev, what);
    read_line(path, text, sizeof(text));

    return atoll(text);
}

static int mask_bits(const struct sockaddr *m) {

    if (!m)
        return 0;

    if (m->sa_family == AF_INET)
        return __builtin_popcount(((const struct sockaddr_in *)m)->sin_addr.s_addr);

    int bits = 0;

    for (int i = 0; i < 16; i++)
        bits += __builtin_popcount(((const struct sockaddr_in6 *)m)->sin6_addr.s6_addr[i]);

    return bits;
}

static void addr_text(const struct sockaddr *a, char *out, size_t size) {

    out[0] = 0;

    if (a && a->sa_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)a)->sin_addr, out, (socklen_t)size);
    else if (a && a->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)a)->sin6_addr, out, (socklen_t)size);
}

/* the network cards, in the kernel's order */
static int list_links(char names[][IF_NAMESIZE], int max) {

    struct if_nameindex *all = if_nameindex(), *p;
    int n = 0;

    for (p = all; p && p->if_index && n < max; p++)
        snprintf(names[n++], IF_NAMESIZE, "%s", p->if_name);

    if (all)
        if_freenameindex(all);

    return n;
}

static int link_flags(const char *name) {

    struct ifreq r;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, IFNAMSIZ, "%s", name);

    int ok = s >= 0 && ioctl(s, SIOCGIFFLAGS, &r) == 0;

    if (s >= 0)
        close(s);

    return ok ? r.ifr_flags : 0;
}

static void print_link(int nr, const char *name) {

    char path[96], mac[32], mtu[16], state[16];
    int fl = link_flags(name);

    snprintf(path, sizeof(path), "/sys/class/net/%.40s/address", name);
    read_line(path, mac, sizeof(mac));
    snprintf(path, sizeof(path), "/sys/class/net/%.40s/mtu", name);
    read_line(path, mtu, sizeof(mtu));
    snprintf(path, sizeof(path), "/sys/class/net/%.40s/operstate", name);
    read_line(path, state, sizeof(state));

    for (char *c = state; *c; c++)
        *c = (char)toupper((unsigned char)*c);

    printf("%d: %s: <%s%s%s%s> mtu %s state %s\n", nr, name,
           fl & IFF_LOOPBACK ? "LOOPBACK," : "BROADCAST,MULTICAST,",
           fl & IFF_UP ? "UP" : "DOWN", fl & IFF_RUNNING ? ",LOWER_UP" : "", "",
           mtu, state);
    printf("    link/%s %s\n", fl & IFF_LOOPBACK ? "loopback" : "ether", mac);
}

static void print_addrs(const char *name, struct ifaddrs *all) {

    for (struct ifaddrs *a = all; a; a = a->ifa_next) {

        if (!a->ifa_addr || strcmp(a->ifa_name, name) ||
            (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6))
            continue;

        char t[64];

        addr_text(a->ifa_addr, t, sizeof(t));
        printf("    %s %s/%d scope %s\n", a->ifa_addr->sa_family == AF_INET ? "inet" : "inet6",
               t, mask_bits(a->ifa_netmask),
               !strcmp(name, "lo") ? "host" : (!strncmp(t, "fe80", 4) ? "link" : "global"));
    }
}

static int show_routes(void) {

    FILE *f = fopen("/proc/net/route", "r");
    char line[256];

    if (!f)
        return fail("cannot read /proc/net/route");

    if (!fgets(line, sizeof(line), f)) {

        fclose(f);
        return 0;
    }

    while (fgets(line, sizeof(line), f)) {

        char dev[32];
        unsigned dst, gw, flags, mask, metric;

        if (sscanf(line, "%31s %x %x %x %*d %*d %u %x", dev, &dst, &gw, &flags, &metric, &mask) != 6)
            continue;

        char d[16], g[16];

        inet_ntop(AF_INET, &dst, d, sizeof(d));
        inet_ntop(AF_INET, &gw, g, sizeof(g));

        if (dst == 0 && mask == 0)
            printf("default via %s dev %s metric %u\n", g, dev, metric);
        else if (flags & RTF_GATEWAY)
            printf("%s/%d via %s dev %s\n", d, __builtin_popcount(mask), g, dev);
        else
            printf("%s/%d dev %s scope link\n", d, __builtin_popcount(mask), dev);
    }

    fclose(f);

    return 0;
}

/* ip link set IF up|down, ip addr add A/N dev IF, ip route add default via G */
static int ip_change(int argc, char **argv) {

    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq r;

    memset(&r, 0, sizeof(r));

    if (argc >= 5 && !strcmp(argv[2], "set")) {

        snprintf(r.ifr_name, IFNAMSIZ, "%s", argv[3]);

        if (ioctl(s, SIOCGIFFLAGS, &r) != 0)
            return fail("%s: %s", argv[3], strerror(errno));

        if (!strcmp(argv[4], "up")) r.ifr_flags |= IFF_UP;
        else if (!strcmp(argv[4], "down")) r.ifr_flags &= ~IFF_UP;
        else return fail("use: ip link set NAME up|down");

        if (ioctl(s, SIOCSIFFLAGS, &r) != 0)
            return fail("%s (try: sudo ip ...)", strerror(errno));

        return 0;
    }

    if (argc >= 6 && !strcmp(argv[2], "add") && !strcmp(argv[4], "dev") &&
        (argv[1][0] == 'a')) {

        char addr[40];
        snprintf(addr, sizeof(addr), "%s", argv[3]);

        char *slash = strchr(addr, '/');
        int bits = slash ? atoi(slash + 1) : 32;

        if (slash)
            *slash = 0;

        struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;

        snprintf(r.ifr_name, IFNAMSIZ, "%s", argv[5]);
        sin->sin_family = AF_INET;

        if (inet_pton(AF_INET, addr, &sin->sin_addr) != 1)
            return fail("bad address %s", argv[3]);

        if (ioctl(s, SIOCSIFADDR, &r) != 0)
            return fail("%s (try: sudo ip ...)", strerror(errno));

        sin->sin_addr.s_addr = bits ? htonl(0xFFFFFFFFu << (32 - bits)) : 0;

        return ioctl(s, SIOCSIFNETMASK, &r) == 0 ? 0 : fail("%s", strerror(errno));
    }

    if (argc >= 6 && (!strcmp(argv[2], "add") || !strcmp(argv[2], "del")) &&
        !strcmp(argv[3], "default") && !strcmp(argv[4], "via")) {

        struct rtentry rt;
        struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
        struct sockaddr_in *gw = (struct sockaddr_in *)&rt.rt_gateway;
        struct sockaddr_in *mask = (struct sockaddr_in *)&rt.rt_genmask;

        memset(&rt, 0, sizeof(rt));
        dst->sin_family = gw->sin_family = mask->sin_family = AF_INET;

        if (inet_pton(AF_INET, argv[5], &gw->sin_addr) != 1)
            return fail("bad address %s", argv[5]);

        rt.rt_flags = RTF_UP | RTF_GATEWAY;

        if (argc >= 8 && !strcmp(argv[6], "dev"))
            rt.rt_dev = argv[7];

        if (ioctl(s, !strcmp(argv[2], "add") ? SIOCADDRT : SIOCDELRT, &rt) != 0)
            return fail("%s (try: sudo ip ...)", strerror(errno));

        return 0;
    }

    return fail("not understood. Examples:\n"
                "  ip link set eth0 up\n  ip addr add 192.168.1.5/24 dev eth0\n"
                "  ip route add default via 192.168.1.1");
}

COMMAND(ip) {

    const char *what = argc > 1 ? argv[1] : "";
    int is_addr = !strncmp(what, "a", 1), is_link = !strncmp(what, "l", 1), is_route = !strncmp(what, "r", 1);

    if (argc < 2 || (!is_addr && !is_link && !is_route))
        return fail("usage: ip a (addresses) | ip link (cards) | ip r (routes)\n"
                    "       ip link set NAME up|down | ip addr add A/N dev NAME | ip route add default via G");

    if (argc > 2 && strcmp(argv[2], "show") && strcmp(argv[2], "list"))
        return ip_change(argc, argv);

    if (is_route)
        return show_routes();

    char names[32][IF_NAMESIZE];
    int n = list_links(names, 32);
    struct ifaddrs *all = NULL;

    if (is_addr)
        getifaddrs(&all);

    for (int i = 0; i < n; i++) {

        if (argc > 3 && strcmp(argv[3], names[i]))
            continue;

        print_link(i + 1, names[i]);

        if (is_addr)
            print_addrs(names[i], all);
    }

    if (all)
        freeifaddrs(all);

    return 0;
}

COMMAND(ifconfig) {

    char names[32][IF_NAMESIZE];
    int n = list_links(names, 32);
    struct ifaddrs *all = NULL;

    if (argc > 2)
        return fail("to change a card use: sudo ip link set NAME up|down, sudo ip addr add ...");

    getifaddrs(&all);

    for (int i = 0; i < n; i++) {

        if (argc == 2 && strcmp(argv[1], names[i]))
            continue;

        char path[96], mac[32], mtu[16], rx[16], tx[16];
        int fl = link_flags(names[i]);

        snprintf(path, sizeof(path), "/sys/class/net/%.40s/address", names[i]);
        read_line(path, mac, sizeof(mac));
        snprintf(path, sizeof(path), "/sys/class/net/%.40s/mtu", names[i]);
        read_line(path, mtu, sizeof(mtu));

        printf("%s: flags=%d<%s%s> mtu %s\n", names[i], fl, fl & IFF_UP ? "UP" : "DOWN",
               fl & IFF_RUNNING ? ",RUNNING" : "", mtu);

        for (struct ifaddrs *a = all; a; a = a->ifa_next) {

            if (!a->ifa_addr || strcmp(a->ifa_name, names[i]))
                continue;

            char t[64], m[64];

            addr_text(a->ifa_addr, t, sizeof(t));
            addr_text(a->ifa_netmask, m, sizeof(m));

            if (a->ifa_addr->sa_family == AF_INET)
                printf("        inet %s  netmask %s\n", t, m);
            else if (a->ifa_addr->sa_family == AF_INET6)
                printf("        inet6 %s  prefixlen %d\n", t, mask_bits(a->ifa_netmask));
        }

        if (!(fl & IFF_LOOPBACK))
            printf("        ether %s\n", mac);

        human(read_count(names[i], "rx_bytes"), rx, sizeof(rx));
        human(read_count(names[i], "tx_bytes"), tx, sizeof(tx));
        printf("        RX packets %lld  bytes %s\n", read_count(names[i], "rx_packets"), rx);
        printf("        TX packets %lld  bytes %s\n\n", read_count(names[i], "tx_packets"), tx);
    }

    if (all)
        freeifaddrs(all);

    return 0;
}

/* ---------- ping ---------- */

static volatile int stop_ping;

static void on_interrupt(int sig) {

    (void)sig;
    stop_ping = 1;
}

static double ms_since(struct timespec *a) {

    struct timespec b;

    clock_gettime(CLOCK_MONOTONIC, &b);

    return (double)(b.tv_sec - a->tv_sec) * 1000.0 + (double)(b.tv_nsec - a->tv_nsec) / 1e6;
}

COMMAND(ping) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "cWq", "cW", flags, values);

    if (i != argc - 1)
        return fail("usage: ping [-c COUNT] HOST     (Ctrl-C stops)");

    int count = values['c'] ? atoi(values['c']) : 0;
    struct addrinfo hints = {.ai_family = AF_INET}, *res;
    int e = getaddrinfo(argv[i], NULL, &hints, &res);

    if (e)
        return fail("%s: %s", argv[i], e == EAI_NONAME ? "unknown name (check: network)" : gai_strerror(e));

    struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
    char ip[16];

    freeaddrinfo(res);
    inet_ntop(AF_INET, &to.sin_addr, ip, sizeof(ip));

    /* "ping sockets": no root rights needed (init allows them for everyone) */
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);

    if (s < 0)
        return fail("cannot open a ping socket: %s", strerror(errno));

    signal(SIGINT, on_interrupt);
    printf("PING %s (%s) 56 bytes of data.\n", argv[i], ip);

    int sent = 0, got = 0;
    double min = 1e9, max = 0, sum = 0;

    for (int seq = 1; !stop_ping && (!count || seq <= count); seq++) {

        unsigned char pkt[64];
        struct icmphdr *h = (struct icmphdr *)pkt;
        struct timespec start;

        memset(pkt, 0, sizeof(pkt));
        h->type = ICMP_ECHO;
        h->un.echo.sequence = htons((uint16_t)seq);
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {

            fprintf(stderr, "ping: %s\n", strerror(errno));

            if (count && seq >= count)
                break;

            sleep(1);
            continue;
        }

        sent++;

        /* wait for this answer, up to a second */
        while (!stop_ping) {

            double left = 1000 - ms_since(&start);
            struct pollfd p = {.fd = s, .events = POLLIN};

            if (left <= 0 || poll(&p, 1, (int)left) <= 0)
                break;

            unsigned char reply[256];
            ssize_t n = recv(s, reply, sizeof(reply), 0);

            if (n < (ssize_t)sizeof(struct icmphdr))
                continue;

            struct icmphdr *r = (struct icmphdr *)reply;

            if (r->type != ICMP_ECHOREPLY || ntohs(r->un.echo.sequence) != seq)
                continue;

            double t = ms_since(&start);

            got++;
            sum += t;
            if (t < min) min = t;
            if (t > max) max = t;

            printf("%zd bytes from %s: icmp_seq=%d time=%.1f ms\n", n, ip, seq, t);
            fflush(stdout);
            break;
        }

        /* one per second */
        double used = ms_since(&start);

        if (!stop_ping && (!count || seq < count) && used < 1000)
            usleep((useconds_t)((1000 - used) * 1000));
    }

    close(s);

    printf("\n--- %s ping statistics ---\n%d sent, %d received, %d%% lost\n",
           argv[i], sent, got, sent ? (sent - got) * 100 / sent : 0);

    if (got)
        printf("time min/avg/max = %.1f/%.1f/%.1f ms\n", min, sum / got, max);

    return got ? 0 : 1;
}

/* ---------- host, nslookup ---------- */

static void name_servers(char *out, size_t size) {

    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[256];

    out[0] = 0;

    while (f && fgets(line, sizeof(line), f)) {

        char ns[64];

        if (sscanf(line, "nameserver %63s", ns) == 1 && strlen(out) + strlen(ns) + 2 < size) {

            if (out[0])
                strcat(out, " ");

            strcat(out, ns);
        }
    }

    if (f)
        fclose(f);
}

static int lookup(const char *name, int nslookup_style) {

    struct in6_addr dummy;

    /* an address: find its name */
    if (inet_pton(AF_INET, name, &dummy) == 1 || inet_pton(AF_INET6, name, &dummy) == 1) {

        struct addrinfo hints = {.ai_flags = AI_NUMERICHOST}, *res;
        char host[256];

        if (getaddrinfo(name, NULL, &hints, &res) != 0)
            return fail("%s: not an address", name);

        int e = getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof(host), NULL, 0, NI_NAMEREQD);

        freeaddrinfo(res);

        if (e)
            return fail("%s has no name", name);

        printf("%s is %s\n", name, host);
        return 0;
    }

    struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_CANONNAME}, *res;
    int e = getaddrinfo(name, NULL, &hints, &res);

    if (e)
        return fail("%s: %s", name, e == EAI_NONAME ? "not found" : gai_strerror(e));

    if (nslookup_style) {

        char ns[200];

        name_servers(ns, sizeof(ns));
        printf("Server:  %s\n\nName:    %s\n", ns[0] ? ns : "(none)",
               res->ai_canonname ? res->ai_canonname : name);
    }

    for (struct addrinfo *a = res; a; a = a->ai_next) {

        char t[64];

        addr_text(a->ai_addr, t, sizeof(t));

        if (nslookup_style)
            printf("Address: %s\n", t);
        else
            printf("%s has %saddress %s\n", name, a->ai_family == AF_INET6 ? "IPv6 " : "", t);
    }

    freeaddrinfo(res);

    return 0;
}

COMMAND(host) {

    if (argc != 2)
        return fail("usage: host NAME-OR-ADDRESS");

    return lookup(argv[1], 0);
}

COMMAND(nslookup) {

    if (argc != 2)
        return fail("usage: nslookup NAME");

    return lookup(argv[1], 1);
}

/* ---------- ss, netstat ---------- */

static const char *tcp_states[] = {"?", "ESTAB", "SYN-SENT", "SYN-RECV", "FIN-WAIT-1", "FIN-WAIT-2",
                                   "TIME-WAIT", "CLOSE", "CLOSE-WAIT", "LAST-ACK", "LISTEN", "CLOSING"};

static void hex_addr(const char *hex, int v6, char *out, size_t size) {

    char ip[40];
    unsigned port = 0;
    const char *colon = strchr(hex, ':');

    if (colon)
        port = (unsigned)strtoul(colon + 1, NULL, 16);

    if (!v6) {

        unsigned a = (unsigned)strtoul(hex, NULL, 16);

        inet_ntop(AF_INET, &a, ip, sizeof(ip));
        snprintf(out, size, "%s:%u", ip, port);
    }
    else {

        struct in6_addr a;

        for (int w = 0; w < 4; w++) {

            char part[9];
            uint32_t v;

            memcpy(part, hex + 8 * w, 8);
            part[8] = 0;
            v = (uint32_t)strtoul(part, NULL, 16);
            memcpy(a.s6_addr + 4 * w, &v, 4);
        }

        inet_ntop(AF_INET6, &a, ip, sizeof(ip));
        snprintf(out, size, "[%s]:%u", ip, port);
    }
}

static int sockets(int argc, char **argv) {

    int flags[128] = {0};
    char *values[128] = {0};

    if (options(argc, argv, "tulanp", "", flags, values) < 0)
        return 1;

    int tcp = flags['t'] || !flags['u'], udp = flags['u'] || !flags['t'];

    printf("%-5s %-11s %-28s %s\n", "Proto", "State", "Local", "Peer");

    const char *files[] = {"/proc/net/tcp", "/proc/net/tcp6", "/proc/net/udp", "/proc/net/udp6"};

    for (int k = 0; k < 4; k++) {

        int is_tcp = k < 2;

        if ((is_tcp && !tcp) || (!is_tcp && !udp))
            continue;

        FILE *f = fopen(files[k], "r");
        char line[512];

        if (!f || !fgets(line, sizeof(line), f)) {

            if (f)
                fclose(f);

            continue;
        }

        while (fgets(line, sizeof(line), f)) {

            char local[64], peer[64], la[80], pa[80];
            unsigned st;

            if (sscanf(line, "%*d: %63s %63s %x", local, peer, &st) != 3)
                continue;

            int listening = is_tcp ? st == 10 : st == 7;

            /* default: connections; -l: listening; -a: both */
            if (!flags['a'] && (flags['l'] ? !listening : listening))
                continue;

            hex_addr(local, k % 2, la, sizeof(la));
            hex_addr(peer, k % 2, pa, sizeof(pa));
            printf("%-5s %-11s %-28s %s\n", is_tcp ? (k ? "tcp6" : "tcp") : (k == 3 ? "udp6" : "udp"),
                   is_tcp ? (st < 12 ? tcp_states[st] : "?") : (listening ? "UNCONN" : "ESTAB"), la, pa);
        }

        fclose(f);
    }

    return 0;
}

COMMAND(ss) { return sockets(argc, argv); }
COMMAND(netstat) { return sockets(argc, argv); }

/* ---------- network ---------- */

static int ask_netd(const char *request) {

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    char reply[512];

    snprintf(a.sun_path, sizeof(a.sun_path), "/run/netd.sock");

    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) != 0)
        return fail("the network service (netd) is not running");

    if (write(s, request, strlen(request)) < 0)
        return fail("cannot talk to netd");

    ssize_t n = read(s, reply, sizeof(reply) - 1);

    close(s);

    if (n <= 0)
        return fail("no answer from netd");

    reply[n] = 0;

    if (!strncmp(reply, "ok", 2)) {

        printf("%s", reply[2] ? reply + 3 : "\n");
        return 0;
    }

    fprintf(stderr, "network: %s", !strncmp(reply, "error ", 6) ? reply + 6 : reply);
    return 1;
}

COMMAND(network) {

    if (argc >= 2 && strcmp(argv[1], "status")) {

        char req[64];

        if (strcmp(argv[1], "renew") && strcmp(argv[1], "on") && strcmp(argv[1], "off"))
            return fail("usage: network [status] | network renew|on|off [CARD]");

        snprintf(req, sizeof(req), "%s %s\n", argv[1], argc > 2 ? argv[2] : "");

        return ask_netd(req);
    }

    FILE *f = fopen("/run/network/state", "r");
    char line[256];
    int any = 0;

    while (f && fgets(line, sizeof(line), f)) {

        char name[32], state[32], addr[40], gw[20], dns[20], how[16];

        if (sscanf(line, "%31s %31s %39s %19s %19s %15s", name, state, addr, gw, dns, how) != 6)
            continue;

        any = 1;
        printf("%s: %s", name, state);

        if (!strcmp(state, "connected"))
            printf(" (%s)\n  address  %s\n  gateway  %s\n  DNS      %s\n", how, addr, gw, dns);
        else if (!strcmp(state, "no-cable"))
            printf(" - plug in a network cable\n");
        else if (!strcmp(state, "no-answer"))
            printf(" - nobody gave an address (DHCP); trying again every minute\n");
        else
            printf("\n");
    }

    if (f)
        fclose(f);

    if (!any)
        printf("No wired network card found. (Wi-Fi is not supported yet.)\n");

    return 0;
}
