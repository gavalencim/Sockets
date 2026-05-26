"""
cliente.py - Cliente del juego multijugador con GUI
Arquitectura y Protocolos de Internet - EAFIT 2026-1

Uso:
    python3 cliente.py

Requisitos:
    - Python 3.x (tkinter viene incluido)
    - Servidor de juego corriendo en localhost:8080
"""

import tkinter as tk
from tkinter import messagebox
import socket
import threading
import queue

# ─── Configuración ────────────────────────────────────────
GAME_HOST = "localhost"   # nombre de dominio, nunca IP hardcodeada
GAME_PORT = 8080

MAP_W     = 100
MAP_H     = 100
CELL_SIZE = 6
CANVAS_W  = MAP_W * CELL_SIZE
CANVAS_H  = MAP_H * CELL_SIZE

COLOR_BG      = "#0a0f1e"
COLOR_GRID    = "#112211"
COLOR_TEXT    = "#00ff88"
COLOR_PANEL   = "#0d1a0d"
COLOR_RECURSO = "#ff4444"
COLOR_ALERT   = "#ffaa00"

RECURSOS = [
    {"id": 0, "x": 25, "y": 30, "nombre": "Servidor-01"},
    {"id": 1, "x": 70, "y": 65, "nombre": "Servidor-02"},
]


# ═══════════════════════════════════════════════════════════
class ClienteRed:
    def __init__(self, cola):
        self.cola   = cola
        self.sock   = None
        self.activo = False

    def conectar(self, sala_id, token):
        try:
            infos = socket.getaddrinfo(
                GAME_HOST, GAME_PORT,
                socket.AF_UNSPEC, socket.SOCK_STREAM
            )
            if not infos:
                self.cola.put(("error", "No se pudo resolver el servidor"))
                return False
            familia, tipo, proto, _, addr = infos[0]
            self.sock = socket.socket(familia, tipo, proto)
            self.sock.connect(addr)
            self.activo = True
            self.enviar(f"JOIN|{sala_id}|{token}")
            threading.Thread(target=self._recibir, daemon=True).start()
            return True
        except OSError as e:
            self.cola.put(("error", f"Error de conexión: {e}"))
            return False

    def enviar(self, mensaje):
        if not self.activo or not self.sock:
            return
        try:
            self.sock.sendall((mensaje + "\n").encode("utf-8"))
        except OSError:
            self.activo = False
            self.cola.put(("desconectado", ""))

    def _recibir(self):
        buffer = ""
        while self.activo:
            try:
                datos = self.sock.recv(512)
                if not datos:
                    break
                buffer += datos.decode("utf-8")
                while "\n" in buffer:
                    linea, buffer = buffer.split("\n", 1)
                    linea = linea.strip()
                    if linea:
                        self._parsear(linea)
            except OSError:
                break
        self.activo = False
        self.cola.put(("desconectado", "Conexión cerrada por el servidor"))

    def _parsear(self, linea):
        partes = linea.split("|")
        cmd    = partes[0].upper()
        if cmd == "OK":
            self.cola.put(("ok", "|".join(partes[1:])))
        elif cmd == "ERROR":
            self.cola.put(("error_srv", f"[{partes[1] if len(partes)>1 else '?'}] {partes[2] if len(partes)>2 else ''}"))
        elif cmd == "FOUND":
            self.cola.put(("found", partes[1] if len(partes) > 1 else "0"))
        elif cmd == "ALERT":
            rid = partes[1] if len(partes) > 1 else "?"
            x   = partes[2] if len(partes) > 2 else "?"
            y   = partes[3] if len(partes) > 3 else "?"
            self.cola.put(("alert", (rid, x, y)))
        else:
            self.cola.put(("raw", linea))

    def cerrar(self):
        self.activo = False
        if self.sock:
            try:
                self.sock.sendall("QUIT\n".encode())
                self.sock.close()
            except OSError:
                pass


# ═══════════════════════════════════════════════════════════
class VentanaLogin(tk.Toplevel):
    def __init__(self, padre, callback):
        super().__init__(padre)
        self.callback = callback
        self.title("CyberSim — Acceso")
        self.configure(bg=COLOR_BG)
        self.resizable(False, False)
        self._construir()
        self.grab_set()

    def _construir(self):
        pad = {"padx": 20, "pady": 6}
        tk.Label(self, text="> CyberSim // Acceso",
                 font=("Courier", 14, "bold"),
                 fg=COLOR_TEXT, bg=COLOR_BG).pack(**pad, pady=(20, 4))
        tk.Label(self, text="Sala ID:",
                 font=("Courier", 11), fg=COLOR_TEXT, bg=COLOR_BG).pack(**pad)
        self.sala_var = tk.StringVar(value="0")
        tk.Entry(self, textvariable=self.sala_var,
                 font=("Courier", 11), bg="#111", fg=COLOR_TEXT,
                 insertbackground=COLOR_TEXT, width=20).pack(**pad)
        tk.Label(self, text="Token de sesión:",
                 font=("Courier", 11), fg=COLOR_TEXT, bg=COLOR_BG).pack(**pad)
        self.token_var = tk.StringVar(value="token123")
        tk.Entry(self, textvariable=self.token_var,
                 font=("Courier", 11), bg="#111", fg=COLOR_TEXT,
                 insertbackground=COLOR_TEXT, width=20).pack(**pad)
        tk.Button(self, text="CONECTAR",
                  font=("Courier", 11, "bold"),
                  bg=COLOR_TEXT, fg=COLOR_BG,
                  relief="flat", cursor="hand2",
                  command=self._conectar).pack(**pad, pady=(14, 20))

    def _conectar(self):
        sala  = self.sala_var.get().strip()
        token = self.token_var.get().strip()
        if not sala or not token:
            messagebox.showerror("Error", "Completa todos los campos", parent=self)
            return
        self.destroy()
        self.callback(sala, token)


# ═══════════════════════════════════════════════════════════
class VentanaJuego(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("CyberSim — Plano de juego")
        self.configure(bg=COLOR_BG)
        self.resizable(False, False)

        self.rol      = None
        self.pos_x    = 0
        self.pos_y    = 0
        self.sala_id  = 0
        self.alertas  = []
        self.recursos_encontrados = set()

        self.cola = queue.Queue()
        self.red  = ClienteRed(self.cola)

        self._construir_ui()
        self._pedir_conexion()
        self.after(50, self._procesar_cola)
        self.protocol("WM_DELETE_WINDOW", self._salir)

    def _construir_ui(self):
        frame_mapa = tk.Frame(self, bg=COLOR_BG)
        frame_mapa.pack(side=tk.LEFT, padx=10, pady=10)

        tk.Label(frame_mapa, text="> PLANO DEL CENTRO DE DATOS",
                 font=("Courier", 10, "bold"),
                 fg=COLOR_TEXT, bg=COLOR_BG).pack(anchor="w")

        self.canvas = tk.Canvas(
            frame_mapa, width=CANVAS_W, height=CANVAS_H,
            bg=COLOR_BG, highlightthickness=1,
            highlightbackground=COLOR_TEXT
        )
        self.canvas.pack()

        frame_panel = tk.Frame(self, bg=COLOR_PANEL, width=220)
        frame_panel.pack(side=tk.RIGHT, fill=tk.Y, padx=(0, 10), pady=10)
        frame_panel.pack_propagate(False)

        tk.Label(frame_panel, text="JUGADOR",
                 font=("Courier", 10, "bold"),
                 fg=COLOR_TEXT, bg=COLOR_PANEL).pack(pady=(14, 2))
        self.lbl_rol  = tk.Label(frame_panel, text="Rol: —",
                                 font=("Courier", 10), fg=COLOR_TEXT, bg=COLOR_PANEL)
        self.lbl_rol.pack()
        self.lbl_pos  = tk.Label(frame_panel, text="Pos: (0, 0)",
                                 font=("Courier", 10), fg=COLOR_TEXT, bg=COLOR_PANEL)
        self.lbl_pos.pack()
        self.lbl_sala = tk.Label(frame_panel, text="Sala: —",
                                 font=("Courier", 10), fg=COLOR_TEXT, bg=COLOR_PANEL)
        self.lbl_sala.pack(pady=(0, 10))

        tk.Label(frame_panel, text="MOVIMIENTO",
                 font=("Courier", 10, "bold"),
                 fg=COLOR_TEXT, bg=COLOR_PANEL).pack()
        tk.Label(frame_panel, text="Flechas · Shift+flecha (x5)",
                 font=("Courier", 8), fg="#558866", bg=COLOR_PANEL).pack(pady=(0, 10))

        tk.Label(frame_panel, text="ACCIONES",
                 font=("Courier", 10, "bold"),
                 fg=COLOR_TEXT, bg=COLOR_PANEL).pack()

        tk.Button(frame_panel, text="SCAN",
                  font=("Courier", 10), bg="#112211", fg=COLOR_TEXT,
                  relief="flat", cursor="hand2",
                  command=self._scan).pack(fill=tk.X, padx=10, pady=3)

        tk.Button(frame_panel, text="ATTACK",
                  font=("Courier", 10), bg="#330000", fg="#ff4444",
                  relief="flat", cursor="hand2",
                  command=self._attack).pack(fill=tk.X, padx=10, pady=3)

        tk.Button(frame_panel, text="MITIGATE",
                  font=("Courier", 10), bg="#001133", fg="#4488ff",
                  relief="flat", cursor="hand2",
                  command=self._mitigate).pack(fill=tk.X, padx=10, pady=3)

        tk.Label(frame_panel, text="LOG",
                 font=("Courier", 10, "bold"),
                 fg=COLOR_TEXT, bg=COLOR_PANEL).pack(pady=(14, 2))

        self.log_text = tk.Text(
            frame_panel, font=("Courier", 8),
            bg="#050f05", fg=COLOR_TEXT,
            state=tk.DISABLED, wrap=tk.WORD, relief="flat"
        )
        self.log_text.pack(fill=tk.BOTH, expand=True, padx=6, pady=(0, 10))

        self.bind("<Up>",          lambda e: self._mover(0, -1))
        self.bind("<Down>",        lambda e: self._mover(0,  1))
        self.bind("<Left>",        lambda e: self._mover(-1, 0))
        self.bind("<Right>",       lambda e: self._mover(1,  0))
        self.bind("<Shift-Up>",    lambda e: self._mover(0, -5))
        self.bind("<Shift-Down>",  lambda e: self._mover(0,  5))
        self.bind("<Shift-Left>",  lambda e: self._mover(-5, 0))
        self.bind("<Shift-Right>", lambda e: self._mover(5,  0))

        self._dibujar_mapa()

    def _dibujar_mapa(self):
        self.canvas.delete("all")
        for x in range(0, CANVAS_W, CELL_SIZE * 10):
            self.canvas.create_line(x, 0, x, CANVAS_H, fill=COLOR_GRID)
        for y in range(0, CANVAS_H, CELL_SIZE * 10):
            self.canvas.create_line(0, y, CANVAS_W, y, fill=COLOR_GRID)

        for r in RECURSOS:
            if self.rol == "DEFENSOR" or r["id"] in self.recursos_encontrados:
                rx    = r["x"] * CELL_SIZE
                ry    = r["y"] * CELL_SIZE
                color = COLOR_ALERT if str(r["id"]) in [str(a) for a in self.alertas] else COLOR_RECURSO
                self.canvas.create_rectangle(rx-5, ry-5, rx+5, ry+5,
                                             fill=color, outline="#fff", width=1)
                self.canvas.create_text(rx, ry-10, text=r["nombre"],
                                        fill=color, font=("Courier", 7))
                if str(r["id"]) in [str(a) for a in self.alertas]:
                    self.canvas.create_text(rx, ry+14, text="⚠ BAJO ATAQUE",
                                            fill=COLOR_ALERT, font=("Courier", 7, "bold"))

        px     = self.pos_x * CELL_SIZE
        py     = self.pos_y * CELL_SIZE
        color_j = "#ff4444" if self.rol == "ATACANTE" else "#4488ff"
        self.canvas.create_oval(px-5, py-5, px+5, py+5,
                                fill=color_j, outline=COLOR_TEXT, width=1)
        label = "ATK" if self.rol == "ATACANTE" else ("DEF" if self.rol == "DEFENSOR" else "?")
        self.canvas.create_text(px, py-12, text=label,
                                fill=COLOR_TEXT, font=("Courier", 7, "bold"))

    def _mover(self, dx, dy):
        if not self.red.activo:
            return
        nx = max(0, min(MAP_W - 1, self.pos_x + dx))
        ny = max(0, min(MAP_H - 1, self.pos_y + dy))
        self.red.enviar(f"MOVE|{nx}|{ny}")

    def _scan(self):
        if not self.red.activo:
            return
        self.red.enviar("SCAN|5")
        self._log("Escaneando zona...")

    def _attack(self):
        if not self.red.activo or self.rol != "ATACANTE":
            self._log("Solo el atacante puede atacar")
            return
        for r in RECURSOS:
            if abs(r["x"] - self.pos_x) <= 5 and abs(r["y"] - self.pos_y) <= 5:
                self.red.enviar(f"ATTACK|{r['id']}")
                self._log(f"Atacando {r['nombre']}...")
                return
        self._log("No hay recurso cerca para atacar")

    def _mitigate(self):
        if not self.red.activo or self.rol != "DEFENSOR":
            self._log("Solo el defensor puede mitigar")
            return
        for r in RECURSOS:
            if (abs(r["x"] - self.pos_x) <= 5 and
                    abs(r["y"] - self.pos_y) <= 5 and
                    str(r["id"]) in [str(a) for a in self.alertas]):
                self.red.enviar(f"MITIGATE|{r['id']}")
                self._log(f"Mitigando {r['nombre']}...")
                return
        self._log("No hay recurso bajo ataque cerca")

    def _procesar_cola(self):
        try:
            while True:
                evento, datos = self.cola.get_nowait()
                if evento == "ok":
                    self._log(f"OK: {datos}")
                    if "pos=" in datos:
                        for parte in datos.split("|"):
                            if parte.startswith("pos="):
                                coords = parte.replace("pos=", "").split(",")
                                if len(coords) == 2:
                                    self.pos_x = int(coords[0])
                                    self.pos_y = int(coords[1])
                                    self.lbl_pos.config(text=f"Pos: ({self.pos_x}, {self.pos_y})")
                    if "rol=" in datos:
                        for parte in datos.split("|"):
                            if parte.startswith("rol="):
                                self.rol = parte.replace("rol=", "")
                                self.lbl_rol.config(text=f"Rol: {self.rol}")
                            if parte.startswith("sala="):
                                self.sala_id = parte.replace("sala=", "")
                                self.lbl_sala.config(text=f"Sala: {self.sala_id}")
                    self._dibujar_mapa()
                elif evento == "found":
                    rid = int(datos)
                    self.recursos_encontrados.add(rid)
                    nombre = next((r["nombre"] for r in RECURSOS if r["id"] == rid), f"recurso {rid}")
                    self._log(f"¡FOUND! Recurso encontrado: {nombre}")
                    self._dibujar_mapa()
                elif evento == "alert":
                    rid, x, y = datos
                    if rid not in self.alertas:
                        self.alertas.append(rid)
                    self._log(f"⚠ ALERTA: recurso {rid} bajo ataque en ({x},{y})")
                    self._dibujar_mapa()
                elif evento == "error_srv":
                    self._log(f"Error servidor: {datos}")
                elif evento == "error":
                    messagebox.showerror("Error de red", datos)
                elif evento == "desconectado":
                    self._log("Desconectado del servidor")
                elif evento == "raw":
                    self._log(f"< {datos}")
        except queue.Empty:
            pass
        self.after(50, self._procesar_cola)

    def _log(self, texto):
        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, f"> {texto}\n")
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)

    def _pedir_conexion(self):
        VentanaLogin(self, self._iniciar_conexion)

    def _iniciar_conexion(self, sala, token):
        self._log(f"Conectando a sala {sala}...")
        ok = self.red.conectar(sala, token)
        if not ok:
            self._log("No se pudo conectar. ¿Está el servidor corriendo?")

    def _salir(self):
        self.red.cerrar()
        self.destroy()


if __name__ == "__main__":
    app = VentanaJuego()
    app.mainloop()
