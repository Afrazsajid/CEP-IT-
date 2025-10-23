// attendance.c
// Simple CLI attendance system using SQLite3
// Build (Linux/Mac):   gcc attendance.c -lsqlite3 -o attendance
// Build (MSYS2 UCRT):  gcc attendance.c -lsqlite3 -o attendance.exe
// Run: ./attendance attendance.db

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>

#define LINE 256

static void die(sqlite3 *db, const char *msg, int rc) {
    fprintf(stderr, "[ERROR] %s (rc=%d): %s\n", msg, rc, db ? sqlite3_errmsg(db) : "no db");
    if (db) sqlite3_close(db);
    exit(1);
}

static void trim_newline(char *s){
    if(!s) return;
    size_t n=strlen(s);
    while(n>0 && (s[n-1]=='\n' || s[n-1]=='\r')) s[--n]='\0';
}

static void get_line(const char *prompt, char *buf, size_t sz){
    printf("%s", prompt);
    if (fgets(buf, (int)sz, stdin) == NULL) { buf[0]='\0'; return; }
    trim_newline(buf);
}

static void today_iso(char out[11]) {
    time_t t=time(NULL);
    struct tm *tm = localtime(&t);
    strftime(out, 11, "%Y-%m-%d", tm);
}

static void exec_ddl(sqlite3 *db, const char *sql){
    char *err=NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if(rc!=SQLITE_OK){
        fprintf(stderr, "DDL error: %s\n", err?err:"(unknown)");
        sqlite3_free(err);
        sqlite3_close(db);
        exit(1);
    }
}

static void init_schema(sqlite3 *db){
    exec_ddl(db, "PRAGMA foreign_keys = ON;");
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
}

static int get_id(sqlite3 *db, const char *sql, const char *key){
    int id = -1;
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(st)==SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

static void add_student(sqlite3 *db){
    char roll[LINE], name[LINE];
    get_line("Enter roll no: ", roll, sizeof(roll));
    get_line("Enter name   : ", name, sizeof(name));
    const char *sql = "INSERT INTO students(roll,name) VALUES(?,?)";
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if(rc!=SQLITE_OK) die(db, "prepare add_student", rc);
    sqlite3_bind_text(st,1,roll,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,name,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(st);
    if(rc!=SQLITE_DONE){ fprintf(stderr,"Could not add student (roll unique?)\n"); }
    else printf("Student added.\n");
    sqlite3_finalize(st);
}

static void add_course(sqlite3 *db){
    char code[LINE], title[LINE];
    get_line("Enter course code : ", code, sizeof(code));
    get_line("Enter course title: ", title, sizeof(title));
    const char *sql = "INSERT INTO courses(code,title) VALUES(?,?)";
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if(rc!=SQLITE_OK) die(db, "prepare add_course", rc);
    sqlite3_bind_text(st,1,code,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,title,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(st);
    if(rc!=SQLITE_DONE){ fprintf(stderr,"Could not add course (code unique?)\n"); }
    else printf("Course added.\n");
    sqlite3_finalize(st);
}

static void enroll_student(sqlite3 *db){
    char roll[LINE], code[LINE];
    get_line("Roll no       : ", roll, sizeof(roll));
    get_line("Course code   : ", code, sizeof(code));
    int sid = get_id(db, "SELECT id FROM students WHERE roll=?", roll);
    int cid = get_id(db, "SELECT id FROM courses WHERE code=?", code);
    if(sid<0){ printf("No such student.\n"); return; }
    if(cid<0){ printf("No such course.\n"); return; }

    const char *sql = "INSERT OR IGNORE INTO enrollments(student_id,course_id) VALUES(?,?)";
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if(rc!=SQLITE_OK) die(db, "prepare enroll", rc);
    sqlite3_bind_int(st,1,sid);
    sqlite3_bind_int(st,2,cid);
    rc=sqlite3_step(st);
    if(rc!=SQLITE_DONE){ fprintf(stderr,"Enroll failed.\n"); }
    else printf("Enrolled (or already enrolled).\n");
    sqlite3_finalize(st);
}

static void mark_attendance(sqlite3 *db){
    char roll[LINE], code[LINE], date[LINE], status[LINE];
    get_line("Roll no                : ", roll, sizeof(roll));
    get_line("Course code            : ", code, sizeof(code));
    get_line("Date (YYYY-MM-DD, blank=today): ", date, sizeof(date));
    if(date[0]=='\0'){ today_iso(date); }
    get_line("Status [P=Present, A=Absent, L=Late]: ", status, sizeof(status));
    if(!(status[0]=='P'||status[0]=='A'||status[0]=='L')){
        printf("Invalid status.\n"); return;
    }

    int sid = get_id(db, "SELECT id FROM students WHERE roll=?", roll);
    int cid = get_id(db, "SELECT id FROM courses WHERE code=?", code);
    if(sid<0){ printf("No such student.\n"); return; }
    if(cid<0){ printf("No such course.\n"); return; }

    // Optional: ensure student is enrolled before marking
    int enrolled = -1;
    {
        sqlite3_stmt *chk=NULL;
        sqlite3_prepare_v2(db,
            "SELECT 1 FROM enrollments WHERE student_id=? AND course_id=?",
            -1, &chk, NULL);
        sqlite3_bind_int(chk,1,sid);
        sqlite3_bind_int(chk,2,cid);
        enrolled = (sqlite3_step(chk)==SQLITE_ROW);
        sqlite3_finalize(chk);
    }
    if(!enrolled){
        printf("Student not enrolled in this course; enrolling now.\n");
        sqlite3_stmt *ins=NULL;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO enrollments(student_id,course_id) VALUES(?,?)",
            -1, &ins, NULL);
        sqlite3_bind_int(ins,1,sid);
        sqlite3_bind_int(ins,2,cid);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }

    const char *sql="INSERT INTO attendance(student_id,course_id,date,status) VALUES(?,?,?,?)";
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if(rc!=SQLITE_OK) die(db,"prepare attendance",rc);
    sqlite3_bind_int(st,1,sid);
    sqlite3_bind_int(st,2,cid);
    sqlite3_bind_text(st,3,date,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,4,status,-1,SQLITE_TRANSIENT);
    rc=sqlite3_step(st);
    if(rc!=SQLITE_DONE){ fprintf(stderr,"Could not insert attendance row.\n"); }
    else printf("Attendance recorded.\n");
    sqlite3_finalize(st);
}

static void list_students(sqlite3 *db){
    printf("\n-- Students --\n");
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(db, "SELECT roll,name FROM students ORDER BY roll", -1, &st, NULL)!=SQLITE_OK){
        printf("Query failed.\n"); return;
    }
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *roll = sqlite3_column_text(st,0);
        const unsigned char *name = sqlite3_column_text(st,1);
        printf("%-12s  %s\n", roll? (const char*)roll : "", name? (const char*)name : "");
    }
    sqlite3_finalize(st);
}

static void list_courses(sqlite3 *db){
    printf("\n-- Courses --\n");
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(db, "SELECT code,title FROM courses ORDER BY code", -1, &st, NULL)!=SQLITE_OK){
        printf("Query failed.\n"); return;
    }
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *code = sqlite3_column_text(st,0);
        const unsigned char *title = sqlite3_column_text(st,1);
        printf("%-10s  %s\n", code? (const char*)code : "", title? (const char*)title : "");
    }
    sqlite3_finalize(st);
}

static void report_attendance_by_course(sqlite3 *db){
    char code[LINE];
    get_line("Course code: ", code, sizeof(code));
    const char *sql =
        "SELECT a.date, s.roll, s.name, a.status "
        "FROM attendance a "
        "JOIN students s ON s.id=a.student_id "
        "JOIN courses c ON c.id=a.course_id "
        "WHERE c.code=? "
        "ORDER BY a.date, s.roll";
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL)!=SQLITE_OK){ printf("Query failed.\n"); return; }
    sqlite3_bind_text(st,1,code,-1,SQLITE_TRANSIENT);
    printf("\nDate        Roll         Name                         Status\n");
    printf("----------- ------------ ---------------------------- ------\n");
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *date = sqlite3_column_text(st,0);
        const unsigned char *roll = sqlite3_column_text(st,1);
        const unsigned char *name = sqlite3_column_text(st,2);
        const unsigned char *status = sqlite3_column_text(st,3);
        printf("%-11s %-12s %-28s %-1s\n",
               date? (const char*)date : "",
               roll? (const char*)roll : "",
               name? (const char*)name : "",
               status? (const char*)status : "");
    }
    sqlite3_finalize(st);
}

static void report_attendance_by_student(sqlite3 *db){
    char roll[LINE];
    get_line("Roll no: ", roll, sizeof(roll));
    const char *sql =
        "SELECT a.date, c.code, c.title, a.status "
        "FROM attendance a "
        "JOIN courses c ON c.id=a.course_id "
        "JOIN students s ON s.id=a.student_id "
        "WHERE s.roll=? "
        "ORDER BY a.date, c.code";
    sqlite3_stmt *st=NULL;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL)!=SQLITE_OK){ printf("Query failed.\n"); return; }
    sqlite3_bind_text(st,1,roll,-1,SQLITE_TRANSIENT);
    printf("\nDate        Course  Title                         Status\n");
    printf("----------- ------- ----------------------------- ------\n");
    while(sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *date = sqlite3_column_text(st,0);
        const unsigned char *code = sqlite3_column_text(st,1);
        const unsigned char *title= sqlite3_column_text(st,2);
        const unsigned char *status= sqlite3_column_text(st,3);
        printf("%-11s %-7s %-29s %-1s\n",
               date? (const char*)date : "",
               code? (const char*)code : "",
               title? (const char*)title: "",
               status? (const char*)status: "");
    }
    sqlite3_finalize(st);
}

static void menu(){
    puts("\n===== Attendance System =====");
    puts("1) Add student");
    puts("2) Add course");
    puts("3) Enroll student in course");
    puts("4) Mark attendance (by roll + course code)");
    puts("5) List students");
    puts("6) List courses");
    puts("7) Report: attendance by course");
    puts("8) Report: attendance by student");
    puts("0) Exit");
}

int main(int argc, char **argv){
    if(argc<2){
        fprintf(stderr, "Usage: %s <sqlite_db_file>\n", argv[0]);
        return 1;
    }
    sqlite3 *db=NULL;
    int rc=sqlite3_open(argv[1], &db);
    if(rc!=SQLITE_OK) die(db,"open db", rc);

    init_schema(db);

    char choice[LINE];
    for(;;){
        menu();
        get_line("Select: ", choice, sizeof(choice));
        if(choice[0]=='0') break;
        switch(choice[0]){
            case '1': add_student(db); break;
            case '2': add_course(db); break;
            case '3': enroll_student(db); break;
            case '4': mark_attendance(db); break;
            case '5': list_students(db); break;
            case '6': list_courses(db); break;
            case '7': report_attendance_by_course(db); break;
            case '8': report_attendance_by_student(db); break;
            default: puts("Invalid option.");
        }
    }
    sqlite3_close(db);
    puts("Goodbye!");
    return 0;
}
