#define _GNU_SOURCE
#include "evdev_input.h"
#include "calibration_hid.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define MAX_MESSAGE 4096
#define QUEUE_SIZE 32
#define OUT_SIZE 192
typedef struct {
    int used, role, version, complete, sample, calibration_player, calibration_gun;
    struct sockaddr_un address;
    socklen_t address_len;
    dev_t dev;
    ino_t ino;
    char queue[QUEUE_SIZE][OUT_SIZE];
    int head, count;
    uint64_t next_sample, last_pong;
    char saved_result[2048], calibration_status[32];
    unsigned held[2];
} Client;
typedef struct {
    int fd, mock, width, height, calibration_gun, player[2], delay, first_delay;
    int move_x[2], move_y[2];
    unsigned char move_dirty[2];
    uint64_t move_next[2];
    char socket_path[108], result_dir[512];
    Client clients[MAX_CLIENTS];
    GunInput guns[2];
    GunKeyboard keyboards[2];
    unsigned char trigger_sources[2];
    unsigned char disconnect_reset_sent[2];
} Service;
static volatile sig_atomic_t running = 1;
static void stop(int signal_number) { (void)signal_number; running = 0; }
static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static int number(const char *s, int low, int high, int *out)
{
    char *end;
    long n;
    if (!s || !*s) return 0;
    errno = 0; n = strtol(s, &end, 10);
    if (errno || *end || n < low || n > high) return 0;
    *out = (int)n;
    return 1;
}
static int tokenize(char *line, char **tokens, int limit)
{
    char *save = NULL, *p = strtok_r(line, " \t\r\n", &save);
    int n = 0;
    while (p) {
        if (n == limit) return -1;
        tokens[n++] = p;
        p = strtok_r(NULL, " \t\r\n", &save);
    }
    return n;
}
static void drop(Client *c)
{
    if (c->used) printf("CLIENT_REMOVED path=%s\n", c->address.sun_path);
    memset(c, 0, sizeof(*c));
}
static int alive(Client *c)
{
    struct stat st;
    return lstat(c->address.sun_path, &st) == 0 && S_ISSOCK(st.st_mode) &&
           st.st_dev == c->dev && st.st_ino == c->ino;
}
static void flush(Service *s, Client *c)
{
    for (int i = 0; c->used && c->count && i < QUEUE_SIZE; ++i) {
        const char *line = c->queue[c->head];
        ssize_t n = sendto(s->fd, line, strlen(line), MSG_NOSIGNAL,
                           (struct sockaddr *)&c->address, c->address_len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 || (size_t)n != strlen(line)) { drop(c); break; }
        c->head = (c->head + 1) % QUEUE_SIZE;
        c->count--;
    }
}
static void reply(Service *s, Client *c, const char *line)
{
    if (!c->used) return;
    if (c->count == QUEUE_SIZE || strlen(line) + 2 > OUT_SIZE) {
        fprintf(stderr, "CLIENT_QUEUE_OVERFLOW: disconnecting slow client\n");
        drop(c); return;
    }
    snprintf(c->queue[(c->head + c->count) % QUEUE_SIZE], OUT_SIZE, "%s\n", line);
    c->count++;
    flush(s, c);
}
static void direct_reply(Service *s, const struct sockaddr_un *a, socklen_t len, const char *line)
{
    (void)sendto(s->fd, line, strlen(line), MSG_NOSIGNAL, (const struct sockaddr *)a, len);
}
static int find_client(Service *s, const struct sockaddr_un *a)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (s->clients[i].used && !strcmp(s->clients[i].address.sun_path, a->sun_path)) return i;
    return -1;
}
static int calibration_active(Service *s)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (s->clients[i].used && s->clients[i].role == 2 && !s->clients[i].complete) return 1;
    return 0;
}
static int gun_for_player(Service *s, int player)
{
    for (int gun = 0; gun < 2; ++gun) if (s->player[gun] == player) return gun + 1;
    return 0;
}
static void click(void *ctx, int gun, int x, int y)
{
    Service *s = ctx;
    int calibrating = calibration_active(s);
    char line[128];
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        Client *c = &s->clients[i];
        if (!c->used || c->complete) continue;
        if (calibrating) {
            if (c->role != 2) continue;
            int player = s->player[gun - 1];
            if (!c->calibration_gun) { c->calibration_gun = gun; c->calibration_player = player; }
            if (gun != c->calibration_gun) continue;
            snprintf(line, sizeof(line), "GUN_DOWN %d %d %d", player, x, y);
        } else {
            if (c->role != 1) continue;
            snprintf(line, sizeof(line), "GUN_DOWN %d %d %d", s->player[gun - 1], x, y);
            c->held[gun-1] |= 1u << GUN_TRIGGER;
        }
        reply(s, c, line);
    }
}
static void motion(void *ctx, int gun, int x, int y)
{
    Service *s=ctx;
    if(calibration_active(s)) return;
    int game=0;
    for(int i=0;i<MAX_CLIENTS;++i)
        if(s->clients[i].used && !s->clients[i].complete && s->clients[i].role==1) { game=1; break; }
    if(!game) return;
    s->move_x[gun-1]=x; s->move_y[gun-1]=y; s->move_dirty[gun-1]=1;
}
static void flush_motion(Service *s, uint64_t now)
{
    if(calibration_active(s)) { s->move_dirty[0]=s->move_dirty[1]=0; return; }
    for(int gun=0;gun<2;++gun) if(s->move_dirty[gun] && now>=s->move_next[gun]) {
        char line[128];
        snprintf(line,sizeof(line),"GUN_MOVE %d %d %d",s->player[gun],s->move_x[gun],s->move_y[gun]);
        for(int i=0;i<MAX_CLIENTS;++i) {
            Client *c=&s->clients[i];
            if(c->used && !c->complete && c->role==1) reply(s,c,line);
        }
        s->move_dirty[gun]=0;
        s->move_next[gun]=now+16; /* Cap each gun near 60 Hz. */
    }
}
static void button(void *ctx, int gun, int key, int value, int x, int y)
{
    Service *s=ctx;
    if(key==GUN_RESET) s->trigger_sources[gun-1]=0;
    if(key==GUN_TRIGGER && value==1) { click(ctx,gun,x,y); return; }
    if(calibration_active(s)) {
        /* Forward resets and ordinary button edges to calibration clients.
           Trigger presses are GUN_DOWN; releases are not button messages. */
        if(key!=GUN_RESET && (key<GUN_A || key>GUN_PAUSE)) return;
        for(int i=0;i<MAX_CLIENTS;++i) {
            Client *c=&s->clients[i];
            if(!c->used || c->complete || c->role!=2) continue;
            char line[128];
            if(key==GUN_RESET) {
                c->held[gun-1]=0;
                snprintf(line,sizeof(line),"GUN_RESET %d",s->player[gun-1]);
            } else {
                unsigned bit=1u<<key;
                if(!value && !(c->held[gun-1]&bit)) continue;
                if(value) c->held[gun-1]|=bit; else c->held[gun-1]&=~bit;
                snprintf(line,sizeof(line),"GUN_BUTTON %d %s %s",s->player[gun-1],gun_button_name(key),value?"DOWN":"UP");
            }
            reply(s,c,line);
        }
        return;
    }
    for(int i=0;i<MAX_CLIENTS;++i) {
        Client *c=&s->clients[i];
        if(!c->used || c->role!=1) continue;
        char line[128];
        if(key==GUN_RESET) {
            c->held[gun-1]=0;
            snprintf(line,sizeof(line),"GUN_RESET %d",s->player[gun-1]);
        } else {
            unsigned bit=1u<<key;
            /* A newly joined client must not receive an orphan release. */
            if(!value && !(c->held[gun-1]&bit)) continue;
            if(value) c->held[gun-1]|=bit; else c->held[gun-1]&=~bit;
            if(key==GUN_TRIGGER) snprintf(line,sizeof(line),"GUN_UP %d %d %d",s->player[gun-1],x,y);
            else snprintf(line,sizeof(line),"GUN_BUTTON %d %s %s",s->player[gun-1],gun_button_name(key),value?"DOWN":"UP");
        }
        reply(s,c,line);
    }
}
static void reset_game_clients(Service *s)
{
    s->trigger_sources[0]=s->trigger_sources[1]=0;
    for(int gun=0;gun<2;++gun) for(int i=0;i<MAX_CLIENTS;++i) {
        Client *c=&s->clients[i];
        if(!c->used || c->role!=1) continue;
        c->held[gun]=0;
        char line[64];
        snprintf(line,sizeof(line),"GUN_RESET %d",s->player[gun]);
        reply(s,c,line);
    }
}
static void source_button(Service *s, int gun, int key, int value, int x, int y,
                          unsigned char source)
{
    if (key == GUN_RESET) {
        s->trigger_sources[gun - 1] = 0;
        button(s, gun, key, value, x, y);
        return;
    }
    if (key != GUN_TRIGGER) {
        button(s, gun, key, value, x, y);
        return;
    }
    unsigned char before = s->trigger_sources[gun - 1];
    if (value) s->trigger_sources[gun - 1] |= source;
    else s->trigger_sources[gun - 1] &= (unsigned char)~source;
    if (!!before != !!s->trigger_sources[gun - 1])
        button(s, gun, GUN_TRIGGER, value, x, y);
}
static void aim_button(void *ctx, int gun, int key, int value, int x, int y)
{
    Service *s = ctx;
    if(key==GUN_TRIGGER && calibration_active(s)) {
        GunInput *input=&s->guns[gun-1];
        if(input->absolute) { x=input->raw_x; y=input->raw_y; }
        else {
            x=(int)((int64_t)x*32767/(input->width>1?input->width-1:1));
            y=(int)((int64_t)y*32767/(input->height>1?input->height-1:1));
        }
    }
    source_button(s, gun, key, value, x, y, 1);
}
static void keyboard_input_button(void *ctx, int gun, int key, int value, int x, int y)
{
    Service *s = ctx;
    (void)x; (void)y;
    source_button(s, gun, key, value, s->guns[gun - 1].x, s->guns[gun - 1].y, 2);
}
static int save_result(Service *s, Client *c, char **tokens, int n)
{
    int player, count, coords[48], targets[16], gun, gun_field=-1;
    int explicit_gun=n==68, has_targets=n==67 || explicit_gun, token=explicit_gun?4:3;
    if(n!=51 && n!=67 && n!=68)return -1;
    if(explicit_gun) {
        /* The current calibration application counts every coordinate pair in
           the payload: 24 measured shots plus 8 target positions = 32.  Some
           earlier builds kept count=24 after appending the targets, so accept
           both encodings while deriving the layout from the field count. */
        if(!number(tokens[1],0,1,&gun_field) ||
           !number(tokens[2],0,3,&player) ||
           !number(tokens[3],has_targets?24:24,has_targets?32:24,&count) ||
           (has_targets && count!=24 && count!=32))return -1;
        gun=gun_field+1;
        if(!c->calibration_gun) c->calibration_gun=gun;
        if(gun!=c->calibration_gun)return -1;
    } else {
        if(!number(tokens[1],1,4,&player) || !number(tokens[2],24,24,&count))return -1;
        if (c->version == 1) {
            if (player != s->calibration_gun) return -1;
            gun = s->calibration_gun;
        } else {
            if (!c->calibration_player || player != c->calibration_player) return -1;
            gun = gun_for_player(s, player);
            if (!gun) return -1;
        }
    }
    if(has_targets) {
        for(int point=0;point<8;++point) {
            for(int sample=0;sample<6;++sample)
                if(!number(tokens[token++],0,32767,&coords[point*6+sample])) return -1;
            for(int axis=0;axis<2;++axis)
                if(!number(tokens[token++],0,32767,&targets[point*2+axis])) return -1;
        }
    } else for (int i = 0; i < 48; ++i)
        if (!number(tokens[token + i], 0, (i % 2 ? s->height : s->width) - 1, &coords[i])) return -1;
    char canonical[2048];
    size_t off = explicit_gun ?
        (size_t)snprintf(canonical,sizeof(canonical),"CALIB_RESULT %d %d %d",gun_field,player,count) :
        (size_t)snprintf(canonical,sizeof(canonical),"CALIB_RESULT %d 24",player);
    for(int point=0;point<8;++point) {
        for(int sample=0;sample<6;++sample)
            off+=(size_t)snprintf(canonical+off,sizeof(canonical)-off," %d",coords[point*6+sample]);
        if(has_targets) for(int axis=0;axis<2;++axis)
            off+=(size_t)snprintf(canonical+off,sizeof(canonical)-off," %d",targets[point*2+axis]);
    }
    if (c->complete) return strcmp(canonical, c->saved_result) == 0 ? 0 : -1;

    char hidraw[512]={0};
    int transmit=CALIB_HID_OK;
    int profile=explicit_gun?player:player-1;
    if(!s->mock) transmit=calib_transmit(s->guns[gun-1].physical,s->guns[gun-1].unique,profile,coords,
                                         has_targets?targets:NULL,hidraw,sizeof(hidraw));
    if(transmit!=CALIB_HID_OK) return transmit==CALIB_HID_DEVICE?-3:transmit==CALIB_HID_WRITE?-4:
                                      transmit==CALIB_HID_NO_RESPONSE?-5:-6;

    char path[640];
    snprintf(path, sizeof(path), "%s/capture-gun%d-XXXXXX", s->result_dir, gun);
    int fd = mkstemp(path);
    if (fd < 0) return -2;
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(path); return -2; }
    const char *status=s->mock?"CAPTURE_ONLY":"APPLIED";
    int failed = fprintf(file, "status=%s\nmode=%s\nprotocol=%d\ngun=%d\nplayer_field=%d\ntargets=%s\nhidraw=%s\nscreen=%dx%d\n%s\n",
                         status,
                         s->mock ? "mock" : "evdev", c->version, gun, player,
                         has_targets?"CLIENT":"BUILTIN",hidraw,s->width, s->height, canonical) < 0;
    if (fflush(file) != 0) failed = 1;
    if (!failed && fsync(fd) != 0) failed = 1;
    if (fclose(file) != 0) failed = 1;
    if (failed) { unlink(path); return -2; }
    strcpy(c->saved_result, canonical);
    strcpy(c->calibration_status,status);
    c->complete = 1;
    printf("CALIBRATION_STORED file=%s count=24 status=%s hidraw=%s\n",path,status,hidraw);
    return 0;
}
static void process(Service *s, char *line, struct sockaddr_un *from, socklen_t len)
{
    char *tokens[80];
    int n = tokenize(line, tokens, 80);
    if (n <= 0) { direct_reply(s, from, len, "ERROR BAD_MESSAGE\n"); return; }
    int idx = find_client(s, from);
    /* A datagram already queued by a known, credential-checked client remains
       valid even if that client unlinks its pathname immediately after send.
       Liveness is checked by the periodic cleanup path instead. */
    if (!strcmp(tokens[0], "HELLO")) {
        int version, role = n >= 2 ? (!strcmp(tokens[1], "game") ? 1 : !strcmp(tokens[1], "calibration") ? 2 : 0) : 0;
        if (n != 3 || !role || !number(tokens[2], 1, 2, &version) || (role == 1 && version != 1)) {
            direct_reply(s, from, len, "ERROR UNSUPPORTED_HELLO\n"); return;
        }
        if (idx >= 0 && (s->clients[idx].role != role || s->clients[idx].version != version)) {
            reply(s, &s->clients[idx], "ERROR ROLE_CHANGE_REQUIRES_NEW_PATH"); return;
        }
        if (idx < 0) {
            if (role == 2 && calibration_active(s)) {
                direct_reply(s, from, len, "ERROR CALIBRATION_BUSY\n"); return;
            }
            for (int i = 0; i < MAX_CLIENTS; ++i) if (!s->clients[i].used) { idx = i; break; }
            if (idx < 0) { direct_reply(s, from, len, "ERROR CLIENT_LIMIT\n"); return; }
            struct stat st;
            if (lstat(from->sun_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) return;
            Client *c = &s->clients[idx];
            c->used = 1; c->role = role; c->version = version; c->address = *from; c->address_len = len;
            c->dev = st.st_dev; c->ino = st.st_ino;
            c->next_sample = now_ms() + s->first_delay;
            c->last_pong = now_ms();
            printf("CLIENT_HELLO role=%s path=%s\n", tokens[1], from->sun_path);
        }
        char hello[32];
        snprintf(hello, sizeof(hello), "HELLO_OK %d", version);
        reply(s, &s->clients[idx], hello);
        return;
    }
    if (idx < 0) { direct_reply(s, from, len, "ERROR HELLO_REQUIRED\n"); return; }
    Client *c = &s->clients[idx];
    if (!strcmp(tokens[0], "CALIB_RESULT")) {
        printf("CALIB_RESULT_RECEIVED fields=%d version=%d path=%s\n", n, c->version, c->address.sun_path);
        if (n >= 4)
            printf("CALIB_RESULT_HEADER gun_or_player=%s player_or_count=%s count_or_x=%s session_gun=%d session_player=%d\n",
                   tokens[1], tokens[2], tokens[3], c->calibration_gun, c->calibration_player);
        if (c->role != 2) { reply(s, c, "ERROR CALIBRATION_ROLE_REQUIRED"); return; }
        int r = save_result(s, c, tokens, n);
        printf("CALIB_RESULT_STATUS result=%d\n", r);
        if(r==0) {
            char result[64]; snprintf(result,sizeof(result),"CALIB_STORED 1 %s",c->calibration_status); reply(s,c,result);
        } else reply(s,c,r==-1?"ERROR BAD_CALIB_RESULT":r==-2?"ERROR SAVE_FAILED":
                         r==-3?"ERROR CALIB_DEVICE_NOT_FOUND":r==-4?"ERROR CALIB_WRITE_FAILED":
                         r==-5?"ERROR CALIB_NO_RESPONSE":"ERROR CALIB_DEVICE_REJECTED");
    } else if (!strcmp(tokens[0], "SET_PROFILE")) {
        int gun, profile;
        if (c->role != 1) { reply(s,c,"ERROR GAME_ROLE_REQUIRED"); return; }
        if (n != 3 || !number(tokens[1],0,1,&gun) || !number(tokens[2],0,3,&profile)) {
            reply(s,c,"ERROR BAD_SET_PROFILE"); return;
        }
        if (calibration_active(s)) { reply(s,c,"ERROR CALIBRATION_BUSY"); return; }
        char path[512]={0};
        int result=s->mock ? CALIB_HID_OK :
            calib_select_profile(s->guns[gun].physical,s->guns[gun].unique,profile,path,sizeof(path));
        if(result==CALIB_HID_OK) {
            char response[64];
            snprintf(response,sizeof(response),"PROFILE_SET %d %d",gun,profile);
            reply(s,c,response);
            printf("PROFILE_SET gun=%d profile=%d hidraw=%s\n",gun,profile,path);
        } else reply(s,c,result==CALIB_HID_DEVICE?"ERROR PROFILE_DEVICE_NOT_FOUND":
                       result==CALIB_HID_WRITE?"ERROR PROFILE_WRITE_FAILED":
                       result==CALIB_HID_NO_RESPONSE?"ERROR PROFILE_NO_RESPONSE":
                       "ERROR PROFILE_DEVICE_REJECTED");
    } else if (n == 2 && !strcmp(tokens[0], "PING") && !strcmp(tokens[1], "1")) {
        reply(s, c, "PONG 1");
    } else if (n == 2 && !strcmp(tokens[0], "PONG") && !strcmp(tokens[1], "1")) {
        c->last_pong = now_ms();
    } else if (n == 1 && !strcmp(tokens[0], "BYE")) {
        drop(c);
    } else reply(s, c, "ERROR UNKNOWN_MESSAGE");
}
static void receive_messages(Service *s)
{
    for (int i = 0; i < 32; ++i) {
        char buffer[MAX_MESSAGE + 1];
        struct sockaddr_un from = {0};
        struct iovec io = {buffer, MAX_MESSAGE};
        struct msghdr message = {0};
        char control[CMSG_SPACE(sizeof(struct ucred))];
        message.msg_name = &from; message.msg_namelen = sizeof(from);
        message.msg_iov = &io; message.msg_iovlen = 1;
        message.msg_control = control; message.msg_controllen = sizeof(control);
        ssize_t n = recvmsg(s->fd, &message, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("recvmsg");
            break;
        }
        if (message.msg_namelen <= offsetof(struct sockaddr_un, sun_path) ||
            message.msg_namelen > sizeof(from) || from.sun_family != AF_UNIX || !from.sun_path[0] ||
            !memchr(from.sun_path, 0, sizeof(from.sun_path))) continue;
        int trusted = 0;
        for (struct cmsghdr *h = CMSG_FIRSTHDR(&message); h; h = CMSG_NXTHDR(&message, h)) {
            if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_CREDENTIALS && h->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
                struct ucred cred;
                memcpy(&cred, CMSG_DATA(h), sizeof(cred));
                trusted = cred.uid == geteuid();
            }
        }
        if (!trusted || (message.msg_flags & MSG_CTRUNC)) continue;
        if ((message.msg_flags & MSG_TRUNC) || n <= 0 || memchr(buffer, '\0', (size_t)n)) {
            direct_reply(s, &from, message.msg_namelen, "ERROR BAD_MESSAGE\n"); continue;
        }
        buffer[n] = '\0';
        process(s, buffer, &from, message.msg_namelen);
    }
}
static void mock_inputs(Service *s, uint64_t now)
{
    int calibrating = calibration_active(s);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        Client *c = &s->clients[i];
        if (!c->used || c->complete || c->count || now < c->next_sample ||
            (calibrating && c->role != 2) || (c->role == 2 && c->sample >= 24)) continue;
        int point = (c->sample / 3) % 8;
        const int grid_x[] = {1,5,9,1,9,1,5,9}, grid_y[] = {1,1,1,5,5,9,9,9};
        int x = (s->width - 1) * grid_x[point] / 10;
        int y = (s->height - 1) * grid_y[point] / 10;
        char line[128];
        if (c->role == 2) {
            if (!c->calibration_gun) { c->calibration_gun=1; c->calibration_player = s->player[0]; }
            snprintf(line, sizeof(line), "GUN_DOWN %d %d %d", c->calibration_player, x, y);
        }
        else snprintf(line, sizeof(line), "GUN_DOWN %d %d %d", s->player[c->sample % 2], x, y);
        reply(s, c, line);
        if (c->used) { c->sample = (c->sample + 1) % 240000; c->next_sample = now + s->delay; }
    }
}
static void usage(void)
{
    puts("gaime_input_service --socket PATH --result-dir DIR --mode mock|evdev\n"
         "  [--screen WIDTHxHEIGHT] [--calibration-gun 1|2]\n"
         "  [--device1 /dev/input/eventX] [--device2 /dev/input/eventY]\n"
         "  [--uniq1 ID] [--uniq2 ID] (exclusive with device path for that gun)\n"
         "  [--phys1 EVIOCGPHYS_BASE] [--phys2 EVIOCGPHYS_BASE]\n"
         "  [--player1 1|3] [--player2 2|4] [--delay-ms N] [--first-delay-ms N]\n"
         "Calibration protocol 2 writes 8 packets to the gun custom hidraw interface.");
}
int main(int argc, char **argv)
{
    Service s = {.fd=-1, .mock=1, .width=32768, .height=32768, .calibration_gun=1,
                 .player={1,2}, .delay=1200, .first_delay=2500};
    struct stat result_stat, socket_stat;
    int mode_set = 0;
    for (int i = 0; i < 2; ++i) {
        s.guns[i].fd = -1; s.guns[i].gun = i + 1;
        s.guns[i].callback=aim_button; s.guns[i].context=&s;
        s.guns[i].move_callback=motion;
        s.keyboards[i].fd=-1; s.keyboards[i].gun=i+1;
        s.keyboards[i].callback=keyboard_input_button; s.keyboards[i].context=&s;
    }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        if (i + 1 >= argc) { usage(); return 2; }
        const char *key = argv[i++], *value = argv[i];
        if (!strcmp(key, "--socket") && strlen(value) < sizeof(s.socket_path) && value[0] == '/') strcpy(s.socket_path, value);
        else if (!strcmp(key, "--result-dir") && strlen(value) < sizeof(s.result_dir) && value[0] == '/') strcpy(s.result_dir, value);
        else if (!strcmp(key, "--mode") && (!strcmp(value,"mock") || !strcmp(value,"evdev"))) { s.mock = !strcmp(value,"mock"); mode_set = 1; }
        else if (!strcmp(key, "--screen")) {
            char temp[40], *sep;
            if (strlen(value) >= sizeof(temp)) return 2;
            strcpy(temp, value); sep = strchr(temp, 'x');
            if (!sep) return 2;
            *sep++ = 0;
            if (!number(temp, 32, 32768, &s.width) || !number(sep, 32, 32768, &s.height)) return 2;
        } else if (!strcmp(key, "--calibration-gun") && number(value,1,2,&s.calibration_gun)) {}
        else if (!strcmp(key, "--delay-ms") && number(value,1,60000,&s.delay)) {}
        else if (!strcmp(key, "--first-delay-ms") && number(value,1,60000,&s.first_delay)) {}
        else if (!strcmp(key, "--player1") && number(value,1,3,&s.player[0]) && s.player[0] != 2) {}
        else if (!strcmp(key, "--player2") && number(value,2,4,&s.player[1]) && s.player[1] != 3) {}
        else if ((!strcmp(key,"--device1") || !strcmp(key,"--device2")) && strlen(value) < sizeof(s.guns[0].path) && value[0] == '/') strcpy(s.guns[key[8] - '1'].path, value);
        else if ((!strcmp(key,"--uniq1") || !strcmp(key,"--uniq2")) && value[0] && strlen(value) < sizeof(s.guns[0].unique)) strcpy(s.guns[key[6] - '1'].unique, value);
        else if ((!strcmp(key,"--phys1") || !strcmp(key,"--phys2")) && value[0] && strlen(value) < sizeof(s.guns[0].physical)) strcpy(s.guns[key[6] - '1'].physical, value);
        else { fprintf(stderr,"Invalid option: %s %s\n",key,value); return 2; }
    }
    if (!mode_set || !s.socket_path[0] || !s.result_dir[0] ||
        (!s.mock && !s.guns[0].path[0] && !s.guns[1].path[0] && !s.guns[0].unique[0] && !s.guns[1].unique[0] && !s.guns[0].physical[0] && !s.guns[1].physical[0])) { usage(); return 2; }
    for (int i=0;i<2;++i) {
        int methods=!!s.guns[i].path[0]+!!s.guns[i].unique[0]+!!s.guns[i].physical[0];
        if(methods>1) return 2;
    }
    if(s.guns[0].unique[0] && !strcmp(s.guns[0].unique,s.guns[1].unique)) return 2;
    if(s.guns[0].physical[0] && !strcmp(s.guns[0].physical,s.guns[1].physical)) return 2;
    if (s.guns[0].path[0] && !strcmp(s.guns[0].path,s.guns[1].path)) return 2;
    for(int i=0;i<2;++i) {
        strcpy(s.keyboards[i].unique,s.guns[i].unique);
        strcpy(s.keyboards[i].physical,s.guns[i].physical);
    }
    if (lstat(s.result_dir,&result_stat) || !S_ISDIR(result_stat.st_mode) ||
        result_stat.st_uid != geteuid() || (result_stat.st_mode & 0022)) {
        fprintf(stderr,"result-dir must be an existing owned directory, not group/world writable\n"); return 2;
    }
    /* Never unlink a preexisting endpoint, even if it looks stale. */
    if (lstat(s.socket_path,&socket_stat) == 0 || errno != ENOENT) {
        fprintf(stderr,"Socket path exists or cannot be checked; use a fresh private directory\n"); return 2;
    }
    umask(0077);
    s.fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int passcred = 1;
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    strcpy(address.sun_path,s.socket_path);
    if (s.fd < 0 || setsockopt(s.fd,SOL_SOCKET,SO_PASSCRED,&passcred,sizeof(passcred)) ||
        bind(s.fd,(struct sockaddr *)&address,sizeof(address))) { perror("socket setup"); if(s.fd>=0)close(s.fd); return 1; }
    if (lstat(s.socket_path,&socket_stat)) { close(s.fd); return 1; }
    signal(SIGINT,stop); signal(SIGTERM,stop); signal(SIGPIPE,SIG_IGN);
    setvbuf(stdout,NULL,_IOLBF,0);
    printf("READY transport=SOCK_DGRAM mode=%s screen=%dx%d socket=%s calibration_gun=%d\n",
           s.mock?"mock":"evdev",s.width,s.height,s.socket_path,s.calibration_gun);
    uint64_t next_device_check = 0, next_probe = 0;
    int was_calibrating=0;
    while (running) {
        uint64_t now = now_ms();
        /* Drain queued client datagrams before checking whether their pathname
           still exists.  The calibration app sends CALIB_RESULT and then
           immediately closes/unlinks its socket; the datagram remains valid
           and must be processed while the authenticated client entry exists. */
        receive_messages(&s);
        for (int i=0;i<MAX_CLIENTS;++i) {
            if (s.clients[i].used && !alive(&s.clients[i])) drop(&s.clients[i]);
            if (s.clients[i].used) flush(&s,&s.clients[i]);
        }
        if (now >= next_probe) {
            next_probe = now + 1000;
            for (int i=0;i<MAX_CLIENTS;++i)
                if(s.clients[i].used) reply(&s,&s.clients[i],"PING 1");
        }
        if (!s.mock && now >= next_device_check) {
            next_device_check = now + 1000;
            for (int i=0;i<2;++i) if(s.guns[i].fd<0 && (s.guns[i].path[0] || s.guns[i].unique[0] || s.guns[i].physical[0])) {
                s.guns[i].width=s.width; s.guns[i].height=s.height;
                if(gun_open(&s.guns[i])) fprintf(stderr,"DEVICE_WAIT gun=%d uniq=%s phys=%s path=%s error=%s\n",i+1,s.guns[i].unique,s.guns[i].physical,s.guns[i].path,strerror(errno));
            }
            for(int i=0;i<2;++i) if(s.guns[i].fd>=0 && s.keyboards[i].fd<0 && (s.keyboards[i].unique[0] || s.keyboards[i].physical[0])) {
                if(keyboard_open(&s.keyboards[i])) fprintf(stderr,"KEYBOARD_WAIT gun=%d uniq=%s phys=%s error=%s\n",i+1,s.keyboards[i].unique,s.keyboards[i].physical,strerror(errno));
            }
            for(int i=0;i<2;++i)
                if(s.guns[i].fd>=0 && s.keyboards[i].fd>=0) s.disconnect_reset_sent[i]=0;
        }
        struct pollfd fds[5]={{s.fd,POLLIN,0},{s.guns[0].fd,POLLIN,0},{s.guns[1].fd,POLLIN,0},
                              {s.keyboards[0].fd,POLLIN,0},{s.keyboards[1].fd,POLLIN,0}};
        int ready = poll(fds,5,10);
        if(ready<0 && errno!=EINTR) { perror("poll"); break; }
        if(fds[0].revents & POLLIN) receive_messages(&s);
        int calibrating=calibration_active(&s);
        if(calibrating!=was_calibrating) {
            s.move_dirty[0]=s.move_dirty[1]=0;
            /* Switching input ownership resets game state, not the calibration
               app. An actual device reset is routed through button(). */
            reset_game_clients(&s);
            was_calibrating=calibrating;
        }
        if(s.mock) mock_inputs(&s,now_ms());
        else for(int i=0;i<2;++i) if(s.guns[i].fd>=0) {
            int bad = fds[i+1].revents & (POLLERR|POLLHUP|POLLNVAL);
            if(!bad && (fds[i+1].revents & POLLIN)) bad=gun_drain(&s.guns[i],NULL,NULL)!=0;
            if(bad) {
                printf("DEVICE_CLOSED gun=%d\n",i+1);
                fprintf(stderr,"INPUT_RESET gun=%d source=aim reason=DISCONNECTED_OR_READ_ERROR\n",i+1);
                if(!s.disconnect_reset_sent[i]) {
                    s.disconnect_reset_sent[i]=1;
                    button(&s,i+1,GUN_RESET,0,0,0);
                }
                gun_close(&s.guns[i]); keyboard_close(&s.keyboards[i]);
            }
        }
        if(!s.mock) for(int i=0;i<2;++i) if(s.keyboards[i].fd>=0) {
            int bad=fds[i+3].revents & (POLLERR|POLLHUP|POLLNVAL);
            if(!bad && (fds[i+3].revents&POLLIN)) bad=keyboard_drain(&s.keyboards[i])!=0;
            if(bad) {
                printf("KEYBOARD_CLOSED gun=%d\n",i+1);
                fprintf(stderr,"INPUT_RESET gun=%d source=keyboard reason=DISCONNECTED_OR_READ_ERROR\n",i+1);
                if(!s.disconnect_reset_sent[i]) {
                    s.disconnect_reset_sent[i]=1;
                    button(&s,i+1,GUN_RESET,0,0,0);
                }
                keyboard_close(&s.keyboards[i]);
            }
        }
        flush_motion(&s,now_ms());
    }
    for(int i=0;i<2;++i) { gun_close(&s.guns[i]); keyboard_close(&s.keyboards[i]); }
    close(s.fd);
    struct stat current;
    if(lstat(s.socket_path,&current)==0 && current.st_dev==socket_stat.st_dev && current.st_ino==socket_stat.st_ino) unlink(s.socket_path);
    puts("STOPPED");
    return 0;
}
