#include "co_service.h"
#include "co_routine.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <stack>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

int co_accept(int fd, struct sockaddr *addr, socklen_t *len );

static int g_listen_fd = -1;
static const int MAX_PACKAGE_SIZE = 1024 * 30;
readcallback_t read_pfn = NULL;
closecallback_t close_pfn = NULL;
acceptcallback_t accept_pfn = NULL;
// struct decleare
struct task_t {
    stCoRoutine_t * co;
    int fd;
    bool timeoutclose; // timeout close.
};

// static functions
static void SetAddr(const char *pszIP,const unsigned short shPort,struct sockaddr_in &addr)
{
	bzero(&addr,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(shPort);
	int nIP = 0;
	if( !pszIP || '\0' == *pszIP   
	    || 0 == strcmp(pszIP,"0") || 0 == strcmp(pszIP,"0.0.0.0") 
		|| 0 == strcmp(pszIP,"*") 
	  )
	{
		nIP = htonl(INADDR_ANY);
	}
	else
	{
		nIP = inet_addr(pszIP);
	}
	addr.sin_addr.s_addr = nIP;
}

static int CreateTcpSocket(const unsigned short shPort /* = 0 */,const char *pszIP /* = "*" */,bool bReuse /* = false */)
{
	int fd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if( fd >= 0 )
	{
		if(shPort != 0)
		{
			if(bReuse)
			{
				int nReuseAddr = 1;
				setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&nReuseAddr,sizeof(nReuseAddr));
			}
			struct sockaddr_in addr ;
			SetAddr(pszIP,shPort,addr);
			int ret = bind(fd,(struct sockaddr*)&addr,sizeof(addr));
			if( ret != 0)
			{
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}
static int SetNonBlock(int iSock)
{
    int iFlags;

    iFlags = fcntl(iSock, F_GETFL, 0);
    iFlags |= O_NONBLOCK;
    iFlags |= O_NDELAY;
    int ret = fcntl(iSock, F_SETFL, iFlags);
    return ret;
}
static void *readco( void *arg )
{
    co_enable_hook_sys();
    task_t *t = (task_t*)arg;
    int fd = t->fd;
    char buf[ MAX_PACKAGE_SIZE ];

	int err = 0;

    // read header
    int32_t header = 0;
    int readlen = 0;
    // read body
    int pos = 0;
    int ret;
    for(;;)
    {
        struct pollfd pf = { 0 };
        pf.fd = fd;
        pf.events = (POLLIN|POLLERR|POLLHUP);
        int nfs = co_poll( co_get_epoll_ct(), &pf, 1, 60*1000);
        // timeout
        if (nfs == 0) {
            if (t->timeoutclose){
                printf("fd %d timeout\n", fd);
                close(fd);
                break;
            }else{
                continue;
            }
        }
        
        // header
        if (header == 0 ) {
            ret = read( fd, &header, sizeof(int32_t) );
            if (ret != sizeof(int32_t)){
                printf("fd %d read error", fd);
                close(fd);
                break;
            }else{
                readlen = ntohl(header);
                // is heart data
                if (readlen == 0) {
                    continue;
                }
                // error data 
				if (readlen > MAX_PACKAGE_SIZE || readlen < 0){
					close(fd);
					err = -1;
					break;
				}
                continue;
            }
        }

        // body
        if (readlen > 0 && pos < readlen){
            ret = read(fd, (buf+pos), readlen-pos);
            pos += ret;

            if( ret <= 0 ){
                close( fd );
                break;
            }

            if (pos == readlen){
				if (read_pfn != NULL){
					read_pfn(fd, buf, readlen);
				}
                readlen = 0;
                pos = 0;
                header = 0;
            }
        }
    }

    free(t);

	if (close_pfn != NULL){
		close_pfn(fd, err);
	}
    return 0;
}
static void *default_readwrite_routine( void *arg )
{
    co_enable_hook_sys();
    task_t *t = (task_t*)arg;
    int fd = t->fd;
    char buf[ 1024 * 16 ];

    for(;;)
    {
        struct pollfd pf = { 0 };
        pf.fd = fd;
        pf.events = (POLLIN|POLLERR|POLLHUP);
        co_poll( co_get_epoll_ct(), &pf, 0, 60*1000);

        int ret = read( fd, buf, sizeof(buf) );
        if( ret > 0 ) {
            ret = write( fd,buf,ret );
        }
        if( ret <= 0 )
        {
           close( fd );
           break;
           continue;
        }
    }
    free(t);
    return 0;
}
static void *accept_routine( void * )
{
	co_enable_hook_sys();
	fflush(stdout);
    
    // looping accept
    for (;;) {
        struct sockaddr_in addr; //maybe sockaddr_un;
        memset( &addr,0,sizeof(addr) );
        socklen_t len = sizeof(addr);

        int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);
        if( fd < 0 )
        {
            struct pollfd pf = { 0 };
            pf.fd = g_listen_fd;
            pf.events = (POLLIN|POLLERR|POLLHUP);
            co_poll( co_get_epoll_ct(),&pf,1, 60*1000 );
            continue;
        }
        SetNonBlock( fd );

        // accept callback for user.
        if (accept_pfn != NULL){
            accept_pfn(fd, inet_ntoa(addr.sin_addr), addr.sin_port);
        }

        task_t * t = (task_t*)malloc(sizeof(task_t));
        stCoRoutine_t *read_co = NULL;
        t->co = NULL;
        t->fd = fd;
        t->timeoutclose = true;
        co_create( &read_co, NULL, readco, t);
        co_resume( read_co );
    }
    return 0;
}

int co_service(int port, int progress_num)
{
    g_listen_fd = CreateTcpSocket( port, "*", true );
    int ret = listen( g_listen_fd,1024);
    if (ret < 0 ){
        printf("listen at %d error!\n", port);
        exit(-1);
        return -1;
    }

    printf("listen fd=%d port=%d\n",g_listen_fd, port);

    SetNonBlock( g_listen_fd );

    // 不需要子进程,即单进程
    stCoRoutine_t *accept_co = NULL;
    co_create( &accept_co,NULL,accept_routine,0 );
    co_resume( accept_co );
    co_eventloop( co_get_epoll_ct(),0,0 );

    exit(0);
    return 0;
}

void co_setreadcb(readcallback_t f)
{
	read_pfn = f;
}

void co_setclosecb(closecallback_t f)
{
	close_pfn = f;
}

void co_setaccpectcb(acceptcallback_t f)
{
    accept_pfn = f;
}
int co_connect(const char *ip, int port)
{
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    SetAddr(ip, port, addr);
    if (connect(fd,(struct sockaddr*)&addr,sizeof(addr)) == -1){
        return -1;
    }
    SetNonBlock( fd );
    task_t * t = (task_t*)malloc(sizeof(task_t));
    stCoRoutine_t *read_co = NULL;
    t->co = NULL;
    t->fd = fd;
    t->timeoutclose = false;
    co_create( &read_co, NULL, readco, t);
    co_resume( read_co );
    return fd;
}

void co_loop()
{
    co_eventloop( co_get_epoll_ct(),0,0 );
}

int co_send(int fd, const char * buf, int len)
{
    int pos = 0;

    while(true){
        int sendlen = send(fd,(void*)(pos + buf), len - pos, 0);
        if (sendlen <= 0 ){
            break;
        }
        pos += sendlen;
        if (pos >= len )
            break;
    }

    return pos;
}
int co_connect_noloop(const char *ip, int port)
{    
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    SetAddr(ip, port, addr);
    if (connect(fd,(struct sockaddr*)&addr,sizeof(addr)) == -1){
        return -1;
    }
    SetNonBlock( fd );
    return fd; 
}
