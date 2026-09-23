#ifndef GAIME_EVDEV_INPUT_H
#define GAIME_EVDEV_INPUT_H
#include <linux/input.h>
#include <stddef.h>

enum { GUN_RESET = -1, GUN_TRIGGER, GUN_A, GUN_COIN, GUN_PAUSE };
typedef void (*GunButton)(void *, int, int, int, int, int);
typedef void (*GunMove)(void *, int, int, int);
typedef struct {
    unsigned held;
    unsigned char button[64], value[64];
    int count, dropped;
} GunKeys;
typedef struct {
    int fd, gun;
    char unique[128], physical[128], path[512];
    GunKeys keys;
    GunButton callback;
    void *context;
} GunKeyboard;

typedef struct {
    int fd, gun, width, height, absolute;
    int x, y, raw_x, raw_y, min_x, max_x, min_y, max_y;
    unsigned buttons;
    int down_pending, dropped, moved;
    char path[512];
    char unique[128]; /* Optional exact EVIOCGUNIQ binding, resolved on every open. */
    char physical[128]; /* Optional EVIOCGPHYS base such as usb-sunxi-ehci-1.2. */
    GunKeys edges;
    GunButton callback;
    GunMove move_callback;
    void *context;
} GunInput;
typedef void (*GunClick)(void *, int, int, int);
int gun_open(GunInput *g);
void gun_close(GunInput *g);
void gun_event(GunInput *g, const struct input_event *e, GunClick click, void *ctx);
int gun_drain(GunInput *g, GunClick click, void *ctx);
int keyboard_open(GunKeyboard *k);
void keyboard_close(GunKeyboard *k);
void keyboard_event(GunKeyboard *k, const struct input_event *e);
int keyboard_drain(GunKeyboard *k);
const char *gun_button_name(int button);
#endif
