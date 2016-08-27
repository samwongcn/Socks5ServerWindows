// +----------------------------------------------------------------------
// | ZYSOFT [ MAKE IT OPEN ]
// +----------------------------------------------------------------------
// | Copyright (c) 2016 ZYSOFT All rights reserved.
// +----------------------------------------------------------------------
// | Licensed ( http://www.apache.org/licenses/LICENSE-2.0 )
// +----------------------------------------------------------------------
// | Author: zy_cwind <391321232@qq.com>
// +----------------------------------------------------------------------

/**
 * 如果无法发送数据到 deamon 则启动 deamon
 *
 * $ gcc -o server server.c ./turnclient/win32/lib/libturnclient.a ./libevent-release-2.0.22-stable/win32/lib/libevent.a -I./libevent-release-2.0.22-stable/win32/include/ -I./turnclient/ -lws2_32 -lgdi32 -static-libgcc
 * $ gcc -o server server.c ./turnclient/linux/lib/libturnclient.a ./libevent-release-2.0.22-stable/linux/lib/libevent.a -I./libevent-release-2.0.22-stable/linux/include/ -I./turnclient/ -lrt -static-libgcc -DPATH_MAX=4096
 * $ gcc -o server server.c ./turnclient/macos/lib/libturnclient.a ./libevent-release-2.0.22-stable/macos/lib/libevent.a -I./libevent-release-2.0.22-stable/macos/include/ -I./turnclient/ -DPATH_MAX=4096
 *
 *
 */

#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include <stdio.h>
#include <event.h>
#include <evdns.h>
#include <event2/util.h>
#include <event2/listener.h>
#include <turn_client.h>

#define MAX_BUF_SIZE 512
#define CLOSETIME 5

#define F_VERSION "1.003"

/**
 * TURN 刷新时间
 *
 *
 */
#define LIFETIME 600
#define REFRESHTIME (LIFETIME - 60)

/**
 * 根
 *
 *
 */
#define ROOT_ADDR "proxy.zed1.cn"
#define ROOT_PORT 9000

/**
 * windows 下要是用 closesocket 函数关闭连接
 *
 *
 */
#ifndef WIN32
#include <signal.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * 安卓下使用 logcat
 *
 * 编译的时候需要加 debug
 *
 * ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk NDK_DEBUG=1
 *
 */
#ifdef ANDROID
#include<android/log.h>

#define fprintf(f, ...) __android_log_print(ANDROID_LOG_DEBUG, "server", __VA_ARGS__)
#endif

#define closesocket close
#endif

#define SETTIMEOUT(ev, sec, cb, arg) do {struct timeval tv = {sec}; if (ev) event_free(ev); ev = evtimer_new(base, cb, arg); evtimer_add(ev, &tv);} while (0);

typedef int sint;
    
/**
 * 服务器结构,包含了当前 socks5 状态
 *
 *
 */
struct context_t {
    long status;

    /**
     * 处理后剩下的数据
     *
     *
     */
    char buf[MAX_BUF_SIZE];
    long pos;

    struct event *tick;
    struct bufferevent *server;
    struct bufferevent *remote;
};

struct socks5_request_t {
    char ver;
    char cmd;
    char rsv;
    
    /**
     * 地址类型
     *
     *
     */
    char atyp;
};

/**
 * HTTP报文
 *
 */
struct http_t {
    char  buf[MAX_BUF_SIZE];
    long  pos;
    void *arg;
    /**
     * 回调
     *
     */
    void (*callback)(struct http_t *context);
};

struct event_base *base;

/**
 * TURN 连接事件
 *
 *
 */
struct event *ev = NULL;
struct event *tick = NULL;

char *id;

/**
 * 设备类型
 *
 *
 *
 */
char *F_DEVICE_TYPE = "test";

char manage_address[MAX_BUF_SIZE];
unsigned short manage_port;
char server_address[MAX_BUF_SIZE];
unsigned short server_port;
char report_address[MAX_BUF_SIZE];
unsigned short report_port;

/**
 * 上报频率 (秒)
 *
 *
 */
char *beat_freq ="10";
char  turn_lock = 0;

int g_argc;
char **g_argv;
char *dir = ".";

/**
 * 统计信息
 *
 *
 */
int connected;
float tx;
float rx;


/**
 * 绑定地址
 *
 *
 */
struct sockaddr_in source_address = {0};

void conn(int *fd) {
    if (turnclient_refresh(*fd, server_address, server_port, LIFETIME)) {
        closesocket(*fd);
        event_free(ev);
        *fd = -1;
    }
}

void freecontext(struct context_t *context) {
    fprintf(stdout, "connection closed\n");
    /**
     * 修正内存问题
     *
     *
     */
    if (context->tick)
        event_free(context->tick);
    
    if (connected > 0)
        connected--;
    fprintf(stdout, "current connections: %d\n", connected);
    
    bufferevent_free(context->server);
    if (context->remote)
        bufferevent_free(context->remote);
    free(context);
}

void close_later(int fd, short events, void *arg) {
    freecontext((struct context_t *) arg);
}

/**
 * 出错时断开
 *
 *
 */
void status_quit(struct bufferevent *bev, short events, void *arg) {
    struct context_t *context = (struct context_t *) arg;
    
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF))
        freecontext(context);
    return ;
}

void remote_quit(struct bufferevent *bev, short events, void *arg) {
    struct context_t *context = (struct context_t *) arg;

    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
        /**
         * 等待 server 数据完成
         *
         *
         */
        SETTIMEOUT(context->tick, CLOSETIME, close_later, arg);
        context->status = 4;
    }
    return ;
}

void remote_read(struct bufferevent *bev, void *arg) {
    struct context_t *context = (struct context_t *) arg;
    struct evbuffer *buf = bufferevent_get_input(bev);
    
    tx += evbuffer_get_length(buf);
    bufferevent_write_buffer(context->server, buf);
    return ;
}

/**
 * 创建一个新连接
 *
 *
 */
void open_remote(struct context_t *context, struct sockaddr_in *sin) {
    fprintf(stdout, "connect to %s:%d\n", inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
    
    int fd;
    
    if (!((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)) {
        struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
        
        if (bind(fd, (struct sockaddr *) &source_address, sizeof(struct sockaddr_in)) < 0)
            fprintf(stdout, "failed binding address\n");
        evutil_make_socket_nonblocking(fd);
        bufferevent_setfd(bev, fd);
        bufferevent_socket_connect(bev, (struct sockaddr *) sin, sizeof(struct sockaddr_in));
        bufferevent_setcb(bev, remote_read, NULL, remote_quit, context);
        bufferevent_enable(bev, EV_READ | EV_PERSIST);
        context->remote = bev;
    }
}

/**
 * 使用异步域名解析
 *
 *
 */
void open_byhost(struct context_t *context, const char *domain_name, unsigned short dst_port) {
    dst_port = ntohs(dst_port);
    fprintf(stdout, "connect to %s:%d\n", domain_name, dst_port);
    
    int fd;
    
    if (!((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)) {
        struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
        
        if (bind(fd, (struct sockaddr *) &source_address, sizeof(struct sockaddr_in)) < 0)
            fprintf(stdout, "failed binding address\n");
        evutil_make_socket_nonblocking(fd);
        bufferevent_setfd(bev, fd);
        bufferevent_socket_connect_hostname(bev, NULL, AF_INET, domain_name, dst_port);
        bufferevent_setcb(bev, remote_read, NULL, remote_quit, context);
        bufferevent_enable(bev, EV_READ | EV_PERSIST);
        context->remote = bev;
    }
}

void server_read(struct bufferevent *bev, void *arg) {
    struct context_t *context = (struct context_t *) arg;
    
    long size;
    if ((size = bufferevent_read(bev, &context->buf[context->pos], sizeof(context->buf) - context->pos)) < 0) {
        freecontext(context);
        return ;
    }
    /**
     * 统计的是对客户端的上行/下行流量
     *
     *
     *
     */
    rx += size;
    context->pos += size;
    while (1) {
        long len;
        if (context->status == 0) {
            /**
             * VER NMETHOD METHOD
             *
             *
             */
            if (context->pos >= 2 && context->pos >= (len = context->buf[1] + 2)) {
                if (context->buf[0]!= 5) {
                    freecontext(context);
                    return ;
                }
                
                unsigned long i;
                context->status = 3;
                for (i = 0; i <(unsigned)  context->buf[1]; i++)
                    if (context->buf[2 + i] == 0) {
                        context->status = 1;
                        break;
                    }
                /**
                 * 没有支持的方法
                 *
                 *
                 */
                if (context->status == 3)
                    bufferevent_write(bev, "\x05\xFF", 2);
                else
                    bufferevent_write(bev, "\x05\x00", 2);
                
                context->pos -= len;
                if (context->pos)
                    memmove(&context->buf[0],&context->buf[len], context->pos);
            } else
                return ;
        } else if (context->status == 1) {
            /**
             * VER CMD RSV ATYP DST.ADDR DST.PORT
             *
             *
             */
            if (context->pos >= 4) {
                if (context->buf[0]!= 5) {
                    freecontext(context);
                    return ;
                }
                
                struct socks5_request_t *request = (struct socks5_request_t *) &context->buf[0];
                char buf[MAX_BUF_SIZE] = {0x05, 0x00, 0x00, 0x01};
                /**
                 * 返回的地址类型不能是 domain_name
                 *
                 *
                 */
                
                if (request->cmd == 1) {
                    /**
                     * CONNECT
                     *
                     *
                     */
                    if (request->atyp == 1) {
                        if (context->pos >= (len = 10)) {
                            struct sockaddr_in sin;
                            
                            sin.sin_family = AF_INET;
                            sin.sin_addr = * (struct in_addr *) &context->buf[4];
                            sin.sin_port = * (unsigned short *) &context->buf[8];
                            memcpy(&buf[8], &sin.sin_port, sizeof(unsigned short));
                            
                            context->pos -= len;
                            if (context->pos)
                                memmove(&context->buf[0], &context->buf[len], context->pos);
                            open_remote(context,  &sin);
                        } else
                            return ;
                    } else if (request->atyp == 3) {
                        if (context->pos >= 4 && context->pos >= (len = context->buf[4] + 7)) {
                            char domain_name[MAX_BUF_SIZE];
                            unsigned short dst_port;
                            
                            memcpy(domain_name, &context->buf[5], context->buf[4]);
                            domain_name[context->buf[4]] = 0;
                            dst_port = * (unsigned short *) &context->buf[len - 2];
                            
                            memcpy(&buf[8], &context->buf[len - 2], sizeof(unsigned short));
                            
                            context->pos -= len;
                            if (context->pos)
                                memmove(&context->buf[0], &context->buf[len], context->pos);
                            open_byhost(context, domain_name, dst_port);
                        } else
                            return ;
                    } else {
                        /**
                         * 不支持的地址类型
                         *
                         *
                         */
                        buf[1] = 8;
                        context->status = 3;
                    }
                    if (context->status!= 3) {
                        /**
                         * BND.ADDR BND.PORT 返回相关端口
                         *
                         *
                         */
                        bufferevent_write(bev, buf, 10);
                        context->status = 2;
                    }
                } else {
                    /**
                     * 不支持的命令
                     *
                     *
                     */
                    {
                        buf[1] = 7;
                        context->status = 3;
                    }
                }
            } else
                return ;
        } else if (context->status == 2) {
            if (context->pos > 0) {
                bufferevent_write(context->remote, context->buf, context->pos);
                context->pos = 0;
            }
            return ;
        } else if (context->status == 3) {
            /**
             * 返回协议错误
             *
             *
             */
            SETTIMEOUT(context->tick, CLOSETIME, close_later, context);
            context->status = 4;
        } else if (context->status == 4)
            return ;
    }
    return ;
}

/**
 * 简易的 HTTP 处理
 *
 *
 */
void http_packet(struct bufferevent *bev, void *arg) {
    struct http_t *context = (struct http_t *) arg;
    if (context->callback)
        if ((context->pos = bufferevent_read(bev, context->buf, sizeof(context->buf))) > 0)
            context->callback(context);
    /**
     * 不解析报头以及 content
     *
     *
     */
    bufferevent_free(bev);
    free(context);
}

void http_status(struct bufferevent *bev, short events, void *arg) {
    struct http_t *context = (struct http_t *) arg;
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
        bufferevent_free(bev);
        free(context);
    }
    /**
     * 一次性的
     *
     *
     */
    if (events &  BEV_EVENT_CONNECTED)
        bufferevent_write(bev, context->buf, context->pos);
    return ;
}

/**
 * 定时上报参数
 *
 *
 */
void beat() {
    int fd;
    
    if (!((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)) {
        struct http_t *context = (struct http_t *) malloc(sizeof(struct http_t));
        
        if (context) {
            sprintf(context->buf, "GET /manage/cgi/api!register.action?uid=%s&turn_server=%s:%d&relay_info=%s:%d&size=%d&type=%s&ver=" F_VERSION "&mac= HTTP/1.1\r\nHost: %s:%d\r\nConnection: Keep-Alive\r\n\r\n", id, server_address, server_port, report_address, report_port, connected, F_DEVICE_TYPE, manage_address, manage_port);
            context->pos = strlen(context->buf);
            context->arg = NULL;
            context->callback = NULL;
            /**
             * 心跳
             *
             *
             *
             */
            fprintf(stdout, "beat\n%s", context->buf);
            
            struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
            
            if (bind(fd, (struct sockaddr *) &source_address, sizeof(struct sockaddr_in)) < 0)
                fprintf(stdout, "failed binding address\n");
            evutil_make_socket_nonblocking(fd);
            bufferevent_setfd(bev, fd);
            bufferevent_socket_connect_hostname(bev, NULL, AF_INET, manage_address, manage_port);
            bufferevent_setcb(bev, http_packet, NULL, http_status, context);
            /**
             * 如果初始化 event 的时候设置了 EV_PERSIST,则使用 event_add 将其添加到侦听事件集合后(pending 状态),该 event 会持续保持 pending 状态,即该 event 可以无限次参加 libevent 的事件侦听
             *
             *
             */
            bufferevent_enable(bev, EV_READ | EV_PERSIST);
        } else
            closesocket(fd);
    }
}

#ifdef WITHOUT_TURNCLIENT

void open_server(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *sin, int slen, void *arg) { 
    fprintf(stdout, "accept a connection\n");
    
    struct context_t *context = (struct context_t *) malloc(sizeof(struct context_t));
    if (context) {
        memset(context, 0, sizeof(struct context_t));
        connected++;
        fprintf(stdout, "current connections: %d\n", connected);
        
        struct bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        
        context->server = bev;
        bufferevent_setcb(bev, server_read, NULL, status_quit, context);
        bufferevent_setwatermark(bev, EV_READ, 0, MAX_BUF_SIZE);
        bufferevent_enable(bev, EV_READ | EV_PERSIST);
    } else 
        closesocket(fd);
    return ; 
}

#else

/**
 * 创建一个新出口
 *
 *
 */
void open_server(int fd, short events, void *arg) {
    int  new_fd;
    int *tun_fd = (int *) arg;
    turn_lock = 1;
    if (turnclient_wait_connection(fd, server_address, server_port, &new_fd, report_address)) {
        closesocket(fd);
        event_free(ev);
        *tun_fd = -1;
    } else {
        fprintf(stdout, "accept a connection\n");
        /**
         * 当读写错误时关闭 fd
         *
         *
         */
        struct context_t *context = (struct context_t *) malloc(sizeof(struct context_t));
        if (context) {
            memset(context, 0, sizeof(struct context_t));
            connected++;
            fprintf(stdout, "current connections: %d\n", connected);
            
            struct bufferevent *bev = bufferevent_socket_new(base, new_fd, BEV_OPT_CLOSE_ON_FREE);
            
            context->server = bev;
            bufferevent_setcb(bev, server_read, NULL, status_quit, context);
            bufferevent_setwatermark(bev, EV_READ, 0, MAX_BUF_SIZE);
            bufferevent_enable(bev, EV_READ | EV_PERSIST);
        } else
            closesocket(new_fd);
    }
    turn_lock = 0;
    return ;
}

#endif

/**
 * 输出帮助,必要参数为管理服务器地址
 *
 *
 */
void show_useage() {
    const char *useage = \
        "useage:\n"                                       \
        "\t[-t report frequency], default is 30(s)\n"     \
        "\t[-d working dir]\n"                            \
        "\t[-f factory type]\n"                           \
        "\t[-b bind address]\n";
    fprintf(stdout, "%s", useage);
    return ;
}

/**
 * 设置管理服务器
 *
 *
 */
void init_manage(struct http_t *context) {
    char *address = strstr(context->buf, "\r\n\r\n") + 4;
    char *port;
    if (address) {
        port = strstr(address, ":");
        if (port) {
            memset(manage_address, 0, MAX_BUF_SIZE);
            memcpy(manage_address, address, port - address);
            
            char buf[MAX_BUF_SIZE];
            memset(buf, 0, MAX_BUF_SIZE);
            port++;
            memcpy(buf, port, context->buf + context->pos - port);
            manage_port = atoi(buf);
            * (int *) context->arg = 1;
            return ;
        }
    }
    return ;
}

void load() {
    if (dir) {
        char uid_file[PATH_MAX];
        FILE *fp;
        
        sprintf(uid_file, "%s/id.txt", dir);
        if ((fp = fopen(uid_file, "rb"))) {
            if (id == NULL)
                id = (char *)  malloc(MAX_BUF_SIZE);
            fgets(id, MAX_BUF_SIZE, fp);
            /**
             * 没有读到 id
             *
             *
             */
            if (!strlen(id)) {
                free(id);
                id = NULL;
            }
            fclose(fp);
        }
    }
}

void save() {
    if (dir) {
        char uid_file[PATH_MAX];
        FILE *fp;
        
        sprintf(uid_file, "%s/id.txt", dir);
        if ((fp = fopen(uid_file, "wb"))) {
            fputs(id, fp);
            fclose(fp);
        }
    }
}

void guid(struct http_t *context) {
    char *uid = strstr(context->buf, "\"uid\":") + 7;
    if (uid) {
        char *address = strstr(context->buf, "\"uri\":") + 7;
        char *port;
        if (address) {
            port = strstr(address, ":");
            if (port) {
                /**
                 * 如果没有设置 id
                 *
                 *
                 */
                if (id == NULL) {
                    id = (char *)  malloc(MAX_BUF_SIZE);
                    if (id) {
                        memset(id, 0, MAX_BUF_SIZE);
                        memcpy(id, uid, strstr(uid, "\"") - uid);
                        save();
                    }
                }
                memset(server_address, 0, MAX_BUF_SIZE);
                memcpy(server_address, address, port - address);
                
                char buf[MAX_BUF_SIZE];
                memset(buf, 0, MAX_BUF_SIZE);
                port++;
                memcpy(buf, port, strstr(port, "\"") - port);
                server_port = atoi(buf);
                
                * (int *) context->arg = 2;
                return ;
            }
        }
    }
    return ;
}

#ifdef JNI

#include "jni.h"

/**
 * fix 定义
 *
 *
 */
sint main(int argc, char *argv[]);

jint Java_com_zed1_System_server(JNIEnv* env, jobject thiz, jint argc, jobjectArray args) {
    /**
     * 转换参数
     *
     *
     */
    char *argv[16];
    long  i;
    for ( i = 0; i < (*env)->GetArrayLength(env, args); i++)
        argv[i] = (*env)->GetStringUTFChars(env, (jstring) (*env)->GetObjectArrayElement(env, args, i), 0);
    pid_t pid = fork();
    if (pid > 0);
    else if (pid == 0)
        exit(main(argc, argv));
    return 0;
}
#elif ANDROID

#include "fcntl.h"

int pfds[2][2];
int z;

/**
 * 守护进程
 *
 *
 */
long server();

long deamon() {
    if (dir) {
        char buf[16] = {0};
        char pid_file[PATH_MAX];
        FILE *fp;
        
        sprintf(pid_file, "%s/deamon.pid", dir);
        if (fp = fopen(pid_file, "wb")) {
            sprintf(buf, "%d", getpid());
            fputs(buf, fp);
            fclose(fp);
        }
    }
    int c;
    c = 0;
    while (1) {
        char b[MAX_BUF_SIZE];
        
        c = read(pfds[0][0], b, sizeof(b)) > 0 ? 0 : c + 1;
        if (c == 5) {
            /**
             * 新建进程运行代理
             *
             *
             *
             */
            pid_t pid = fork();
            if (pid > 0)
                c = 0;
            else if (pid == 0) {
                fprintf(stdout, "server process killed, restarting\n");
                exit(server());
            }
        } else write(pfds[1][1], "D", 1);
        usleep(1000000);
    }
    return 0;
}
#endif

/**
 * 定时任务
 *
 *
 */
void step(int fd, short events, void *arg) {
    static unsigned long second;
    static int tun_fd = -1;
    
    /**
     * 管理流程
     *
     *
     */
    static int state;
    static int timer;
    
    /**
     * 重连
     *
     *
     */
    if (tun_fd == -1) {
        switch(state) {
        case 0:
        {
            /**
             * 从指定的接口获取管理
             *
             *
             */
            struct http_t *context = (struct http_t *) malloc(sizeof(struct http_t));
            sprintf(context->buf, "GET /manage/cgi/root!getManageServer.action?type=%s HTTP/1.1\r\nHost: %s:%d\r\nConnection: Keep-Alive\r\n\r\n", F_DEVICE_TYPE, ROOT_ADDR, ROOT_PORT);
            context->pos = strlen(context->buf);
            context->arg = &state;
            context->callback = init_manage;
                
            struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
            
            bufferevent_socket_connect_hostname(bev, NULL, AF_INET, ROOT_ADDR, ROOT_PORT);
            bufferevent_setcb(bev, http_packet, NULL, http_status, context);
            bufferevent_enable(bev, EV_READ | EV_PERSIST);
            /**
             * 等待设定
             *
             *
             */
            timer = 0;
            state = 3;
        }
        break;
        case 1:
        {
            /**
             * 从管理获取 id
             *
             *
             */
            struct http_t *context = (struct http_t *) malloc(sizeof(struct http_t));
            sprintf(context->buf, "GET /manage/cgi/api!getTurnList.action HTTP/1.1\r\nHost: %s:%d\r\nConnection: Keep-Alive\r\n\r\n", manage_address, manage_port);
            context->pos = strlen(context->buf);
            context->arg = &state;
            context->callback = guid;
            
            struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
            
            bufferevent_socket_connect_hostname(bev, NULL, AF_INET, manage_address, manage_port);
            bufferevent_setcb(bev, http_packet, NULL, http_status, context);
            bufferevent_enable(bev, EV_READ | EV_PERSIST);
                
            timer = 0;
            state = 3;
        }
        break;
        case 2:
        {
            if (init_turn_client(server_address, server_port, &tun_fd, report_address, &report_port) == 0) {
                fprintf(stdout, "id: %s\nserver: %s:%d\nturn: %s:%d\nrelay: %s:%d\n", id, manage_address, manage_port, server_address, server_port, report_address, report_port);
                ev = event_new(base, tun_fd, EV_READ | EV_PERSIST, open_server, &tun_fd);
                event_add(ev, NULL);
                second =  0;
            } else if (tun_fd != -1) {
                closesocket(tun_fd);
                tun_fd = -1;
            }
            state = 0;
        }
        break;
        case 3:
            /**
             * 超时
             *
             *
             */
            if (++timer == 30)
                state = 0;
            break;
        }
    }
    
    if (tun_fd != -1 && !turn_lock)
        if (second % REFRESHTIME == 0)
            conn(&tun_fd);
    if (tun_fd != -1)
        if (!(second % atoi(beat_freq)))
            beat();

#if !defined(JNI) && defined(ANDROID)
    /**
     * 新建进程运行守护
     *
     *
     */
    {
        char b[MAX_BUF_SIZE];
        
        z = read(pfds[1][0], b, sizeof(b)) > 0 ? 0 : z + 1;
        if (z == 5) {
            pid_t pid = fork();
            if (pid > 0)
                z = 0;
            else if (pid == 0) {
                fprintf(stdout, "deamon process killed, restarting\n");
                exit(deamon());
            }
        } else write(pfds[0][1], "S", 1);
    }
#endif
    
    second++;
    
    /**
     * 定时
     *
     *
     */
    struct timeval tv = {
        1
    };
    
    if (tick)
        event_free(tick);
    tick = evtimer_new(base, step, NULL);
    evtimer_add(tick, &tv);
}

long server() {    
#ifdef ANDROID

/**
 * 新进程会共享资源
 * 
 *
 */
#if !defined(JNI)
    z = 0;
#endif
    if (dir) {
        char buf[16] = {0};
        char pid_file[PATH_MAX];
        FILE *fp;
        
        sprintf(pid_file, "%s/server.pid", dir);
        if (fp = fopen(pid_file, "rb")) {
            if (fgets(buf, sizeof(buf), fp))
                kill(atoi(buf), SIGTERM);
            fclose(fp);
        }
        if (fp = fopen(pid_file, "wb")) {
            sprintf(buf, "%d", getpid());
            fputs(buf, fp);
            fclose(fp);
        }
    }
#endif
    
    /**
     * 启动服务
     *
     *
     */
    assert(base = event_base_new());
    
    /**
     * 消息循环
     *
     *
     */
#ifdef WITHOUT_TURNCLIENT
    /**
     * 不带 TURN 测试使用
     *
     *
     */
    struct sockaddr_in sin;

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(8888);
    assert(evconnlistener_new_bind(base, open_server, NULL, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, 5, (struct sockaddr *) &sin, sizeof(struct sockaddr)));
#else
    load();
    step(0, EV_TIMEOUT, NULL);
#endif
    event_base_dispatch(base);
    fclose(stdout);
    return 0;
}

#ifdef BUILDLIB
sint work(int argc, char *argv[]) {
#else
sint main(int argc, char *argv[]) {
#endif
    g_argc = argc;
    g_argv = argv;
    
#ifndef WIN32
    signal(SIGPIPE, SIG_IGN);
#else
    WSADATA wsaData;
    assert(WSAStartup(MAKEWORD(1, 1), &wsaData) == 0);
#endif
    
    source_address.sin_family = AF_INET;
    source_address.sin_addr.s_addr = htonl(INADDR_ANY);
    source_address.sin_port = 0;
    
    int c;
    opterr = 0;
    while ((c = getopt(g_argc, g_argv, "d:t:f:b:")) != -1)
        switch (c) {
        case 'd':
            dir              = optarg;
            break;
        case 't':
            beat_freq        = optarg;
            break;
        case 'f':
            F_DEVICE_TYPE    = optarg;
            break;
        case 'b':
        {
            struct hostent *host;

            /**
             * 指定地址
             *
             *
             */
            if ((host = gethostbyname(optarg))) 
                source_address.sin_addr = * (struct in_addr *) host->h_addr;
            else
                fprintf(stdout, "gethostbyname failed\n");
            break;
        }
        }
    if (opterr) {
        show_useage();
        exit(0);
    }
#if !defined(JNI) && defined(ANDROID)
    /**
     * 只有安卓的进程版有守护
     *
     *
     */
    pipe(pfds[0]);
    pipe(pfds[1]);
    
    fcntl(pfds[0][0], F_SETFL, O_NONBLOCK);
    fcntl(pfds[0][1], F_SETFL, O_NONBLOCK);
    fcntl(pfds[1][0], F_SETFL, O_NONBLOCK);
    fcntl(pfds[1][1], F_SETFL, O_NONBLOCK);
    
    if (dir) {
        /**
         * 防止重复启动
         *
         *
         */
        char buf[16] = {0};
        char pid_file[PATH_MAX];
        FILE *fp;
        
        sprintf(pid_file, "%s/deamon.pid", dir);
        if (fp = fopen(pid_file, "rb")) {
            if (fgets(buf, sizeof(buf), fp))
                kill(atoi(buf), SIGTERM);
            fclose(fp);
        }
        sprintf(pid_file, "%s/server.pid", dir);
        if (fp = fopen(pid_file, "rb")) {
            if (fgets(buf, sizeof(buf), fp))
                kill(atoi(buf), SIGTERM);
            fclose(fp);
        }
    }
    
    /**
     * 主进程守护
     *
     *
     */
    pid_t pid = fork();
    if (pid > 0)
        exit(deamon());
    else if (pid == 0)
#endif
        exit(server());
    
    return 0;
}
