#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void stop(int signal_number) { (void)signal_number; running = 0; }

static void usage(void)
{
    puts("game_receiver SERVER_SOCKET CLIENT_SOCKET\n"
         "Example: game_receiver /tmp/gaime/input.sock /tmp/gaime/game.sock");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(); return 0; }
    if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/' ||
        strlen(argv[1]) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
        strlen(argv[2]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        usage(); return 2;
    }
    struct stat st;
    if (lstat(argv[2], &st) == 0 || errno != ENOENT) {
        fputs("Client socket path already exists; choose a fresh path.\n", stderr);
        return 2;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un local = {.sun_family = AF_UNIX};
    struct sockaddr_un server = {.sun_family = AF_UNIX};
    strcpy(local.sun_path, argv[2]);
    strcpy(server.sun_path, argv[1]);
    umask(0077);
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) ||
        connect(fd, (struct sockaddr *)&server, sizeof(server))) {
        perror("UDS setup"); if (fd >= 0) close(fd); unlink(argv[2]); return 1;
    }
    const char hello[] = "HELLO game 1";
    if (send(fd, hello, sizeof(hello) - 1, MSG_NOSIGNAL) != sizeof(hello) - 1) {
        perror("HELLO"); close(fd); unlink(argv[2]); return 1;
    }
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGPIPE, SIG_IGN);
    puts("Waiting for HELLO_OK and input messages. Ctrl+C to stop.");
    setvbuf(stdout, NULL, _IOLBF, 0);
    int acknowledged = 0, result = 0;
    while (running) {
        struct pollfd pollfd = {fd, POLLIN, 0};
        int ready = poll(&pollfd, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            perror("poll"); result = 1; break;
        }
        if (!(pollfd.revents & POLLIN)) continue;
        char line[4097];
        ssize_t n = recv(fd, line, sizeof(line) - 1, 0);
        if (n <= 0) { perror("recv"); result = 1; break; }
        line[n] = 0;
        if (!strncmp(line, "HELLO_OK 1", 10)) acknowledged = 1;
        if (strncmp(line, "PING 1", 6)) fputs(line, stdout);
    }
    if (acknowledged) (void)send(fd, "BYE", 3, MSG_NOSIGNAL);
    close(fd); unlink(argv[2]);
    return result;
}
