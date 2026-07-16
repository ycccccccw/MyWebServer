#ifndef LST_TIMER
#define LST_TIMER

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

class Utils
{
public:
    int setnonblocking(int fd);
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    static void sig_handler(int sig);
    void addsig(int sig, void(handler)(int), bool restart = true);
    void show_error(int connfd, const char *info);

    static int *u_pipefd;
};

#endif
