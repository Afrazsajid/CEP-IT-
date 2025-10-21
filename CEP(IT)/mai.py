# attendance_app.py
# Student Attendance Management System (Tkinter + SQLite)
# UI: Add Student (left), Mark Attendance (top-right), Records (bottom-right)

import tkinter as tk
from tkinter import ttk, messagebox
import sqlite3
from datetime import datetime, date

DB_FILE = "attendance.db"

# ----------------------------- Database Layer -----------------------------
class Database:
    def __init__(self, db_path=DB_FILE):
        self.conn = sqlite3.connect(db_path)
        self.conn.row_factory = sqlite3.Row
        self.create_tables()

    def create_tables(self):
        cur = self.conn.cursor()
        # Students table
        cur.execute("""
            CREATE TABLE IF NOT EXISTS students (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                roll_number TEXT UNIQUE NOT NULL,
                department TEXT
            )
        """)
        # Attendance table (1 record per student per date)
        cur.execute("""
            CREATE TABLE IF NOT EXISTS attendance (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                student_id INTEGER NOT NULL,
                date TEXT NOT NULL,
                status TEXT NOT NULL CHECK(status IN ('Present','Absent')),
                timestamp TEXT NOT NULL DEFAULT (datetime('now')),
                UNIQUE(student_id, date) ON CONFLICT REPLACE,
                FOREIGN KEY(student_id) REFERENCES students(id) ON DELETE CASCADE
            )
        """)
        self.conn.commit()

    # --- Students
    def add_student(self, name: str, roll: str, dept: str) -> bool:
        try:
            self.conn.execute(
                "INSERT INTO students (name, roll_number, department) VALUES (?, ?, ?)",
                (name.strip(), roll.strip(), dept.strip()),
            )
            self.conn.commit()
            return True
        except sqlite3.IntegrityError:
            return False

    def get_student_id_by_roll(self, roll: str):
        row = self.conn.execute(
            "SELECT id FROM students WHERE roll_number = ?", (roll.strip(),)
        ).fetchone()
        return None if row is None else row["id"]

    # --- Attendance
    def mark(self, roll: str, status: str) -> bool:
        sid = self.get_student_id_by_roll(roll)
        if not sid:
            return False
        today = date.today().isoformat()
        self.conn.execute(
            "INSERT INTO attendance (student_id, date, status) VALUES (?, ?, ?)",
            (sid, today, status),
        )
        self.conn.commit()
        return True

    def fetch_records(self, only_today=False):
        if only_today:
            q = """
              SELECT a.id, s.name, s.roll_number, s.department, a.date, a.status, a.timestamp
              FROM attendance a
              JOIN students s ON s.id = a.student_id
              WHERE a.date = ?
              ORDER BY a.timestamp DESC
            """
            return self.conn.execute(q, (date.today().isoformat(),)).fetchall()
        else:
            q = """
              SELECT a.id, s.name, s.roll_number, s.department, a.date, a.status, a.timestamp
              FROM attendance a
              JOIN students s ON s.id = a.student_id
              ORDER BY a.timestamp DESC
            """
            return self.conn.execute(q).fetchall()

    def today_counts(self):
        today = date.today().isoformat()
        cur = self.conn.cursor()
        total_students = cur.execute("SELECT COUNT(*) AS c FROM students").fetchone()["c"]
        present = cur.execute(
            "SELECT COUNT(*) AS c FROM attendance WHERE date=? AND status='Present'",
            (today,),
        ).fetchone()["c"]
        absent = cur.execute(
            "SELECT COUNT(*) AS c FROM attendance WHERE date=? AND status='Absent'",
            (today,),
        ).fetchone()["c"]
        marked = cur.execute(
            "SELECT COUNT(*) AS c FROM attendance WHERE date=?",
            (today,),
        ).fetchone()["c"]
        return total_students, present, absent, marked


# ----------------------------- GUI Layer -----------------------------
class App:
    def __init__(self, root):
        self.root = root
        self.root.title("Student Attendance Management System")
        self.root.geometry("1000x600")
        self.db = Database()

        self._build_styles()
        self._build_header()
        self._build_left_add_student()
        self._build_right_mark()
        self._build_right_records()
        self._build_statusbar()

        self.refresh_records()
        self.update_counts()

    # ----- Styles / Header
    def _build_styles(self):
        self.root.configure(bg="#283844")
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TLabel", background="#283844", foreground="#EAF2F8", font=("Segoe UI", 10))
        style.configure("Header.TLabel", font=("Segoe UI Semibold", 18))
        style.configure("Small.TLabel", foreground="#C7D3DD", font=("Segoe UI", 9))
        style.configure("TButton", font=("Segoe UI", 10), padding=6)
        style.map("TButton", background=[("active", "#3c5566")])
        style.configure("Primary.TButton", background="#2ecc71", foreground="white")
        style.configure("Danger.TButton", background="#e74c3c", foreground="white")
        style.configure("Info.TButton", background="#3498db", foreground="white")
        style.configure("Card.TFrame", background="#324757", relief="ridge")
        style.configure("Plain.TFrame", background="#283844")
        style.configure("Treeview", background="#F8FBFF", fieldbackground="#F8FBFF", rowheight=26)
        style.configure("Treeview.Heading", font=("Segoe UI Semibold", 10))

    def _build_header(self):
        top = ttk.Frame(self.root, style="Plain.TFrame")
        top.pack(fill="x", pady=(10, 6), padx=10)
        ttk.Label(top, text="🎓 STUDENT ATTENDANCE MANAGEMENT SYSTEM", style="Header.TLabel").pack(side="left")
        self.date_lbl = ttk.Label(top, text=datetime.now().strftime("%A, %B %d, %Y  %H:%M:%S"), style="Small.TLabel")
        self.date_lbl.pack(side="right")
        self._tick_clock()

    def _tick_clock(self):
        self.date_lbl.config(text=datetime.now().strftime("%A, %B %d, %Y  %H:%M:%S"))
        self.root.after(1000, self._tick_clock)

    # ----- Left: Add Student
    def _build_left_add_student(self):
        wrap = ttk.Frame(self.root, style="Plain.TFrame")
        wrap.pack(side="left", fill="y", padx=(10, 6), pady=(0, 10))

        self.card_left = ttk.Frame(wrap, style="Card.TFrame")
        self.card_left.pack(fill="y", ipadx=10, ipady=10)

        title = ttk.Label(self.card_left, text="➕ ADD NEW STUDENT", font=("Segoe UI Semibold", 12))
        title.grid(row=0, column=0, columnspan=2, pady=(10, 10), padx=10, sticky="w")

        ttk.Label(self.card_left, text="Full Name:").grid(row=1, column=0, padx=12, pady=6, sticky="e")
        ttk.Label(self.card_left, text="Roll Number:").grid(row=2, column=0, padx=12, pady=6, sticky="e")
        ttk.Label(self.card_left, text="Department:").grid(row=3, column=0, padx=12, pady=6, sticky="e")

        self.name_var = tk.StringVar()
        self.roll_var = tk.StringVar()
        self.dept_var = tk.StringVar()

        ttk.Entry(self.card_left, textvariable=self.name_var, width=28).grid(row=1, column=1, padx=12, pady=6)
        ttk.Entry(self.card_left, textvariable=self.roll_var, width=28).grid(row=2, column=1, padx=12, pady=6)
        ttk.Entry(self.card_left, textvariable=self.dept_var, width=28).grid(row=3, column=1, padx=12, pady=6)

        ttk.Button(self.card_left, text="✅ ADD STUDENT",
                   style="Primary.TButton", command=self.add_student)\
            .grid(row=4, column=0, columnspan=2, padx=12, pady=(12, 16), sticky="ew")

    # ----- Right: Mark Attendance & Records
    def _build_right_mark(self):
        right = ttk.Frame(self.root, style="Plain.TFrame")
        right.pack(side="right", fill="both", expand=True, padx=(6, 10), pady=(0, 10))

        self.card_mark = ttk.Frame(right, style="Card.TFrame")
        self.card_mark.pack(fill="x", ipadx=10, ipady=10)

        ttk.Label(self.card_mark, text="🟥 MARK ATTENDANCE", font=("Segoe UI Semibold", 12))\
            .grid(row=0, column=0, columnspan=3, padx=10, pady=(10, 4), sticky="w")

        ttk.Label(self.card_mark, text="Enter Roll Number:").grid(row=1, column=0, padx=12, pady=8, sticky="e")
        self.mark_roll_var = tk.StringVar()
        ttk.Entry(self.card_mark, textvariable=self.mark_roll_var, width=22).grid(row=1, column=1, padx=6, pady=8)

        ttk.Button(self.card_mark, text="🟢 PRESENT", style="Primary.TButton",
                   command=lambda: self.mark("Present")).grid(row=1, column=2, padx=6, pady=8)
        ttk.Button(self.card_mark, text="❌ ABSENT", style="Danger.TButton",
                   command=lambda: self.mark("Absent")).grid(row=1, column=3, padx=(0, 12), pady=8)

    def _build_right_records(self):
        self.card_records = ttk.Frame(self.root, style="Card.TFrame")
        self.card_records.place(relx=0.36, rely=0.28, relwidth=0.62, relheight=0.65)  # nicely fills bottom-right

        head = ttk.Frame(self.card_records, style="Plain.TFrame")
        head.pack(fill="x", padx=6, pady=(8, 4))
        ttk.Label(head, text="📘 ATTENDANCE RECORDS", font=("Segoe UI Semibold", 11)).pack(side="left")

        self.today_only = tk.BooleanVar(value=True)
        ttk.Checkbutton(head, text="Today only", variable=self.today_only,
                        command=self.refresh_records).pack(side="left", padx=10)

        ttk.Button(head, text="↻ Refresh", style="Info.TButton",
                   command=self.refresh_records).pack(side="right", padx=6)

        # Table
        cols = ("id", "name", "roll", "dept", "date", "status", "time")
        self.tree = ttk.Treeview(self.card_records, columns=cols, show="headings", height=10)
        self.tree.pack(fill="both", expand=True, padx=8, pady=(0, 8))

        self.tree.heading("id", text="ID")
        self.tree.heading("name", text="Name")
        self.tree.heading("roll", text="Roll")
        self.tree.heading("dept", text="Department")
        self.tree.heading("date", text="Date")
        self.tree.heading("status", text="Status")
        self.tree.heading("time", text="Timestamp")

        self.tree.column("id", width=50, anchor="center")
        self.tree.column("name", width=180)
        self.tree.column("roll", width=120, anchor="center")
        self.tree.column("dept", width=120)
        self.tree.column("date", width=100, anchor="center")
        self.tree.column("status", width=90, anchor="center")
        self.tree.column("time", width=160, anchor="center")

        # Scrollbars
        ybar = ttk.Scrollbar(self.card_records, orient="vertical", command=self.tree.yview)
        ybar.place(relx=0.98, rely=0.13, relheight=0.82)
        self.tree.configure(yscrollcommand=ybar.set)

    def _build_statusbar(self):
        bar = ttk.Frame(self.root, style="Plain.TFrame")
        bar.pack(side="bottom", fill="x", padx=10, pady=(0, 10))

        self.lbl_total = ttk.Label(bar, text="Total Students: 0")
        self.lbl_present = ttk.Label(bar, text="Present (today): 0")
        self.lbl_absent = ttk.Label(bar, text="Absent (today): 0")
        self.lbl_marked = ttk.Label(bar, text="Marked (today): 0")

        for w in (self.lbl_total, self.lbl_present, self.lbl_absent, self.lbl_marked):
            w.pack(side="left", padx=12)

    # ----- Actions
    def add_student(self):
        name = self.name_var.get().strip()
        roll = self.roll_var.get().strip()
        dept = self.dept_var.get().strip()
        if not name or not roll:
            messagebox.showwarning("Validation", "Full Name and Roll Number are required.")
            return
        ok = self.db.add_student(name, roll, dept)
        if not ok:
            messagebox.showerror("Duplicate", f"Roll Number '{roll}' already exists.")
        else:
            messagebox.showinfo("Success", "Student added.")
            self.name_var.set(""); self.roll_var.set(""); self.dept_var.set("")
            self.update_counts()

    def mark(self, status: str):
        roll = self.mark_roll_var.get().strip()
        if not roll:
            messagebox.showwarning("Validation", "Please enter a roll number.")
            return
        ok = self.db.mark(roll, status)
        if not ok:
            messagebox.showerror("Not found", f"No student with roll '{roll}'. Add the student first.")
        else:
            messagebox.showinfo("Saved", f"Marked {status} for roll {roll} on {date.today():%Y-%m-%d}.")
            self.refresh_records()
            self.update_counts()
            self.mark_roll_var.set("")

    def refresh_records(self):
        # Clear
        for row in self.tree.get_children():
            self.tree.delete(row)
        # Load
        rows = self.db.fetch_records(only_today=self.today_only.get())
        for r in rows:
            self.tree.insert("", "end", values=(r["id"], r["name"], r["roll_number"], r["department"],
                                                r["date"], r["status"], r["timestamp"]))

    def update_counts(self):
        total, present, absent, marked = self.db.today_counts()
        self.lbl_total.config(text=f"Total Students: {total}")
        self.lbl_present.config(text=f"Present (today): {present}")
        self.lbl_absent.config(text=f"Absent (today): {absent}")
        self.lbl_marked.config(text=f"Marked (today): {marked}")


# ----------------------------- Run App -----------------------------
if __name__ == "__main__":
    root = tk.Tk()
    app = App(root)
    root.mainloop()
