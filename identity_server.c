/*
 * identity_server.c - Servicio de identidad
 * Arquitectura y Protocolos de Internet - EAFIT 2026-1
 *
 * Uso: ./identity_server <puerto> <archivoDeLogs>
 * Ejemplo: ./identity_server 9090 identity_logs.txt
 *
 * Protocolo propio (texto plano, TCP):
 *   Petición:  AUTH|usuario|password\n
 *   Respuesta: OK|rol|token\n
 *              ERROR|codigo|mensaje\n
 *
 * Usuarios disponibles (base de datos simulada):
 *   atacante1 / pass123 → ATACANTE
 *   atacante2 / pass123 → ATACANTE
 *   defensor1 / pass123 → DEFENSOR
 *   defensor2 / pass123 → DEFENSOR
 *   admin     / admin   → DEFENSOR
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
#define BUF_SIZE    512
#define MAX_USERS    10

/* ─── Base de datos de usuarios (simulada en memoria) ─────── */
typedef struct {
    char usuario[32];
    char password[32];
    char rol[16];       /* "ATACANTE" o "DEFENSOR" */
} Usuario;

static const Usuario USUARIOS[MAX_USERS] = {
    {"atacante1", "pass123", "ATACANTE"},
    {"atacante2", "pass123", "ATACANTE"},
    {"defensor1", "pass123", "DEFENSOR"},
    {"defensor2", "pass123", "DEFENSOR"},
    {"admin",     "admin",   "DEFENSOR"},
    {"atacante",  "1234",    "ATACANTE"},   /* compatibilidad con pruebas HTTP */
    {"defensor",  "1234",    "DEFENSOR"},
    {"",          "",        ""}            /* centinela */
};

/* ─── Archivo de logs ─────────────────────────────────────── */
static FILE *log_file = NULL;

/* ═══════════════════════════════════════════════════════════
   LOGGING
   ═══════════════════════════════════════════════════════════ */
void log_msg(const char *ip, int puerto, const char *dir, const char *msg) {
    time_t ahora = time(NULL);
    struct tm *t  = localtime(&ahora);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    printf("[%s] %s %s:%d -> %s\n", ts, dir, ip, puerto, msg);
    fflush(stdout);

    if (log_file) {
        fprintf(log_file, "[%s] %s %s:%d -> %s\n", ts, dir, ip, puerto, msg);
        fflush(log_file);
    }
}

/* ═══════════════════════════════════════════════════════════
   GENERADOR DE TOKEN SIMPLE
   Formato: tok_<usuario>_<timestamp>
   ═══════════════════════════════════════════════════════════ */
void generar_token(const char *usuario, char *token_out, int max) {
    snprintf(token_out, max, "tok_%s_%ld", usuario, (long)time(NULL));
}

/* ═══════════════════════════════════════════════════════════
   BUSCAR USUARIO EN LA BASE DE DATOS
   Retorna puntero al Usuario si existe, NULL si no
   ═══════════════════════════════════════════════════════════ */
const Usuario *buscar_usuario(const char *usuario, const char *password) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (strlen(USUARIOS[i].usuario) == 0) break;  /* centinela */
        if (strcmp(USUARIOS[i].usuario,  usuario)  == 0 &&
            strcmp(USUARIOS[i].password, password) == 0) {
            return &USUARIOS[i];
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
   PROCESAR PETICIÓN DE AUTENTICACIÓN
   Protocolo: AUTH|usuario|password\n
   ═══════════════════════════════════════════════════════════ */
void procesar_auth(int fd, const char *buf,
                   const char *ip, int puerto) {
    char copia[BUF_SIZE];
    strncpy(copia, buf, BUF_SIZE - 1);
    copia[BUF_SIZE - 1] = '\0';

    /* Parsear tokens separados por | */
    char *tokens[4];
    int   n = 0;
    char *tok = strtok(copia, "|");
    while (tok && n < 4) {
        /* Quita \r \n al final */
        int len = strlen(tok);
        while (len > 0 && (tok[len-1] == '\n' || tok[len-1] == '\r'))
            tok[--len] = '\0';
        tokens[n++] = tok;
        tok = strtok(NULL, "|");
    }

    char respuesta[BUF_SIZE];

    /* Validar formato: AUTH|usuario|password */
    if (n < 3 || strcmp(tokens[0], "AUTH") != 0) {
        snprintf(respuesta, sizeof(respuesta),
                 "ERROR|400|formato invalido\n");
        send(fd, respuesta, strlen(respuesta), 0);
        log_msg(ip, puerto, "RESP", "ERROR|400|formato invalido");
        return;
    }

    const char *usuario  = tokens[1];
    const char *password = tokens[2];

    log_msg(ip, puerto, "RECV", buf[strlen(buf)-1] == '\n'
            ? (char[]){[0 ... BUF_SIZE-2] = 0} : buf);

    /* Buscar en la base de datos */
    const Usuario *u = buscar_usuario(usuario, password);

    if (!u) {
        snprintf(respuesta, sizeof(respuesta),
                 "ERROR|401|credenciales incorrectas\n");
        send(fd, respuesta, strlen(respuesta), 0);
        log_msg(ip, puerto, "RESP", "ERROR|401|credenciales incorrectas");
        return;
    }

    /* Generar token y responder */
    char token[128];
    generar_token(usuario, token, sizeof(token));

    snprintf(respuesta, sizeof(respuesta),
             "OK|%s|%s\n", u->rol, token);
    send(fd, respuesta, strlen(respuesta), 0);

    /* Log sin mostrar el token completo por seguridad */
    char log_buf[BUF_SIZE];
    snprintf(log_buf, sizeof(log_buf), "OK|%s|<token>", u->rol);
    log_msg(ip, puerto, "RESP", log_buf);
}

/* ═══════════════════════════════════════════════════════════
   HILO POR CLIENTE
   ═══════════════════════════════════════════════════════════ */
typedef struct {
    int  fd;
    char ip[INET6_ADDRSTRLEN];
    int  puerto;
} ArgHilo;

void *hilo_cliente(void *arg) {
    ArgHilo *a = (ArgHilo *)arg;
    char buf[BUF_SIZE];

    log_msg(a->ip, a->puerto, "CONN", "cliente conectado");

    memset(buf, 0, sizeof(buf));
    ssize_t n = recv(a->fd, buf, sizeof(buf) - 1, 0);

    if (n > 0) {
        buf[n] = '\0';
        procesar_auth(a->fd, buf, a->ip, a->puerto);
    }

    log_msg(a->ip, a->puerto, "DISC", "cliente desconectado");
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

    /* Resuelve dirección local — sin IPs hardcodeadas */
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

    printf("Servicio de identidad escuchando en puerto %s\n", puerto_str);
    printf("Usuarios disponibles:\n");
    for (int i = 0; i < MAX_USERS && strlen(USUARIOS[i].usuario) > 0; i++)
        printf("  %-12s / %-10s → %s\n",
               USUARIOS[i].usuario, USUARIOS[i].password, USUARIOS[i].rol);
    printf("\n");

    /* Bucle principal */
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
        if (pthread_create(&tid, NULL, hilo_cliente, a) != 0) {
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
