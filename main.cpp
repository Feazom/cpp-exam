#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <cerrno>
#include <sys/epoll.h>
#include <cstring>
#include <pthread.h>
#include <string>
#include <regex>
#include <iostream>
#include <signal.h>
#include <sys/stat.h>
#include "getopt.h"

std::string serv_dir{};

#define MAX_EVENT_NUMBER 1024
#define BUFFER_SIZE 10

struct fds {
    int epollfd;
    int sockfd;
};

int set_nonblock(int fd) {
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, (unsigned) flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIONBIO, &flags);
#endif

}

inline void add_fd(int epollfd, int fd, bool oneshot) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    if (oneshot)
        event.events |= EPOLLONESHOT;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    set_nonblock(fd);
}

inline void reset_oneshot(int &epfd, int &fd) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event);
}

std::string parse_request(const std::string &for_parse) {

    std::size_t pos1 = for_parse.find("GET /");
    std::size_t pos2 = for_parse.find(" HTTP/1");
    if (pos1 == std::string::npos || pos2 == std::string::npos) return "";
    std::string ind = for_parse.substr(pos1 + 5, pos2 - pos1 - 5);
    if (ind.size() == 0) return "index.html";

    auto pos = ind.find('?');
    if (pos == std::string::npos)
        return ind;
    else
        return ind.substr(0, pos);
}

std::string http_error_404() {
    std::stringstream ss;
    ss << "HTTP/1.0 404 NOT FOUND\r\n";
    ss << "Content-length: 0\r\n";
    ss << "Content-Type: text/html\r\n\r\n";
    return ss.str();
}

std::string http_ok_200(const std::string& data) {
    std::stringstream ss;
    ss << "HTTP/1.0 200 OK\r\n";
    ss << "Content-length: " << data.size() << "\r\n";
    ss << "Content-Type: text/html\r\n\r\n";
    ss << data;
    return ss.str();
}

inline void f(int &fd, const std::string &request) {
    std::string f_name = parse_request(request);
    if (f_name == "") {
        std::string err = http_error_404();
        send(fd, err.c_str(), err.length() + 1, MSG_NOSIGNAL);
        return;
    } else {
        std::stringstream ss;
        ss << serv_dir;
        if (serv_dir.length() > 0 && serv_dir[serv_dir.length() - 1] != '/')
            ss << "/";
        ss << f_name;

        FILE *file_in = fopen(ss.str().c_str(), "r");
        char arr[1024];
        if (file_in) {
            std::stringstream ss;
            std::string tmp_str;
            char c = '\0';
            while ((c = fgetc(file_in)) != EOF) {
                ss << c;
            }
            tmp_str = ss.str();
            std::string ok = http_ok_200(tmp_str);
            send(fd, ok.c_str(), ok.size(), MSG_NOSIGNAL);
            fclose(file_in);
        } else {
            std::string err = http_error_404();
            send(fd, err.c_str(), err.size(), MSG_NOSIGNAL);
        }

    }
}


void *worker(void *arg) {
    int sockfd = ((struct fds *) arg)->sockfd;
    int epollfd = ((struct fds *) arg)->epollfd;
    printf("start new thread to receive data on fd: %d\n", sockfd);
    char buf[BUFFER_SIZE];
    memset(buf, '\0', BUFFER_SIZE);

    std::string receive_buf;

    for (;;) {
        int ret = recv(sockfd, buf, BUFFER_SIZE - 1, 0);
        if (ret == 0) {
            close(sockfd);
            printf("foreigner closed the connection\n");
            break;
        } else if (ret < 0) {
            if (errno = EAGAIN) {
                reset_oneshot(epollfd, sockfd);
                f(sockfd, receive_buf);

                break;
            }
        } else {
            receive_buf += buf;
        }
    }
    printf("end thread receiving data on fd: %d\n", sockfd);
    pthread_exit(0);
}

int run(const int argc, const char **argv) {
    int masterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (masterSocket < 0)
        perror("fail to create socket!\n"), exit(errno);

    struct sockaddr_in sock_addr;
    bzero(&sock_addr, sizeof(sock_addr));
    sock_addr.sin_family = AF_INET;
    get_command_line(argc, (char **) (argv), sock_addr, serv_dir);
    int opt = 1;
    if (setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        exit(errno);
    }
    if (bind(masterSocket, (struct sockaddr *) &sock_addr, sizeof(sock_addr)) < 0) {
        perror("fail to bind socket");
        exit(errno);
    }

    set_nonblock(masterSocket);
    listen(masterSocket, SOMAXCONN);

    struct epoll_event events[MAX_EVENT_NUMBER];
    int epfd = epoll_create1(0);
    if (epfd == -1)
        perror("fail to create epoll\n"), exit(errno);

    add_fd(epfd, masterSocket, false);

    for (;;) {
        int ret = epoll_wait(epfd, events, MAX_EVENT_NUMBER, -1);
        if (ret < 0) {
            printf("epoll wait failure!\n");
            break;
        }
        int i;
        for (i = 0; i < ret; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == masterSocket) {
                struct sockaddr_in slave_address;
                socklen_t slave_addrlength = sizeof(slave_address);
                int slaveSocket = accept(masterSocket, (struct sockaddr *) &slave_address, &slave_addrlength);
                add_fd(epfd, slaveSocket, true);
            } else if (events[i].events & EPOLLIN) {
                pthread_t thread;
                struct fds fds_for_new_worker;
                fds_for_new_worker.epollfd = epfd;
                fds_for_new_worker.sockfd = events[i].data.fd;
                
                pthread_create(&thread, NULL, worker, &fds_for_new_worker);

            } else {
                printf("something unexpected happened!\n");
            }
        }

    }
    close(masterSocket);
    close(epfd);
    return 0;
}

static void skeleton_daemon() {
    pid_t pid;
    
    pid = fork();
    
    if (pid < 0)
        exit(EXIT_FAILURE);
        
    if (pid > 0)
        exit(EXIT_SUCCESS);
        
    if (setsid() < 0)
        exit(EXIT_FAILURE);
        
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    
    pid = fork();
    
    if (pid < 0)
        exit(EXIT_FAILURE);
        
    if (pid > 0)
        exit(EXIT_SUCCESS);
        
    umask(0);
    
    chdir("/");
    
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }
}

int main(const int argc, const char **argv) {
    skeleton_daemon();
    while (1) {
        run(argc, argv);
    }
    return EXIT_SUCCESS;
}