// chat_client.c (robust)
// gcc chat_client.c -lws2_32 -o chat_client.exe
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#define MAXMSG 1024

typedef struct { SOCKET sock; volatile int running; } ctx_t;

unsigned __stdcall recv_thread(void* arg){
    ctx_t* c=(ctx_t*)arg; char buf[MAXMSG+1];
    while(c->running){
        int n=recv(c->sock,buf,MAXMSG,0);
        if(n<=0){ puts("\n[disconnected from server]"); c->running=0; break; }
        buf[n]=0; fputs(buf,stdout); fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv){
    if(argc<4){ fprintf(stderr,"Usage: %s <server-ipv4> <port> <YourName>\n",argv[0]); return 1; }
    const char* host=argv[1]; int port=atoi(argv[2]); const char* name=argv[3];

    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)!=0){ fputs("WSAStartup failed\n",stderr); return 1; }

    SOCKET s=socket(AF_INET,SOCK_STREAM,0); if(s==INVALID_SOCKET){ fputs("socket failed\n",stderr); return 1; }

    // enable TCP keepalive
    BOOL ka=TRUE; setsockopt(s,SOL_SOCKET,SO_KEEPALIVE,(char*)&ka,sizeof(ka));

    struct sockaddr_in srv; srv.sin_family=AF_INET; srv.sin_port=htons((u_short)port); srv.sin_addr.s_addr=inet_addr(host);
    if(connect(s,(struct sockaddr*)&srv,sizeof(srv))==SOCKET_ERROR){ fputs("connect failed (IP/port/firewall?)\n",stderr); closesocket(s); WSACleanup(); return 1; }

    // send name first (as server expects)
    send(s,name,(int)strlen(name),0); send(s,"\n",1,0);
    puts("[connected] Type messages. Use /quit to exit.");

    ctx_t ctx; ctx.sock=s; ctx.running=1;
    uintptr_t th=_beginthreadex(NULL,0,recv_thread,&ctx,0,NULL); if(!th){ fputs("thread create failed\n",stderr); closesocket(s); WSACleanup(); return 1; }

    // main loop: try to read stdin; if EOF, just sleep & keep connection alive
    char line[MAXMSG];
    while(ctx.running){
        if(fgets(line,sizeof(line),stdin)){
            if(strncmp(line,"/quit",5)==0) break;
            int len=(int)strlen(line);
            if(len>0 && send(s,line,len,0)<=0){ puts("[send failed]"); break; }
        }else{
            // stdin closed (e.g., launched without console): keep connection alive
            Sleep(200);
        }
    }

    ctx.running=0; shutdown(s,SD_BOTH); closesocket(s);
    WaitForSingleObject((HANDLE)th,INFINITE); CloseHandle((HANDLE)th);
    WSACleanup(); return 0;
}
