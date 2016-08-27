// +----------------------------------------------------------------------
// | ZYSOFT [ MAKE IT OPEN ]
// +----------------------------------------------------------------------
// | Copyright (c) 2016 ZYSOFT All rights reserved.
// +----------------------------------------------------------------------
// | Licensed ( http://www.apache.org/licenses/LICENSE-2.0 )
// +----------------------------------------------------------------------
// | Author: zy_cwind <391321232@qq.com>
// +----------------------------------------------------------------------

#include <string.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/**
 * 可写目录
 *
 *
 */
extern char *dir;

/**
 * windows 下要是用 closesocket 函数关闭连接
 *
 *
 */
#ifndef WIN32
#include <sys/un.h>
#include <netdb.h>

#define closesocket close
#endif

/**
 * 安卓下 vpn 模式需要处理出 fd
 *
 *
 */
long protect_socket(int fd) {
#ifdef ANDROID_CLIENT
    long l_fd;
    if ((l_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        return -1;
    struct timeval tv = {1};
    setsockopt(l_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(struct timeval));
    setsockopt(l_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(struct timeval));
    
    struct sockaddr_un sin;
    memset(&sin, 0, sizeof(struct sockaddr_un));
    sin.sun_family = AF_UNIX;
    {
        char path[PATH_MAX] = "protect_path";
        if (dir)
            sprintf(path, "%s/protect_path", dir);
        strncpy(sin.sun_path, path, sizeof(sin.sun_path) - 1);
    }
    char b = 0;
    if (connect(l_fd, (struct sockaddr *) &sin, sizeof(struct sockaddr_un)) < 0 || ancil_send_fd(l_fd, fd) < 0 || recv(l_fd, &b, 1, 0) < 0) {
        closesocket(l_fd);
        return -1;
    }
    closesocket(l_fd);
    return b;
#else
    return 0;
#endif
}

/**
 * 通知上层重启
 *
 *
 *
 */
long send_restart() {
#ifdef ANDROID
    long l_fd;
    if ((l_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        return -1;
    struct timeval tv = {1};
    setsockopt(l_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(struct timeval));
    setsockopt(l_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(struct timeval));
    
    struct sockaddr_un sin;
    memset(&sin, 0, sizeof(struct sockaddr_un));
    sin.sun_family = AF_UNIX;
    {
        char path[PATH_MAX] = "luaservice_path";
        if (dir)
            sprintf(path, "%s/luaservice_path", dir);
        strncpy(sin.sun_path, path, sizeof(sin.sun_path) - 1);
    }
    
    if (connect(l_fd, (struct sockaddr *) &sin, sizeof(struct sockaddr_un)) < 0 || send(l_fd, "RESTART\r\n", 9, 0) < 0) {
        closesocket(l_fd);
        return -1;
    }
    closesocket(l_fd);
    return 0;
#else
    return 0;
#endif
}

/**
 * TURN 缺失函数
 *
 *
 */
long pj_rand() {
    srand(time(NULL));
    return (rand() & 0xFFFF) | ((rand() & 0xFFFF) << 16);
}
