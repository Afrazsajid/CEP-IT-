// server.c — TCP attendance server with SQLite3
// Build: gcc -std=c17 -O2 -Wall -Wextra server.c -lsqlite3 -o server
// Run:   ./server 0.0.0.0 5555 attendance.db
// Proto: "ATT|<HEX_ROLL>|<HEX_COURSE>|<HEX_ISO8601>|<HEX_STATUS>\n"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE 4096

static const char *DDL =
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS students ("
    "  student_id  INTEGER PRIMARY KEY,"
    "  roll_int    INTEGER UNIQUE,"
    "  roll_hex    TEXT UNIQUE NOT NULL,"
    "  name        TEXT NOT NULL DEFAULT 'Unknown',"
    "  department  TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS courses ("
    "  course_id   INTEGER PRIMARY KEY,"
    "  course_code TEXT UNIQUE NOT NULL,"
    "  course_hex  TEXT UNIQUE NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS enrollments ("
    "  student_id  INTEGER NOT NULL,"
    "  course_id   INTEGER NOT NULL,"
    "  PRIMARY KEY (student_id, course_id),"
    "  FOREIGN KEY(student_id) REFERENCES students(student_id),"
    "  FOREIGN KEY(course_id)  REFERENCES courses(course_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS attendance ("
    "  attendance_id INTEGER PRIMARY KEY,"
    "  student_id    INTEGER NOT NULL,"
    "  course_id     INTEGER NOT NULL,"
    "  timestamp_utc TEXT NOT NULL,"
    "  status        INTEGER NOT NULL,"
    "  raw_msg_hex   TEXT,"
    "  FOREIGN KEY(student_id) REFERENCES students(student_id),"
    "  FOREIGN KEY(course_id)  REFERENCES courses(course_id)"
    ");";

static int hex_to_bytes(const char *hex, unsigned char *out, size_t outcap) {
    size_t len = strlen(hex);
    if (len % 2) return -1;
    size_t n = len / 2;
    if (n > outcap) return -2;
    for (size_t i = 0; i < n; ++i) {
        unsigned int v;
        if (!isxdigit((unsigned char)hex[2*i]) || !isxdigit((unsigned char)hex[2*i+1]))
            return -3;
        if (sscanf(&hex[2*i], "%2x", &v) != 1) return -4;
        out[i] = (unsigned char)v;
    }
    return (int)n;
}

static int init_db(sqlite3 **pdb, const char *path) {
    if (sqlite3_open(path, pdb) != SQLITE_OK) {
        fprintf(stderr, "DB open: %s\n", sqlite3_errmsg(*pdb));
        return -1;
    }
    char *err = NULL;
    if (sqlite3_exec(*pdb, DDL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "DB init: %s\n", err);
        sqlite3_free(err);
        return -2;
    }
    return 0;
}

static int get_or_create_ids(sqlite3 *db, const char *roll_str, const char *course_code,
                             int *out_student_id, int *out_course_id) {
    sqlite3_stmt *st = NULL;
    int rc;

    // STUDENT by roll_hex(roll_str)
    rc = sqlite3_prepare_v2(db,
        "SELECT student_id FROM students WHERE roll_hex = upper(hex(?1))", -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, roll_str, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out_student_id = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    } else {
        sqlite3_finalize(st);
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO students (roll_int, roll_hex) VALUES (CAST(?1 AS INTEGER), upper(hex(?1)))",
            -1, &st, NULL);
        if (rc != SQLITE_OK) return -2;
        sqlite3_bind_text(st, 1, roll_str, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return -3; }
        sqlite3_finalize(st);
        *out_student_id = (int)sqlite3_last_insert_rowid(db);
    }

    // COURSE by course_code
    rc = sqlite3_prepare_v2(db,
        "SELECT course_id FROM courses WHERE course_code = ?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return -4;
    sqlite3_bind_text(st, 1, course_code, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out_course_id = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    } else {
        sqlite3_finalize(st);
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO courses (course_code, course_hex) VALUES (?1, upper(hex(?1)))",
            -1, &st, NULL);
        if (rc != SQLITE_OK) return -5;
        sqlite3_bind_text(st, 1, course_code, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return -6; }
        sqlite3_finalize(st);
        *out_course_id = (int)sqlite3_last_insert_rowid(db);
    }

    // ensure enrollment (best effort)
    rc = sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO enrollments (student_id, course_id) VALUES (?1, ?2)",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(st, 1, *out_student_id);
        sqlite3_bind_int(st, 2, *out_course_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return 0;
}

static int insert_attendance(sqlite3 *db, int sid, int cid, const char *ts, int status,
                             const char *raw_hex_line) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO attendance (student_id, course_id, timestamp_utc, status, raw_msg_hex) "
        "VALUES (?1, ?2, ?3, ?4, ?5)", -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, sid);
    sqlite3_bind_int(st, 2, cid);
    sqlite3_bind_text(st, 3, ts, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, status);
    sqlite3_bind_text(st, 5, raw_hex_line, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -2;
}

static int handle_line(sqlite3 *db, const char *line, char *resp, size_t rcap) {
    // ATT|HEX_ROLL|HEX_COURSE|HEX_TS|HEX_STATUS
    char tmp[MAX_LINE]; strncpy(tmp, line, sizeof(tmp)); tmp[sizeof(tmp)-1] = 0;
    char *save = NULL;

    char *verb = strtok_r(tmp, "|", &save);
    char *hroll = strtok_r(NULL, "|", &save);
    char *hcourse = strtok_r(NULL, "|", &save);
    char *hts = strtok_r(NULL, "|", &save);
    char *hstat = strtok_r(NULL, "\r\n", &save);

    if (!verb || strcmp(verb, "ATT") || !hroll || !hcourse || !hts || !hstat) {
        snprintf(resp, rcap, "ERR|BAD_FORMAT|Use ATT|HEX_ROLL|HEX_COURSE|HEX_TS|HEX_STATUS\n");
        return -1;
    }

    unsigned char broll[128], bcourse[128], bts[128], bstat[8];
    int nr = hex_to_bytes(hroll,   broll,   sizeof broll);
    int nc = hex_to_bytes(hcourse, bcourse, sizeof bcourse);
    int nt = hex_to_bytes(hts,     bts,     sizeof bts);
    int ns = hex_to_bytes(hstat,   bstat,   sizeof bstat);
    if (nr < 0 || nc < 0 || nt < 0 || ns < 0) {
        snprintf(resp, rcap, "ERR|HEX_DECODE|Invalid hex\n");
        return -2;
    }

    char roll[129]={0}, course[129]={0}, ts[129]={0};
    memcpy(roll,   broll,   (size_t)nr);
    memcpy(course, bcourse, (size_t)nc);
    memcpy(ts,     bts,     (size_t)nt);

    int status = 0; // accept ASCII '1' or byte 0x01
    if (ns == 1) status = (bstat[0] == '1' || bstat[0] == 1) ? 1 : 0;
    else         status = (memchr(bstat, '1', (size_t)ns) != NULL) ? 1 : 0;

    int sid=0, cid=0;
    if (get_or_create_ids(db, roll, course, &sid, &cid) != 0) {
        snprintf(resp, rcap, "ERR|DB_LOOKUP|IDs\n"); return -3;
    }
    if (insert_attendance(db, sid, cid, ts, status, line) != 0) {
        snprintf(resp, rcap, "ERR|DB_INSERT\n"); return -4;
    }
    snprintf(resp, rcap, "OK|Recorded\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <bind_ip> <port> <sqlite_db_path>\n", argv[0]);
        return 1;
    }
    const char *bind_ip = argv[1]; int port = atoi(argv[2]); const char *dbp = argv[3];

    sqlite3 *db = NULL; if (init_db(&db, dbp) != 0) return 1;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) { fprintf(stderr,"bad IP\n"); return 1; }
    if (bind(srv, (struct sockaddr*)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }
    printf("Server listening on %s:%d, DB=%s\n", bind_ip, port, dbp);

    fd_set master, rfds; FD_ZERO(&master); FD_SET(srv, &master); int fdmax = srv;
    char buf[MAX_LINE], resp[256];

    for (;;) {
        rfds = master;
        if (select(fdmax+1, &rfds, NULL, NULL, NULL) < 0) { perror("select"); break; }
        for (int fd = 0; fd <= fdmax; ++fd) if (FD_ISSET(fd, &rfds)) {
            if (fd == srv) {
                int cfd = accept(srv, NULL, NULL);
                if (cfd >= 0) { FD_SET(cfd, &master); if (cfd > fdmax) fdmax = cfd; }
            } else {
                ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
                if (n <= 0) { close(fd); FD_CLR(fd, &master); continue; }
                buf[n] = 0;
                if (handle_line(db, buf, resp, sizeof resp) == 0)
                    send(fd, resp, strlen(resp), 0);
                else
                    send(fd, resp, strlen(resp), 0);
            }
        }
    }
    close(srv); sqlite3_close(db); return 0;
}
