// chat_server.c (fixed)
// Build:  gcc chat_server.c -lws2_32 -o chat_server.exe
// Run:    chat_server.exe 0.0.0.0 5555

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define MAXMSG 1024
#define MAX_CLIENTS 128

typedef struct {
    SOCKET sock;
    char   name[64];  // empty until first message sets it
} Client;

static void die(const char* m) { fprintf(stderr, "%s\n", m); exit(1); }

static void broadcast(Client clients[], int n, SOCKET except, const char* msg) {
    for (int i = 0; i < n; ++i) {
        if (clients[i].sock != INVALID_SOCKET && clients[i].sock != except) {
            send(clients[i].sock, msg, (int)strlen(msg), 0);
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <bind-ip> <port>\nExample: %s 0.0.0.0 5555\n", argv[0], argv[0]);
        return 1;
    }
    const char* bind_ip = argv[1];
    int port = atoi(argv[2]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) die("WSAStartup failed");

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) die("socket failed");

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(bind_ip);

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) die("bind failed");
    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) die("listen failed");

    printf("Chat server listening on %s:%d ...\n", bind_ip, port);

    Client clients[MAX_CLIENTS];
    for (int i=0;i<MAX_CLIENTS;++i){ clients[i].sock = INVALID_SOCKET; clients[i].name[0]=0; }

    fd_set readfds;
    while (1) {
        // build fd set
        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        SOCKET maxsock = listen_sock;
        for (int i=0;i<MAX_CLIENTS;++i) {
            if (clients[i].sock != INVALID_SOCKET) {
                FD_SET(clients[i].sock, &readfds);
                if (clients[i].sock > maxsock) maxsock = clients[i].sock;
            }
        }

        int ready = select(0, &readfds, NULL, NULL, NULL);
        if (ready == SOCKET_ERROR) { fprintf(stderr, "select error\n"); break; }

        // new connection?
        if (FD_ISSET(listen_sock, &readfds)) {
            struct sockaddr_in cli;
            int clen = sizeof(cli);
            SOCKET cs = accept(listen_sock, (struct sockaddr*)&cli, &clen);
            if (cs != INVALID_SOCKET) {
                // find a slot
                int slot = -1;
                for (int i=0;i<MAX_CLIENTS;++i) { if (clients[i].sock == INVALID_SOCKET) { slot = i; break; } }
                if (slot < 0) {
                    const char* full = "[server] room full, try later\n";
                    send(cs, full, (int)strlen(full), 0);
                    closesocket(cs);
                } else {
                    clients[slot].sock = cs;
                    clients[slot].name[0] = 0;
                    char rip[32]; strncpy(rip, inet_ntoa(cli.sin_addr), sizeof(rip)-1); rip[sizeof(rip)-1]=0;
                    printf("New client %s:%d (slot %d)\n", rip, ntohs(cli.sin_port), slot);
                    const char* ask = "[server] send your name (first message)\n";
                    send(cs, ask, (int)strlen(ask), 0);
                }
            }
            if (--ready <= 0) continue;
        }

        // handle each client
        for (int i=0;i<MAX_CLIENTS && ready>0; ++i) {
            SOCKET s = clients[i].sock;
            if (s == INVALID_SOCKET) continue;
            if (!FD_ISSET(s, &readfds)) continue;
            ready--;

            char buf[MAXMSG+1];
            int n = recv(s, buf, MAXMSG, 0);
            if (n <= 0) {
                if (clients[i].name[0]) {
                    char bye[128];
                    snprintf(bye, sizeof(bye), "[server] %s left the chat\n", clients[i].name);
                    printf("%s", bye);
                    broadcast(clients, MAX_CLIENTS, s, bye);
                }
                closesocket(s);
                clients[i].sock = INVALID_SOCKET;
                clients[i].name[0] = 0;
                continue;
            }
            buf[n] = 0;

            if (clients[i].name[0] == 0) {
                // first message = name
                // trim leading/trailing newlines
                char* p = buf; while (*p && (*p=='\r' || *p=='\n')) p++;
                strncpy(clients[i].name, p, sizeof(clients[i].name)-1);
                clients[i].name[sizeof(clients[i].name)-1] = 0;
                for (int k=(int)strlen(clients[i].name)-1; k>=0 && (clients[i].name[k]=='\r' || clients[i].name[k]=='\n'); --k)
                    clients[i].name[k]=0;

                char joined[128];
                snprintf(joined, sizeof(joined), "[server] %s joined the chat\n", clients[i].name);
                printf("%s", joined);
                broadcast(clients, MAX_CLIENTS, s, joined);
                continue;
            }

            // normal message → broadcast
            char line[MAXMSG+128];
            snprintf(line, sizeof(line), "%s: %s", clients[i].name, buf);
            printf("%s", line);
            broadcast(clients, MAX_CLIENTS, s, line);
        }
    }

    // cleanup
    for (int i=0;i<MAX_CLIENTS;++i)
        if (clients[i].sock != INVALID_SOCKET) closesocket(clients[i].sock);
    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
