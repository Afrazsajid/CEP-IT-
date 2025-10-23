// client.c — ncurses TUI client that hex-encodes and sends attendance
// Build: gcc -std=c17 -O2 -Wall -Wextra client.c -lncurses -o client
// Run:   ./client <server_ip> 5555

#include <arpa/inet.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void bytes_to_hex(const unsigned char *in, size_t len, char *out, size_t outcap) {
    static const char *H = "0123456789ABCDEF";
    if (outcap < (len * 2 + 1)) { if (outcap) out[0] = 0; return; }
    for (size_t i = 0; i < len; ++i) {
        out[2*i]   = H[(in[i] >> 4) & 0xF];
        out[2*i+1] = H[in[i] & 0xF];
    }
    out[len*2] = 0;
}

static void utc_iso(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm gm; gmtime_r(&t, &gm);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &gm);
}

static int connect_tcp(const char *ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -2; }
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return -3; }
    return s;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,"Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1]; int port = atoi(argv[2]);
    int sock = connect_tcp(ip, port);
    if (sock < 0) { perror("connect"); return 1; }

    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
    int row, col; getmaxyx(stdscr, row, col);

    char roll[64], course[64], status[8], ts[64];
    char hroll[256], hcourse[256], hts[256], hstat[32];
    char line[1024], srv[256];

    for (;;) {
        clear();
        mvprintw(1, 2, "Networked Attendance Client (press F10 or type 'q' in Roll to quit)");
        mvprintw(3, 2, "Roll number: ");
        mvprintw(4, 2, "Course code: ");
        mvprintw(5, 2, "Status (1=Present, 0=Absent): ");
        mvprintw(7, 2, "Enter to submit");

        move(3, 16); echo(); getnstr(roll, sizeof roll - 1); noecho();
        if (roll[0]=='q' || roll[0]=='Q') break;

        move(4, 16); echo(); getnstr(course, sizeof course - 1); noecho();
        move(5, 31); echo(); getnstr(status, sizeof status - 1); noecho();

        utc_iso(ts, sizeof ts);

        bytes_to_hex((unsigned char*)roll,   strlen(roll),   hroll,   sizeof hroll);
        bytes_to_hex((unsigned char*)course, strlen(course), hcourse, sizeof hcourse);
        bytes_to_hex((unsigned char*)ts,     strlen(ts),     hts,     sizeof hts);
        bytes_to_hex((unsigned char*)status, strlen(status), hstat,   sizeof hstat);

        snprintf(line, sizeof line, "ATT|%s|%s|%s|%s\n", hroll, hcourse, hts, hstat);

        ssize_t n = send(sock, line, strlen(line), 0);
        if (n <= 0) { mvprintw(9,2,"Send failed (connection lost)."); getch(); break; }

        int rn = recv(sock, srv, sizeof srv - 1, 0);
        if (rn > 0) { srv[rn] = 0; mvprintw(9,2, "Server: %s", srv); }
        else        { mvprintw(9,2, "No server response."); }

        mvprintw(11,2,"Press any key to continue...");
        int k = getch(); if (k == KEY_F(10)) break;
    }

    endwin(); close(sock); return 0;
}
