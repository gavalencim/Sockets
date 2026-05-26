/*
 * http_server.c - Servidor HTTP básico
 * Arquitectura y Protocolos de Internet - EAFIT 2026-1
 *
 * Uso: ./http_server <puerto> <archivoDeLogs>
 * Ejemplo: ./http_server 8081 http_logs.txt
 *
 * Rutas:
 *   GET  /          -> página de login
 *   GET  /partidas  -> lista de salas activas (JSON)
 *   POST /login     -> autenticación de usuario
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

/* ─── Constantes ─────────────────────────────────────────── */
#define BUF_SIZE     4096
#define MAX_CLIENTES   32

/* ─── Archivo de logs ─────────────────────────────────────── */
static FILE *log_file = NULL;

/* ═══════════════════════════════════════════════════════════
   LOGGING
   ═══════════════════════════════════════════════════════════ */
void log_http(const char *ip, int puerto, const char *metodo,
              const char *ruta, int codigo) {
    time_t ahora = time(NULL);
    struct tm *t  = localtime(&ahora);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    printf("[%s] %s:%d \"%s %s\" %d\n",
           timestamp, ip, puerto, metodo, ruta, codigo);
    fflush(stdout);

    if (log_file) {
        fprintf(log_file, "[%s] %s:%d \"%s %s\" %d\n",
                timestamp, ip, puerto, metodo, ruta, codigo);
        fflush(log_file);
    }
}

/* ═══════════════════════════════════════════════════════════
   RESPUESTAS HTTP
   ═══════════════════════════════════════════════════════════ */

/* Envía una respuesta HTTP completa */
void enviar_respuesta(int fd, int codigo, const char *estado,
                      const char *content_type, const char *body) {
    char cabeceras[512];
    int  body_len = strlen(body);

    snprintf(cabeceras, sizeof(cabeceras),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "\r\n",
             codigo, estado, content_type, body_len);

    send(fd, cabeceras, strlen(cabeceras), 0);
    send(fd, body,      body_len,          0);
}

/* ═══════════════════════════════════════════════════════════
   PARSER HTTP — extrae método, ruta y body
   ═══════════════════════════════════════════════════════════ */
typedef struct {
    char metodo[16];   /* GET, POST, etc. */
    char ruta[256];    /* /login, /partidas, etc. */
    char body[1024];   /* cuerpo del POST */
    int  valido;
} PeticionHTTP;

PeticionHTTP parsear_peticion(const char *buf) {
    PeticionHTTP p;
    memset(&p, 0, sizeof(p));

    /* Primera línea: "GET /ruta HTTP/1.1" */
    if (sscanf(buf, "%15s %255s", p.metodo, p.ruta) < 2) {
        p.valido = 0;
        return p;
    }
    p.valido = 1;

    /* Busca el body (después de \r\n\r\n) */
    const char *sep = strstr(buf, "\r\n\r\n");
    if (sep) {
        strncpy(p.body, sep + 4, sizeof(p.body) - 1);
    }

    return p;
}

/* Extrae el valor de un campo en el body POST.
 * Ejemplo: body="usuario=ginna&password=1234", campo="usuario" → "ginna" */
int extraer_campo(const char *body, const char *campo, char *valor, int max) {
    char buscar[64];
    snprintf(buscar, sizeof(buscar), "%s=", campo);

    const char *inicio = strstr(body, buscar);
    if (!inicio) return 0;

    inicio += strlen(buscar);
    const char *fin = strchr(inicio, '&');
    int len = fin ? (int)(fin - inicio) : (int)strlen(inicio);
    if (len >= max) len = max - 1;

    strncpy(valor, inicio, len);
    valor[len] = '\0';
    return 1;
}

/* ═══════════════════════════════════════════════════════════
   PÁGINA HTML DE LOGIN
   ═══════════════════════════════════════════════════════════ */
const char *HTML_LOGIN =
    "<!DOCTYPE html>"
    "<html lang='es'>"
    "<head><meta charset='UTF-8'>"
    "<title>CyberSim - Login</title>"
    "<style>"
    "* { box-sizing: border-box; margin: 0; padding: 0; }"
    "body { font-family: monospace; background: #0a0f1e; color: #00ff88; "
    "       display: flex; justify-content: center; align-items: center; "
    "       min-height: 100vh; }"
    ".panel { border: 1px solid #00ff88; padding: 40px; width: 360px; }"
    "h1 { font-size: 20px; margin-bottom: 8px; }"
    "p  { font-size: 12px; color: #558866; margin-bottom: 28px; }"
    "input { width: 100%; padding: 10px; margin-bottom: 14px; "
    "        background: #111; border: 1px solid #00ff88; "
    "        color: #00ff88; font-family: monospace; font-size: 14px; }"
    "button { width: 100%; padding: 10px; background: #00ff88; "
    "         color: #0a0f1e; font-family: monospace; font-size: 14px; "
    "         border: none; cursor: pointer; font-weight: bold; }"
    "button:hover { background: #00cc66; }"
    "#msg { margin-top: 14px; font-size: 13px; min-height: 20px; }"
    ".error { color: #ff4444; }"
    ".ok    { color: #00ff88; }"
    "</style></head>"
    "<body>"
    "<div class='panel'>"
    "  <h1>&gt; CyberSim // Acceso</h1>"
    "  <p>Centro de datos virtual — entrenamiento en ciberseguridad</p>"
    "  <input type='text'     id='usr' placeholder='usuario' />"
    "  <input type='password' id='pwd' placeholder='contraseña' />"
    "  <button onclick='login()'>INICIAR SESIÓN</button>"
    "  <div id='msg'></div>"
    "</div>"
    "<script>"
    "async function login() {"
    "  const usr = document.getElementById('usr').value;"
    "  const pwd = document.getElementById('pwd').value;"
    "  if (!usr || !pwd) { showMsg('Completa los campos', true); return; }"
    "  const res = await fetch('/login', {"
    "    method: 'POST',"
    "    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },"
    "    body: 'usuario=' + encodeURIComponent(usr) + '&password=' + encodeURIComponent(pwd)"
    "  });"
    "  const data = await res.json();"
    "  if (res.ok) {"
    "    showMsg('Bienvenido ' + data.usuario + ' — rol: ' + data.rol, false);"
    "    setTimeout(() => { window.location.href = '/partidas-ui'; }, 1200);"
    "  } else {"
    "    showMsg(data.error || 'Credenciales incorrectas', true);"
    "  }"
    "}"
    "function showMsg(t, err) {"
    "  const el = document.getElementById('msg');"
    "  el.textContent = t;"
    "  el.className = err ? 'error' : 'ok';"
    "}"
    "</script>"
    "</body></html>";

/* ═══════════════════════════════════════════════════════════
   PÁGINA DE PARTIDAS
   ═══════════════════════════════════════════════════════════ */
const char *HTML_PARTIDAS =
    "<!DOCTYPE html>"
    "<html lang='es'>"
    "<head><meta charset='UTF-8'>"
    "<title>CyberSim - Partidas</title>"
    "<style>"
    "* { box-sizing: border-box; margin: 0; padding: 0; }"
    "body { font-family: monospace; background: #0a0f1e; color: #00ff88; padding: 40px; }"
    "h1 { margin-bottom: 20px; }"
    "table { border-collapse: collapse; width: 100%; max-width: 600px; }"
    "th, td { border: 1px solid #225533; padding: 10px 16px; text-align: left; }"
    "th { background: #112211; }"
    "button { padding: 6px 14px; background: #00ff88; color: #0a0f1e; "
    "         border: none; cursor: pointer; font-family: monospace; }"
    "</style></head>"
    "<body>"
    "<h1>&gt; Partidas activas</h1>"
    "<table id='tabla'>"
    "  <tr><th>Sala</th><th>Jugadores</th><th>Estado</th><th></th></tr>"
    "</table>"
    "<script>"
    "async function cargar() {"
    "  const res  = await fetch('/partidas');"
    "  const data = await res.json();"
    "  const tbody = document.getElementById('tabla');"
    "  data.salas.forEach(s => {"
    "    const tr = document.createElement('tr');"
    "    tr.innerHTML = `<td>${s.id}</td><td>${s.jugadores}</td>"
    "      <td>${s.activa ? 'Activa' : 'Terminada'}</td>"
    "      <td><button onclick=\"unirse(${s.id})\">Unirse</button></td>`;"
    "    tbody.appendChild(tr);"
    "  });"
    "}"
    "function unirse(id) {"
    "  alert('Conecta el cliente de juego a la sala ' + id);"
    "}"
    "cargar();"
    "</script>"
    "</body></html>";

/* ═══════════════════════════════════════════════════════════
   MANEJADORES DE RUTAS
   ═══════════════════════════════════════════════════════════ */

/* GET / → página de login */
void handle_get_root(int fd) {
    enviar_respuesta(fd, 200, "OK", "text/html; charset=utf-8", HTML_LOGIN);
}

/* GET /partidas-ui → página HTML de partidas */
void handle_get_partidas_ui(int fd) {
    enviar_respuesta(fd, 200, "OK", "text/html; charset=utf-8", HTML_PARTIDAS);
}

/* GET /partidas → JSON con lista de salas (simulado por ahora) */
void handle_get_partidas(int fd) {
    /* En la entrega final esto consultaría al servidor de juego.
       Por ahora devuelve datos de ejemplo. */
    const char *json =
        "{\"salas\":["
        "{\"id\":0,\"jugadores\":2,\"activa\":true},"
        "{\"id\":1,\"jugadores\":1,\"activa\":true}"
        "]}";
    enviar_respuesta(fd, 200, "OK", "application/json", json);
}

/* ═══════════════════════════════════════════════════════════
   CLIENTE DEL SERVICIO DE IDENTIDAD
   Resuelve el nombre de dominio y consulta AUTH|usuario|pass
   ═══════════════════════════════════════════════════════════ */

/* Nombre de dominio e identidad — nunca una IP hardcodeada */
#define IDENTITY_HOST "localhost"
#define IDENTITY_PORT "9090"

/*
 * Consulta al servicio de identidad.
 * Retorna 1 si autenticó bien y llena rol_out y token_out.
 * Retorna 0 si falló (credenciales malas o servicio caído).
 */
int consultar_identidad(const char *usuario, const char *password,
                        char *rol_out, int rol_max,
                        char *token_out, int token_max) {
    /* ── Resolución DNS — sin IP hardcodeada ─────────────── */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(IDENTITY_HOST, IDENTITY_PORT, &hints, &res);
    if (status != 0) {
        /* El servicio de identidad no resuelve — manejo de excepción
           sin terminar la ejecución del servidor HTTP */
        fprintf(stderr, "identidad: getaddrinfo: %s\n", gai_strerror(status));
        return 0;
    }

    /* ── Crear socket y conectar ──────────────────────────── */
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        perror("identidad: socket");
        return 0;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(sock);
        fprintf(stderr, "identidad: no se pudo conectar a %s:%s\n",
                IDENTITY_HOST, IDENTITY_PORT);
        return 0;
    }
    freeaddrinfo(res);

    /* ── Enviar petición AUTH|usuario|password\n ──────────── */
    char peticion[256];
    snprintf(peticion, sizeof(peticion), "AUTH|%s|%s\n", usuario, password);
    send(sock, peticion, strlen(peticion), 0);

    /* ── Leer respuesta ───────────────────────────────────── */
    char respuesta[512];
    memset(respuesta, 0, sizeof(respuesta));
    ssize_t n = recv(sock, respuesta, sizeof(respuesta) - 1, 0);
    close(sock);

    if (n <= 0) return 0;
    respuesta[n] = '\0';

    /* ── Parsear: OK|ROL|TOKEN  o  ERROR|cod|msg ──────────── */
    char copia[512];
    strncpy(copia, respuesta, sizeof(copia) - 1);

    char *tokens[4];
    int   nt = 0;
    char *tok = strtok(copia, "|");
    while (tok && nt < 4) {
        int len = strlen(tok);
        while (len > 0 && (tok[len-1] == '\n' || tok[len-1] == '\r'))
            tok[--len] = '\0';
        tokens[nt++] = tok;
        tok = strtok(NULL, "|");
    }

    if (nt < 3 || strcmp(tokens[0], "OK") != 0) return 0;

    strncpy(rol_out,   tokens[1], rol_max   - 1);
    strncpy(token_out, tokens[2], token_max - 1);
    return 1;
}

/* POST /login → consulta real al servicio de identidad */
void handle_post_login(int fd, const char *body) {
    char usuario[64]  = {0};
    char password[64] = {0};

    extraer_campo(body, "usuario",  usuario,  sizeof(usuario));
    extraer_campo(body, "password", password, sizeof(password));

    if (strlen(usuario) == 0 || strlen(password) == 0) {
        enviar_respuesta(fd, 400, "Bad Request", "application/json",
                         "{\"error\":\"Faltan campos\"}");
        return;
    }

    char rol[32]    = {0};
    char token[128] = {0};

    int ok = consultar_identidad(usuario, password,
                                 rol,   sizeof(rol),
                                 token, sizeof(token));
    if (!ok) {
        enviar_respuesta(fd, 401, "Unauthorized", "application/json",
                         "{\"error\":\"Credenciales incorrectas o servicio no disponible\"}");
        return;
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"usuario\":\"%s\",\"rol\":\"%s\",\"token\":\"%s\"}",
             usuario, rol, token);
    enviar_respuesta(fd, 200, "OK", "application/json", json);
}

/* ═══════════════════════════════════════════════════════════
   ROUTER — decide qué manejador usar
   ═══════════════════════════════════════════════════════════ */
void router(int fd, const char *ip, int puerto, PeticionHTTP *p) {
    int codigo = 200;

    if (strcmp(p->metodo, "GET") == 0) {
        if (strcmp(p->ruta, "/") == 0) {
            handle_get_root(fd);
        } else if (strcmp(p->ruta, "/partidas") == 0) {
            handle_get_partidas(fd);
        } else if (strcmp(p->ruta, "/partidas-ui") == 0) {
            handle_get_partidas_ui(fd);
        } else {
            codigo = 404;
            enviar_respuesta(fd, 404, "Not Found", "application/json",
                             "{\"error\":\"Ruta no encontrada\"}");
        }
    } else if (strcmp(p->metodo, "POST") == 0) {
        if (strcmp(p->ruta, "/login") == 0) {
            handle_post_login(fd, p->body);
        } else {
            codigo = 404;
            enviar_respuesta(fd, 404, "Not Found", "application/json",
                             "{\"error\":\"Ruta no encontrada\"}");
        }
    } else {
        codigo = 405;
        enviar_respuesta(fd, 405, "Method Not Allowed", "application/json",
                         "{\"error\":\"Metodo no permitido\"}");
    }

    log_http(ip, puerto, p->metodo, p->ruta, codigo);
}

/* ═══════════════════════════════════════════════════════════
   HILO POR CLIENTE HTTP
   ═══════════════════════════════════════════════════════════ */
typedef struct {
    int  fd;
    char ip[INET6_ADDRSTRLEN];
    int  puerto;
} ArgHilo;

void *hilo_http(void *arg) {
    ArgHilo *a = (ArgHilo *)arg;
    char buf[BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    ssize_t n = recv(a->fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        PeticionHTTP p = parsear_peticion(buf);
        if (p.valido) {
            router(a->fd, a->ip, a->puerto, &p);
        } else {
            enviar_respuesta(a->fd, 400, "Bad Request", "application/json",
                             "{\"error\":\"Peticion malformada\"}");
            log_http(a->ip, a->puerto, "???", "/", 400);
        }
    }

    close(a->fd);
    free(a);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <puerto> <archivoDeLogs>\n", argv[0]);
        return 1;
    }

    const char *puerto_str = argv[1];
    const char *log_path   = argv[2];

    log_file = fopen(log_path, "a");
    if (!log_file) { perror("fopen log"); return 1; }

    /* Resuelve la dirección local con getaddrinfo (sin IPs hardcodeadas) */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    int status = getaddrinfo(NULL, puerto_str, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return 1;
    }

    int server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("bind"); return 1;
    }
    freeaddrinfo(res);

    if (listen(server_fd, 10) < 0) { perror("listen"); return 1; }

    printf("Servidor HTTP escuchando en puerto %s\n", puerto_str);
    printf("Abre: http://localhost:%s\n\n", puerto_str);

    while (1) {
        struct sockaddr_storage cliente_addr;
        socklen_t addr_len = sizeof(cliente_addr);

        int cliente_fd = accept(server_fd,
                                (struct sockaddr *)&cliente_addr,
                                &addr_len);
        if (cliente_fd < 0) { perror("accept"); continue; }

        ArgHilo *a = malloc(sizeof(ArgHilo));
        a->fd      = cliente_fd;
        a->puerto  = 0;

        if (cliente_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&cliente_addr;
            inet_ntop(AF_INET, &s->sin_addr, a->ip, sizeof(a->ip));
            a->puerto = ntohs(s->sin_port);
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&cliente_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, a->ip, sizeof(a->ip));
            a->puerto = ntohs(s->sin6_port);
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, hilo_http, a) != 0) {
            perror("pthread_create");
            close(cliente_fd);
            free(a);
            continue;
        }
        pthread_detach(tid);
    }

    fclose(log_file);
    close(server_fd);
    return 0;
}
