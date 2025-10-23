// att_client.c — Menu Client (hex-encodes payloads, talks to server)
// Build:  gcc att_client.c -lws2_32 -o att_client.exe
// Run:    att_client.exe <server-ip> <port>
// Example: att_client.exe 192.168.100.6 5555

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define MAXLINE 2048

static void trim(char* s){
    int n=(int)strlen(s);
    while(n>0 && (s[n-1]=='\r'||s[n-1]=='\n')) s[--n]=0;
}

static void get_line(const char* p, char* buf, size_t sz){
    printf("%s", p);
    if(!fgets(buf,(int)sz,stdin)) { buf[0]=0; return; }
    trim(buf);
}

static void bytes_to_hex(const unsigned char* in, int n, char* out, int outcap){
    static const char* H="0123456789abcdef";
    int j=0;
    for(int i=0;i<n && j+2<outcap;i++){
        unsigned char b=in[i];
        out[j++]=H[b>>4];
        out[j++]=H[b&0xF];
    }
    out[j]=0;
}

static void send_cmd(SOCKET s, const char* op, const char* ascii_payload){
    char hex[MAXLINE*2+4];
    bytes_to_hex((const unsigned char*)ascii_payload, (int)strlen(ascii_payload), hex, (int)sizeof(hex));
    char line[MAXLINE*2+64];
    if(ascii_payload[0]) snprintf(line,sizeof(line),"%s %s\n", op, hex);
    else snprintf(line,sizeof(line),"%s\n", op);
    send(s, line, (int)strlen(line), 0);
}

static void read_until_dot(SOCKET s){
    // Reads lines until a single "."
    char buf[1024]; int totalLines=0;
    while(1){
        int n=recv(s,buf,sizeof(buf)-1,0);
        if(n<=0){ puts("[disconnected]"); exit(0); }
        buf[n]=0;
        // simple splitter
        char* p=buf;
        while(*p){
            char* e=strchr(p,'\n');
            if(e){ *e=0; }
            if(strcmp(p,".")==0){ if(e) puts(""); return; }
            puts(p);
            totalLines++;
            if(!e) break;
            p=e+1;
        }
        if(totalLines==0) break;
    }
}

static void read_simple_reply(SOCKET s){
    char buf[1024]; int n=recv(s,buf,sizeof(buf)-1,0);
    if(n<=0){ puts("[disconnected]"); exit(0); }
    buf[n]=0; fputs(buf,stdout);
}

static void menu(){
    puts("\n===== Network Attendance (Client) =====");
    puts("1) Add student");
    puts("2) Add course");
    puts("3) Enroll student in course");
    puts("4) Mark attendance");
    puts("5) List students");
    puts("6) List courses");
    puts("7) Report by student (roll)");
    puts("8) Report by course (code)");
    puts("0) Quit");
}

int main(int argc, char** argv){
    if(argc!=3){
        fprintf(stderr,"Usage: %s <server-ip> <port>\n", argv[0]);
        return 1;
    }
    const char* host=argv[1]; int port=atoi(argv[2]);

    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)!=0){ fputs("WSAStartup failed\n",stderr); return 1; }
    SOCKET s=socket(AF_INET,SOCK_STREAM,0); if(s==INVALID_SOCKET){ fputs("socket failed\n",stderr); return 1; }
    struct sockaddr_in srv; srv.sin_family=AF_INET; srv.sin_port=htons((u_short)port); srv.sin_addr.s_addr=inet_addr(host);
    if(connect(s,(struct sockaddr*)&srv,sizeof(srv))==SOCKET_ERROR){ fputs("connect failed (IP/port/firewall?)\n",stderr); closesocket(s); WSACleanup(); return 1; }

    char choice[16];
    for(;;){
        menu();
        get_line("Select: ", choice, sizeof(choice));
        if(choice[0]=='0') break;
        if(choice[0]=='1'){
            char roll[128], name[256];
            get_line("Roll: ", roll, sizeof(roll));
            get_line("Name: ", name, sizeof(name));
            char payload[MAXLINE]; snprintf(payload,sizeof(payload),"%s|%s", roll, name);
            send_cmd(s,"ADD_STUDENT",payload); read_simple_reply(s);
        }else if(choice[0]=='2'){
            char code[64], title[256];
            get_line("Course code: ", code, sizeof(code));
            get_line("Course title: ", title, sizeof(title));
            char payload[MAXLINE]; snprintf(payload,sizeof(payload),"%s|%s", code, title);
            send_cmd(s,"ADD_COURSE",payload); read_simple_reply(s);
        }else if(choice[0]=='3'){
            char roll[128], code[64];
            get_line("Roll: ", roll, sizeof(roll));
            get_line("Course code: ", code, sizeof(code));
            char payload[MAXLINE]; snprintf(payload,sizeof(payload),"%s|%s", roll, code);
            send_cmd(s,"ENROLL",payload); read_simple_reply(s);
        }else if(choice[0]=='4'){
            char roll[128], code[64], date[32], status[8];
            get_line("Roll: ", roll, sizeof(roll));
            get_line("Course code: ", code, sizeof(code));
            get_line("Date (YYYY-MM-DD): ", date, sizeof(date));
            get_line("Status [P/A/L]: ", status, sizeof(status));
            char payload[MAXLINE]; snprintf(payload,sizeof(payload),"%s|%s|%s|%s", roll, code, date, status);
            send_cmd(s,"MARK",payload); read_simple_reply(s);
        }else if(choice[0]=='5'){
            send_cmd(s,"LIST_STUDENTS",""); read_until_dot(s);
        }else if(choice[0]=='6'){
            send_cmd(s,"LIST_COURSES",""); read_until_dot(s);
        }else if(choice[0]=='7'){
            char roll[128]; get_line("Roll: ", roll, sizeof(roll));
            send_cmd(s,"REPORT_BY_ROLL",roll); read_until_dot(s);
        }else if(choice[0]=='8'){
            char code[64]; get_line("Course code: ", code, sizeof(code));
            send_cmd(s,"REPORT_BY_CODE",code); read_until_dot(s);
        }else{
            puts("Invalid option.");
        }
    }

    closesocket(s); WSACleanup(); puts("Bye!");
    return 0;
}
