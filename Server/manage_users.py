import sys
import sqlite3
import secrets
import hashlib
import os

DB_PATH = os.path.join(os.path.dirname(__file__), "wrath_users.db")

def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("""
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT UNIQUE NOT NULL COLLATE NOCASE,
            password_hash TEXT NOT NULL,
            salt TEXT NOT NULL,
            hwid TEXT DEFAULT NULL,
            is_active INTEGER DEFAULT 1,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        )
    """)
    conn.commit()
    return conn

def hash_password(password: str, salt: str) -> str:
    return hashlib.sha256((password + salt).encode("utf-8")).hexdigest()

def add_user(username, password):
    salt = secrets.token_hex(16)
    pwd_hash = hash_password(password, salt)
    try:
        with get_db() as conn:
            conn.execute(
                "INSERT INTO users (username, password_hash, salt) VALUES (?, ?, ?)",
                (username, pwd_hash, salt)
            )
            conn.commit()
            print(f"[OK] Usuario '{username}' creado exitosamente.")
    except sqlite3.IntegrityError:
        print(f"[ERROR] El usuario '{username}' ya existe.")

def reset_hwid(username):
    with get_db() as conn:
        cursor = conn.execute("UPDATE users SET hwid = NULL WHERE username = ? COLLATE NOCASE", (username,))
        conn.commit()
        if cursor.rowcount > 0:
            print(f"[OK] HWID de '{username}' reseteado. Podrá vincular una nueva PC al iniciar sesión.")
        else:
            print(f"[ERROR] Usuario '{username}' no encontrado.")

def list_users():
    with get_db() as conn:
        users = conn.execute("SELECT id, username, hwid, is_active, created_at FROM users").fetchall()
        print("\n" + "=" * 70)
        print(f"{'ID':<4} | {'USUARIO':<15} | {'ESTADO':<8} | {'HWID VINCULADO':<30}")
        print("-" * 70)
        for u in users:
            hwid = u["hwid"] if u["hwid"] else "[SIN VINCULAR]"
            status = "ACTIVO" if u["is_active"] else "SUSPENDIDO"
            print(f"{u['id']:<4} | {u['username']:<15} | {status:<8} | {hwid:<30}")
        print("=" * 70 + "\n")

def delete_user(username):
    with get_db() as conn:
        cursor = conn.execute("DELETE FROM users WHERE username = ? COLLATE NOCASE", (username,))
        conn.commit()
        if cursor.rowcount > 0:
            print(f"[OK] Usuario '{username}' eliminado.")
        else:
            print(f"[ERROR] Usuario '{username}' no encontrado.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Uso:")
        print("  python manage_users.py add <usuario> <contraseña>  -> Crear nuevo usuario")
        print("  python manage_users.py list                        -> Listar todos los usuarios")
        print("  python manage_users.py reset_hwid <usuario>        -> Resetear HWID para cambio de PC")
        print("  python manage_users.py delete <usuario>            -> Eliminar usuario")
        sys.exit(0)

    cmd = sys.argv[1].lower()
    if cmd == "add" and len(sys.argv) >= 4:
        add_user(sys.argv[2], sys.argv[3])
    elif cmd == "reset_hwid" and len(sys.argv) >= 3:
        reset_hwid(sys.argv[2])
    elif cmd == "delete" and len(sys.argv) >= 3:
        delete_user(sys.argv[2])
    elif cmd == "list":
        list_users()
    else:
        print("[ERROR] Argumentos inválidos. Ejecuta 'python manage_users.py' para ver la ayuda.")
