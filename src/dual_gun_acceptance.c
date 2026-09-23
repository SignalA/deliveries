#define _GNU_SOURCE
#include "evdev_input.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { unsigned down, up, repeat, coords, clicks; int held, errors; } Counts;
static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void count_event(Counts *c, const struct input_event *e)
{
    if (e->type == EV_SYN && e->code == SYN_DROPPED) c->errors++;
    if (e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_Y)) c->coords++;
    if (e->type != EV_KEY || e->code != BTN_TOUCH) return;
    if (e->value == 2) { c->repeat++; return; }
    if (e->value == 1) { c->down++; if (c->held) c->errors++; c->held = 1; }
    else if (e->value == 0) { c->up++; if (!c->held) c->errors++; c->held = 0; }
    else c->errors++;
}
static void capture(void *ctx, int gun, int x, int y)
{
    Counts *c = ctx;
    (void)x; (void)y;
    c[gun - 1].clicks++;
}
static int valid(const Counts *c, unsigned expected)
{
    return !c->errors && !c->held && c->down == expected && c->up == expected && c->clicks == expected;
}
static const char *selector(const GunInput *g)
{
    return g->physical[0] ? g->physical : g->unique;
}
/* A bounded read per gun prevents a moving gun from starving the other one. */
static int pump(GunInput g[2], Counts c[2], int allowed_disconnect)
{
    struct pollfd fds[2] = {{g[0].fd, POLLIN, 0}, {g[1].fd, POLLIN, 0}};
    if (poll(fds, 2, 20) < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < 2; ++i) {
        int lost = !!(fds[i].revents & (POLLERR | POLLHUP | POLLNVAL));
        if (!lost && (fds[i].revents & POLLIN)) for (int n = 0; n < 32; ++n) {
            struct input_event events[256];
            ssize_t size = read(g[i].fd, events, sizeof(events));
            if (size < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (size <= 0 || size % sizeof(events[0])) { lost = 1; break; }
            for (ssize_t j = 0; j < size / (ssize_t)sizeof(events[0]); ++j) {
                count_event(&c[i], &events[j]);
                gun_event(&g[i], &events[j], capture, c);
                if (g[i].fd < 0) { lost = 1; break; }
            }
            if (lost) break;
        }
        if (lost) {
            printf("DISCONNECTED gun=%c selector=%s held=%d\n", 'A'+i, selector(&g[i]), c[i].held);
            gun_close(&g[i]);
            if (i != allowed_disconnect || c[i].held || c[i].errors) return -1;
        }
    }
    return 0;
}
static int window(GunInput g[2], int seconds, unsigned a, unsigned b, const char *phase)
{
    Counts c[2] = {{0}};
    printf("PHASE=%s seconds=%d expected_A=%u expected_B=%u GO (BTN_TOUCH press/release)\n", phase, seconds, a, b);
    long long end = now_ms() + (long long)seconds * 1000;
    long long retry = 0;
    int ok = 1;
    while (now_ms() < end) {
        if (pump(g, c, -1)) { ok = 0; break; }
        if (now_ms() >= retry) {
            retry = now_ms() + 250;
            for (int i=0;i<2;++i) if(g[i].fd < 0) {
                if (!gun_open(&g[i]) || errno == EEXIST) {
                    puts("FAIL: gun reconnected before RECONNECT prompt"); ok=0;
                }
            }
            if (!ok) break;
        }
    }
    for (int i = 0; i < 2; ++i) {
        printf("SUMMARY phase=%s gun=%c selector=%s down=%u up=%u repeat=%u coords=%u clicks=%u held=%d errors=%d\n",
               phase, 'A'+i, selector(&g[i]), c[i].down, c[i].up, c[i].repeat, c[i].coords, c[i].clicks, c[i].held, c[i].errors);
        if (!valid(&c[i], i ? b : a)) ok = 0;
    }
    printf("PHASE_RESULT phase=%s result=%s\n", phase, ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}
static int transition(GunInput g[2], int target, int reconnect, int timeout)
{
    Counts c[2] = {{0}};
    printf("ACTION=%s gun=%c selector=%s timeout=%ds DO_NOT_FIRE\n", reconnect ? "RECONNECT" : "UNPLUG", 'A'+target, selector(&g[target]), timeout);
    long long end = now_ms() + (long long)timeout * 1000, retry = 0;
    while (now_ms() < end) {
        if (pump(g, c, reconnect ? -1 : target)) return -1;
        if (!valid(&c[0], 0) || !valid(&c[1], 0)) return -1;
        if (!reconnect && g[target].fd < 0) return 0;
        if (reconnect && now_ms() >= retry) {
            retry = now_ms() + 250;
            if (!gun_open(&g[target])) {
                if (g[target].buttons) return -1;
                printf("RECONNECTED gun=%c selector=%s path=%s\n", 'A'+target, selector(&g[target]), g[target].path);
                return 0;
            }
            if (errno == EEXIST) { puts("AMBIGUOUS_SELECTOR"); return -1; }
        }
    }
    puts("TRANSITION_TIMEOUT");
    return -1;
}
static int positive(const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);
    return *s && !*end && n > 0 && n <= 3600 ? (int)n : 0;
}
int main(int argc, char **argv)
{
    if (argc != 7 || !argv[1][0] || !argv[2][0] || !strcmp(argv[1], argv[2]) ||
        strlen(argv[1]) >= 128 || strlen(argv[2]) >= 128 ||
        (strcmp(argv[3], "continuous") && strcmp(argv[3], "unplug-a") && strcmp(argv[3], "unplug-b")) ||
        !positive(argv[4]) || !positive(argv[5]) || !positive(argv[6])) {
        fprintf(stderr, "usage: dual_gun_acceptance uniq:ID|phys:BASE uniq:ID|phys:BASE continuous|unplug-a|unplug-b COUNT SECONDS TRANSITION_TIMEOUT\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    GunInput g[2] = {{.fd=-1, .gun=1, .width=1920, .height=1080}, {.fd=-1, .gun=2, .width=1920, .height=1080}};
    int ok = 0, count = positive(argv[4]), seconds = positive(argv[5]), timeout = positive(argv[6]);
    for (int i = 0; i < 2; ++i) {
        const char *value = argv[i+1];
        if (!strncmp(value, "phys:", 5)) strcpy(g[i].physical, value + 5);
        else strcpy(g[i].unique, !strncmp(value, "uniq:", 5) ? value + 5 : value);
        if ((!g[i].unique[0] && !g[i].physical[0]) ||
            (g[i].physical[0] && i && !strcmp(g[0].physical, g[i].physical))) goto done;
        if (gun_open(&g[i]) || g[i].buttons) {
            fprintf(stderr, "BIND_FAIL gun=%c selector=%s errno=%d (require one capable node and released trigger)\n", 'A'+i, selector(&g[i]), errno);
            goto done;
        }
    }
    if (window(g, seconds, count, count, "continuous")) goto done;
    if (strcmp(argv[3], "continuous")) {
        int target = !strcmp(argv[3], "unplug-a") ? 0 : 1;
        if (transition(g, target, 0, timeout)) goto done;
        if (window(g, seconds, target == 0 ? 0 : count, target == 1 ? 0 : count, "survivor")) goto done;
        if (transition(g, target, 1, timeout)) goto done;
        if (window(g, seconds, count, count, "restored")) goto done;
    }
    ok = 1;
done:
    for (int i = 0; i < 2; ++i) gun_close(&g[i]);
    printf("DUAL_GUN_ACCEPTANCE=%s scenario=%s\n", ok ? "PASS" : "FAIL", argv[3]);
    return ok ? 0 : 1;
}
