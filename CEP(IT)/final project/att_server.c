// att_server.c  — Networked Attendance Server (SQLite + Winsock, hex protocol)
// Build:  gcc att_server.c -lsqlite3 -lws2_32 -o att_server.exe
// Run:    att_server.exe 0.0.0.0 5555 attendance.db
//
// Protocol (client -> server, one command per line):
//   OPCODE <space> HEX_PAYLOAD \n
//   Payload (after hex-decoding) is fields joined by '|'
//   OPCODES & expected payload (ASCII before hex):
//     ADD_STUDENT:     "ROLL|NAME"
//     ADD_COURSE:      "CODE|TITLE"
//     ENROLL:          "ROLL|CODE"
//     MARK:            "ROLL|CODE|YYYY-MM-DD|STATUS"   STATUS in {P,A,L}
//     REPORT_BY_ROLL:  "ROLL"                          (server returns lines)
//     REPORT_BY_CODE:  "CODE"                          (server returns lines)
//     LIST_STUDENTS:   ""                              (no payload)
//     LIST_COURSES:    ""
// Server replies (text):
//   OK\n                            on success without rows
//   ERR:<message>\n                 on failure
//   For listing/report: rows (one per line) then ".\n" sentinel

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#pragma comment(lib, "ws2_32.lib")

#define MAXLINE 2048
#define MAX_CLIENTS 128

typedef struct {
    SOCKET sock;
    char   ibuf[MAXLINE];
} Client;

static void die(const char* m) { fprintf(stderr, "%s\n", m); exit(1); }

static int hex_to_bytes(const char* hex, unsigned char* out, int outcap){
    // returns number of bytes written, or -1 on bad hex
    int n = (int)strlen(hex);
    if(n==0) return 0;
    if(n%2) return -1;
    int w=0;
    for(int i=0;i<n;i+=2){
        if(w>=outcap) return -1;
        char a=hex[i], b=hex[i+1];
        int v=0;
        #define H(x) ((x>='0'&&x<='9')?(x-'0'): (x>='a'&&x<='f')?(x-'a'+10): (x>='A'&&x<='F')?(x-'A'+10):-1)
        int hi=H(a), lo=H(b);
        if(hi<0||lo<0) return -1;
        v=(hi<<4)|lo;
        out[w++]=(unsigned char)v;
    }
    return w;
}

static void send_line(SOCKET s, const char* line){
    send(s, line, (int)strlen(line), 0);
}

static void exec_ddl(sqlite3* db, const char* sql){
    char* err=NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if(rc!=SQLITE_OK){
        fprintf(stderr,"DDL error: %s\n", err?err:"(unknown)");
        sqlite3_free(err);
        sqlite3_close(db);
        exit(1);
    }
}

static void init_schema(sqlite3* db){
    exec_ddl(db, "PRAGMA foreign_keys=ON;");
    exec_ddl(db,
        "CREATE TABLE IF NOT EXISTS students ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  roll TEXT NOT NULL UNIQUE,"
        "  name TEXT NOT NULL"
        ");"
    );
    exec_ddl(db,
        "CREATE TABLE IF NOT EXISTS courses ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  code TEXT NOT NULL UNIQUE,"
        "  title TEXT NOT NULL"
        ");"
    );
    exec_ddl(db,
        "CREATE TABLE IF NOT EXISTS enrollments ("
        "  student_id INTEGER NOT NULL,"
        "  course_id  INTEGER NOT NULL,"
        "  PRIMARY KEY(student_id, course_id),"
        "  FOREIGN KEY(student_id) REFERENCES students(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(course_id) REFERENCES courses(id) ON DELETE CASCADE"
        ");"
    );
    exec_ddl(db,
        "CREATE TABLE IF NOT EXISTS attendance ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  student_id INTEGER NOT NULL,"
        "  course_id  INTEGER NOT NULL,"
        "  date TEXT NOT NULL,"
        "  status TEXT NOT NULL CHECK(status IN('P','A','L')),"
        "  FOREIGN KEY(student_id) REFERENCES students(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(course_id)  REFERENCES courses(id) ON DELETE CASCADE"
        ");"
    );
    exec_ddl(db,
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_att_unique "
        "ON attendance(student_id, course_id, date);"
    );
}

static int get_id(sqlite3* db, const char* sql, const char* key){
    int id=-1; sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(st,1,key,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(st)==SQLITE_ROW) id = sqlite3_column_int(st,0);
    sqlite3_finalize(st);
    return id;
}

static void handle_add_student(sqlite3* db, SOCKET s, const char* roll, const char* name){
    sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db,"INSERT INTO students(roll,name) VALUES(?,?)",-1,&st,NULL)!=SQLITE_OK){
        send_line(s,"ERR:prepare add student\n"); return;
    }
    sqlite3_bind_text(st,1,roll,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,name,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc!=SQLITE_DONE) send_line(s,"ERR:insert student (roll may exist)\n");
    else send_line(s,"OK\n");
}

static void handle_add_course(sqlite3* db, SOCKET s, const char* code, const char* title){
    sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db,"INSERT INTO courses(code,title) VALUES(?,?)",-1,&st,NULL)!=SQLITE_OK){
        send_line(s,"ERR:prepare add course\n"); return;
    }
    sqlite3_bind_text(st,1,code,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,title,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc!=SQLITE_DONE) send_line(s,"ERR:insert course (code may exist)\n");
    else send_line(s,"OK\n");
}

static void handle_enroll(sqlite3* db, SOCKET s, const char* roll, const char* code){
    int sid=get_id(db,"SELECT id FROM students WHERE roll=?", roll);
    int cid=get_id(db,"SELECT id FROM courses WHERE code=?", code);
    if(sid<0){ send_line(s,"ERR:no such student\n"); return; }
    if(cid<0){ send_line(s,"ERR:no such course\n"); return; }
    sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO enrollments(student_id,course_id) VALUES(?,?)",-1,&st,NULL)!=SQLITE_OK){
        send_line(s,"ERR:prepare enroll\n"); return;
    }
    sqlite3_bind_int(st,1,sid); sqlite3_bind_int(st,2,cid);
    int rc=sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc!=SQLITE_DONE) send_line(s,"ERR:enroll failed\n");
    else send_line(s,"OK\n");
}

static void handle_mark(sqlite3* db, SOCKET s, const char* roll, const char* code, const char* date, const char* status){
    if(!(status && (status[0]=='P'||status[0]=='A'||status[0]=='L') && status[1]=='\0')){
        send_line(s,"ERR:bad status\n"); return;
    }
    int sid=get_id(db,"SELECT id FROM students WHERE roll=?", roll);
    int cid=get_id(db,"SELECT id FROM courses WHERE code=?", code);
    if(sid<0){ send_line(s,"ERR:no such student\n"); return; }
    if(cid<0){ send_line(s,"ERR:no such course\n"); return; }

    // ensure enrollment
    sqlite3_stmt* chk=NULL;
    sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO enrollments(student_id,course_id) VALUES(?,?)",-1,&chk,NULL);
    sqlite3_bind_int(chk,1,sid); sqlite3_bind_int(chk,2,cid);
    sqlite3_step(chk); sqlite3_finalize(chk);

    sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db,"INSERT INTO attendance(student_id,course_id,date,status) VALUES(?,?,?,?)",-1,&st,NULL)!=SQLITE_OK){
        send_line(s,"ERR:prepare mark\n"); return;
    }
    sqlite3_bind_int(st,1,sid);
    sqlite3_bind_int(st,2,cid);
    sqlite3_bind_text(st,3,date,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,4,status,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc!=SQLITE_DONE) send_line(s,"ERR:insert attendance (duplicate day?)\n");
    else send_line(s,"OK\n");
}

static void handle_list_students(sqlite3* db, SOCKET s){
    sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db,"SELECT roll,name FROM students ORDER BY roll",-1,&st,NULL)!=SQLITE_OK){
        send_line(s,"ERR:query\n"); return;
    }
    char line[512];
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *roll=sqlite3_column_text(st,0);
        const unsigned char *name=sqlite3_column_text(st,1);
        snprintf(line,sizeof(line),"%s | %s\n", roll? (const char*)roll:"", name? (const char*)name:"");
        send_line(s,line);
    }
    sqlite3_finalize(st);
    send_line(s,".\n");
}

static void handle_list_courses(sqlite3* db, SOCKET s){
    sqlite3_stmt* st=NULL;
    if(sqlite3_prepare_v2(db,"SELECT code,title FROM courses ORDER BY code",-1,&st,NULL)!=SQLITE_OK){
        send_line(s,"ERR:query\n"); return;
    }
    char line[512];
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *code=sqlite3_column_text(st,0);
        const unsigned char *title=sqlite3_column_text(st,1);
        snprintf(line,sizeof(line),"%s | %s\n", code? (const char*)code:"", title? (const char*)title:"");
        send_line(s,line);
    }
    sqlite3_finalize(st);
    send_line(s,".\n");
}

static void handle_report_by_roll(sqlite3* db, SOCKET s, const char* roll){
    sqlite3_stmt* st=NULL;
    const char* sql=
      "SELECT a.date,c.code,c.title,a.status "
      "FROM attendance a JOIN courses c ON c.id=a.course_id "
      "JOIN students s ON s.id=a.student_id "
      "WHERE s.roll=? ORDER BY a.date,c.code";
    if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)!=SQLITE_OK){ send_line(s,"ERR:query\n"); return; }
    sqlite3_bind_text(st,1,roll,-1,SQLITE_TRANSIENT);
    char line[512];
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *date=sqlite3_column_text(st,0);
        const unsigned char *code=sqlite3_column_text(st,1);
        const unsigned char *title=sqlite3_column_text(st,2);
        const unsigned char *status=sqlite3_column_text(st,3);
        snprintf(line,sizeof(line),"%s | %s | %s | %s\n",
            date? (const char*)date:"", code? (const char*)code:"",
            title? (const char*)title:"", status? (const char*)status:"");
        send_line(s,line);
    }
    sqlite3_finalize(st);
    send_line(s,".\n");
}

static void handle_report_by_code(sqlite3* db, SOCKET s, const char* code){
    sqlite3_stmt* st=NULL;
    const char* sql=
      "SELECT a.date,s.roll,s.name,a.status "
      "FROM attendance a JOIN students s ON s.id=a.student_id "
      "JOIN courses c ON c.id=a.course_id "
      "WHERE c.code=? ORDER BY a.date,s.roll";
    if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)!=SQLITE_OK){ send_line(s,"ERR:query\n"); return; }
    sqlite3_bind_text(st,1,code,-1,SQLITE_TRANSIENT);
    char line[512];
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *date=sqlite3_column_text(st,0);
        const unsigned char *roll=sqlite3_column_text(st,1);
        const unsigned char *name=sqlite3_column_text(st,2);
        const unsigned char *status=sqlite3_column_text(st,3);
        snprintf(line,sizeof(line),"%s | %s | %s | %s\n",
            date? (const char*)date:"", roll? (const char*)roll:"",
            name? (const char*)name:"", status? (const char*)status:"");
        send_line(s,line);
    }
    sqlite3_finalize(st);
    send_line(s,".\n");
}

static void process_command(sqlite3* db, SOCKET s, const char* line){
    // line format: OPCODE SP HEX\n
    char op[64]; const char* sp = strchr(line,' ');
    if(sp){
        size_t n = (size_t)(sp - line);
        if(n >= sizeof(op)) { send_line(s,"ERR:bad opcode\n"); return; }
        memcpy(op, line, n); op[n]=0;
    }else{
        strncpy(op, line, sizeof(op)-1); op[sizeof(op)-1]=0;
    }
    // strip trailing newline(s)
    for(int i=(int)strlen(op)-1; i>=0 && (op[i]=='\r'||op[i]=='\n'); --i) op[i]=0;

    unsigned char payload[MAXLINE];
    int plen=0;
    if(sp){
        const char* hex = sp+1;
        // trim newline
        int L = (int)strlen(hex);
        while(L>0 && (hex[L-1]=='\r'||hex[L-1]=='\n')) L--;
        char tmp[MAXLINE*2+4];
        if(L >= (int)sizeof(tmp)) { send_line(s,"ERR:payload too big\n"); return; }
        memcpy(tmp, hex, L); tmp[L]=0;
        plen = hex_to_bytes(tmp, payload, (int)sizeof(payload));
        if(plen<0){ send_line(s,"ERR:bad hex\n"); return; }
    }

    // make a modifiable ASCII payload string
    char *pstr=NULL;
    if(plen>0){
        pstr=(char*)malloc(plen+1);
        memcpy(pstr,payload,plen); pstr[plen]=0;
    }else{
        pstr=strdup("");
    }

    // split by '|'
    char* fields[8]; int fcnt=0;
    char* save=NULL;
    char* tok = strtok_s(pstr, "|", &save);
    while(tok && fcnt<8){ fields[fcnt++]=tok; tok=strtok_s(NULL,"|",&save); }

    if(strcmp(op,"ADD_STUDENT")==0){
        if(fcnt!=2) send_line(s,"ERR:need ROLL|NAME\n");
        else handle_add_student(db,s,fields[0],fields[1]);
    }else if(strcmp(op,"ADD_COURSE")==0){
        if(fcnt!=2) send_line(s,"ERR:need CODE|TITLE\n");
        else handle_add_course(db,s,fields[0],fields[1]);
    }else if(strcmp(op,"ENROLL")==0){
        if(fcnt!=2) send_line(s,"ERR:need ROLL|CODE\n");
        else handle_enroll(db,s,fields[0],fields[1]);
    }else if(strcmp(op,"MARK")==0){
        if(fcnt!=4) send_line(s,"ERR:need ROLL|CODE|DATE|STATUS\n");
        else handle_mark(db,s,fields[0],fields[1],fields[2],fields[3]);
    }else if(strcmp(op,"LIST_STUDENTS")==0){
        handle_list_students(db,s);
    }else if(strcmp(op,"LIST_COURSES")==0){
        handle_list_courses(db,s);
    }else if(strcmp(op,"REPORT_BY_ROLL")==0){
        if(fcnt!=1) send_line(s,"ERR:need ROLL\n");
        else handle_report_by_roll(db,s,fields[0]);
    }else if(strcmp(op,"REPORT_BY_CODE")==0){
        if(fcnt!=1) send_line(s,"ERR:need CODE\n");
        else handle_report_by_code(db,s,fields[0]);
    }else{
        send_line(s,"ERR:unknown opcode\n");
    }

    free(pstr);
}

int main(int argc, char** argv){
    if(argc!=4){
        fprintf(stderr,"Usage: %s <bind-ip> <port> <sqlite_db>\n", argv[0]);
        return 1;
    }
    const char* bind_ip=argv[1]; int port=atoi(argv[2]); const char* dbfile=argv[3];

    sqlite3* db=NULL;
    if(sqlite3_open(dbfile,&db)!=SQLITE_OK) die("open db failed");
    init_schema(db);

    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)!=0) die("WSAStartup failed");
    SOCKET ls = socket(AF_INET,SOCK_STREAM,0); if(ls==INVALID_SOCKET) die("socket failed");
    int opt=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,(char*)&opt,sizeof(opt));

    struct sockaddr_in addr; addr.sin_family=AF_INET; addr.sin_port=htons((u_short)port);
    addr.sin_addr.s_addr=inet_addr(bind_ip);
    if(bind(ls,(struct sockaddr*)&addr,sizeof(addr))==SOCKET_ERROR) die("bind failed");
    if(listen(ls,SOMAXCONN)==SOCKET_ERROR) die("listen failed");
    printf("Attendance server on %s:%d DB=%s\n", bind_ip, port, dbfile);

    Client clients[MAX_CLIENTS];
    for(int i=0;i<MAX_CLIENTS;++i){ clients[i].sock=INVALID_SOCKET; clients[i].ibuf[0]=0; }

    fd_set rset;
    while(1){
        FD_ZERO(&rset); FD_SET(ls,&rset);
        SOCKET maxfd=ls;
        for(int i=0;i<MAX_CLIENTS;++i) if(clients[i].sock!=INVALID_SOCKET){ FD_SET(clients[i].sock,&rset); if(clients[i].sock>maxfd) maxfd=clients[i].sock; }
        int ready=select(0,&rset,NULL,NULL,NULL);
        if(ready==SOCKET_ERROR){ fprintf(stderr,"select error\n"); break; }

        if(FD_ISSET(ls,&rset)){
            struct sockaddr_in cli; int clen=sizeof(cli);
            SOCKET cs = accept(ls,(struct sockaddr*)&cli,&clen);
            if(cs!=INVALID_SOCKET){
                int slot=-1; for(int i=0;i<MAX_CLIENTS;++i){ if(clients[i].sock==INVALID_SOCKET){ slot=i; break; } }
                if(slot<0){ const char* msg="ERR:server full\n"; send(cs,msg,(int)strlen(msg),0); closesocket(cs); }
                else { clients[slot].sock=cs; clients[slot].ibuf[0]=0; }
            }
            if(--ready<=0) continue;
        }

        for(int i=0;i<MAX_CLIENTS && ready>0; ++i){
            SOCKET s=clients[i].sock; if(s==INVALID_SOCKET) continue;
            if(!FD_ISSET(s,&rset)) continue; ready--;

            char buf[1024]; int n=recv(s,buf,sizeof(buf)-1,0);
            if(n<=0){ closesocket(s); clients[i].sock=INVALID_SOCKET; clients[i].ibuf[0]=0; continue; }
            buf[n]=0;

            // simple line-based: assume one command per line
            // (for robust framing, accumulate until '\n')
            char line[MAXLINE];
            snprintf(line,sizeof(line),"%s",buf);
            process_command(db,s,line);
        }
    }

    closesocket(ls);
    WSACleanup();
    sqlite3_close(db);
    return 0;
}
