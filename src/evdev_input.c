#include "evdev_input.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int clamp(int64_t v, int maximum)
{
    return v < 0 ? 0 : v > maximum ? maximum : (int)v;
}
static int scaled(int value, int low, int high, int pixels)
{
    return clamp(((int64_t)value - low) * (pixels - 1) / ((int64_t)high - low), pixels - 1);
}
static unsigned key_bit(int code)
{
    if (code == BTN_LEFT) return 1;
    if (code == BTN_TRIGGER) return 2;
    if (code == BTN_TOUCH) return 4;
    return 0;
}
const char *gun_button_name(int button)
{
    static const char *names[] = {"TRIGGER", "A", "COIN", "PAUSE"};
    return button >= GUN_TRIGGER && button <= GUN_PAUSE ? names[button] : "RESET";
}
static int keyboard_button(int code)
{
    switch (code) {
    case KEY_SPACE: return GUN_A;     /* shotgun reload / legacy A action */
    case KEY_C: return GUN_COIN;
    case KEY_Q: return GUN_PAUSE;
    default: return -1;
    }
}
static int queue_edge(GunKeys *keys, int button, int value)
{
    if (keys->count == 64) { keys->count = 0; keys->dropped = 1; return -1; }
    keys->button[keys->count] = (unsigned char)button;
    keys->value[keys->count++] = (unsigned char)value;
    return 0;
}
static int identity_matches(int fd, const char *unique, const char *physical)
{
    char actual[128]={0};
    if(unique[0] && (ioctl(fd,EVIOCGUNIQ(sizeof(actual)-1),actual)<0 || strcmp(actual,unique))) return 0;
    if(physical[0]) {
        memset(actual,0,sizeof(actual));
        if(ioctl(fd,EVIOCGPHYS(sizeof(actual)-1),actual)<0) return 0;
        size_t n=strlen(physical);
        if(strncmp(actual,physical,n) || (actual[n] && actual[n]!='/')) return 0;
    }
    return 1;
}
static int sysfs_physical_matches(const char *event_path, const char *physical)
{
    if(!physical[0]) return 1;
    const char *name=strrchr(event_path,'/');
    if(!name || strncmp(++name,"event",5) || strchr(name,'/')) return 0;
    char path[256], actual[128]={0};
    if(snprintf(path,sizeof(path),"/sys/class/input/%s/device/phys",name)>=(int)sizeof(path)) return 0;
    FILE *file=fopen(path,"r");
    if(!file) return 0;
    int ok=fgets(actual,sizeof(actual),file)!=NULL;
    fclose(file);
    actual[strcspn(actual,"\r\n")]=0;
    size_t n=strlen(physical);
    return ok && !strncmp(actual,physical,n) && (!actual[n] || actual[n]=='/');
}
static void emit_edges(GunKeys *keys, GunButton cb, void *ctx, int gun, int x, int y)
{
    for (int i=0;i<keys->count;++i)
        if (cb) cb(ctx,gun,keys->button[i],keys->value[i],x,y);
    keys->count = 0;
}
static int resync(GunInput *g)
{
    unsigned char keys[(KEY_MAX + 8) / 8] = {0};
    if (ioctl(g->fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return -1;
    g->buttons = 0;
    const int codes[] = {BTN_LEFT, BTN_TRIGGER, BTN_TOUCH};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
        if (keys[codes[i] / 8] & (1u << (codes[i] % 8))) g->buttons |= key_bit(codes[i]);
    if (g->absolute) {
        struct input_absinfo x, y;
        if (ioctl(g->fd, EVIOCGABS(ABS_X), &x) < 0 ||
            ioctl(g->fd, EVIOCGABS(ABS_Y), &y) < 0) return -1;
        g->x = scaled(x.value, g->min_x, g->max_x, g->width);
        g->y = scaled(y.value, g->min_y, g->max_y, g->height);
        g->raw_x = scaled(x.value, g->min_x, g->max_x, 32768);
        g->raw_y = scaled(y.value, g->min_y, g->max_y, 32768);
    }
    g->down_pending = 0; g->moved = 0;
    memset(&g->edges, 0, sizeof(g->edges));
    return 0;
}
static int open_path(GunInput *g)
{
    struct input_absinfo x, y;
    unsigned char rel[(REL_MAX + 8) / 8] = {0};
    unsigned char keys[(KEY_MAX + 8) / 8] = {0};
    char name[128] = "unknown", unique[128] = "";
    g->fd = open(g->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (g->fd < 0) return -1;
    if (!identity_matches(g->fd,g->unique,g->physical)) goto fail;
    g->absolute = ioctl(g->fd, EVIOCGABS(ABS_X), &x) == 0 &&
                  ioctl(g->fd, EVIOCGABS(ABS_Y), &y) == 0;
    if (g->absolute) {
        if (x.maximum <= x.minimum || y.maximum <= y.minimum) goto fail;
        g->min_x = x.minimum; g->max_x = x.maximum;
        g->min_y = y.minimum; g->max_y = y.maximum;
    } else {
        if (ioctl(g->fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0 ||
            !(rel[REL_X / 8] & (1u << (REL_X % 8))) ||
            !(rel[REL_Y / 8] & (1u << (REL_Y % 8)))) goto fail;
    }
    if (ioctl(g->fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
        (!(keys[BTN_LEFT / 8] & (1u << (BTN_LEFT % 8))) &&
         !(keys[BTN_TRIGGER / 8] & (1u << (BTN_TRIGGER % 8))) &&
         !(keys[BTN_TOUCH / 8] & (1u << (BTN_TOUCH % 8))))) goto fail;
    g->x = g->width / 2; g->y = g->height / 2;
    g->dropped = 0;
    if (resync(g) != 0) goto fail;
    (void)ioctl(g->fd, EVIOCGNAME(sizeof(name)-1), name);
    (void)ioctl(g->fd, EVIOCGUNIQ(sizeof(unique)-1), unique);
    printf("%s gun=%d path=%s name=%s uniq=%s phys=%s mode=%s\n", (g->unique[0]||g->physical[0]) ? "DEVICE_CANDIDATE" : "DEVICE_OPEN", g->gun,
           g->path, name, unique, g->physical, g->absolute ? "absolute" : "relative");
    return 0;
fail:
    gun_close(g);
    errno = ENOTSUP;
    return -1;
}
int gun_open(GunInput *g)
{
    if (!g->unique[0] && !g->physical[0]) return open_path(g);
    glob_t paths = {0};
    GunInput found = *g;
    int matches = 0;
    found.fd = -1;
    int result = glob("/dev/input/event*", 0, NULL, &paths);
    if (result != 0 && result != GLOB_NOMATCH) {
        globfree(&paths); errno = EIO; return -1;
    }
    for (size_t i = 0; i < paths.gl_pathc; ++i) {
        GunInput candidate = *g;
        candidate.fd = -1;
        if (strlen(paths.gl_pathv[i]) >= sizeof(candidate.path)) continue;
        /* Do not open nodes on another physical gun: this GAIME driver can
           disturb an existing reader when the same node is opened/closed. */
        if (!sysfs_physical_matches(paths.gl_pathv[i],g->physical)) continue;
        strcpy(candidate.path, paths.gl_pathv[i]);
        if (open_path(&candidate)) continue;
        if (++matches == 1) found = candidate;
        else gun_close(&candidate);
    }
    globfree(&paths);
    if (matches != 1) {
        gun_close(&found);
        errno = matches ? EEXIST : ENODEV;
        return -1;
    }
    *g = found;
    printf("DEVICE_BOUND gun=%d uniq=%s phys=%s path=%s\n", g->gun, g->unique, g->physical, g->path);
    return 0;
}
void gun_close(GunInput *g)
{
    if (g->fd >= 0) close(g->fd);
    g->fd = -1;
    g->buttons = 0; g->down_pending = 0; g->dropped = 0; g->moved = 0;
    memset(&g->edges, 0, sizeof(g->edges));
}
void gun_event(GunInput *g, const struct input_event *e, GunClick click, void *ctx)
{
    if (e->type == EV_SYN && e->code == SYN_DROPPED) {
        fprintf(stderr,"INPUT_RESET gun=%d source=aim reason=SYN_DROPPED\n",g->gun);
        g->dropped = 1; g->down_pending = 0; g->buttons = 0;
        memset(&g->edges, 0, sizeof(g->edges));
        if (g->callback) g->callback(g->context,g->gun,GUN_RESET,0,g->x,g->y);
        return;
    }
    if (g->dropped) {
        if (e->type == EV_SYN && e->code == SYN_REPORT) {
            if (resync(g) == 0) g->dropped = 0;
            else gun_close(g);
        }
        return;
    }
    if (e->type == EV_REL && !g->absolute) {
        int old_x=g->x, old_y=g->y;
        if (e->code == REL_X) g->x = clamp((int64_t)g->x + e->value, g->width - 1);
        if (e->code == REL_Y) g->y = clamp((int64_t)g->y + e->value, g->height - 1);
        if(g->x!=old_x || g->y!=old_y) g->moved=1;
    } else if (e->type == EV_ABS && g->absolute) {
        int old_x=g->x, old_y=g->y;
        if (e->code == ABS_X) {
            g->x = scaled(e->value, g->min_x, g->max_x, g->width);
            g->raw_x = scaled(e->value, g->min_x, g->max_x, 32768);
        }
        if (e->code == ABS_Y) {
            g->y = scaled(e->value, g->min_y, g->max_y, g->height);
            g->raw_y = scaled(e->value, g->min_y, g->max_y, 32768);
        }
        if(g->x!=old_x || g->y!=old_y) g->moved=1;
    } else if (e->type == EV_KEY && e->value != 2) {
        unsigned bit = key_bit(e->code), before = g->buttons;
        if (e->value == 1) g->buttons |= bit;
        else if (e->value == 0) g->buttons &= ~bit;
        if (!before && g->buttons) g->down_pending++;
        if (!!before != !!g->buttons && queue_edge(&g->edges,GUN_TRIGGER,!!g->buttons)) {
            fprintf(stderr,"INPUT_RESET gun=%d source=aim reason=EDGE_QUEUE_OVERFLOW\n",g->gun);
            g->dropped=1; g->down_pending=0;
            if(g->callback) g->callback(g->context,g->gun,GUN_RESET,0,g->x,g->y);
        }
    } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        /* Coalesce X/Y changes into one movement message per evdev report. */
        if(g->moved && g->move_callback) g->move_callback(g->context,g->gun,g->x,g->y);
        g->moved=0;
        while (g->down_pending > 0) {
            g->down_pending--;
            if (click) click(ctx, g->gun, g->x, g->y);
        }
        emit_edges(&g->edges,g->callback,g->context,g->gun,g->x,g->y);
    }
}
int gun_drain(GunInput *g, GunClick click, void *ctx)
{
    /* Read batches rather than one syscall per event. These guns can produce
       thousands of absolute-coordinate events per second. */
    for (int batch = 0; batch < 32; ++batch) {
        struct input_event events[256];
        ssize_t n = read(g->fd, events, sizeof(events));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || n % (ssize_t)sizeof(events[0])) return -1;
        size_t count=(size_t)n/sizeof(events[0]);
        for(size_t i=0;i<count;++i) {
            gun_event(g,&events[i],click,ctx);
            if(g->fd<0) return -1;
        }
        if(count < sizeof(events)/sizeof(events[0])) return 0;
    }
    return 0;
}

static int keyboard_resync(GunKeyboard *k)
{
    unsigned char bits[(KEY_MAX+8)/8] = {0};
    if (ioctl(k->fd,EVIOCGKEY(sizeof(bits)),bits)<0) return -1;
    memset(&k->keys,0,sizeof(k->keys));
    const int codes[] = {KEY_SPACE,KEY_C,KEY_Q};
    for (size_t i=0;i<sizeof(codes)/sizeof(codes[0]);++i)
        if(bits[codes[i]/8] & (1u<<(codes[i]%8))) k->keys.held |= 1u<<keyboard_button(codes[i]);
    return 0;
}
void keyboard_close(GunKeyboard *k)
{
    if(k->fd>=0) close(k->fd);
    k->fd=-1;
    memset(&k->keys,0,sizeof(k->keys));
}
int keyboard_open(GunKeyboard *k)
{
    if(!k->unique[0] && !k->physical[0]) { errno=EINVAL; return -1; }
    glob_t paths={0};
    int result=glob("/dev/input/event*",0,NULL,&paths), matches=0, selected=-1;
    if(result && result!=GLOB_NOMATCH) { globfree(&paths); errno=EIO; return -1; }
    for(size_t i=0;i<paths.gl_pathc;++i) {
        if(!sysfs_physical_matches(paths.gl_pathv[i],k->physical)) continue;
        unsigned char bits[(KEY_MAX+8)/8]={0};
        int fd=open(paths.gl_pathv[i],O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if(fd<0) continue;
        int good=identity_matches(fd,k->unique,k->physical) &&
                 ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(bits)),bits)>=0;
        const int codes[]={KEY_SPACE,KEY_C,KEY_Q};
        for(size_t j=0;j<sizeof(codes)/sizeof(codes[0]);++j)
            if(!(bits[codes[j]/8] & (1u<<(codes[j]%8)))) good=0;
        if(!good || strlen(paths.gl_pathv[i])>=sizeof(k->path)) { close(fd); continue; }
        if(++matches==1) { selected=fd; strcpy(k->path,paths.gl_pathv[i]); }
        else close(fd);
    }
    globfree(&paths);
    if(matches!=1) {
        if(selected>=0) close(selected);
        errno=matches ? EEXIST : ENODEV; return -1;
    }
    k->fd=selected;
    if(keyboard_resync(k)) { keyboard_close(k); return -1; }
    printf("KEYBOARD_BOUND gun=%d uniq=%s phys=%s path=%s\n",k->gun,k->unique,k->physical,k->path);
    return 0;
}
void keyboard_event(GunKeyboard *k, const struct input_event *e)
{
    if(e->type==EV_SYN && e->code==SYN_DROPPED) {
        fprintf(stderr,"INPUT_RESET gun=%d source=keyboard reason=SYN_DROPPED\n",k->gun);
        k->keys.count=0; k->keys.dropped=1;
        if(k->callback) k->callback(k->context,k->gun,GUN_RESET,0,0,0);
        return;
    }
    if(k->keys.dropped) {
        if(e->type==EV_SYN && e->code==SYN_REPORT && keyboard_resync(k)) keyboard_close(k);
        return;
    }
    if(e->type==EV_KEY && (e->value==0 || e->value==1)) {
        int button=keyboard_button(e->code);
        if(button<0) return;
        unsigned bit=1u<<button;
        if(!!(k->keys.held & bit)==e->value) return;
        if(e->value) k->keys.held |= bit;
        else k->keys.held &= ~bit;
        if(queue_edge(&k->keys,button,e->value) && k->callback) {
            fprintf(stderr,"INPUT_RESET gun=%d source=keyboard reason=EDGE_QUEUE_OVERFLOW\n",k->gun);
            k->callback(k->context,k->gun,GUN_RESET,0,0,0);
        }
    } else if(e->type==EV_SYN && e->code==SYN_REPORT)
        emit_edges(&k->keys,k->callback,k->context,k->gun,0,0);
}
int keyboard_drain(GunKeyboard *k)
{
    for(int batch=0;batch<4;++batch) {
        struct input_event events[64];
        ssize_t n=read(k->fd,events,sizeof(events));
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
        if(n<0 && errno==EINTR) continue;
        if(n<=0 || n%(ssize_t)sizeof(events[0])) return -1;
        size_t count=(size_t)n/sizeof(events[0]);
        for(size_t i=0;i<count;++i) {
            keyboard_event(k,&events[i]);
            if(k->fd<0) return -1;
        }
        if(count < sizeof(events)/sizeof(events[0])) return 0;
    }
    return 0;
}
