/*
 * cliente.c - Cliente del juego en C con interfaz ASCII
 * Arquitectura y Protocolos de Internet - EAFIT 2026-1
 *
 * Uso: ./cliente <host> <puerto>
 * Ejemplo: ./cliente localhost 8080
 *
 * Controles:
 *   w/a/s/d  → mover (1 paso)
 *   W/A/S/D  → mover (5 pasos)
 *   e        → scan
 *   f        → attack (solo atacante)
 *   m        → mitigate (solo defensor)
 *   q        → salir
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <termios.h>    /* para leer tecla sin Enter */

/* ─── Constantes ─────────────────────────────────────────── */
#define BUF_SIZE     512
#define MAP_W         40   /* ancho del mapa en caracteres ASCII */
#define MAP_H         20   /* alto del mapa en caracteres ASCII  */
#define ESCALA_X       3   /* cuántas celdas lógicas por carácter horizontal */
#define ESCALA_Y       5   /* cuántas celdas lógicas por carácter vertical   */
#define NUM_RECURSOS   2

/* ─── Recursos críticos ───────────────────────────────────── */
typedef struct { int id; int x; int y; char nombre[32]; } Recurso;
static const Recurso RECURSOS[NUM_RECURSOS] = {
    {0, 25, 30, "Srv-01"},
    {1, 70, 65, "Srv-02"},
};

/* ─── Estado global del juego ─────────────────────────────── */
static int    sock_fd    = -1;
static int    pos_x      = 0;
static int    pos_y      = 0;
static char   rol[16]    = "?";
static char   sala[8]    = "?";
static int    corriendo  = 1;
static int    alertas[NUM_RECURSOS]           = {0};
static int    encontrados[NUM_RECURSOS]       = {0};
static char   ultimo_msg[BUF_SIZE]            = "";
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* ─── Terminal en modo raw (sin esperar Enter) ────────────── */
static struct termios term_orig;

void activar_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &term_orig);
    raw = term_orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void restaurar_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &term_orig);
}

/* ═══════════════════════════════════════════════════════════
   DIBUJO DEL MAPA ASCII
   ═══════════════════════════════════════════════════════════ */
void limpiar_pantalla(void) {
    printf("\033[H\033[J");   /* ANSI: ir a inicio y borrar pantalla */
}

void dibujar_mapa(void) {
    limpiar_pantalla();

    /* Cabecera */
    printf("\033[1;32m");   /* verde brillante */
    printf("> CyberSim // Cliente C — Plano del centro de datos\n");
    printf("\033[0m");

    /* Fila superior del mapa */
    printf("+");
    for (int x = 0; x < MAP_W; x++) printf("-");
    printf("+\n");

    /* Filas del mapa */
    for (int my = 0; my < MAP_H; my++) {
        printf("|");
        for (int mx = 0; mx < MAP_W; mx++) {
            /* Convierte coordenada ASCII a coordenada lógica */
            
            

            /* ¿Hay un recurso aquí? */
            int es_recurso = 0;
            int recurso_id = -1;
            for (int r = 0; r < NUM_RECURSOS; r++) {
                int rx = RECURSOS[r].x / ESCALA_X;
                int ry = RECURSOS[r].y / ESCALA_Y;
                if (mx == rx && my == ry) {
                    es_recurso = 1;
                    recurso_id = r;
                    break;
                }
            }

            /* ¿Está el jugador aquí? */
            int jx = pos_x / ESCALA_X;
            int jy = pos_y / ESCALA_Y;
            int es_jugador = (mx == jx && my == jy);

            if (es_jugador) {
                if (strcmp(rol, "ATACANTE") == 0)
                    printf("\033[1;31mA\033[0m");   /* rojo */
                else
                    printf("\033[1;34mD\033[0m");   /* azul */
            } else if (es_recurso) {
                int visible = (strcmp(rol, "DEFENSOR") == 0)
                              || encontrados[recurso_id];
                if (visible) {
                    if (alertas[recurso_id])
                        printf("\033[1;33m!\033[0m");  /* amarillo = bajo ataque */
                    else
                        printf("\033[1;31mR\033[0m");  /* rojo = recurso */
                } else {
                    printf(".");
                }
            } else {
                printf(".");
            }
        }
        printf("|\n");
    }

    /* Fila inferior del mapa */
    printf("+");
    for (int x = 0; x < MAP_W; x++) printf("-");
    printf("+\n");

    /* Panel de info */
    printf("\033[1;32m");
    printf("Rol: %-10s  Sala: %-4s  Pos: (%3d, %3d)\n",
           rol, sala, pos_x, pos_y);
    printf("\033[0m");

    /* Alertas activas */
    for (int r = 0; r < NUM_RECURSOS; r++) {
        if (alertas[r])
            printf("\033[1;33m⚠  ALERTA: %s bajo ataque!\033[0m\n",
                   RECURSOS[r].nombre);
    }

    /* Último mensaje del servidor */
    printf("\033[0;32mServidor: %s\033[0m\n", ultimo_msg);

    /* Controles */
    printf("\033[0;90m");
    printf("Mover: w/a/s/d (x1)  W/A/S/D (x5) | ");
    printf("e=scan  f=attack  m=mitigate  q=salir\n");
    printf("\033[0m");
}

/* ═══════════════════════════════════════════════════════════
   ENVÍO DE MENSAJES AL SERVIDOR
   ═══════════════════════════════════════════════════════════ */
void enviar(const char *msg) {
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    if (send(sock_fd, buf, strlen(buf), 0) < 0)
        perror("send");
}

/* ═══════════════════════════════════════════════════════════
   HILO RECEPTOR — escucha mensajes del servidor
   ═══════════════════════════════════════════════════════════ */
void *hilo_recibir(void *arg) {
    (void)arg;
    char  buf[BUF_SIZE];
    char  acum[BUF_SIZE * 4] = "";   /* buffer acumulador */

    while (corriendo) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = recv(sock_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            corriendo = 0;
            break;
        }
        buf[n] = '\0';

        /* Acumula y procesa líneas completas */
        strncat(acum, buf, sizeof(acum) - strlen(acum) - 1);

        char *linea = strtok(acum, "\n");
        char  resto[BUF_SIZE * 4] = "";

        while (linea) {
            char *sig = strtok(NULL, "\n");

            /* Quita \r */
            int len = strlen(linea);
            while (len > 0 && linea[len-1] == '\r') linea[--len] = '\0';

            /* Parsear la línea */
            char copia[BUF_SIZE];
            strncpy(copia, linea, BUF_SIZE - 1);

            char *tokens[8];
            int   nt = 0;
            char *tok = strtok(copia, "|");
            while (tok && nt < 8) { tokens[nt++] = tok; tok = strtok(NULL, "|"); }

            if (nt > 0) {
                pthread_mutex_lock(&mutex);

                if (strcmp(tokens[0], "OK") == 0) {
                    /* Guarda el mensaje para mostrarlo */
                    strncpy(ultimo_msg, linea, BUF_SIZE - 1);

                    /* Extrae pos= y rol= */
                    for (int i = 1; i < nt; i++) {
                        if (strncmp(tokens[i], "pos=", 4) == 0) {
                            sscanf(tokens[i] + 4, "%d,%d", &pos_x, &pos_y);
                        }
                        if (strncmp(tokens[i], "rol=", 4) == 0) {
                            strncpy(rol, tokens[i] + 4, sizeof(rol) - 1);
                        }
                        if (strncmp(tokens[i], "sala=", 5) == 0) {
                            strncpy(sala, tokens[i] + 5, sizeof(sala) - 1);
                        }
                    }

                } else if (strcmp(tokens[0], "FOUND") == 0 && nt > 1) {
                    int rid = atoi(tokens[1]);
                    if (rid >= 0 && rid < NUM_RECURSOS)
                        encontrados[rid] = 1;
                    snprintf(ultimo_msg, BUF_SIZE,
                             "¡FOUND! Recurso %s descubierto", RECURSOS[rid].nombre);

                } else if (strcmp(tokens[0], "ALERT") == 0 && nt > 1) {
                    int rid = atoi(tokens[1]);
                    if (rid >= 0 && rid < NUM_RECURSOS)
                        alertas[rid] = 1;
                    snprintf(ultimo_msg, BUF_SIZE,
                             "⚠ ALERTA: %s bajo ataque!", RECURSOS[rid].nombre);

                } else if (strcmp(tokens[0], "ERROR") == 0) {
                    strncpy(ultimo_msg, linea, BUF_SIZE - 1);
                }

                pthread_mutex_unlock(&mutex);
                dibujar_mapa();
            }

            if (sig) strncpy(resto, sig, sizeof(resto) - 1);
            linea = sig;
        }
        strncpy(acum, resto, sizeof(acum) - 1);
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <host> <puerto>\n", argv[0]);
        fprintf(stderr, "Ej:  %s localhost 8080\n", argv[0]);
        return 1;
    }

    const char *host   = argv[1];
    const char *puerto = argv[2];

    /* ── Sala y token por consola ─────────────────────────── */
    char sala_id[8]  = "0";
    char token[64]   = "token123";

    printf("Sala ID [0]: ");
    fflush(stdout);
    if (fgets(sala_id, sizeof(sala_id), stdin)) {
        sala_id[strcspn(sala_id, "\n")] = '\0';
        if (strlen(sala_id) == 0) strcpy(sala_id, "0");
    }

    printf("Token [token123]: ");
    fflush(stdout);
    if (fgets(token, sizeof(token), stdin)) {
        token[strcspn(token, "\n")] = '\0';
        if (strlen(token) == 0) strcpy(token, "token123");
    }

    /* ── Resolución DNS — sin IP hardcodeada ──────────────── */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, puerto, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(status));
        return 1;
    }

    /* ── Crear socket y conectar ──────────────────────────── */
    sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_fd < 0) { perror("socket"); return 1; }

    if (connect(sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        fprintf(stderr, "¿Está el servidor corriendo en %s:%s?\n", host, puerto);
        return 1;
    }
    freeaddrinfo(res);

    printf("Conectado a %s:%s\n", host, puerto);

    /* ── JOIN ─────────────────────────────────────────────── */
    char join_msg[128];
    snprintf(join_msg, sizeof(join_msg), "JOIN|%s|%s", sala_id, token);
    enviar(join_msg);

    /* ── Hilo receptor ────────────────────────────────────── */
    pthread_t tid;
    pthread_create(&tid, NULL, hilo_recibir, NULL);
    pthread_detach(tid);

    /* ── Modo raw para leer teclas sin Enter ──────────────── */
    activar_raw();
    dibujar_mapa();

    /* ── Bucle de entrada ─────────────────────────────────── */
    while (corriendo) {
        char tecla = getchar();
        char cmd[BUF_SIZE];
        int  nx, ny;

        pthread_mutex_lock(&mutex);
        nx = pos_x;
        ny = pos_y;
        pthread_mutex_unlock(&mutex);

        switch (tecla) {
            /* Movimiento x1 */
            case 'w': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx, ny-1); enviar(cmd); break;
            case 's': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx,   ny+1); enviar(cmd); break;
            case 'a': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx-1, ny);   enviar(cmd); break;
            case 'd': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx+1, ny);   enviar(cmd); break;
            /* Movimiento x5 */
            case 'W': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx,   ny-5); enviar(cmd); break;
            case 'S': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx,   ny+5); enviar(cmd); break;
            case 'A': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx-5, ny);   enviar(cmd); break;
            case 'D': snprintf(cmd, sizeof(cmd), "MOVE|%d|%d", nx+5, ny);   enviar(cmd); break;
            /* Acciones */
            case 'e': enviar("SCAN|5"); break;
            case 'f': {
                /* Busca recurso cercano */
                int atacado = 0;
                for (int r = 0; r < NUM_RECURSOS; r++) {
                    int dx = abs(RECURSOS[r].x - nx);
                    int dy = abs(RECURSOS[r].y - ny);
                    if (dx <= 5 && dy <= 5) {
                        snprintf(cmd, sizeof(cmd), "ATTACK|%d", r);
                        enviar(cmd);
                        atacado = 1;
                        break;
                    }
                }
                if (!atacado) {
                    pthread_mutex_lock(&mutex);
                    strncpy(ultimo_msg, "No hay recurso cerca para atacar", BUF_SIZE-1);
                    pthread_mutex_unlock(&mutex);
                    dibujar_mapa();
                }
                break;
            }
            case 'm': {
                /* Busca recurso bajo ataque cercano */
                int mitigado = 0;
                for (int r = 0; r < NUM_RECURSOS; r++) {
                    int dx = abs(RECURSOS[r].x - nx);
                    int dy = abs(RECURSOS[r].y - ny);
                    if (dx <= 5 && dy <= 5 && alertas[r]) {
                        snprintf(cmd, sizeof(cmd), "MITIGATE|%d", r);
                        enviar(cmd);
                        mitigado = 1;
                        break;
                    }
                }
                if (!mitigado) {
                    pthread_mutex_lock(&mutex);
                    strncpy(ultimo_msg, "No hay recurso bajo ataque cerca", BUF_SIZE-1);
                    pthread_mutex_unlock(&mutex);
                    dibujar_mapa();
                }
                break;
            }
            case 'q':
            case 'Q':
                corriendo = 0;
                break;
            default:
                break;
        }
    }

    /* ── Salida limpia ────────────────────────────────────── */
    restaurar_terminal();
    enviar("QUIT");
    close(sock_fd);
    limpiar_pantalla();
    printf("Sesión terminada.\n");
    return 0;
}
