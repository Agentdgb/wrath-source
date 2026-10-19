import os
import sys
import uuid
import secrets
import asyncio
import logging
import time
import hashlib
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from typing import Dict, Optional

from fastapi import FastAPI, HTTPException, Response, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel
import uvicorn
import discord
from discord import app_commands
from discord.ext import commands
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from dotenv import load_dotenv

load_dotenv()

logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s: %(message)s")
logger = logging.getLogger("WrathAuthServer")

DISCORD_BOT_TOKEN = os.getenv("DISCORD_BOT_TOKEN", "")
GUILD_ID = int(os.getenv("GUILD_ID", "0"))
REQUIRED_ROLE_ID = int(os.getenv("REQUIRED_ROLE_ID", "0"))
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", "8000"))
SERVER_PUBLIC_URL = os.getenv("SERVER_PUBLIC_URL", "https://localhost:8000")
PAYLOAD_SECRET_KEY = os.getenv("PAYLOAD_SECRET_KEY", "01234567890123456789012345678901")[:32].encode("utf-8")
PAYLOAD_DLL_PATH = os.path.abspath(os.getenv("PAYLOAD_DLL_PATH", os.path.join(os.path.dirname(__file__), "..", "WrathPayload.dll")))
LOADER_EXE_PATH = os.path.abspath(os.getenv("LOADER_EXE_PATH", os.path.join(os.path.dirname(__file__), "..", "WrathLoader.exe")))
DISCORD_LOG_WEBHOOK_URL = os.getenv("DISCORD_LOG_WEBHOOK_URL", "")

import aiohttp

async def send_security_webhook(title: str, description: str, color: int = 0xE61E28, fields: list = None):
    if not DISCORD_LOG_WEBHOOK_URL or not DISCORD_LOG_WEBHOOK_URL.startswith("http"):
        return
    try:
        embed = {
            "title": title,
            "description": description,
            "color": color,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "footer": {"text": "Wrath Security System"}
        }
        if fields:
            embed["fields"] = fields
        async with aiohttp.ClientSession() as session:
            await session.post(DISCORD_LOG_WEBHOOK_URL, json={"embeds": [embed]}, timeout=aiohttp.ClientTimeout(total=4))
    except Exception as e:
        logger.debug(f"Webhook error: {e}")

# ─────────────────────────────────────────────────────────────────────────────
# Modelos de Sesión y Códigos de Discord
# ─────────────────────────────────────────────────────────────────────────────
class AccessCode:
    def __init__(self, user_id: int, user_tag: str):
        self.user_id = user_id
        self.user_tag = user_tag
        self.code = secrets.token_hex(3).upper()  # Ej: "9B4F1A"
        self.created_at = datetime.now(timezone.utc)
        self.expires_at = self.created_at + timedelta(minutes=30)
        self.used = False

class ActiveSession:
    def __init__(self, session_id: str, user_id: int, hwid: str):
        self.session_id = session_id
        self.user_id = user_id
        self.hwid = hwid
        self.status = "pending"  # pending, approved, rejected, expired
        self.created_at = datetime.now(timezone.utc)
        self.expires_at = self.created_at + timedelta(minutes=5)
        self.download_token: Optional[str] = None
        self.heartbeat_token: Optional[str] = None
        self.last_heartbeat: float = time.time()
        self.sequence: int = 0

import sqlite3

DB_PATH = os.path.join(os.path.dirname(__file__), "wrath_users.db")

def get_db_connection() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=15.0)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA busy_timeout=15000")
    return conn

# Almacenes en memoria para alto rendimiento y cero bloqueos de disco
sessions: Dict[str, ActiveSession] = {}         # session_id -> ActiveSession

def init_hwid_db():
    conn = get_db_connection()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS discord_hwid_locks (
            user_id INTEGER PRIMARY KEY,
            hwid TEXT NOT NULL,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP
        )
    """)
    conn.commit()
    conn.close()

def init_session_db():
    conn = get_db_connection()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS active_sessions (
            session_id TEXT PRIMARY KEY,
            user_id INTEGER NOT NULL,
            hwid TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending',
            created_at TEXT NOT NULL,
            expires_at TEXT NOT NULL,
            download_token TEXT,
            heartbeat_token TEXT,
            last_heartbeat REAL NOT NULL,
            sequence INTEGER NOT NULL DEFAULT 0,
            client_nonce TEXT
        )
    """)
    conn.commit()
    # Cargar sesiones activas existentes a memoria
    try:
        cur = conn.execute("SELECT * FROM active_sessions")
        for row in cur.fetchall():
            s = _row_to_session(row)
            if s:
                sessions[s.session_id] = s
    except Exception:
        pass
    conn.close()

def _row_to_session(row) -> Optional[ActiveSession]:
    if not row:
        return None
    s = ActiveSession(row[0], row[1], row[2])
    s.status = row[3]
    s.created_at = datetime.fromisoformat(row[4])
    s.expires_at = datetime.fromisoformat(row[5])
    s.download_token = row[6]
    s.heartbeat_token = row[7]
    s.last_heartbeat = row[8] if row[8] is not None else time.time()
    s.sequence = row[9] if row[9] is not None else 0
    s.client_nonce = row[10]
    return s

init_hwid_db()
init_session_db()

def save_session(session: ActiveSession):
    sessions[session.session_id] = session
    try:
        conn = get_db_connection()
        conn.execute("""
            INSERT OR REPLACE INTO active_sessions
            (session_id, user_id, hwid, status, created_at, expires_at, download_token, heartbeat_token, last_heartbeat, sequence, client_nonce)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            session.session_id, session.user_id, session.hwid, session.status,
            session.created_at.isoformat(), session.expires_at.isoformat(),
            session.download_token, session.heartbeat_token, session.last_heartbeat,
            session.sequence, getattr(session, 'client_nonce', None)
        ))
        conn.commit()
        conn.close()
    except Exception as e:
        logger.error(f"[DB] Error al persistir sesión {session.session_id}: {e}")

def load_session(session_id: str) -> Optional[ActiveSession]:
    if session_id in sessions:
        return sessions[session_id]
    try:
        conn = get_db_connection()
        cur = conn.execute("SELECT * FROM active_sessions WHERE session_id = ?", (session_id,))
        row = cur.fetchone()
        conn.close()
        s = _row_to_session(row)
        if s:
            sessions[session_id] = s
        return s
    except Exception as e:
        logger.error(f"[DB] Error al cargar sesión {session_id}: {e}")
        return None

def delete_session(session_id: str):
    sessions.pop(session_id, None)
    try:
        conn = get_db_connection()
        conn.execute("DELETE FROM active_sessions WHERE session_id = ?", (session_id,))
        conn.commit()
        conn.close()
    except Exception as e:
        logger.error(f"[DB] Error al eliminar sesión {session_id}: {e}")

# Rate limiting adicional por endpoint (peticiones/minuto)
rate_limit_by_ip: Dict[str, list] = defaultdict(list)
SESSION_RATE_LIMIT = 20

def _rate_limited(ip: str) -> bool:
    now = time.time()
    window = [t for t in rate_limit_by_ip[ip] if now - t < 60]
    rate_limit_by_ip[ip] = window
    if len(window) >= SESSION_RATE_LIMIT:
        return True
    window.append(now)
    rate_limit_by_ip[ip] = window
    return False

def get_persisted_hwid(user_id: int) -> Optional[str]:
    conn = get_db_connection()
    cur = conn.execute("SELECT hwid FROM discord_hwid_locks WHERE user_id = ?", (user_id,))
    row = cur.fetchone()
    conn.close()
    return row[0] if row else None

def set_persisted_hwid(user_id: int, hwid: str):
    conn = get_db_connection()
    conn.execute("INSERT OR REPLACE INTO discord_hwid_locks (user_id, hwid) VALUES (?, ?)", (user_id, hwid))
    conn.commit()
    conn.close()

# Almacenes en memoria
code_registry: Dict[str, AccessCode] = {}       # code -> AccessCode
failed_attempts_by_ip: Dict[str, list] = defaultdict(list)  # ip -> [timestamp, ...]
SERVER_START_TIME = time.time()
AUTH_SYSTEM_ACTIVE = True  # True = Operativo / Online, False = Mantenimiento / Pausado

# ─────────────────────────────────────────────────────────────────────────────
# Discord Bot
# ─────────────────────────────────────────────────────────────────────────────
intents = discord.Intents.default()
bot = commands.Bot(command_prefix="!", intents=intents)

class DiscordAuthConfirmView(discord.ui.View):
    def __init__(self, session_id: str):
        super().__init__(timeout=180)
        self.session_id = session_id

    @discord.ui.button(label="✅ Permitir Acceso", style=discord.ButtonStyle.green)
    async def confirm(self, interaction: discord.Interaction, button: discord.ui.Button):
        session = sessions.get(self.session_id)
        if not session or session.status != "pending":
            await interaction.response.send_message("Esta solicitud ya fue procesada o expiró.", ephemeral=True)
            return

        session.status = "approved"
        session.download_token = secrets.token_urlsafe(32)
        sessions[session_id] = session
        save_session(session)
        set_persisted_hwid(session.user_id, session.hwid)

        asyncio.create_task(send_security_webhook(
            title="✅ Acceso 2FA Concedido",
            description=f"El usuario <@{session.user_id}> autorizó el acceso desde WrathLoader.",
            color=0x2ECC71,
            fields=[
                {"name": "Usuario", "value": f"<@{session.user_id}> (`{session.user_id}`)", "inline": True},
                {"name": "HWID Vinculado", "value": f"`{session.hwid[:20]}...`", "inline": True}
            ]
        ))

        for child in self.children:
            child.disabled = True
        await interaction.response.edit_message(
            content=f"✅ **Acceso Concedido.** El Loader ha sido autorizado e inyectará el cliente.\n`HWID Vinculado: {session.hwid[:20]}...`",
            view=self
        )

    @discord.ui.button(label="❌ Denegar / Bloquear", style=discord.ButtonStyle.red)
    async def deny(self, interaction: discord.Interaction, button: discord.ui.Button):
        session = sessions.get(self.session_id)
        if session:
            session.status = "rejected"
            asyncio.create_task(send_security_webhook(
                title="🚫 Acceso 2FA Denegado",
                description=f"El usuario <@{session.user_id}> bloqueó la solicitud del Loader.",
                color=0xE74C3C,
                fields=[
                    {"name": "Usuario", "value": f"<@{session.user_id}> (`{session.user_id}`)", "inline": True},
                    {"name": "HWID", "value": f"`{session.hwid[:20]}...`", "inline": True}
                ]
            ))

        for child in self.children:
            child.disabled = True
        await interaction.response.edit_message(
            content="🚫 **Acceso Rechazado.** La solicitud del Loader fue bloqueada.",
            view=self
        )

@bot.event
async def on_ready():
    logger.info(f"Bot de Discord conectado como {bot.user} (ID: {bot.user.id})")
    try:
        guild = discord.Object(id=GUILD_ID) if GUILD_ID else None
        if guild:
            bot.tree.copy_global_to(guild=guild)
            synced = await bot.tree.sync(guild=guild)
            logger.info(f"Comandos sincronizados en servidor {GUILD_ID} ({len(synced)}: {[c.name for c in synced]}).")
        else:
            synced = await bot.tree.sync()
            logger.info(f"Comandos registrados globalmente ({len(synced)}: {[c.name for c in synced]}).")
    except Exception as e:
        logger.error(f"Error registrando comandos de Discord: {e}")

@bot.tree.command(name="download", description="Descarga WrathLoader y genera tu clave temporal de acceso.")
async def cmd_download(interaction: discord.Interaction):
    user_roles = getattr(interaction.user, "roles", [])
    role_names = [r.name.lower() for r in user_roles]
    role_ids = [r.id for r in user_roles]

    logger.info(f"Usuario {interaction.user} (ID: {interaction.user.id}) ejecuto /download. Roles detectados: {[f'{r.name} ({r.id})' for r in user_roles]}")

    is_admin = False
    if hasattr(interaction.user, "guild_permissions") and interaction.user.guild_permissions.administrator:
        is_admin = True
    if interaction.guild and interaction.guild.owner_id == interaction.user.id:
        is_admin = True

    # Verificar por ID de rol, por nombre de rol ("wrath"), o permisos de Administrador/Owner
    has_role = is_admin or (REQUIRED_ROLE_ID in role_ids) or ("wrath" in role_names)
    if not has_role and REQUIRED_ROLE_ID != 0:
        await interaction.response.send_message(
            "❌ **Acceso Denegado:** No posees el rol requerido para descargar Wrath.",
            ephemeral=True
        )
        return

    # 2. Generar el código de acceso
    access = AccessCode(interaction.user.id, str(interaction.user))
    code_registry[access.code] = access

    download_url = f"{SERVER_PUBLIC_URL}/api/v1/download/loader?code={access.code}"

    embed = discord.Embed(
        title="⚡ Wrath Client — Descarga & Activación",
        description="Aquí tienes tu instalador y tu clave para iniciar sesión en el Loader.",
        color=0xE61E28
    )
    embed.add_field(name="🔑 Tu Código de Acceso", value=f"```\n{access.code}\n```", inline=False)
    embed.add_field(name="⏳ Validez", value="30 minutos (un solo uso)", inline=True)
    embed.add_field(name="📥 Descarga Directa", value=f"[Descargar WrathLoader.exe]({download_url})", inline=False)
    embed.set_footer(text="Ingresa este código en el campo de Login del Loader. Recibirás un mensaje aquí para confirmar.")

    await interaction.response.send_message(embed=embed, ephemeral=True)

@bot.tree.command(name="auth", description="Genera tu clave de autenticación para WrathLoader o consulta tu auto-login.")
async def cmd_auth(interaction: discord.Interaction):
    user_roles = getattr(interaction.user, "roles", [])
    role_names = [r.name.lower() for r in user_roles]
    role_ids   = [r.id for r in user_roles]

    is_admin = False
    if hasattr(interaction.user, "guild_permissions") and interaction.user.guild_permissions.administrator:
        is_admin = True
    if interaction.guild and interaction.guild.owner_id == interaction.user.id:
        is_admin = True

    has_role = is_admin or (REQUIRED_ROLE_ID in role_ids) or ("wrath" in role_names)
    if not has_role and REQUIRED_ROLE_ID != 0:
        await interaction.response.send_message(
            "❌ **Acceso Denegado:** No posees el rol requerido para autenticarte en Wrath.",
            ephemeral=True
        )
        return

    # Verificar si el usuario ya tiene HWID vinculado (Auto-Login habilitado)
    conn = get_db_connection()
    cur = conn.execute("SELECT hwid, created_at FROM discord_hwid_locks WHERE user_id = ?", (interaction.user.id,))
    row = cur.fetchone()
    conn.close()

    if row:
        hwid, created_at = row
        preview_hwid = f"`{hwid[:16]}...{hwid[-8:]}`"
        embed = discord.Embed(
            title="⚡ Wrath Client — Autenticación",
            description="✅ **Tu equipo ya se encuentra verificado.**",
            color=0x2ECC71
        )
        embed.add_field(name="🔒 HWID Vinculado", value=preview_hwid, inline=False)
        embed.add_field(name="⚡ Auto-Login", value="`Habilitado` (Solo ejecuta WrathLoader.exe)", inline=True)
        embed.add_field(name="📅 Vinculado", value=f"`{created_at}`", inline=True)
        embed.set_footer(text="No necesitas ingresar ningún código. Abre WrathLoader.exe y entrará directo.")
        await interaction.response.send_message(embed=embed, ephemeral=True)
        return

    # Si es nuevo equipo, generar código de acceso
    access = AccessCode(interaction.user.id, str(interaction.user))
    code_registry[access.code] = access

    embed = discord.Embed(
        title="🔑 Wrath Client — Clave de Autenticación",
        description="Ingresa este código en el campo de Login del Loader para verificar tu equipo.",
        color=0xE61E28
    )
    embed.add_field(name="🔑 Tu Código de Acceso", value=f"```\n{access.code}\n```", inline=False)
    embed.add_field(name="⏳ Validez", value="30 minutos (un solo uso)", inline=True)
    embed.add_field(name="💡 Siguiente paso", value="Pega el código en WrathLoader y dale a 'Iniciar Sesión'. Recibirás un mensaje privado aquí para confirmar.", inline=False)
    embed.set_footer(text="Una vez confirmado, tu equipo quedará vinculado y no necesitarás volver a usar códigos.")

    await interaction.response.send_message(embed=embed, ephemeral=True)

@bot.tree.command(name="resethwid", description="[Admin] Restablece el bloqueo de HWID de un usuario de Discord.")
@app_commands.describe(usuario="Usuario de Discord al que deseas restablecer el HWID")
async def cmd_resethwid(interaction: discord.Interaction, usuario: discord.User):
    is_admin = False
    if hasattr(interaction.user, "guild_permissions") and interaction.user.guild_permissions.administrator:
        is_admin = True
    if interaction.guild and interaction.guild.owner_id == interaction.user.id:
        is_admin = True

    if not is_admin:
        await interaction.response.send_message("❌ **Acceso Denegado:** Requiere permisos de Administrador.", ephemeral=True)
        return

    conn = get_db_connection()
    cur = conn.execute("DELETE FROM discord_hwid_locks WHERE user_id = ?", (usuario.id,))
    rows_affected = cur.rowcount
    conn.commit()
    conn.close()

    if rows_affected > 0:
        await interaction.response.send_message(f"✅ **HWID Restablecido:** El usuario {usuario.mention} puede vincular una nueva PC.", ephemeral=True)
        logger.info(f"Admin {interaction.user} restableció el HWID de {usuario} (ID: {usuario.id})")
        asyncio.create_task(send_security_webhook(
            title="🔄 HWID Restablecido por Administrador",
            description=f"El administrador {interaction.user.mention} liberó el HWID de {usuario.mention}.",
            color=0xF1C40F,
            fields=[
                {"name": "Admin", "value": f"{interaction.user.mention}", "inline": True},
                {"name": "Usuario", "value": f"{usuario.mention}", "inline": True}
            ]
        ))
    else:
        await interaction.response.send_message(f"ℹ️ El usuario {usuario.mention} no tenía ningún HWID bloqueado registrado.", ephemeral=True)

@bot.tree.command(name="status", description="Consulta si el servicio de Auth está activo y el estado de tu cuenta.")
async def cmd_status(interaction: discord.Interaction):
    user_id = interaction.user.id

    auth_online = AUTH_SYSTEM_ACTIVE
    service_status_text = "🟢 **ONLINE — Activo y Operativo**" if auth_online else "🔴 **MANTENIMIENTO — Servicio Temporalmente Pausado**"
    color = 0x2ECC71 if auth_online else 0xE74C3C

    uptime_sec = int(time.time() - SERVER_START_TIME)
    hours, rem = divmod(uptime_sec, 3600)
    minutes, seconds = divmod(rem, 60)
    uptime_str = f"{hours}h {minutes}m {seconds}s" if hours > 0 else f"{minutes}m {seconds}s"

    latency_ms = int(bot.latency * 1000) if bot.latency and not (bot.latency != bot.latency) else 0

    conn = get_db_connection()
    cur_total = conn.execute("SELECT count(*) FROM discord_hwid_locks")
    total_users = cur_total.fetchone()[0]

    cur = conn.execute("SELECT hwid, created_at FROM discord_hwid_locks WHERE user_id = ?", (user_id,))
    row = cur.fetchone()
    conn.close()

    now_ts = time.time()
    active_players = sum(1 for s in sessions.values() if s.status == "approved" and s.heartbeat_token and (now_ts - s.last_heartbeat < 90))

    embed = discord.Embed(
        title="⚡ Estado de Wrath Auth — Sistema & Cuenta",
        description="Monitor en tiempo real del servicio de autenticación y vinculación.",
        color=color
    )
    if interaction.user.display_avatar:
        embed.set_thumbnail(url=interaction.user.display_avatar.url)

    embed.add_field(name="🌐 Servicio de Autenticación", value=service_status_text, inline=False)
    embed.add_field(name="🤖 Bot Latencia", value=f"`{latency_ms} ms`", inline=True)
    embed.add_field(name="⏱️ Tiempo Activo (Uptime)", value=f"`{uptime_str}`", inline=True)
    embed.add_field(name="👥 Usuarios Registrados", value=f"`{total_users}`", inline=True)
    embed.add_field(name="🎮 Jugadores en Partida", value=f"`{active_players}`", inline=True)

    if row:
        hwid, created_at = row
        preview_hwid = f"`{hwid[:16]}...{hwid[-8:]}`"
        embed.add_field(name="👤 Tu Cuenta", value="✅ **Equipo Verificado** (Auto-Login disponible)", inline=False)
        embed.add_field(name="🔒 Tu HWID", value=preview_hwid, inline=True)
        embed.add_field(name="📅 Fecha de Registro", value=f"`{created_at}`", inline=True)
    else:
        embed.add_field(
            name="👤 Tu Cuenta",
            value="ℹ️ **Sin equipo vinculado todavía.** Usa `/auth` o `/download` para generar tu clave y entrar desde WrathLoader.",
            inline=False
        )

    embed.set_footer(text=f"Servidor: {SERVER_PUBLIC_URL} • Wrath Security v3.0")
    await interaction.response.send_message(embed=embed, ephemeral=True)

@bot.tree.command(name="toggleauth", description="[Admin] Activa o desactiva el servicio de autenticación (Modo Mantenimiento).")
async def cmd_toggleauth(interaction: discord.Interaction):
    is_admin = False
    if hasattr(interaction.user, "guild_permissions") and interaction.user.guild_permissions.administrator:
        is_admin = True
    if interaction.guild and interaction.guild.owner_id == interaction.user.id:
        is_admin = True

    if not is_admin:
        await interaction.response.send_message("❌ **Acceso Denegado:** Requiere permisos de Administrador.", ephemeral=True)
        return

    global AUTH_SYSTEM_ACTIVE
    AUTH_SYSTEM_ACTIVE = not AUTH_SYSTEM_ACTIVE

    estado = "🟢 **ACTIVADO (En Línea)**" if AUTH_SYSTEM_ACTIVE else "🔴 **DESACTIVADO (Mantenimiento)**"
    color = 0x2ECC71 if AUTH_SYSTEM_ACTIVE else 0xE74C3C

    embed = discord.Embed(
        title="🔧 Control de Servicio — Autenticación",
        description=f"El estado del servicio de autenticación ha sido actualizado a: {estado}",
        color=color
    )
    embed.set_footer(text=f"Modificado por {interaction.user}")

    await interaction.response.send_message(embed=embed, ephemeral=True)
    logger.info(f"Admin {interaction.user} cambió el estado de autenticación a: {AUTH_SYSTEM_ACTIVE}")

    asyncio.create_task(send_security_webhook(
        title="🔧 Estado del Servicio Auth Modificado",
        description=f"El administrador {interaction.user.mention} cambió el estado a: {estado}.",
        color=color,
        fields=[
            {"name": "Admin", "value": f"{interaction.user.mention}", "inline": True},
            {"name": "Nuevo Estado", "value": "Online" if AUTH_SYSTEM_ACTIVE else "Mantenimiento", "inline": True}
        ]
    ))

# ─────────────────────────────────────────────────────────────────────────────
# FastAPI Web Server (Endpoints para WrathLoader.exe)
# ─────────────────────────────────────────────────────────────────────────────
app = FastAPI(title="Wrath Discord Auth Server", version="3.0.0")

class HandshakeRequest(BaseModel):
    code: str
    hwid: str
    pc_name: Optional[str] = "Unknown-PC"

@app.get("/api/v1/download/loader")
async def download_loader_file(code: str):
    """Permite descargar WrathLoader.exe verificando que el código de Discord sea válido."""
    access = code_registry.get(code.upper().strip())
    if not access or access.expires_at < datetime.now(timezone.utc):
        raise HTTPException(status_code=403, detail="Código de descarga inválido o expirado.")

    if not os.path.exists(LOADER_EXE_PATH):
        raise HTTPException(status_code=404, detail="WrathLoader.exe no encontrado en el servidor.")

    return FileResponse(LOADER_EXE_PATH, filename="WrathLoader.exe", media_type="application/octet-stream")

def get_real_client_ip(request: Request) -> str:
    """Extrae la IP real del cliente soportando Cloudflare y proxies inversos Nginx."""
    cf_connecting = request.headers.get("CF-Connecting-IP")
    if cf_connecting:
        return cf_connecting.strip()

    x_real = request.headers.get("X-Real-IP")
    if x_real:
        return x_real.strip()

    x_forwarded = request.headers.get("X-Forwarded-For")
    if x_forwarded:
        return x_forwarded.split(",")[0].strip()

    return request.client.host if request.client else "unknown"

class AutoLoginRequest(BaseModel):
    hwid: str

@app.post("/api/v1/auth/auto-login")
async def auto_login(req: AutoLoginRequest, request: Request):
    """
    Si el HWID ya fue verificado previamente en Discord y está registrado en la base de datos,
    permite iniciar sesión automáticamente y genera un download_token directo sin pedir código.
    """
    if not AUTH_SYSTEM_ACTIVE:
        raise HTTPException(
            status_code=503,
            detail="El servicio de autenticación se encuentra en mantenimiento temporal."
        )

    client_ip = get_real_client_ip(request)
    if _rate_limited(client_ip):
        raise HTTPException(status_code=429, detail="Demasiadas solicitudes. Intenta de nuevo mas tarde.")

    hwid = req.hwid.strip().upper()

    conn = get_db_connection()
    cur = conn.execute("SELECT user_id FROM discord_hwid_locks WHERE UPPER(hwid) = ?", (hwid,))
    row = cur.fetchone()
    conn.close()

    if not row:
        raise HTTPException(
            status_code=404,
            detail="Equipo no registrado previamente. Ingresa tu codigo de Discord."
        )

    user_id = row[0]
    session_id = str(uuid.uuid4())
    session = ActiveSession(session_id, user_id, req.hwid)
    session.status = "approved"
    session.download_token = secrets.token_urlsafe(32)
    sessions[session_id] = session
    save_session(session)

    logger.info(f"[AUTO-LOGIN] Acceso directo concedido por HWID registrado a usuario Discord ID: {user_id}")
    asyncio.create_task(send_security_webhook(
        title="⚡ Auto-Login Concedido",
        description=f"Inicio de sesión directo concedido a <@{user_id}> por HWID registrado.",
        color=0x3498DB,
        fields=[
            {"name": "Usuario", "value": f"<@{user_id}> (`{user_id}`)", "inline": True},
            {"name": "HWID", "value": f"`{req.hwid[:20]}...`", "inline": True}
        ]
    ))
    return {
        "status": "approved",
        "download_token": session.download_token,
        "message": "Equipo verificado. Acceso autorizado automaticamente."
    }

@app.post("/api/v1/auth/request")
async def request_discord_auth(req: HandshakeRequest, request: Request):
    """
    El Loader envía el código y el HWID.
    El servidor dispara una alerta interactiva con botones al DM del usuario en Discord.
    """
    if not AUTH_SYSTEM_ACTIVE:
        raise HTTPException(
            status_code=503,
            detail="El servicio de autenticación se encuentra en mantenimiento temporal."
        )

    client_ip = get_real_client_ip(request)
    now_ts = time.time()

    # Protección Anti-Fuerza Bruta: Máximo 5 intentos fallidos en 5 minutos por IP
    recent_failures = [t for t in failed_attempts_by_ip[client_ip] if now_ts - t < 300]
    failed_attempts_by_ip[client_ip] = recent_failures

    if len(recent_failures) >= 5:
        logger.warning(f"[SEGURIDAD] IP {client_ip} bloqueada por exceso de intentos de fuerza bruta.")
        asyncio.create_task(send_security_webhook(
            title="🚨 IP Bloqueada por Fuerza Bruta",
            description=f"La IP `{client_ip}` superó 5 intentos fallidos y fue bloqueada por 5 minutos.",
            color=0xE74C3C,
            fields=[
                {"name": "IP", "value": f"`{client_ip}`", "inline": True},
                {"name": "Motivo", "value": "5 intentos fallidos en 5 min", "inline": True}
            ]
        ))
        raise HTTPException(status_code=429, detail="Demasiados intentos fallidos. IP bloqueada temporalmente por 5 minutos.")

    code = req.code.upper().strip()
    access = code_registry.get(code)

    if not access or access.used or access.expires_at < datetime.now(timezone.utc):
        failed_attempts_by_ip[client_ip].append(now_ts)
        await asyncio.sleep(1.0)  # Retraso deliberado para anular ataques de alta frecuencia
        raise HTTPException(status_code=401, detail="Código de acceso inválido o expirado.")

    # Código válido: resetear contador de fallos de la IP
    failed_attempts_by_ip.pop(client_ip, None)

    # Comprobar HWID si ya tiene una máquina registrada
    locked_hwid = get_persisted_hwid(access.user_id)
    if locked_hwid and locked_hwid != req.hwid:
        raise HTTPException(status_code=403, detail="HWID no coincide. Esta cuenta pertenece a otra PC.")

    client_ip = get_real_client_ip(request)
    if _rate_limited(client_ip):
        raise HTTPException(status_code=429, detail="Demasiadas solicitudes. Intenta de nuevo mas tarde.")

    session_id = str(uuid.uuid4())
    session = ActiveSession(session_id, access.user_id, req.hwid)
    sessions[session_id] = session
    save_session(session)
    access.used = True

    # Enviar DM interactivo por Discord
    user = bot.get_user(access.user_id)
    if not user:
        try:
            user = await bot.fetch_user(access.user_id)
        except Exception:
            user = None

    if user:
        view = DiscordAuthConfirmView(session_id)
        embed = discord.Embed(
            title="⚠️ Alerta de Seguridad: Solicitud de Inicio",
            description=f"Se ha detectado una solicitud de acceso desde **WrathLoader**.",
            color=0xFFAA00
        )
        embed.add_field(name="💻 Equipo", value=f"`{req.pc_name}`", inline=True)
        embed.add_field(name="🔒 HWID", value=f"`{req.hwid[:16]}...`", inline=True)
        embed.add_field(name="⏰ Hora", value=datetime.now(timezone.utc).strftime("%H:%M:%S UTC"), inline=False)
        embed.set_footer(text="Haz clic en Permitir para autorizar la inyección en Minecraft.")

        try:
            await user.send(embed=embed, view=view)
        except Exception as e:
            logger.error(f"Fallo al enviar mensaje privado a {access.user_id}: {e}")
            raise HTTPException(status_code=500, detail="No se pudo enviar el DM en Discord (tienes los mensajes directos cerrados).")
    else:
        raise HTTPException(status_code=500, detail="Usuario de Discord no encontrado.")

    return {
        "status": "pending_discord_confirmation",
        "session_id": session_id,
        "message": "Revisa tus mensajes directos de Discord para confirmar."
    }

@app.get("/api/v1/auth/poll/{session_id}")
async def poll_session(session_id: str):
    """El Loader consulta cada segundo si el usuario ya presionó Permitir en Discord."""
    session = sessions.get(session_id)
    if not session:
        raise HTTPException(status_code=404, detail="Sesión no encontrada.")

    if session.expires_at < datetime.now(timezone.utc):
        session.status = "expired"

    return {
        "status": session.status,
        "download_token": session.download_token if session.status == "approved" else None
    }

class PayloadDownloadRequest(BaseModel):
    download_token: str
    hwid: str
    client_pubkey: str

@app.post("/api/v1/payload/download")
async def download_encrypted_dll(req: PayloadDownloadRequest, request: Request):
    """
    Una vez confirmado en Discord, entrega el DLL cifrado con ECDH P-256 + HKDF-SHA256 + AES-256-GCM.
    El cliente en C++ lo descifra en RAM con Forward Secrecy.
    """
    client_ip = get_real_client_ip(request)
    if _rate_limited(client_ip):
        raise HTTPException(status_code=429, detail="Demasiadas solicitudes. Intenta de nuevo mas tarde.")

    session = next((s for s in sessions.values() if s.download_token == req.download_token), None)
    if not session or session.status != "approved":
        raise HTTPException(status_code=401, detail="Token no autorizado o expirado.")

    if session.hwid != req.hwid:
        raise HTTPException(status_code=403, detail="HWID no coincide con la sesión autorizada.")

    if not os.path.exists(PAYLOAD_DLL_PATH):
        raise HTTPException(status_code=500, detail="WrathPayload.dll no encontrado en el servidor.")

    try:
        client_pub_bytes = b"\x04" + bytes.fromhex(req.client_pubkey)
        client_pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), client_pub_bytes)
    except Exception as e:
        logger.error(f"Clave pública de cliente inválida: {e}")
        raise HTTPException(status_code=400, detail="Clave pública efímera inválida.")

    # Nonce vinculado al cliente: hash de su clave pública efímera.
    # Impide que un tercero replantee el heartbeat desde otra máquina.
    session.client_nonce = hashlib.sha256(client_pub_bytes).hexdigest()

    # Generar par de claves efímero del servidor en SECP256R1
    server_priv = ec.generate_private_key(ec.SECP256R1())
    server_pub = server_priv.public_key()
    server_pub_raw = server_pub.public_bytes(
        serialization.Encoding.X962,
        serialization.PublicFormat.UncompressedPoint
    )[1:]  # 64 bytes (X || Y)

    # Acuerdo Diffie-Hellman P-256
    shared_secret = server_priv.exchange(ec.ECDH(), client_pub)

    # Derivación dinámica de clave AES-256 por HKDF-SHA256
    salt = (req.hwid + req.download_token).encode("utf-8")
    info = b"wrath-payload-key-derivation"
    kdf = HKDF(algorithm=hashes.SHA256(), length=32, salt=salt, info=info)
    session_key = kdf.derive(shared_secret)

    with open(PAYLOAD_DLL_PATH, "rb") as f:
        dll_bytes = f.read()

    # Cifrado AES-256-GCM con clave efímera
    aesgcm = AESGCM(session_key)
    nonce = secrets.token_bytes(12)
    encrypted_bytes = aesgcm.encrypt(nonce, dll_bytes, associated_data=req.hwid.encode("utf-8"))

    # Generar token inicial de heartbeat para sesión viva
    session.heartbeat_token = secrets.token_urlsafe(32)
    session.last_heartbeat = time.time()
    session.sequence = 0

    # Consumir token para prevenir reuso
    session.download_token = None
    save_session(session)

    logger.info(f"Payload entregado con clave efímera ECDH a usuario Discord ID: {session.user_id}")
    # Formato: [64 bytes Server PubKey] + [12 bytes Nonce] + [Ciphertext + 16 bytes GCM Tag]
    headers = {
        "X-Heartbeat-Token": session.heartbeat_token,
        "X-Session-ID": session.session_id,
        "Access-Control-Expose-Headers": "X-Heartbeat-Token, X-Session-ID"
    }
    return Response(
        content=(server_pub_raw + nonce + encrypted_bytes),
        media_type="application/octet-stream",
        headers=headers
    )

class HeartbeatRequest(BaseModel):
    session_id: Optional[str] = None
    session_token: str
    hwid: str
    sequence: int = 0
    client_nonce: Optional[str] = None

@app.post("/api/v1/session/heartbeat")
async def session_heartbeat(req: HeartbeatRequest, request: Request):
    """
    Latido en tiempo real enviado periódicamente por WrathPayload.dll dentro de Minecraft.
    Valida vigencia de HWID, estado global de autenticación y rota el token criptográfico.
    """
    if not AUTH_SYSTEM_ACTIVE:
        raise HTTPException(status_code=503, detail="El servicio de autenticación se encuentra en mantenimiento temporal.")

    hwid_norm = req.hwid.strip().upper()
    conn = get_db_connection()
    cur = conn.execute("SELECT user_id FROM discord_hwid_locks WHERE UPPER(hwid) = ?", (hwid_norm,))
    row = cur.fetchone()
    conn.close()

    if not row:
        raise HTTPException(status_code=403, detail="HWID revocado o no registrado.")

    user_id = row[0]

    client_ip = get_real_client_ip(request)
    if _rate_limited(client_ip):
        raise HTTPException(status_code=429, detail="Demasiadas solicitudes. Intenta de nuevo mas tarde.")

    matched_session = None
    for s in sessions.values():
        if s.user_id == user_id and s.status == "approved":
            if s.heartbeat_token and secrets.compare_digest(s.heartbeat_token, req.session_token):
                matched_session = s
                break
            elif req.session_id and s.session_id == req.session_id:
                matched_session = s
                break

    if not matched_session:
        raise HTTPException(status_code=401, detail="Sesión no encontrada o token expirado.")

    if matched_session.client_nonce and req.client_nonce and matched_session.client_nonce != req.client_nonce:
        raise HTTPException(status_code=403, detail="Nonce de cliente no coincide con la sesión autorizada.")

    new_token = secrets.token_urlsafe(32)
    matched_session.heartbeat_token = new_token
    matched_session.last_heartbeat = time.time()
    matched_session.sequence = req.sequence + 1
    save_session(matched_session)

    return {
        "status": "ok",
        "next_token": new_token,
        "interval_sec": 30
    }


# ─────────────────────────────────────────────────────────────────────────────
# Runner
# ─────────────────────────────────────────────────────────────────────────────
async def run_server():
    config = uvicorn.Config(app=app, host=SERVER_HOST, port=SERVER_PORT, log_level="info")
    server = uvicorn.Server(config)

    if DISCORD_BOT_TOKEN and DISCORD_BOT_TOKEN != "YOUR_BOT_TOKEN_HERE":
        await asyncio.gather(
            server.serve(),
            bot.start(DISCORD_BOT_TOKEN)
        )
    else:
        logger.warning("==================================================================")
        logger.warning("ATENCIÓN: Configura DISCORD_BOT_TOKEN en Server/.env para habilitar el bot")
        logger.warning("==================================================================")
        await server.serve()

if __name__ == "__main__":
    try:
        asyncio.run(run_server())
    except KeyboardInterrupt:
        logger.info("Servidor detenido.")
