#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Read-only game-role observer, in a fresh private socket directory. */
int main(int argc,char **argv)
{
    if(argc!=3 || strlen(argv[1])>=sizeof(((struct sockaddr_un *)0)->sun_path) ||
       strlen(argv[2])>=sizeof(((struct sockaddr_un *)0)->sun_path)) return 2;
    int fd=socket(AF_UNIX,SOCK_DGRAM,0);
    struct sockaddr_un server={.sun_family=AF_UNIX}, local={.sun_family=AF_UNIX};
    strcpy(server.sun_path,argv[1]); strcpy(local.sun_path,argv[2]);
    if(fd<0 || bind(fd,(struct sockaddr *)&local,sizeof(local))) { perror("bind"); return 1; }
    int connected=0;
    for(int i=0;i<50;++i) {
        if(!connect(fd,(struct sockaddr *)&server,sizeof(server))) { connected=1; break; }
        usleep(100000);
    }
    int ok=0;
    if(!connected || send(fd,"HELLO game 1\n",13,0)!=13) goto done;
    setvbuf(stdout,NULL,_IOLBF,0);
    struct pollfd p={fd,POLLIN,0};
    char line[4097];
    if(poll(&p,1,3000)<=0) goto done;
    ssize_t n=recv(fd,line,sizeof(line)-1,0);
    if(n<=0) goto done;
    line[n]=0;
    if(strcmp(line,"HELLO_OK 1\n")) goto done;
    puts("READY GAME_BUTTON_MONITOR duration=1800s");
    struct timespec start,now; clock_gettime(CLOCK_MONOTONIC,&start);
    for(;;) {
        clock_gettime(CLOCK_MONOTONIC,&now);
    if(now.tv_sec-start.tv_sec>=1800) { ok=1; break; }
        int ready=poll(&p,1,250);
        if(ready<0) { if(errno==EINTR) continue; break; }
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL)) break;
        if(p.revents&POLLIN) {
            n=recv(fd,line,sizeof(line)-1,0);
            if(n<=0) break;
            line[n]=0;
            if(!strcmp(line,"PING 1\n")) {
                if(send(fd,"PONG 1\n",7,MSG_NOSIGNAL)!=7) break;
            } else printf("%s",line);
        }
    }
done:
    (void)send(fd,"BYE\n",4,MSG_NOSIGNAL);
    close(fd); unlink(local.sun_path);
    printf("MONITOR_FINISHED status=%s\n",ok?"OK":"ERROR"); return ok?0:1;
}
