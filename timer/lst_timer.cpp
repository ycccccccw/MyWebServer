#include "lst_timer.h"

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event = {};
    event.data.fd = fd;
    event.events = TRIGMode == 1 ? EPOLLIN | EPOLLET | EPOLLRDHUP
                                 : EPOLLIN | EPOLLRDHUP;
    if (one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old_option | O_NONBLOCK);
    return old_option;
}

void Utils::sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], reinterpret_cast<char *>(&msg), 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handle)(int), bool restart)
{
    struct sigaction sa = {};
    sa.sa_handler = handle;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, nullptr) != -1);
}

int *Utils::u_pipefd = nullptr;
