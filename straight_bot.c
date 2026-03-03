/*
 * straight_bot.c — C port of the Quoridor "straight-runner" bot.
 *
 * Mirrors the Python bot_base.py + straight_bot.py architecture exactly:
 *
 *   Main thread        — reader loop: reads lines from socket, calls dispatch_line()
 *   Writer thread      — drains send_queue, writes "line\n" to socket
 *   Per-game threads   — one per active game; wait on per-game queue, call make_move()
 *
 * Platform: Linux / POSIX (pthreads + POSIX sockets).
 * Build:    gcc -Wall -Wextra -pthread -o straight_bot straight_bot.c
 * Usage:    ./straight_bot --token <TOKEN> [--host HOST] [--port PORT]
 *           BOT_TOKEN environment variable is also accepted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

#define BOARD_SIZE      9
#define WALL_RANGE      8
#define INITIAL_WALLS  10
#define GOAL_ROW_P1     0
#define GOAL_ROW_P2     8
#define SIDE_P1         0
#define SIDE_P2         1

#define MAX_GAMES      32   /* max concurrent games */
#define MAX_MOVES       8   /* max legal pawn moves per position */

#define GAME_ID_LEN    64
#define NAME_LEN      128
#define READ_BUF    16384   /* socket read buffer */
#define BOARD_STR_BUF 1024  /* board-state string from YOUR_TURN */
#define SEND_BUF      1024  /* outbound line buffer */
#define GAME_Q_CAP       8  /* ring-buffer depth per game */

/* ── Data structures ─────────────────────────────────────────────────────── */

typedef struct {
    int r, c;
} Cell;

/* Lightweight board parsed from a YOUR_TURN board-state string. */
typedef struct {
    int     p1r, p1c;       /* P1 position */
    int     p2r, p2c;       /* P2 position */
    int     wl[2];          /* walls left: wl[SIDE_P1], wl[SIDE_P2] */
    uint8_t hw[8][8];       /* hw[r][c] = 1: H-wall anchored at (r,c) */
    uint8_t vw[8][8];       /* vw[r][c] = 1: V-wall anchored at (r,c) */
    int     my_side;        /* SIDE_P1 or SIDE_P2 */
} LocalBoard;

/* ── Thread-safe global send queue (linked list) ─────────────────────────── */

typedef struct SendNode {
    char           *line;
    struct SendNode *next;
} SendNode;

typedef struct {
    SendNode       *head, *tail;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} SendQueue;

static void sq_init(SendQueue *sq) {
    sq->head = sq->tail = NULL;
    pthread_mutex_init(&sq->mu, NULL);
    pthread_cond_init(&sq->cv, NULL);
}

static void sq_push(SendQueue *sq, const char *line) {
    SendNode *node = malloc(sizeof(SendNode));
    if (!node) { perror("malloc"); return; }
    node->line = strdup(line);
    node->next = NULL;
    pthread_mutex_lock(&sq->mu);
    if (sq->tail) sq->tail->next = node; else sq->head = node;
    sq->tail = node;
    pthread_cond_signal(&sq->cv);
    pthread_mutex_unlock(&sq->mu);
}

/* Pull one node from the front; blocks until one is available or stop is set.
 * Returns the node (caller must free), or NULL if stop was set.            */
static SendNode *sq_pop(SendQueue *sq, volatile int *stop) {
    pthread_mutex_lock(&sq->mu);
    while (sq->head == NULL && !*stop)
        pthread_cond_wait(&sq->cv, &sq->mu);
    SendNode *node = sq->head;
    if (node) {
        sq->head = node->next;
        if (!sq->head) sq->tail = NULL;
    }
    pthread_mutex_unlock(&sq->mu);
    return node;
}

/* ── Per-game message queue (ring buffer) ────────────────────────────────── */

typedef struct {
    char            items[GAME_Q_CAP][BOARD_STR_BUF];
    int             is_sentinel[GAME_Q_CAP]; /* 1 = game-end sentinel */
    int             head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} GameQueue;

static void gq_init(GameQueue *gq) {
    memset(gq, 0, sizeof(*gq));
    pthread_mutex_init(&gq->mu, NULL);
    pthread_cond_init(&gq->cv, NULL);
}

static void gq_push(GameQueue *gq, const char *board_str, int is_sentinel) {
    pthread_mutex_lock(&gq->mu);
    /* Wait if the ring buffer is full (server shouldn't outrun the bot). */
    while (gq->count == GAME_Q_CAP)
        pthread_cond_wait(&gq->cv, &gq->mu);
    gq->is_sentinel[gq->tail] = is_sentinel;
    if (!is_sentinel && board_str)
        strncpy(gq->items[gq->tail], board_str, BOARD_STR_BUF - 1);
    gq->tail = (gq->tail + 1) % GAME_Q_CAP;
    gq->count++;
    pthread_cond_signal(&gq->cv);
    pthread_mutex_unlock(&gq->mu);
}

static void gq_pop(GameQueue *gq, char *out_board, int *out_sentinel) {
    pthread_mutex_lock(&gq->mu);
    while (gq->count == 0)
        pthread_cond_wait(&gq->cv, &gq->mu);
    *out_sentinel = gq->is_sentinel[gq->head];
    if (!*out_sentinel && out_board)
        strncpy(out_board, gq->items[gq->head], BOARD_STR_BUF - 1);
    gq->head = (gq->head + 1) % GAME_Q_CAP;
    gq->count--;
    pthread_cond_broadcast(&gq->cv); /* wake any blocked gq_push */
    pthread_mutex_unlock(&gq->mu);
}

/* ── Per-game state ───────────────────────────────────────────────────────── */

typedef struct {
    char       game_id[GAME_ID_LEN];
    int        my_side;             /* SIDE_P1 or SIDE_P2 */
    char       opponent[NAME_LEN];
    GameQueue  gq;
    pthread_t  worker;
    int        active;
    int        sidestep_dc;         /* current sidestep col-delta: -1 or +1 */
    int        sidestep_set;        /* 0 = not yet chosen for this game */
} GameState;

/* ── Global bot state ────────────────────────────────────────────────────── */

typedef struct BotState {
    int          sockfd;
    SendQueue    sq;
    GameState    games[MAX_GAMES];
    char         name[NAME_LEN];
    int          version;
    volatile int stop;
    char         token[256];
} BotState;

/* ── Utility: formatted send ─────────────────────────────────────────────── */

static void send_linef(BotState *bot, const char *fmt, ...) {
    char buf[SEND_BUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[bot] >> %s\n", buf);
    fflush(stdout);
    sq_push(&bot->sq, buf);
}

/* ── Game slot management ────────────────────────────────────────────────── */

static GameState *find_game(BotState *bot, const char *game_id) {
    for (int i = 0; i < MAX_GAMES; i++)
        if (bot->games[i].active &&
            strcmp(bot->games[i].game_id, game_id) == 0)
            return &bot->games[i];
    return NULL;
}

static GameState *alloc_game(BotState *bot) {
    for (int i = 0; i < MAX_GAMES; i++)
        if (!bot->games[i].active)
            return &bot->games[i];
    return NULL;
}

/* ── Board helpers ───────────────────────────────────────────────────────── */

/*
 * H-wall anchor (r,c) blocks the edge between row r and row r+1 in
 * columns c and c+1.  So moving *south* from (r,c) is blocked by
 * hw[r][c] (anchor at (r,c)) OR hw[r][c-1] (anchor one column left).
 */
static int blocks_south(const LocalBoard *b, int r, int c) {
    return b->hw[r][c] || (c > 0 && b->hw[r][c-1]);
}

/*
 * V-wall anchor (r,c) blocks the edge between col c and col c+1 in
 * rows r and r+1.  So moving *east* from (r,c) is blocked by
 * vw[r][c] OR vw[r-1][c].
 */
static int blocks_east(const LocalBoard *b, int r, int c) {
    return b->vw[r][c] || (r > 0 && b->vw[r-1][c]);
}

/* Returns 1 if a one-step move from (fr,fc) to (tr,tc) is unblocked. */
static int can_step(const LocalBoard *b, int fr, int fc, int tr, int tc) {
    if (tr < 0 || tr >= BOARD_SIZE || tc < 0 || tc >= BOARD_SIZE) return 0;
    int dr = tr - fr, dc = tc - fc;
    if (abs(dr) + abs(dc) != 1) return 0;
    if (dr ==  1) return !blocks_south(b, fr, fc);
    if (dr == -1) return !blocks_south(b, tr, tc);
    if (dc ==  1) return !blocks_east(b, fr, fc);
    /* dc == -1 */  return !blocks_east(b, tr, tc);
}

/* Returns 1 if move (tr,tc) is in the legal-move set. */
static int in_legal(const Cell *moves, int n, int tr, int tc) {
    for (int i = 0; i < n; i++)
        if (moves[i].r == tr && moves[i].c == tc) return 1;
    return 0;
}

/*
 * Compute all legal pawn moves for the active side.
 * Mirrors LocalBoard.legal_moves() in bot_base.py.
 * Returns the number of moves; out[] is filled with (row, col) pairs.
 */
static int legal_moves(const LocalBoard *b, Cell out[MAX_MOVES]) {
    int pr, pc, or_, oc;
    if (b->my_side == SIDE_P1) {
        pr = b->p1r; pc = b->p1c;
        or_ = b->p2r; oc = b->p2c;
    } else {
        pr = b->p2r; pc = b->p2c;
        or_ = b->p1r; oc = b->p1c;
    }

    int count = 0;
    int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};

    for (int d = 0; d < 4 && count < MAX_MOVES; d++) {
        int dr = dirs[d][0], dc = dirs[d][1];
        int nr = pr + dr, nc = pc + dc;

        if (!can_step(b, pr, pc, nr, nc)) continue;

        if (nr == or_ && nc == oc) {
            /* Opponent occupies this cell — try straight jump first. */
            int jr = nr + dr, jc = nc + dc;
            if (can_step(b, nr, nc, jr, jc)) {
                if (count < MAX_MOVES) { out[count].r = jr; out[count].c = jc; count++; }
            } else {
                /* Straight jump blocked — offer lateral (diagonal) jumps. */
                int lat[2][2] = {{-dc, dr}, {dc, -dr}};
                for (int l = 0; l < 2 && count < MAX_MOVES; l++) {
                    int sr = nr + lat[l][0], sc = nc + lat[l][1];
                    if (can_step(b, nr, nc, sr, sc)) {
                        out[count].r = sr; out[count].c = sc; count++;
                    }
                }
            }
        } else {
            out[count].r = nr; out[count].c = nc; count++;
        }
    }
    return count;
}

/* ── parse_board ─────────────────────────────────────────────────────────── */

/*
 * Parse a board-state token string into a LocalBoard.
 * Input format (space-separated key:value tokens):
 *   p1:8,4 p2:0,4 wl:10,10 hw:3,4;5,6 vw:2,3
 * hw/vw values are semicolon-separated "r,c" pairs; empty when no walls.
 */
static void parse_board(const char *board_str, int my_side, LocalBoard *b) {
    memset(b, 0, sizeof(*b));
    b->my_side = my_side;

    char buf[BOARD_STR_BUF];
    strncpy(buf, board_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *sp_outer;
    char *tok = strtok_r(buf, " ", &sp_outer);
    while (tok) {
        char *colon = strchr(tok, ':');
        if (!colon) { tok = strtok_r(NULL, " ", &sp_outer); continue; }
        *colon = '\0';
        const char *key = tok;
        char       *val = colon + 1;

        if (strcmp(key, "p1") == 0) {
            sscanf(val, "%d,%d", &b->p1r, &b->p1c);
        } else if (strcmp(key, "p2") == 0) {
            sscanf(val, "%d,%d", &b->p2r, &b->p2c);
        } else if (strcmp(key, "wl") == 0) {
            sscanf(val, "%d,%d", &b->wl[0], &b->wl[1]);
        } else if (strcmp(key, "hw") == 0) {
            char *sp_inner, *seg = strtok_r(val, ";", &sp_inner);
            while (seg) {
                int r, c;
                if (sscanf(seg, "%d,%d", &r, &c) == 2 &&
                    r >= 0 && r < WALL_RANGE && c >= 0 && c < WALL_RANGE)
                    b->hw[r][c] = 1;
                seg = strtok_r(NULL, ";", &sp_inner);
            }
        } else if (strcmp(key, "vw") == 0) {
            char *sp_inner, *seg = strtok_r(val, ";", &sp_inner);
            while (seg) {
                int r, c;
                if (sscanf(seg, "%d,%d", &r, &c) == 2 &&
                    r >= 0 && r < WALL_RANGE && c >= 0 && c < WALL_RANGE)
                    b->vw[r][c] = 1;
                seg = strtok_r(NULL, ";", &sp_inner);
            }
        }
        tok = strtok_r(NULL, " ", &sp_outer);
    }
}

/* ── Straight-bot move strategy ──────────────────────────────────────────── */

/*
 * Mirrors StraightBot.make_move() in straight_bot.py:
 *
 *   1. Move forward toward goal if that cell is legal.
 *   2. Else jump-over if (r + 2*fdr, c) is legal.
 *   3. Else sidestep: choose a column-delta, try it, flip if blocked.
 *   4. Fallback: any legal move.
 *
 * Never places walls.
 */
static void make_move(BotState *bot, GameState *game, const char *board_str) {
    LocalBoard board;
    parse_board(board_str, game->my_side, &board);

    Cell moves[MAX_MOVES];
    int  nmoves = legal_moves(&board, moves);
    if (nmoves == 0) {
        fprintf(stderr, "[bot] WARNING: no legal moves for game %s\n",
                game->game_id);
        return;
    }

    int pr = (game->my_side == SIDE_P1) ? board.p1r : board.p2r;
    int pc = (game->my_side == SIDE_P1) ? board.p1c : board.p2c;
    int fdr = (game->my_side == SIDE_P1) ? -1 : 1; /* row-delta toward goal */

    /* 1. Forward */
    if (in_legal(moves, nmoves, pr + fdr, pc)) {
        game->sidestep_set = 0;
        send_linef(bot, "MOVE %s %d %d", game->game_id, pr + fdr, pc);
        return;
    }

    /* 2. Jump-over (fwd_op: two steps forward, valid when opponent is adjacent) */
    if (in_legal(moves, nmoves, pr + 2 * fdr, pc)) {
        game->sidestep_set = 0;
        send_linef(bot, "MOVE %s %d %d", game->game_id, pr + 2 * fdr, pc);
        return;
    }

    /* 3. Sidestep: initialise direction once per game, then try / flip */
    if (!game->sidestep_set) {
        game->sidestep_dc  = (rand() % 2 == 0) ? -1 : 1;
        game->sidestep_set = 1;
    }
    int try_dc[2] = {game->sidestep_dc, -game->sidestep_dc};
    for (int t = 0; t < 2; t++) {
        if (in_legal(moves, nmoves, pr, pc + try_dc[t])) {
            game->sidestep_dc = try_dc[t];
            send_linef(bot, "MOVE %s %d %d", game->game_id, pr, pc + try_dc[t]);
            return;
        }
    }

    /* 4. Fallback: any legal move */
    send_linef(bot, "MOVE %s %d %d", game->game_id, moves[0].r, moves[0].c);
}

/* ── Per-game worker thread ──────────────────────────────────────────────── */

typedef struct { BotState *bot; GameState *game; } WorkerArg;

static void *game_worker(void *arg) {
    WorkerArg *wa   = (WorkerArg *)arg;
    BotState  *bot  = wa->bot;
    GameState *game = wa->game;
    free(wa);

    char board_str[BOARD_STR_BUF];
    int  sentinel;

    while (1) {
        gq_pop(&game->gq, board_str, &sentinel);
        if (sentinel) break;           /* GAME_END received */
        make_move(bot, game, board_str);
    }
    return NULL;
}

/* ── Writer thread ───────────────────────────────────────────────────────── */

static void *writer_thread(void *arg) {
    BotState *bot = (BotState *)arg;

    while (!bot->stop) {
        SendNode *node = sq_pop(&bot->sq, &bot->stop);
        if (!node) break;

        /* Write "line\n" to the socket, handling short sends. */
        char   buf[SEND_BUF + 2];
        int    len = snprintf(buf, sizeof(buf), "%s\n", node->line);
        free(node->line);
        free(node);

        int sent = 0;
        while (sent < len) {
            ssize_t n = send(bot->sockfd, buf + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) { bot->stop = 1; break; }
            sent += (int)n;
        }
    }
    return NULL;
}

/* ── Protocol dispatcher ─────────────────────────────────────────────────── */

/*
 * Called for every complete line received from the server.
 * `line` is a mutable NUL-terminated string; dispatch_line may modify it
 * via strtok_r (the caller has already moved it into a local copy).
 */
static void dispatch_line(BotState *bot, char *line) {
    printf("[bot] << %s\n", line);
    fflush(stdout);

    char *sp;
    char *cmd = strtok_r(line, " ", &sp);
    if (!cmd) return;

    /* ── HELLO ── */
    if (strcmp(cmd, "HELLO") == 0) {
        send_linef(bot, "AUTH %s", bot->token);
    }

    /* ── AUTH_OK <name> <version> ── */
    else if (strcmp(cmd, "AUTH_OK") == 0) {
        char *name = strtok_r(NULL, " ", &sp);
        char *ver  = strtok_r(NULL, " ", &sp);
        if (name) strncpy(bot->name, name, NAME_LEN - 1);
        if (ver)  bot->version = atoi(ver);
        printf("[bot] authenticated as %s v%d\n", bot->name, bot->version);
        fflush(stdout);
    }

    /* ── AUTH_FAIL <reason> ── */
    else if (strcmp(cmd, "AUTH_FAIL") == 0) {
        char *reason = strtok_r(NULL, " ", &sp);
        fprintf(stderr, "[bot] auth failed: %s\n", reason ? reason : "?");
        bot->stop = 1;
    }

    /* ── HEARTBEAT ── */
    else if (strcmp(cmd, "HEARTBEAT") == 0) {
        send_linef(bot, "HEARTBEAT_ACK");
    }

    /* ── GAME_START <game_id> <opponent> <your_side> <first_mover> <walls> ── */
    else if (strcmp(cmd, "GAME_START") == 0) {
        char *game_id  = strtok_r(NULL, " ", &sp);
        char *opponent = strtok_r(NULL, " ", &sp);
        char *side_str = strtok_r(NULL, " ", &sp);
        if (!game_id || !opponent || !side_str) return;

        int my_side = (strcmp(side_str, "P1") == 0) ? SIDE_P1 : SIDE_P2;

        GameState *game = alloc_game(bot);
        if (!game) {
            fprintf(stderr, "[bot] too many concurrent games — dropping %s\n",
                    game_id);
            return;
        }

        memset(game, 0, sizeof(*game));
        strncpy(game->game_id,  game_id,  GAME_ID_LEN - 1);
        strncpy(game->opponent, opponent, NAME_LEN - 1);
        game->my_side     = my_side;
        game->active      = 1;
        game->sidestep_set = 0;
        gq_init(&game->gq);

        WorkerArg *wa = malloc(sizeof(WorkerArg));
        if (!wa) { perror("malloc"); game->active = 0; return; }
        wa->bot  = bot;
        wa->game = game;
        pthread_create(&game->worker, NULL, game_worker, wa);

        printf("[bot] game started: %.36s  (%s vs %s)\n",
               game_id, side_str, opponent);
        fflush(stdout);
    }

    /*
     * ── YOUR_TURN <game_id> p1:r,c p2:r,c wl:n,n hw:r,c;... vw:r,c;... ──
     *
     * After strtok_r extracts game_id, the save-pointer `sp` points to the
     * start of the board-state tokens ("p1:...").  We copy that substring
     * before returning so the reader can reuse its buffer.
     */
    else if (strcmp(cmd, "YOUR_TURN") == 0) {
        char *game_id   = strtok_r(NULL, " ", &sp);
        /* sp now points to the board-state part of the line */
        char *board_str = sp;
        if (!game_id || !board_str || !*board_str) return;

        GameState *game = find_game(bot, game_id);
        if (!game) return;

        gq_push(&game->gq, board_str, 0);
    }

    /* ── MOVE_OK <game_id> ── */
    else if (strcmp(cmd, "MOVE_OK") == 0) {
        /* no-op — server acknowledged our move */
    }

    /* ── GAME_END <game_id> WIN|LOSS|DRAW <reason> ── */
    else if (strcmp(cmd, "GAME_END") == 0) {
        char *game_id = strtok_r(NULL, " ", &sp);
        char *result  = strtok_r(NULL, " ", &sp);
        char *reason  = strtok_r(NULL, " ", &sp);
        printf("[bot] game %s ended — %s (%s)\n",
               game_id ? game_id : "?",
               result  ? result  : "?",
               reason  ? reason  : "?");
        fflush(stdout);

        if (!game_id) return;
        GameState *game = find_game(bot, game_id);
        if (!game) return;

        gq_push(&game->gq, NULL, 1);   /* sentinel — stops game worker */
        pthread_join(game->worker, NULL);
        game->active = 0;
    }

    /* ── ERROR <message> ── */
    else if (strcmp(cmd, "ERROR") == 0) {
        fprintf(stderr, "[bot] server error: %s\n", sp ? sp : "");
    }
}

/* ── Reader loop (main thread) ───────────────────────────────────────────── */

static void reader_loop(BotState *bot) {
    static char rbuf[READ_BUF];
    int rlen = 0;

    while (!bot->stop) {
        ssize_t n = recv(bot->sockfd, rbuf + rlen,
                         sizeof(rbuf) - rlen - 1, 0);
        if (n <= 0) { bot->stop = 1; break; }
        rlen += (int)n;
        rbuf[rlen] = '\0';

        /* Dispatch every complete newline-terminated line. */
        char *start = rbuf;
        char *nl;
        while ((nl = memchr(start, '\n', (size_t)(rbuf + rlen - start))) != NULL) {
            *nl = '\0';
            if (nl > start && *(nl - 1) == '\r') *(nl - 1) = '\0'; /* strip \r */
            if (*start)
                dispatch_line(bot, start);
            start = nl + 1;
        }

        /* Preserve any partial line at the beginning of rbuf. */
        int remaining = (int)(rbuf + rlen - start);
        memmove(rbuf, start, remaining);
        rlen = remaining;
    }

    printf("[bot] connection closed by server\n");
    fflush(stdout);
}

/* ── TCP connect helper ───────────────────────────────────────────────────── */

static int connect_to_server(const char *host, const char *port_str) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *host   = "127.0.0.1";
    const char *port_s = "5555";
    char        token[256] = "";

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--host")  == 0 && i + 1 < argc) host   = argv[++i];
        else if (strcmp(argv[i], "--port")  == 0 && i + 1 < argc) port_s = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc)
            strncpy(token, argv[++i], sizeof(token) - 1);
    }

    /* Fall back to BOT_TOKEN environment variable, matching the Python bot. */
    if (!*token) {
        const char *env = getenv("BOT_TOKEN");
        if (env) strncpy(token, env, sizeof(token) - 1);
    }

    if (!*token) {
        fprintf(stderr,
                "Error: no token provided. Use --token or set BOT_TOKEN env var.\n");
        return 1;
    }

    srand((unsigned)time(NULL));

    BotState bot;
    memset(&bot, 0, sizeof(bot));
    strncpy(bot.token, token, sizeof(bot.token) - 1);
    sq_init(&bot.sq);

    bot.sockfd = connect_to_server(host, port_s);
    if (bot.sockfd < 0) {
        fprintf(stderr, "[bot] failed to connect to %s:%s\n", host, port_s);
        return 1;
    }
    printf("[bot] connected to %s:%s\n", host, port_s);
    fflush(stdout);

    pthread_t writer;
    pthread_create(&writer, NULL, writer_thread, &bot);

    reader_loop(&bot);   /* blocks until the connection closes */

    /* Tear down: signal the writer thread and wait for it. */
    bot.stop = 1;
    pthread_cond_signal(&bot.sq.cv);
    pthread_join(writer, NULL);

    close(bot.sockfd);
    return 0;
}
