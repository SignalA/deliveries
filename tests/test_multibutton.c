/* Real evdev handlers -> real service routing -> Unix datagrams. */
#define main input_service_program_main
#include "../dgram/input_service.c"
#undef main
#include <assert.h>
#include <glob.h>

static void expect(int fd, const char *text)
{
    char line[256]={0};
    ssize_t n=recv(fd,line,sizeof(line)-1,0);
    if(n<0 || strcmp(line,text)) {
        fprintf(stderr,"expected [%s] got [%s] n=%zd\n",text,line,n);
        abort();
    }
}
static void empty(int fd)
{
    char c;
    assert(recv(fd,&c,1,MSG_DONTWAIT)<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
}
static void key(GunKeyboard *k,int code,int value)
{
    struct input_event e={.type=EV_KEY,.code=code,.value=value};
    keyboard_event(k,&e);
}
static void syn(GunKeyboard *k)
{
    struct input_event e={.type=EV_SYN,.code=SYN_REPORT}; keyboard_event(k,&e);
}
static void trigger(GunInput *g,int value)
{
    struct input_event e={.type=EV_KEY,.code=BTN_TOUCH,.value=value}; gun_event(g,&e,NULL,NULL);
}
static void frame(GunInput *g)
{
    struct input_event e={.type=EV_SYN,.code=SYN_REPORT}; gun_event(g,&e,NULL,NULL);
}
static void relative(GunInput *g,int code,int value)
{
    struct input_event e={.type=EV_REL,.code=code,.value=value}; gun_event(g,&e,NULL,NULL);
}
int main(void)
{
    char dir[]="/tmp/gaime-buttons-XXXXXX";
    assert(mkdtemp(dir));
    int receiver=socket(AF_UNIX,SOCK_DGRAM,0); assert(receiver>=0);
    struct timeval timeout={.tv_sec=1};
    assert(!setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    snprintf(addr.sun_path,sizeof(addr.sun_path),"%s/client",dir);
    assert(!bind(receiver,(struct sockaddr *)&addr,sizeof(addr)));
    Service s={.fd=socket(AF_UNIX,SOCK_DGRAM,0),.player={3,4},.calibration_gun=1};
    assert(s.fd>=0);
    snprintf(s.result_dir,sizeof(s.result_dir),"%s",dir);
    s.clients[0]=(Client){.used=1,.role=1,.version=1,.address=addr,.address_len=sizeof(addr)};
    GunKeyboard k[2]={{.fd=-1,.gun=1,.callback=keyboard_input_button,.context=&s}, {.fd=-1,.gun=2,.callback=keyboard_input_button,.context=&s}};
    GunInput g={.fd=-1,.gun=1,.width=1920,.height=1080,.x=100,.y=200,.callback=aim_button,.move_callback=motion,.context=&s};
    const int codes[]={KEY_SPACE,KEY_C,KEY_Q};
    const char *names[]={"A","COIN","PAUSE"};
    for(int round=0;round<20;++round) for(int n=0;n<3;++n) {
        for(int j=0;j<2;++j) {
            key(&k[j],codes[n],1); key(&k[j],codes[n],1);
            for(int r=0;r<10;++r) key(&k[j],codes[n],2);
        }
        empty(receiver); /* Frame not yet submitted. */
        for(int j=0;j<2;++j) {
            syn(&k[j]); char msg[80];
            snprintf(msg,sizeof(msg),"GUN_BUTTON %d %s DOWN\n",j+3,names[n]); expect(receiver,msg);
        }
        for(int j=1;j>=0;--j) {
            key(&k[j],codes[n],0); key(&k[j],codes[n],0); syn(&k[j]);
            char msg[80]; snprintf(msg,sizeof(msg),"GUN_BUTTON %d %s UP\n",j+3,names[n]); expect(receiver,msg);
        }
        empty(receiver);
    }
    /* Simultaneous distinct keys and trigger, without merged state. */
    key(&k[0],KEY_SPACE,1); key(&k[0],KEY_Q,1); syn(&k[0]);
    expect(receiver,"GUN_BUTTON 3 A DOWN\n"); expect(receiver,"GUN_BUTTON 3 PAUSE DOWN\n");
    trigger(&g,1); trigger(&g,2); trigger(&g,0); trigger(&g,1); trigger(&g,0); frame(&g);
    expect(receiver,"GUN_DOWN 3 100 200\n"); expect(receiver,"GUN_UP 3 100 200\n");
    expect(receiver,"GUN_DOWN 3 100 200\n"); expect(receiver,"GUN_UP 3 100 200\n");
    key(&k[0],KEY_SPACE,0); syn(&k[0]); expect(receiver,"GUN_BUTTON 3 A UP\n");
    assert(k[0].keys.held==(1u<<GUN_PAUSE));
    key(&k[0],KEY_Q,0); syn(&k[0]); expect(receiver,"GUN_BUTTON 3 PAUSE UP\n");
    key(&k[0],KEY_B,1); key(&k[0],KEY_B,0); syn(&k[0]); empty(receiver);
    key(&k[0],KEY_ENTER,1); key(&k[0],KEY_ENTER,0); syn(&k[0]); empty(receiver);
    /* X/Y changes are coalesced into one game-only movement per frame. */
    relative(&g,REL_X,10); relative(&g,REL_Y,20); frame(&g);
    flush_motion(&s,now_ms());
    expect(receiver,"GUN_MOVE 3 110 220\n"); empty(receiver);
    /* Bursts within 16 ms are collapsed, then the latest position is sent. */
    relative(&g,REL_X,10); frame(&g); flush_motion(&s,now_ms()); empty(receiver);
    relative(&g,REL_X,10); frame(&g); flush_motion(&s,s.move_next[0]);
    expect(receiver,"GUN_MOVE 3 130 220\n"); empty(receiver);
    /* No orphan UP for a client joining while a key was already held. */
    k[0].keys.held=1u<<GUN_PAUSE;
    key(&k[0],KEY_Q,0); syn(&k[0]); empty(receiver);
    /* Partial/unreliable frames reset client state rather than adding a coin. */
    key(&k[0],KEY_Q,1);
    struct input_event dropped={.type=EV_SYN,.code=SYN_DROPPED};
    keyboard_event(&k[0],&dropped); expect(receiver,"GUN_RESET 3\n");
    key(&k[0],KEY_Q,1); syn(&k[0]); empty(receiver); assert(k[0].fd==-1 && !k[0].keys.held);
    for(int n=0;n<65;++n) key(&k[0],KEY_Q,n%2==0);
    expect(receiver,"GUN_RESET 3\n"); syn(&k[0]); empty(receiver);
    /* The new gun reports the trigger only as BTN_TOUCH on the aim endpoint. */
    s.clients[0].role=2;
    s.guns[0].absolute=1; s.guns[0].raw_x=14000; s.guns[0].raw_y=23000;
    relative(&g,REL_X,10); relative(&g,REL_Y,10); frame(&g); empty(receiver);
    k[0].keys=(GunKeys){0};
    s.guns[0].x=g.x; s.guns[0].y=g.y;
    trigger(&g,1); trigger(&g,0); frame(&g); expect(receiver,"GUN_DOWN 3 14000 23000\n"); empty(receiver);
    /* Calibration v2 locks onto the first logical player and includes it. */
    s.clients[0].version=2; s.clients[0].calibration_player=0;
    trigger(&g,1); trigger(&g,0); frame(&g); expect(receiver,"GUN_DOWN 3 14000 23000\n"); empty(receiver);
    click(&s,2,500,600); empty(receiver);
    /* Calibration receives RESET and ordinary button edges as well as clicks. */
    button(&s,1,GUN_PAUSE,1,0,0); expect(receiver,"GUN_BUTTON 3 PAUSE DOWN\n");
    button(&s,1,GUN_PAUSE,0,0,0); expect(receiver,"GUN_BUTTON 3 PAUSE UP\n");
    key(&k[0],KEY_C,1); syn(&k[0]); expect(receiver,"GUN_BUTTON 3 COIN DOWN\n");
    key(&k[0],KEY_C,0); syn(&k[0]); expect(receiver,"GUN_BUTTON 3 COIN UP\n");
    button(&s,1,GUN_A,1,0,0); expect(receiver,"GUN_BUTTON 3 A DOWN\n");
    button(&s,1,GUN_A,0,0,0); expect(receiver,"GUN_BUTTON 3 A UP\n");
    button(&s,1,GUN_RESET,0,0,0); expect(receiver,"GUN_RESET 3\n");
    button(&s,1,GUN_TRIGGER,0,0,0); empty(receiver);
    reset_game_clients(&s); empty(receiver); /* Joining calibration must not cancel it. */
    /* Disconnect/reset cannot leave a game button held. */
    s.clients[0].role=1;
    reset_game_clients(&s);
    expect(receiver,"GUN_RESET 3\n"); expect(receiver,"GUN_RESET 4\n");
    s.guns[1].x=500; s.guns[1].y=600;
    aim_button(&s,2,GUN_TRIGGER,1,500,600); expect(receiver,"GUN_DOWN 4 500 600\n");
    aim_button(&s,2,GUN_TRIGGER,0,500,600); expect(receiver,"GUN_UP 4 500 600\n");
    /* The current application reports 32 coordinate pairs: 24 shots + 8 targets. */
    char *same[68]={"CALIB_RESULT","0","0","32"};
    for(int i=4;i<68;i+=8) {
        for(int j=0;j<6;j+=2) { same[i+j]="100"; same[i+j+1]="200"; }
        same[i+6]="300"; same[i+7]="400";
    }
    s.mock=1; s.clients[0].calibration_gun=1;
    assert(save_result(&s,&s.clients[0],same,68)==0);
    button(&s,2,GUN_RESET,0,0,0); keyboard_close(&k[1]);
    expect(receiver,"GUN_RESET 4\n"); assert(!s.clients[0].held[1] && !k[1].keys.held);
    glob_t captures={0}; char pattern[512];
    snprintf(pattern,sizeof(pattern),"%s/capture-gun*-*",dir);
    if(!glob(pattern,0,NULL,&captures))
        for(size_t i=0;i<captures.gl_pathc;++i) unlink(captures.gl_pathv[i]);
    globfree(&captures);
    close(s.fd); close(receiver); unlink(addr.sun_path); rmdir(dir);
    puts("MULTIBUTTON_UDS=PASS"); return 0;
}
