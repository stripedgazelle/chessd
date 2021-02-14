#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/random.h>
#include <dirent.h>

#define MAX_PLAYERS 20
#define MAX_NAME 30
#define MAX_PATH (100+MAX_NAME)
#define MAX_CHAT 1500
#define MAX_TRANSCRIPT 1000
#define MAX_CHAT_NL 26
#define MAX_IP 50
#define cti(XX,YY) ((('8' - (YY)) * 8) + ((XX) - 'a') + 1)
#define VS "_vs_"
#define VSLEN 4

static int legit_move (char fromx, char fromy, char tox, char toy, char *pos);

static char *log_path = "chessd.log";
static FILE *log_out;

struct game {
    char name[MAX_NAME];
    char start_pos[66];
    char sel[2]; //0->SELX, 1->SELY
    char pos[66];
    char unused[2];
    int sequence;
    time_t update_t;
    char chat[MAX_CHAT];
    int chatlen;
    int chatshift;
    int movenum;
    char transcript[MAX_TRANSCRIPT];
    int transcriptlen;
    int seen;
    int fics;
    int fics_style;
    struct game *next;
};
static struct game *games = NULL;

struct player {
    char name[MAX_NAME];
    char touch;
    char show;
    struct player *next;
};
static struct player *players = NULL;
static int players_count = 0;

static char prom = 'Q';

static int god_sequence;
static time_t save_t;
static int save_sequence;

static char current_ip[MAX_IP];

static struct game *current_game = NULL;

static int http_fd;
static FILE *http_out;

struct png {
    struct png *next;
    char path[MAX_PATH + 1];
    char data[4096];
    int size;
};
static struct png *pngs;

static int term = 0;

static char *
cnow (void)
{
    time_t now_t = time (NULL);
    static char str[50];
    struct tm *now_tm;

    now_tm = localtime (&now_t);
    sprintf (str, "%04d-%02d-%02d %02d:%02d:%02d",
             now_tm->tm_year + 1900, now_tm->tm_mon + 1, now_tm->tm_mday,
             now_tm->tm_hour, now_tm->tm_min, now_tm->tm_sec);

    return (str);
}

static void
tick_game (struct game *g)
{
    g->sequence++;
    if (!g->fics) {
        god_sequence++;
    }
    g->update_t = time (NULL);
}

static void
tick (void)
{
    tick_game (current_game);
}

static struct game *
get_game (char *name)
{
    struct game *g = NULL;

    if (name) {
        for (g = games; g; g = g->next) {
            if (!strcmp (g->name, name)) {
                return (g);
            }
        }
    }

    return (g);
}

static void
set_current_game (char *name)
{
    for (current_game = games;
         current_game;
         current_game = current_game->next) {
        if (!strcasecmp (current_game->name, name)) {
            return;
        }
    }
}

static int
save_game (struct game *g)
{
    char path[MAX_PATH];
    char tmppath[MAX_PATH + 4];
    FILE *out;
    int i;

    sprintf (path, ".chessd/%s", g->name);
    sprintf (tmppath, "%s.tmp", path);

    out = fopen (tmppath, "w");
    if (!out) {
        fprintf (log_out, "%s: failed to open %s\n", cnow (), tmppath);
        return (-1);
    }

    if (fprintf (out, "%c%c%.65s%d\n",
                 g->unused[0], g->unused[1], g->start_pos, g->movenum) < 0
     || fprintf (out, "%c%c%.65s%d\n",
                 g->sel[0], g->sel[1], g->pos, g->sequence) < 0
     || fwrite (g->transcript, 1, g->transcriptlen, out) < g->transcriptlen
     || fwrite ("\0", 1, 1, out) < 1
     || fwrite (g->chat, 1, g->chatlen, out) < g->chatlen) {
        fprintf (log_out, "%s: failed to write %s\n", cnow (), tmppath);
        return (-1);
    }

    if (fclose (out)) {
        fprintf (log_out, "%s: failed to close %s\n", cnow (), tmppath);
        return (-1);
    }

    return (rename (tmppath, path));
}

static int
save_games (void)
{
    struct game *g;
    int rc = 0;

    for (g = games; g; g = g->next) {
        if (save_game (g)) {
            rc = 1;
        } else {
            fprintf (log_out, "%s: save_games %s\n",
                     cnow (), g->name);
        }
    }

    return (rc);
}

static struct player *
get_player (char *name)
{
    struct player *p;

    for (p = players; p; p = p->next) {
        if (!strcmp (name, p->name)) {
            return (p);
        }
    }

    return (NULL);
}

static void
add_player (char *name)
{
    struct player *p;

    if ((p = get_player (name))) {
        return;
    }

    p = calloc (1, sizeof (*p));
    strncpy (p->name, name, sizeof (p->name));
    p->name[sizeof (p->name) - 1] = '\0';
    p->touch = 'N'; //default touch move
    p->show = 'Y'; //default show moves
    p->next = players;
    players = p;

    fprintf (log_out, "%s: add_player %d %s\n",
             cnow (), ++players_count, p->name);
}

static void
add_players (char *game_name)
{
    char *vs;
    char buf[MAX_NAME];

    vs = strstr (game_name, VS);
    if (vs) {
        strcpy (buf, game_name);
        buf[vs-game_name] = '\0';
        add_player (buf);
        add_player (vs + VSLEN);
    } else {
        add_player (game_name);
    }
}

static struct game *
load_game (char *name)
{
    char path[MAX_PATH];
    FILE *in;
    struct game *g;
    int c;
    int i;

    if (strlen (name) > MAX_NAME) {
        return (NULL);
    }

    sprintf (path, ".chessd/%s", name);
    in = fopen (path, "r");

    g = calloc (1, sizeof (*g));

    strncpy (g->name, name, sizeof (g->name));
    g->name[sizeof (g->name) - 1] = '\0';

    /* scan start position */
    c = fscanf (in, "%c%c%65c%d\n",
                &g->unused[0], &g->unused[1], g->start_pos, &g->movenum);
    if (c != 4) {
        free (g);
        fprintf (log_out, "%s: failed to scan %s\n", cnow (), path);
        return (NULL);
    }

    /* scan current position */
    c = fscanf (in, "%c%c%65c%d\n",
                &g->sel[0], &g->sel[1], g->pos, &g->sequence);
    if (c != 4) {
        free (g);
        fprintf (log_out, "%s: failed to scan %s\n", cnow (), path);
        return (NULL);
    }

    /* scan transcript */
    while ((c = fgetc (in))) {
        if (c == EOF) {
            goto load_game_close;
        }
        if (g->transcriptlen < MAX_TRANSCRIPT) {
            g->transcript[g->transcriptlen++] = c;
        }
    } while (c);

    /* scan chat */
    while ((c = fgetc (in))) {
        if (c == EOF) {
            goto load_game_close;
        }
        if (g->chatlen < MAX_CHAT) {
            g->chat[g->chatlen++] = c;
        }
    }

load_game_close:
    fclose (in);

    add_players (g->name);

    return (g);
}

static int
load_games (void)
{
    DIR *dp;
    struct dirent *ep;
    int rc = 0;

    games = NULL; //FIXME free existing? shouldn't be any...

    mkdir (".chessd", 0777);
    dp = opendir (".chessd/");
    if (dp != NULL) {
        while ((ep = readdir (dp))) {
            if (ep->d_name[0] && ep->d_name[0] != '.') {
                struct game *g;
                if ((g = load_game (ep->d_name))) {
                    g->next = games;
                    games = g;
                    fprintf (log_out, "%s: load_games %s\n",
                             cnow (), g->name);
                } else {
                    rc = 1;
                }
            }
        }
        closedir (dp);
    } else {
        fprintf (log_out, "%s: could not open .chessd/\n", cnow ());
        rc = -1;
    }

    return (rc);
}

static int
persist (void)
{
    save_t = time (NULL);

    if (save_games ()) {
        fprintf (log_out, "%s: save_games failed!\n", cnow ());
        return (1);
    }

    save_sequence = god_sequence;
    return (0);
}

static int
rb (void)
{
    int size;
    char r[1];

    size = getrandom (r, 1, GRND_NONBLOCK);
    if (size != 1) {
        fprintf (log_out, "%s: getrandom failed!\n", cnow ());
    }

    return (r[0]);
}

static char *
fischer (int lb, int db, int q, int n1, int n2, int k)
{
    static char pos[66];
    int temp;

    memcpy (pos,
            "W++++++++pppppppp++++++++++++++++++++++++++++++++PPPPPPPP++++++++",
            66);

    /* dark square bishop */
    temp = 'a' + (db * 2);
    pos[cti(temp, '8')] = 'b';
    pos[cti(temp, '1')] = 'B';

    /* light square bishop */
    temp = 'a' + (lb * 2) + 1;
    pos[cti(temp, '8')] = 'b';
    pos[cti(temp, '1')] = 'B';

    /* queen */
    temp = 'a' - 1;
    do {
        while (pos[cti(++temp, '8')] != '+');
    } while (q-- > 0);
    pos[cti(temp, '8')] = 'q';
    pos[cti(temp, '1')] = 'Q';

    /* knight */
    temp = 'a' - 1;
    do {
        while (pos[cti(++temp, '8')] != '+');
    } while (n1-- > 0);
    pos[cti(temp, '8')] = 'n';
    pos[cti(temp, '1')] = 'N';

    /* knight */
    temp = 'a' - 1;
    do {
        while (pos[cti(++temp, '8')] != '+');
    } while (n2-- > 0);
    pos[cti(temp, '8')] = 'n';
    pos[cti(temp, '1')] = 'N';

    /* rook, king, rook */
    temp = 'a' - 1;
    while (pos[cti(++temp, '8')] != '+');
    if (k == 0) {
        pos[cti(temp, '8')] = 'k';
        pos[cti(temp, '1')] = 'K';
    } else if (k < 0) {
        pos[cti(temp, '8')] = 'o';
        pos[cti(temp, '1')] = 'O';
    } else {
        pos[cti(temp, '8')] = 'r';
        pos[cti(temp, '1')] = 'R';
    }
    while (pos[cti(++temp, '8')] != '+');
    if (k == 1 || k == -1) {
        pos[cti(temp, '8')] = 'k';
        pos[cti(temp, '1')] = 'K';
    } else {
        pos[cti(temp, '8')] = 'r';
        pos[cti(temp, '1')] = 'R';
    }
    while (pos[cti(++temp, '8')] != '+');
    if (k == 2) {
        pos[cti(temp, '8')] = 'k';
        pos[cti(temp, '1')] = 'K';
    } else if (k < 0) {
        pos[cti(temp, '8')] = 'o';
        pos[cti(temp, '1')] = 'O';
    } else {
        pos[cti(temp, '8')] = 'r';
        pos[cti(temp, '1')] = 'R';
    }

    return (pos);
}

static char *
classical_pos (void)
{
    static char pos[66];

    if (pos[0] != 'W') {
        memcpy (pos, fischer (2, 1, 2, 1, 2, -1), 66);
    }

    return (pos);
}

static char *
chess960 (void)
{
    int lb; // light square bishop 0->b, 1->d, 2->f, 3->h
    int db; // dark square bishop 0->a, 1->c, 2->e, 3->g
    int q; // queen 0-5
    int n1; // knight 0-4
    int n2; // knight 0-3

    int temp;

    temp = rb ();
    lb = 3 & temp;

    temp >>= 2;
    db = 3 & temp;

    temp >>= 2;
    n2 = 3 & temp;

    do {
        q = 7 & rb ();
    } while (q > 5);

    do {
        n1 = 7 & rb ();
    } while (n1 > 4);

    return (fischer (lb, db, q, n1, n2, -1));
}

static char *
chess2880 (void)
{
    int lb; // light square bishop 0->b, 1->d, 2->f, 3->h
    int db; // dark square bishop 0->a, 1->c, 2->e, 3->g
    int q; // queen 0-5
    int n1; // knight 0-4
    int n2; // knight 0-3
    int k; // king 0-2

    int temp;

    temp = rb ();
    lb = 3 & temp;

    temp >>= 2;
    db = 3 & temp;

    temp >>= 2;
    n2 = 3 & temp;

    do {
        q = 7 & rb ();
    } while (q > 5);

    do {
        n1 = 7 & rb ();
    } while (n1 > 4);

    do {
        k = 3 & rb ();
    } while (k > 2);

    return (fischer (lb, db, q, n1, n2, k));
}

static char *
piece_to_img (char piece)
{
    switch (piece) {
    default:
        return ("clear");
    case 'K':
        return ("wk");
    case 'k':
        return ("bk");
    case 'Q':
        return ("wq");
    case 'q':
        return ("bq");
    case 'R':
    case 'O':
        return ("wr");
    case 'r':
    case 'o':
        return ("br");
    case 'B':
        return ("wb");
    case 'b':
        return ("bb");
    case 'N':
        return ("wn");
    case 'n':
        return ("bn");
    case 'P':
        return ("wp");
    case 'p':
        return ("bp");
    }
}

static char
piece_to_alt (char piece)
{
    switch (piece) {
    default:
        return (piece);
    case 'O':
        return ('R');
        break;
    case 'o':
        return ('r');
        break;
    case 'e':
        return ('+');
        break;
    }
}

static char
color (char piece)
{
    switch (piece) {
    default:
        return ('\0');
    case 'K':
    case 'Q':
    case 'R':
    case 'O':
    case 'B':
    case 'N':
    case 'P':
    case 'E':
        return ('W');
    case 'k':
    case 'q':
    case 'r':
    case 'o':
    case 'b':
    case 'n':
    case 'p':
    case 'e':
        return ('B');
    }
}

static int
legit_bishop_move (char fromx, char fromy, char tox, char toy, char *pos)
{
    char temp;

    if (abs (fromx - tox) != abs (fromy - toy)) {
        return (0);
    }

    if (fromx < tox) {
        for (temp = fromx + 1; temp < tox; ++temp) {
            if (toy > fromy) {
                ++fromy;
            } else {
                --fromy;
            }
            if (pos[cti (temp, fromy)] != '+') {
                return (0);
            }
        }
    } else if (fromx > tox) {
        for (temp = fromx - 1; temp > tox; --temp) {
            if (toy > fromy) {
                ++fromy;
            } else {
                --fromy;
            }
            if (pos[cti (temp, fromy)] != '+') {
                return (0);
            }
        }
    } else {
        return (0);
    }
    return (1);
}

static char
other_color (char color)
{
    return ((color == 'W') ? 'B' : 'W');
}

static int
attacked (char tox, char toy, char *pos)
{
    char fromx;
    char fromy;
    char *p;
    char true_color;
    char try[66];

    memcpy (try, pos, 65);
    try[0] = other_color (try[0]);

    p = pos + 1;
    for (fromy = '8'; fromy >= '1'; --fromy) {
        for (fromx = 'a'; fromx <= 'h'; ++fromx) {
            if (color (*(p++)) == try[0]) {
                if (legit_move (fromx, fromy, tox, toy, try)) {
                    return (1);
                }
            }
        }
    }

    return (0);
}

static int
legit_king_move (char fromx, char fromy, char tox, char toy, char *pos)
{
    if (abs (fromy - toy) > 1) {
        return (0);
    } else if (abs (fromy - toy) == 1
            || (fromy > '1' && fromy < '8')
            || attacked (fromx, fromy, pos)
            || ((tox != 'g') && (tox != 'c'))) {
        return (abs (fromx - tox) <= 1
             && color (pos[cti(tox,toy)]) != color (pos[cti(fromx,fromy)]));
    } else {
        char src;
        char dest;
        char temp;
        int direction;
        char jump_rook = '\0';
        char castle_rook = '\0';

        temp = fromx;
        if (temp < tox) {
            direction = 1;
        } else if (temp > tox) {
            direction = -1;
        }

        while (temp != tox) {
            temp += direction;
            switch (pos[cti (temp, fromy)]) {
                default:
                    return (abs (fromx - tox) == 1);
                case 'o':
                case 'O':
                    jump_rook = temp;
                    /* fall through */
                case '+':
                    if (attacked (temp, fromy, pos)) {
                        return (abs (fromx - tox) == 1);
                    }
                    break;
            }
        }

        if (tox == 'c') {
            dest = 'd';
            direction = -1;
        } else {
            dest = 'f';
            direction = 1;
        }

        for (src = fromx; src >= 'a' && src <= 'h'; src += direction) {
            switch (pos[cti (src, fromy)]) {
            default:
                break;
            case 'O':
            case 'o':
                castle_rook = src;
                if (jump_rook && jump_rook != castle_rook) {
                    /* other rook is in king's way */
                    return (0);
                }
                temp = src;
                if (temp < dest) {
                    direction = 1;
                } else if (temp > dest) {
                    direction = -1;
                }
                while (1) {
                    if (temp == src) {
                        /* do nothing (rook's own position) */
                    } else if (temp == fromx) {
                        /* do nothing (passing over king) */
                    } else if (pos[cti (temp, fromy)] != '+') {
                        /* obstacle in rook's way */
                        return (abs (fromx - tox) == 1);
                    }
                    if (temp == dest) {
                        break;
                    }
                    temp += direction;
                }
                return (1);
            }
        }
        return (abs (fromx - tox) == 1); /* no rook to castle with */
    }
    return (1);
}

static int
legit_rook_move (char fromx, char fromy, char tox, char toy, char *pos)
{
    char temp;

    if (fromx == tox) {
        if (fromy < toy) {
            for (temp = fromy + 1; temp < toy; ++temp) {
                if (pos[cti (fromx, temp)] != '+') {
                    return (0);
                }
            }
        } else if (fromy > toy) {
            for (temp = fromy - 1; temp > toy; --temp) {
                if (pos[cti (fromx, temp)] != '+') {
                    return (0);
                }
            }
        } else {
            return (0);
        }
    } else if (fromy == toy) {
        char me = pos[cti (fromx, fromy)];
        if (fromx < tox) {
            for (temp = fromx + 1; temp < tox; ++temp) {
                switch (pos[cti (temp, fromy)]) {
                default:
                    return (0);
                case '+':
                    break;
                case 'K':
                    if ((me == 'O') && (fromy == '1') && (tox == 'd')) {
                        if (!legit_king_move (temp, fromy, 'c', toy, pos)) {
                            return (0);
                        }
                        break;
                    }
                    return (0);
                case 'k':
                    if ((me == 'o') && (fromy == '8') && (tox == 'd')) {
                        if (!legit_king_move (temp, fromy, 'c', toy, pos)) {
                            return (0);
                        }
                        break;
                    }
                    return (0);
                }
            }
        } else if (fromx > tox) {
            for (temp = fromx - 1; temp > tox; --temp) {
                switch (pos[cti (temp, fromy)]) {
                default:
                    return (0);
                case '+':
                    break;
                case 'K':
                    if ((me == 'O') && (fromy == '1') && (tox == 'f')) {
                        if (!legit_king_move (temp, fromy, 'g', toy, pos)) {
                            return (0);
                        }
                        break;
                    }
                    return (0);
                case 'k':
                    if ((me == 'o') && (fromy == '8') && (tox == 'f')) {
                        if (!legit_king_move (temp, fromy, 'g', toy, pos)) {
                            return (0);
                        }
                        break;
                    }
                    return (0);
                }
            }
        } else {
            return (0);
        }
    } else {
        return (0);
    }
    return (1);
}

static int
legit_knight_move (char fromx, char fromy, char tox, char toy, char *pos)
{
    switch (abs (fromx - tox)) {
    default:
        return (0);
    case 1:
        return (abs (fromy - toy) == 2);
    case 2:
        return (abs (fromy - toy) == 1);
    }
}

static int
legit_pawn_move (char fromx, char fromy, char tox, char toy, char *pos,
                 char start, int direction, char opponent)
{
    if (toy != fromy + direction) {
        return ((fromx == tox)
             && (fromy == start)
             && (toy == start + direction + direction)
             && (pos[cti (fromx, start + direction)] == '+')
             && (pos[cti (tox, toy)] == '+'));
    }
    switch (abs (tox - fromx)) {
    default:
        return (0);
    case 0:
        return (pos[cti (tox, toy)] == '+');
    case 1:
        return (color (pos[cti (tox, toy)]) == opponent);
    }
}

static int
legit_move (char fromx, char fromy, char tox, char toy, char *pos)
{
    char piece; 

    piece = pos[cti (tox, toy)];
    if (color (piece) == pos[0]) {
        char from_piece = pos[cti (fromx, fromy)];
        if ((from_piece == 'K' && piece == 'O')
         || (from_piece == 'k' && piece == 'o')) {
            if (tox == 'c' || tox == 'g') {
                return (legit_king_move (fromx, fromy, tox, toy, pos));
            } else {
                return (0);
            }
        } else if ((from_piece == 'O' && piece == 'K')
         || (from_piece == 'o' && piece == 'k')) {
            if (tox == 'd' || tox == 'f') {
                return (legit_rook_move (fromx, fromy, tox, toy, pos));
            } else {
                return (0);
            }
        } else {
            return (0);
        }
    }
    piece = pos[cti (fromx, fromy)];
    switch (piece) {
    case 'K':
    case 'k':
        return (legit_king_move (fromx, fromy, tox, toy, pos));
    case 'Q':
    case 'q':
        return (legit_rook_move (fromx, fromy, tox, toy, pos)
             || legit_bishop_move (fromx, fromy, tox, toy, pos));
    case 'B':
    case 'b':
        return (legit_bishop_move (fromx, fromy, tox, toy, pos));
    case 'N':
    case 'n':
        return (legit_knight_move (fromx, fromy, tox, toy, pos));
    case 'R':
    case 'O':
    case 'r':
    case 'o':
        return (legit_rook_move (fromx, fromy, tox, toy, pos));
    case 'P':
        return (legit_pawn_move (fromx, fromy, tox, toy, pos,
                                 '2', 1, 'B'));
    case 'p':
        return (legit_pawn_move (fromx, fromy, tox, toy, pos,
                                 '7', -1, 'W'));
    }
    return (1);
}

static void
move_piece (char fromx, char fromy, char tox, char toy, char *pos)
{
    int from;
    int to;
    char from_piece;
    char to_piece;
    int temp;

    pos[0] = other_color (pos[0]);

    from = cti (fromx, fromy);
    to = cti (tox, toy);
    from_piece = pos[from];
    to_piece = pos[to];

    /* en passant */
    if ((from_piece == 'P') && (to_piece == 'e')) {
        temp = cti (tox, '5');
        pos[temp] = '+';
    } else if ((from_piece == 'p') && (to_piece == 'E')) {
        temp = cti (tox, '4');
        pos[temp] = '+';
    }
    /* en passant opportunity only lasts one move */
    for (temp = 1; temp <= 64; ++temp) {
        char c;
        c = pos[temp];
        if ((c == 'e') || (c == 'E')) {
            pos[temp] = '+';
            break;
        }
    }

    switch (from_piece) {
    case 'O':
        if (fromy == '1' && toy == '1') {
            int castling = 0;
            if (tox == 'd') {
                for (temp = from + 1; temp <= to; ++temp) {
                    if (pos[temp] == 'K') {
                        pos[temp] = '+';
                        pos[cti('c','1')] = 'K';
                        castling = 1;
                        break;
                    }
                }
            } else if (tox == 'f') {
                for (temp = from - 1; temp >= to; --temp) {
                    if (pos[temp] == 'K') {
                        pos[temp] = '+';
                        pos[cti('g','1')] = 'K';
                        castling = 1;
                        break;
                    }
                }
            }
            if (castling) {
                for (temp = cti ('a', '1'); temp <= cti ('h', '1'); ++temp) {
                    if (pos[temp] == 'O') {
                        pos[temp] = 'R';
                    }
                }
            }
        }
        /* once rook moves it is no longer castle-able */
        from_piece = 'R';
        break;
    case 'K':
        /* castle rook */
        if ((fromy == '1') && (toy == '1')) {
            if (tox == 'g' && ((tox - fromx > 1)
                               || fromx == 'g'
                               || pos[cti(tox,toy)] == 'O')) {
                for (temp = cti ('h', '1'); temp > cti (fromx, '1'); --temp) {
                    if (pos[temp] == 'O') {
                        pos[temp] = '+';
                        temp = cti ('f', '1');
                        pos[temp] = 'R';
                    }
                }
            } else if (tox == 'c' && ((fromx - tox > 1)
                                   || fromx == 'c')
                                   || pos[cti(tox,toy)] == 'O') {
                for (temp = cti ('a', '1'); temp < cti (fromx, '1'); ++temp) {
                    if (pos[temp] == 'O') {
                        pos[temp] = '+';
                        temp = cti ('d', '1');
                        pos[temp] = 'R';
                    }
                }
            }
        }
        /* once king moves neither rook is castle-able */
        for (temp = cti ('a', '1'); temp <= cti ('h', '1'); ++temp) {
            if (pos[temp] == 'O') {
                pos[temp] = 'R';
            }
        }
        break;
    case 'o':
        if (fromy == '8' && toy == '8') {
            int castling = 0;
            if (tox == 'd') {
                for (temp = from + 1; temp <= to; ++temp) {
                    if (pos[temp] == 'k') {
                        pos[temp] = '+';
                        pos[cti('c','8')] = 'k';
                        castling = 1;
                        break;
                    }
                }
            } else if (tox == 'f') {
                for (temp = from - 1; temp >= to; --temp) {
                    if (pos[temp] == 'k') {
                        pos[temp] = '+';
                        pos[cti('g','8')] = 'k';
                        castling = 1;
                        break;
                    }
                }
            }
            if (castling) {
                for (temp = cti ('a', '8'); temp <= cti ('h', '8'); ++temp) {
                    if (pos[temp] == 'o') {
                        pos[temp] = 'r';
                    }
                }
            }
        }
        /* once rook moves it is no longer castle-able */
        from_piece = 'r';
        break;
    case 'k':
        /* castle rook */
        if ((fromy == '8') && (toy == '8')) {
            if (tox == 'g' && ((tox - fromx > 1)
                               || fromx == 'g'
                               || pos[cti(tox,toy)] == 'o')) {
                for (temp = cti ('h', '8'); temp > cti (fromx, '8'); --temp) {
                    if (pos[temp] == 'o') {
                        pos[temp] = '+';
                        temp = cti ('f', '8');
                        pos[temp] = 'r';
                    }
                }
            } else if (tox == 'c' && ((fromx - tox > 1)
                                   || fromx == 'c')
                                   || pos[cti(tox,toy)] == 'o') {
                for (temp = cti ('a', '8'); temp < cti (fromx, '8'); ++temp) {
                    if (pos[temp] == 'o') {
                        pos[temp] = '+';
                        temp = cti ('d', '8');
                        pos[temp] = 'r';
                    }
                }
            }
        }
        /* once king moves neither rook is castle-able */
        for (temp = cti ('a', '8'); temp <= cti ('h', '8'); ++temp) {
            if (pos[temp] == 'o') {
                pos[temp] = 'r';
            }
        }
        break;
    case 'P':
        if (toy == '8') {
            /* promote pawn */
            from_piece = prom;
        } else if (fromy == '2' && toy == '4') {
            /* two step pawn move from start square
             * may create en-passant opportunity */
            if (tox != 'a') {
                temp = cti (tox - 1, toy);
                if (pos[temp] == 'p') {
                    temp = cti (tox, '3');
                    pos[temp] = 'E';
                }
            }
            if (tox != 'h') {
                temp = cti (tox + 1, toy);
                if (pos[temp] == 'p') {
                    temp = cti (tox, '3');
                    pos[temp] = 'E';
                }
            }
        }
        break;
    case 'p':
        if (toy == '1') {
            /* promote pawn */
            from_piece = tolower (prom);
        } else if (fromy == '7' && toy == '5') {
            /* two step pawn move from start square
             * may create en-passant opportunity */
            if (tox != 'a') {
                temp = cti (tox - 1, toy);
                if (pos[temp] == 'P') {
                    temp = cti (tox, '6');
                    pos[temp] = 'e';
                }
            }
            if (tox != 'h') {
                temp = cti (tox + 1, toy);
                if (pos[temp] == 'P') {
                    temp = cti (tox, '6');
                    pos[temp] = 'e';
                }
            }
        }
        break;
    }
    /* move piece */
    pos[to] = from_piece;
    if (from_piece == 'K' || from_piece == 'k'
     || pos[from] == 'K' || pos[from] == 'k') {
        if (pos[from] == from_piece) {
            pos[from] = '+';
        }
    } else {
        pos[from] = '+';
    }
}

int
legal_move (char fromx, char fromy, char tox, char toy, char *pos)
{
    char try[66];
    char kingx;
    char kingy;
    char king;
    char *p;

    if (!legit_move (fromx, fromy, tox, toy, pos)) {
        return (0);
    }

    king = ((pos[0] == 'W') ? 'K' : 'k');
    memcpy (try, pos, 66);
    move_piece (fromx, fromy, tox, toy, try);
    try[0] = other_color (try[0]);
    p = try + 1;
    for (kingy = '8'; kingy >= '1'; --kingy) {
        for (kingx = 'a'; kingx <= 'h'; ++kingx) {
            if (*(p++) == king) {
                return (!attacked (kingx, kingy, try));
            }
        }
    }

    return (1);
}

static int
ambiguous (char piece, char tox, char toy, char *pos)
{
    int i;
    int count = 0;
    char x, y;

    y = '8';
    x = 'a';
    for (i = 1; i <= 64; ++i) {
        if ((pos[i] == piece)
         && legit_move (x, y, tox, toy, pos)) {
            if (++count > 1) {
                return (1);
            }
        }
        if (++x > 'h') {
            x = 'a';
            --y;
        }
    }

    return (0);
}

static int
king_between (char fromx, char fromy, char tox, char *pos)
{
    int direction;
    char x;
    char king;

    if (tox > fromx) {
        direction = 1;
    } else if (tox < fromx) {
        direction = -1;
    } else {
        return (0);
    }

    x = fromx;
    do {
        x += direction;
        switch (pos[cti(x,fromy)]) {
        default:
            break;
        case 'K':
        case 'k':
            return (1);
        }
    } while (x != tox);

    return (0);
}

static char *
notate (char fromx, char fromy, char tox, char toy, char *pos)
{
    static char coords[10];
    char piece;
    char to_piece;
    int capture;

    piece = pos[cti (fromx, fromy)];
    to_piece = pos[cti (tox, toy)];
    if ((piece == 'P') || (piece == 'p')) {
        char p[4] = "";
        if ((toy == '1') || (toy == '8')) {
            p[0] = '(';
            p[1] = prom;
            p[2] = ')';
            p[3] = '\0';
        }
        if (fromx == tox) {
            sprintf (coords, "%c%c%s", tox, toy, p);
        } else {
            sprintf (coords, "%cx%c%c%s",
                     fromx, tox, toy, p);
        }
    } else if (((piece == 'K') || (piece == 'k'))
             && (fromy == toy)
             && (fromy == '1' || fromy == '8')
             && (tox == 'g' || tox == 'c')
             && (abs (fromx - tox) != 1 || to_piece - piece == 'o' - 'k')) {
        if (tox == 'g') {
            sprintf (coords, "O-O");
        } else {
            sprintf (coords, "O-O-O");
        }
    } else if (((piece == 'O') || (piece == 'o'))
             && (fromy == toy)
             && (tox == 'f' || tox == 'd')
             && king_between (fromx, fromy, tox, pos)) {
        if (tox == 'f') {
            sprintf (coords, "O-O");
        } else {
            sprintf (coords, "O-O-O");
        }
    } else {
        char d[3] = "";
        if (ambiguous (piece, tox, toy, pos)) {
            d[0] = fromx;
            d[1] = fromy;
            d[2] = '\0';
        }
        piece = toupper (piece);
        if (piece == 'O') {
            piece = 'R';
        }
        if (pos[cti (tox, toy)] == '+') {
            sprintf (coords, "%c%s%c%c", piece, d, tox, toy);
        } else {
            sprintf (coords, "%c%sx%c%c", piece, d, tox, toy);
        }
    }

    return (coords);
}

char *
one_move_diff (char *start_pos, char *end_pos)
{
    int i;
    int diff = 0;
    char x;
    char y;
    char fromx = '\0';
    char fromy = '\0';
    char tox = '\0';
    char toy = '\0';
    char ofromx = '\0';
    char ofromy = '\0';
    char otox = '\0';
    char otoy = '\0';

    if (start_pos[0] == end_pos[0]) {
        return (NULL);
    }
    y = '8';
    x = 'a';
    for (i = 1; i <= 64; ++i) {
        if (start_pos[i] != end_pos[i]) {
            if (++diff > 6) {
                return (NULL);
            }
            if (color (start_pos[i]) == start_pos[0]) {
                /* piece moved from */
                switch (start_pos[i]) {
                case 'O':
                case 'o':
                    if (end_pos[i] != 'R' && end_pos[i] != 'r') {
                        ofromx = x;
                        ofromy = y;
                    }
                default:
                    if (fromx) {
                        break;
                    }
                case 'K':
                case 'k':
                    fromx = x;
                    fromy = y;
                    break;
                case 'E':
                case 'e':
                    break;
                }
            }
            if (color (end_pos[i]) == start_pos[0]) {
                /* piece moved to */
                switch (end_pos[i]) {
                case 'R':
                case 'r':
                    if (start_pos[i] != 'O' && start_pos[i] != 'o') {
                        otox = x;
                        otoy = y;
                    }
                default:
                    if (tox) {
                        break;
                    }
                case 'K':
                case 'k':
                    tox = x;
                    toy = y;
                    break;
                case 'E':
                case 'e':
                    break;
                }
            }
        }
        if (++x > 'h') {
            x = 'a';
            --y;
        }
    }

    if (ofromx && otox && (ofromx != fromx) && abs (fromx - tox) <= 1) {
        fromx = ofromx;
        fromy = ofromy;
        tox = otox;
        toy = otoy;
    }
    if (fromx && tox && legal_move (fromx, fromy, tox, toy, start_pos)) {
        char try[66];

        memcpy (try, start_pos, 66);
        move_piece (fromx, fromy, tox, toy, try);
        if (!strncmp (try, end_pos, 65)) {
            return (notate (fromx, fromy, tox, toy, start_pos));
        }
    }

    return (NULL);
}

static void
http_png (FILE *http_out, char *path)
{
    struct png *img;
    int len;

    len = strlen (path);
    if ((len < 5) || (len > MAX_PATH) || strcmp (path + len - 4, ".png")) {
        fprintf (http_out, "HTTP/1.0 404 Not Found\n\n");
        return;
    }

    for (img = pngs; img; img = img->next) {
        if (!strcmp (img->path, path)) {
            break;
        }
    }

    if (!img) {
        FILE *in;

        in = fopen (path, "r");
        if (!in) {
            fprintf (http_out, "HTTP/1.0 404 Not Found\n\n");
            return;
        }
        img = calloc (1, sizeof (*img));
        strcpy (img->path, path);
        img->next = pngs;
        img->size = fread (&img->data, 1, sizeof (img->data), in);
        pngs = img;
        fclose (in);
    }

    fprintf (http_out, "HTTP/1.0 200 OK\n"
             "Cache-Control: public, immutable, max-age=31536000\n"
             "Content-Length: %d\n"
             "Content-Type: application/png\n\n",
             img->size);
    fwrite (img->data, 1, img->size, http_out);
}

static void
play_anchor (char prom, char unused, char flip, char move,
             char selx, char sely, char *pos, char *text)
{
    if (text) {
        fprintf (http_out,
                 "<a id=\"%s\" href=\"%s?%c%c%c%c%c%c%.65s\">%s</a>\n",
                 text, current_game->name, prom, unused, flip, move, selx, sely, pos, text);
    } else {
        fprintf (http_out,
                 "<a href=\"%s?%c%c%c%c%c%c%.65s\" />",
                 current_game->name, prom, unused, flip, move, selx, sely, pos);
    }
}

static void
promotion_links (char flip)
{
    char p[4] = "QRBN";
    int i;
    for (i = 0; i < 4; ++i) {
        if (prom == p[i]) {
            fprintf (http_out, "%c\n", prom);
        } else {
            char text[2];
            text[0] = p[i];
            text[1] = '\0';
            play_anchor (p[i], 'X', flip, 'X',
                         current_game->sel[0], current_game->sel[1],
                         current_game->pos, text);
        }
    }
}

static char *
print_white_name (struct game *g, int link)
{
    char *vs;
    static char wn[MAX_NAME];
    char *fmt;

    if (g->pos[0] == 'W') {
        fmt = "<span style=\"background-color:white; color:black\">%s</span>";
    } else {
        fmt = "<span style=\"color:white\">%s</span>";
    }

    strcpy (wn, g->name);
    vs = strstr (wn, VS);
    if (vs) {
        *vs = '\0';
    }

    if (link) {
        fprintf (http_out, "<a href=\"/?T%s\">", wn);
    }

    fprintf (http_out, fmt, wn);

    if (link) {
        fprintf (http_out, "</a>&nbsp;");
    } else {
        fprintf (http_out, "&nbsp;");
    }

    return (wn);
}

static char *
print_black_name (struct game *g, int link)
{
    char *vs;
    char *bn;
    char *fmt;

    if (g->pos[0] == 'B') {
        fmt = "<span style=\"background-color:white; color:black\">%s</span>";
    } else {
        fmt = "<span style=\"color:white\">%s</span>";
    }

    vs = strstr (g->name, VS);
    if (vs) {
        bn = vs + VSLEN;
    } else {
        bn = g->name;
    }

    if (link) {
        fprintf (http_out, "<a href=\"/?T%s\">", bn);
    }

    fprintf (http_out, fmt, bn);

    if (link) {
        fprintf (http_out, "</a>&nbsp;");
    } else {
        fprintf (http_out, "&nbsp;");
    }

    return (bn);
}

static void
transcribe (char *str)
{
    while (*str && current_game->transcriptlen < MAX_TRANSCRIPT) {
        char c = *(str++);
        current_game->transcript[current_game->transcriptlen++] = c;
    }
}

static void
transcribe_fen (void)
{
    int i;
    char fen[100];
    char *p;
    char *temp;
    int blanks = 0;
    char square;
    char x = 'a';
    char y = '8';
    char enpassant[2] = { '-', '\0' };
    char K = ' ';
    char Q = ' ';
    char k = ' ';
    char q = ' ';

    p = fen;
    *(p++) = '[';
    *(p++) = 'F';
    *(p++) = 'E';
    *(p++) = 'N';
    *(p++) = ' ';
    *(p++) = '"';
    for (i = 0; i < 64; ++i) {
        if (i && ((i & 7) == 0)) {
            if (blanks) {
                *(p++) = '0' + blanks;
                blanks = 0;
            }
            *(p++) = '/';
        }

        square = current_game->pos[i+1];

        if (blanks && (square != '+')) {
            *(p++) = '0' + blanks;
            blanks = 0;
        }

        switch (square) {
        default:
            *(p++) = square;
            break;
        case 'e':
            enpassant[0] = x;
            enpassant[1] = y;
            /* fall through */
        case '+':
            ++blanks;
            break;
        case 'k':
            if (q == ' ') {
                /* eliminates black queenside castling */
                q = '-';
            }
            *(p++) = square;
            break;
        case 'o':
            if (q == ' ') {
                q = 'q';
            } else {
                k = 'k';
            }
            *(p++) = 'r';
            break;
        case 'K':
            if (Q == ' ') {
                /* eliminates white queenside castling */
                Q = '-';
            }
            *(p++) = square;
            break;
        case 'O':
            if (Q == ' ') {
                Q = 'Q';
            } else {
                K = 'K';
            }
            *(p++) = 'R';
            break;
        }

        if (++x == 'h') {
            x = 'a';
            --y;
        }
    }

    if (blanks) {
        *(p++) = '0' + blanks;
    }

    *(p++) = ' ';

    *(p++) = tolower (current_game->pos[0]);

    *(p++) = ' ';

    temp = p;
    if (K == 'K') *(p++) = 'K';
    if (Q == 'Q') *(p++) = 'Q';
    if (k == 'k') *(p++) = 'k';
    if (q == 'q') *(p++) = 'q';
    if (temp == p) *(p++) = '-';

    *(p++) = ' ';

    if (enpassant[0] == '-') {
        *(p++) = '-';
    } else {
        *(p++) = enpassant[0];
        *(p++) = enpassant[1];
    }

    *(p++) = ' ';

    *(p++) = '0'; //FIXME half-move clock for 50 move rule

    *(p++) = ' ';

    sprintf (p, "%d\" ]\n", current_game->movenum + 1);
    transcribe (fen);
}

static void
chatchar (int c)
{
    int gap = 0;
    int i;

    switch (c) {
    default:
        if (c < 32) {
            c = ' ';
        }
        break;
    case '\n':
        break;
    case '&':
        c = '+';
        break;
    }

    if (current_game->chatlen == MAX_CHAT) {
        char c;

        do {
            c = current_game->chat[gap++];
            if (isspace (c)) {
                break;
            }
        } while (gap < current_game->chatlen);
    } else {
        int nlcount = 0;
        int width = 0;
        int maxwidth;

        if (current_game->fics) {
            maxwidth = 80;
        } else {
            maxwidth = 60;
        }

        gap = current_game->chatlen;
        while (gap > 0) {
            if ((++width > maxwidth)
             || (current_game->chat[gap-1] == '\n')) {
                if (++nlcount > MAX_CHAT_NL) {
                    break;
                }
                width = 0;
            }
            --gap;
        }
    }

    if (gap) {
        for (i = gap; i < current_game->chatlen; ++i) {
            current_game->chat[i-gap] = current_game->chat[i];
        }
        current_game->chatlen -= gap;
    }

    current_game->chat[current_game->chatlen++] = c;
    tick ();
}

static void
chatstr (char *str)
{
    while (*str) {
        chatchar (*(str++));
    }
}

static void
send2fics (char *cmd)
{
    int len;

    if (!current_game->fics) {
        return;
    }

    len = strlen (cmd);
    if (len != write (current_game->fics, cmd, len)) {
        chatstr ("===== ERROR: write truncated\n");
    }
}

static void
chatkey (int key)
{
    switch (key) {
    case 8: //backspace
    case 46: //delete
        if (current_game->chatlen > 0) {
            current_game->chatlen--;
            tick ();
        }
        return;
    case 13: //enter
        key = 10;
    case 32: //space
        break;
    case 48: //close parenthesis
        key = 41;
        break;
    case 49: //exclamation
        key = 33;
        break;
    case 50: //at
        key = 64;
        break;
    case 51: //hash
        key = 35;
        break;
    case 52: //dollar
        key = 36;
        break;
    case 53: //percent
        key = 37;
        break;
    case 54: //caret
        key = 94;
        break;
    case 55: //ampersand=>plus
        key = 43;
        break;
    case 56: //asterisk
        key = 42;
        break;
    case 57: //open parenthesis
        key = 40;
        break;
    case 59: //semicolon
        key = 59;
        break;
    case 61: //equal
        key = 61;
        break;
    case 173: //dash
        key = 45;
        break;
    case 188: //comma
        key = 44;
        break;
    case 190: //period
        key = 46;
        break;
    case 191: //question
        key = 63;
        break;
    case 222: //apostrophe
        key = 39;
        break;
    default: //0-9 and A-Z
        if (key >= 96 && key <= 105) {
            key -= 48;
        } else if (key < 65 || key > 90) {
            return;
        }
    }

    chatchar (key);
    if (key == 10) {
        send2fics ("\n");
    }
}

static void
chat (char *keys)
{
    int key = 0;

    while (1) {
        if (*keys == '\0') {
            chatkey (key);
            return;
        } else if (*keys == ',') {
            chatkey (key);
            key = 0;
        } else if (*keys >= '0' && *keys <= '9') {
            key *= 10;
            key += (*keys - '0');
        } else {
            return;
        }
        ++keys;
    }
}

static void
chatquery (char *str)
{
    char c;
    int ficslen = 0;
    char fics[100];

    if (!str) {
        return;
    }

    if (current_game->chatlen) {
        switch (current_game->chat[current_game->chatlen - 1]) {
        default:
            chatchar ('\n');
            break;
        case ' ':
        case '\n':
            break;
        }
    }

    while ((c = *(str++))) {
        switch (c) {
        default:
            break;
        case '&':
            return;
        case '%':
            if (str[0] && str[1]) {
                int i;
                int ascii = 0;
                for (i = 0; i <= 1; ++i) {
                    ascii *= 16;
                    c = tolower(str[i]);
                    if (c >= '0' && c <= '9') {
                        ascii += (c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        ascii += (c - ('a' - 10));
                    }
                }
                c = ascii;
                str += 2;
                break;
            }
            return;
        case '+':
            c = ' ';
            break;
        }

        chatchar (c);
        if (ficslen < sizeof (fics) - 2) {
            fics[ficslen++] = c;
        }
    }

    chatchar ('\n');
    fics[ficslen++] = '\n';
    fics[ficslen] = '\0';
    send2fics (fics);
}

static void
transcribe_move (char *notation)
{
    char str[15];
    if (current_game->pos[0] == 'W') {
        chatstr ("\n... ");
        chatstr (notation);
        chatstr (" == ");
        if (!current_game->movenum) {
            sprintf (str, "%d. ...", ++current_game->movenum);
            transcribe (str);
        }
        transcribe (" ");
        transcribe (notation);
        transcribe ("\n");
    } else {
        chatstr ("\n");
        chatstr (notation);
        chatstr (" ... == ");
        sprintf (str, "%d. ", ++current_game->movenum);
        transcribe (str);
        transcribe (notation);
    }
}

static void
print_game_link (struct game *g, int id)
{
    fprintf (http_out, "<a id=\"%d\" name=\"%d\" href=\"/%s\">",
             id, id, g->name);
    print_white_name (g, 0);
    fprintf (http_out, "<span style=\"color:gray\"> vs </span>");
    print_black_name (g, 0);
    fprintf (http_out, "</a>\n");
}

static char *
reversed_name (struct game *g)
{
    char *vs;
    char *bn;
    static char reversed_name[MAX_NAME];

    vs = strstr (g->name, VS);
    if (!vs) {
        return (NULL);
    }
    bn = vs + VSLEN;

    sprintf (reversed_name, "%s_vs_%.*s",
             bn, (int)(vs - g->name), g->name);

    return (reversed_name);
}

static void
print_reversed_link (struct game *g)
{
    char *rn = reversed_name (g);
    if (rn) {
        fprintf (http_out, "<a href=\"/%s\">reversed</a>\n", rn);
    }
}

static void
http_game (struct game *g, int id, char flip)
{
    char piece;
    int toggle;
    char y, x;

    if (flip == 'F') {
        print_white_name (g, (id >= 0));
    } else {
        print_black_name (g, (id >= 0));
    }

    if (id >= 0) {
        fprintf (http_out, "<a id=\"%d\" name=\"%d\" href=\"%s\">",
                 id, id, g->name);
    }

    fprintf (http_out, "<table style=\"background-color: inherit\"><tr style=\"background-color: inherit\">\n");
    fprintf (http_out, "<td valign=\"top\">"
             "<table cellspacing=\"0\" cellpadding=\"0\">\n");
    toggle = 0;
    y = ((flip == 'F') ? '1' : '8');
    while (y >= '1' && y <= '8') {
        fprintf (http_out, "<tr>");
        x = ((flip == 'F') ? 'h' : 'a');
        while (x >= 'a' && x <= 'h') {
            char *img;
            char *bg;
            int a = 0;

            piece = g->pos[cti (x, y)];

            if ((g->sel[0] == x) && (g->sel[1] == y)) {
                bg = "yellow";
            } else {
                bg = (toggle ? "teal" : "silver");
            }

            fprintf (http_out, "<td bgcolor=\"%s\">", bg);
            img = piece_to_img (piece);
            fprintf (http_out, "<img src=\"/images/%s.png\" alt=\"%c\"/>",
                     img, piece_to_alt (piece));
            fprintf (http_out, "</td>");
            toggle = !toggle;
            if (flip == 'F') { --x; } else { ++x; }
        }
        toggle = !toggle;
        fprintf (http_out, "</tr>\n");
        if (flip == 'F') { ++y; } else { --y; }
    }
    fprintf (http_out, "</table></td>\n");
    fprintf (http_out, "<td valign=\"top\" style=\"background-color: inherit\">\n");
    fprintf (http_out, "<pre id=\"chat\" style=\"background-color: inherit; width:%dpx; height:300px; white-space: pre-wrap; word-break: keep-all; font-size:xx-small;\">%.*s</pre>",
             (g->fics ? 520 : 320),
             g->chatlen, g->chat);
    fprintf (http_out, "</td></tr></table>\n");

    if (id >= 0) {
        fprintf (http_out, "</a>\n");
    }

    if (flip == 'F') {
        print_black_name (g, (id >= 0));
    } else {
        print_white_name (g, (id >= 0));
    }
}

struct player *
white_player (char *game_name)
{
    char *vs;
    char name[MAX_NAME];
    char *nmp;

    strcpy (name, game_name);
    vs = strstr (name, VS);
    if (vs) {
        *vs = '\0';
    }
    nmp = name;

    return (get_player (nmp));
}

struct player *
black_player (char *game_name)
{
    char *vs;
    char name[MAX_NAME];
    char *nmp;

    strcpy (name, game_name);
    vs = strstr (name, VS);
    if (vs) {
        nmp = vs + VSLEN;
    } else {
        nmp = name;
    }

    return (get_player (nmp));
}

struct player *
player_to_move (struct game *g)
{
    char *vs;
    char name[MAX_NAME];
    char *nmp;

    strcpy (name, g->name);
    vs = strstr (name, VS);
    if (vs) {
        if (g->pos[0] == 'W') {
            *vs = '\0';
            nmp = name;
        } else {
            nmp = vs + VSLEN;
        }
    } else {
        nmp = name;
    }

    return (get_player (nmp));
}

static int
fics_connect (void)
{
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;

    sockfd = socket (AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        chatstr ("socket creation failed!\n");
        return (0);
    }

    server = gethostbyname("freechess.org");
    if (server == NULL) {
        chatstr ("gethostbyname failed for freechess.org!\n");
        return (0);
    }
   
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(23);
   
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        chatstr ("connect failed for freechess.org!\n");
        return (0);
    }

    current_game->fics = sockfd;
    current_game->fics_style = 0;
    return (sockfd);
}

#define PROMOTE 0
#define CHAT 1
#define FLIP 2
#define MODE 3
#define SELX 4
#define SELY 5
static void
http_play (char *query)
{
    char flip;
    char try[66];
    int toggle;
    char y, x;
    char piece;
    int possible = 0;
    int can_move = 1; //FIXME protect via identity
    struct player *ptm; //player to move
    int show = 1;
    int touch = 1;
    char white_name[MAX_NAME];
    char *black_name;
    char *vs;
    char *name;
    int can_fics;

    strcpy (white_name, current_game->name);
    vs = strstr (white_name, VS);
    if (vs) {
        *vs = '\0';
        black_name = vs + VSLEN;
        can_fics = 0;
    } else {
        black_name = white_name;
        can_fics = 1;
    }

    if (!query) {
        query = "";
    }

    /********** chat query ***********/
    if (query[0] == 'C' && (query[1] == 'F' || query[1] == 'X')) {
        char flip = query[1];
        if (query[2] == '=') {
            chatquery (query + 3);
            if (flip == 'F') {
                query = "QXFP00";
            } else {
                query = "QXXP00";
            }
        } else {
            fprintf (http_out,
                     "<html>\n"
                     "  <head>\n"
                     "    <title>chat %s</title>\n"
                     "  </head>\n"
                     "  <body bgcolor=\"black\" style=\"color:white\">\n",
                     current_game->name);

            http_game (current_game, -1, flip);

            fprintf (http_out,
                     "    <form id=\"form\" method=\"GET\">\n"
                     "      <input type=\"text\" id=\"C%c\" name=\"C%c\" style=\"background-color:black; color:white; width:320px; font-size:xx-small;\" autofocus /><br/>\n"
                     "      <input value=\"submit chat\" type=\"submit\" />\n"
                     "    </form>\n"
                     "  </body>\n"
                     "</html>\n",
                     flip, flip);
            return;
        }
    }

    if (query[0] == 'S') {
        /********** sequence query ***********/
        chat (query + 1);
        fprintf (http_out, "%d", current_game->sequence);
        return;
    } else if (strlen (query) < 6) {
        /********** transcript query ***********/
        if (!strcmp (query, "T")) {
            fprintf (http_out, "<html><pre>%.*s</pre></html>",
                     current_game->transcriptlen,
                     current_game->transcript);
            return;
        }
        query = "QXXP00";
    } else if (strlen (query) == 71) {
        /********** MODE queries ***********/
        char *notation = NULL;
        if (query[MODE] == 'S') {
            /********** restart game ***********/
            struct game *rg;

            current_game->transcriptlen = 0;
            current_game->movenum = 0;
            current_game->sel[0] = '0';
            current_game->sel[1] = '0';

            if (current_game->chatlen) {
                chatstr ("\n==========================================================\n");
            }

            /********** mirror reversed game if still at initial ***********/
            rg = get_game (reversed_name (current_game));
            if (rg
             && (rg->sel[0] == '0')
             && !memcmp (rg->start_pos, rg->pos, 66))
            {
                strcpy (rg->pos, query + 6);
                strcpy (rg->start_pos, rg->pos);
                tick_game (rg);
            }

            strcpy (current_game->pos, query + 6);
            strcpy (current_game->start_pos, current_game->pos);
            tick ();
        }

        if ((query[MODE] == 'M')
         && strcmp (current_game->pos, query + 6)
         && (notation = one_move_diff (current_game->pos, query + 6))) {
            if (!current_game->movenum) {
                transcribe_fen ();
            }
            strcpy (current_game->pos, query + 6);
            current_game->sel[0] = '0';
            current_game->sel[1] = '0';
            if (notation) {
                if (current_game->fics) {
                    send2fics (notation);
                    send2fics ("\n");
                }
                transcribe_move (notation);
            }
            tick ();
        }
    }

    /********** CHAT queries ***********/
    switch (query[CHAT]) {
    default:
        break;
    case 'F':
        if (can_fics) {
            current_game->chatlen = 0;
            fics_connect ();
        }
        break;
    case 'B':
        current_game->chatlen = 0;
        break;
    }

    /********** PROMOTE queries ***********/
    if ((query[PROMOTE] == 'R')
     || (query[PROMOTE] == 'B')
     || (query[PROMOTE] == 'N')) {
        prom = query[PROMOTE];
    } else {
        prom = 'Q';
    }

    /********** FLIP queries ***********/
    flip = query[FLIP];

    /********** SELection queries ***********/
    if ((ptm = player_to_move (current_game))) {
        touch = ((ptm->touch == 'Y') ? 1 : 0);
        show = ((ptm->show == 'Y') ? 1 : 0);
    }

    if (!strcmp (current_game->pos, query + 6)
     && (!touch ||
         ((current_game->sel[0] == '0' && current_game->sel[1] == '0')
           && (query[SELX] >= 'a') && (query[SELX] <= 'h')
           && (query[SELY] >= '1') && (query[SELY] <= '8')))) {
        if (can_move) {
            current_game->sel[0] = query[SELX];
            current_game->sel[1] = query[SELY];
            tick ();
        }
    }

    /********** html ***********/
    fprintf (http_out,
             "<html>\n"
             "  <head>\n");

    /********** prevent restart urls from being bookmarked ***********/
    if (query[MODE] == 'S') {
        fprintf (http_out,
                 "  <meta http-equiv=\"Refresh\" content=\"0; url='/%s?QX%cX00'\" />\n",
                 current_game->name, flip);
    }

    /********** javascript ***********/
    fprintf (http_out,
             "    <title>play %s</title>\n"
             "    <script>\n"
             "      var timer = null;\n"
             "      var sequence = %d;\n"
             "      var keys = \"\";\n"
             "      document.onkeydown = function(evt) {\n"
             "        var chat = document.getElementById('chat').innerHTML;\n"
             "        var delay = %d;\n"
             "        if (chat.length && chat[chat.length-1] == '_') {\n"
             "          chat = chat.substring(0,chat.length-1);\n"
             "        }\n"
             "        evt = evt || window.event;\n"
             "        switch(evt.which) {\n"
             "          case 8:\n" //backspace
             "          case 46:\n" //delete
             "            if (chat.length == 0) {\n"
             "              return;\n"
             "            }\n"
             "            chat = chat.substring(0,chat.length-1);\n"
             "            break;\n"
             "          case 13:\n" //enter
             "            chat += String.fromCharCode(10);\n"
             "            delay = 500;\n"
             "            break;\n"
             "          case 27:\n" //escape
             "            window.alert('"
             "left arrow: show games for player at top of board\\n"
             "right arrow: show games for player at bottom of board\\n"
             "up arrow: orient board with white at top\\n"
             "down arrow: orient board with white at bottom\\n"
             "a-z,space,enter,backspace,etc: chat');\n"
             "          case 32:\n" //space
             "            chat += ' ';\n"
             "            break;\n"
             "          case 37:\n" //left arrow
             "            window.location = '/?T%s';\n"
             "            break;\n"
             "          case 40:\n" //down arrow
             "            %s\n"
             "            break;\n"
             "          case 38:\n" //up arrow
             "            %s\n"
             "            break;\n"
             "          case 39:\n" //right arrow
             "            window.location = '/?T%s';\n"
             "            break;\n"
             "          case 48:\n" //close parenthesis
             "            chat += ')';\n"
             "            break;\n"
             "          case 49:\n" //exclamation
             "            chat += '!';\n"
             "            break;\n"
             "          case 50:\n" //at
             "            chat += '@';\n"
             "            break;\n"
             "          case 51:\n" //hash
             "            chat += '#';\n"
             "            break;\n"
             "          case 52:\n" //dollar
             "            chat += '$';\n"
             "            break;\n"
             "          case 53:\n" //percent
             "            chat += '%%';\n"
             "            break;\n"
             "          case 54:\n" //caret
             "            chat += '^';\n"
             "            break;\n"
             "          case 55:\n" //ampersand=>plus
             "            chat += '+';\n"
             "            break;\n"
             "          case 56:\n" //asterisk
             "            chat += '*';\n"
             "            break;\n"
             "          case 57:\n" //open parenthesis
             "            chat += '(';\n"
             "            break;\n"
             "          case 59:\n" //semicolon
             "            chat += ';';\n"
             "            break;\n"
             "          case 61:\n" //equal
             "            chat += '=';\n"
             "            break;\n"
             "          case 173:\n" //dash
             "            chat += '-';\n"
             "            break;\n"
             "          case 188:\n" //comma
             "            chat += ',';\n"
             "            break;\n"
             "          case 190:\n" //period
             "            chat += '.';\n"
             "            break;\n"
             "          case 191:\n" //question
             "            chat += '?';\n"
             "            break;\n"
             "          case 222:\n" //apostrophe
             "            chat += '\\'';\n"
             "            break;\n"
             "          default:\n" //0-9 and A-Z
             "            if ((evt.which >= 96) && (evt.which <= 105)) {\n"
             "              chat += String.fromCharCode(evt.which - 48);\n"
             "            } else if ((evt.which < 65) || (evt.which > 90)) {\n"
             "              return;\n"
             "            } else {\n"
             "              chat += String.fromCharCode(evt.which);\n"
             "            }\n"
             "            break;\n"
             "        }\n"
             "        if (delay) {\n"
             "          if (timer) clearTimeout(timer);\n"
             "          document.getElementById('chat').innerHTML = chat+'_';\n"
             "          keys += evt.which;\n"
             "          keys += ',';\n"
             "          ++sequence;\n"
             "          timer = setTimeout('auto_reload()',delay);\n"
             "        } else if (evt.which == 32) {\n" //playing on FICS
             "          window.location = '?C%c';\n"
             "        }\n"
             "      };\n"
             "      document.onmousemove = function(evt) {\n"
             "        if (keys.length > 0) {\n"
             "          auto_reload();\n"
             "        }\n"
             "      };\n"
             "      function auto_reload()\n"
             "      {\n"
             "        var xhr = new XMLHttpRequest();\n"
             "        xhr.onerror = function () {\n"
             "          clearTimeout(timer);\n"
             "          timer = setTimeout('auto_reload()',1000);\n"
             "        };\n"
             "        xhr.onload = function () {\n"
             "          clearTimeout(timer);\n"
             "          var ns = parseInt(xhr.response,10);\n"
             "          if ((ns != sequence) && !keys.length) {\n"
             "            window.location = '%s?%c%c%c%c%c%c';\n"
             "          } else {\n"
             "            timer = setTimeout('auto_reload()',1000);\n"
             "          }\n"
             "        };\n"
             "        xhr.open('GET', '?S' + keys);\n"
             "        keys = '';\n"
             "        xhr.responseType = 'text';\n"
             "        xhr.send();\n"
             "        var chat = document.getElementById('chat').innerHTML;\n"
             "        if (chat.length && chat[chat.length-1] == '_') {\n"
             "          chat = chat.substring(0,chat.length-1);\n"
             "          document.getElementById('chat').innerHTML = chat;\n"
             "        }\n"
             "      }\n"
             "    </script>\n"
             "  </head>\n"
             "  <body onload=\"timer = setTimeout('auto_reload()',1000);\" "
             "        bgcolor=\"black\" style=\"color:white\">\n",
             current_game->name,
             current_game->sequence,
             (current_game->fics ? 0 : 5000), //delay
             ((flip == 'F') ? white_name : black_name), //left arrow
             ((flip == 'F') ? "document.getElementById('flip').click();"
                  : "/* do nothing */"), //down arrow
             ((flip == 'F') ? "/* do nothing */"
                  : "document.getElementById('flip').click();"), //up arrow
             ((flip == 'F') ? black_name : white_name), //right arrow
             flip,
             current_game->name, prom, 'X', flip, 'X', '0', '0');

    /************** top player ***************/
    if (flip == 'F') {
        name = print_white_name (current_game, 1);
    } else {
        name = print_black_name (current_game, 1);
    }

    /************** top player links ***************/
    print_reversed_link (current_game);
    if (can_move && !current_game->fics) {
        if ((current_game->sel[0] == '0' && current_game->sel[1] == '0')
                && (!memcmp (current_game->pos, current_game->start_pos, 66))) {
            play_anchor (prom, 'X', flip, 'S', '0', '0',
                         classical_pos (), "classical");
            play_anchor (prom, 'X', flip, 'S', '0', '0',
                         chess960 (), "chess960");
            play_anchor (prom, 'X', flip, 'S', '0', '0',
                         chess2880 (), "dynamite");
        } else {
            play_anchor (prom, 'X', flip, 'S', '0', '0',
                         current_game->start_pos, "restart");
        }
        if (current_game->chatlen) {
            play_anchor (prom, 'B', flip, 'X',
                         current_game->sel[0], current_game->sel[1],
                         current_game->pos, "blank");
        }
        if (can_fics) {
            play_anchor (prom, 'F', flip, 'S', '0', '0',
                         current_game->start_pos, "fics");
        }
    }

    /************** chess board ***************/
    fprintf (http_out, "<table><tr>\n");
    fprintf (http_out, "<td valign=\"top\">"
             "<table cellspacing=\"0\" cellpadding=\"0\">\n");
    toggle = 0;
    y = ((flip == 'F') ? '1' : '8');
    while (y >= '1' && y <= '8') {
        fprintf (http_out, "<tr>");
        x = ((flip == 'F') ? 'h' : 'a');
        while (x >= 'a' && x <= 'h') {
            char *img;
            char *bg;
            int a = 0;

            piece = current_game->pos[cti (x, y)];

            if ((current_game->sel[0] == x) && (current_game->sel[1] == y)) {
                bg = "yellow";
            } else if (show
                    && (current_game->sel[0] != '0')
                    && legal_move (current_game->sel[0], current_game->sel[1],
                                   x, y, current_game->pos)) {
                bg = (toggle ? "darkolivegreen" : "darkkhaki");
            } else {
                bg = (toggle ? "teal" : "silver");
            }

            fprintf (http_out, "<td bgcolor=\"%s\">", bg);
            if (current_game->sel[0] != '0') {
                if (legal_move (current_game->sel[0], current_game->sel[1],
                                x, y, current_game->pos)) {
                    memcpy (try, current_game->pos, 66);
                    move_piece (current_game->sel[0], current_game->sel[1], x, y, try);
                    play_anchor (prom, 'X', flip, 'M', '0', '0', try, NULL);
                    a = 1;
                    ++possible;
                } else if (!touch && (color (piece) == current_game->pos[0])) {
                    play_anchor (prom, 'X', flip, 'X',
                                 x, y, current_game->pos, NULL);
                    a = 1;
                } else if (piece == 'O' || piece == 'o') {
                    char kingx = current_game->sel[0];
                    char kingy = current_game->sel[1];
                    char kingp = current_game->pos[cti(kingx, kingy)];
                    if ((kingp == 'K' || kingp == 'k') && (kingy == y)) {
                        /* shorthand methods for castling */
                        char rooktox = ((x < kingx) ? 'd' : 'f');
                        char kingtox = ((x < kingx) ? 'c' : 'g');
                        if (abs (kingx - kingtox) > 1
                         && legal_move (kingx, kingy, kingtox, y, current_game->pos)) {
                            a = 1;
                            memcpy (try, current_game->pos, 66);
                            move_piece (kingx, kingy, kingtox, y, try);
                        } else if ((((x < kingx) && (kingx <= rooktox))
                                 || ((rooktox <= kingx) && (kingx < x)))
                                && (legal_move (x, y, rooktox, y, current_game->pos))) {
                            a = 1;
                            memcpy (try, current_game->pos, 66);
                            move_piece (x, y, rooktox, y, try);
                        }
                        if (a) {
                            play_anchor (prom, 'X', flip, 'M',
                                         '0', '0', try, NULL);
                        }
                    }
                }
            } else if (color (piece) == current_game->pos[0]) {
                play_anchor (prom, 'X', flip, 'X',
                             x, y, current_game->pos, NULL);
                a = 1;
                ++possible;
            }
            img = piece_to_img (piece);
            fprintf (http_out, "<img src=\"/images/%s.png\" alt=\"%c\" />",
                     img, piece_to_alt (piece));
            if (a) {
                fprintf (http_out, "</a>");
            }
            fprintf (http_out, "</td>");
            toggle = !toggle;
            if (flip == 'F') { --x; } else { ++x; }
        }
        toggle = !toggle;
        fprintf (http_out, "</tr>\n");
        if (flip == 'F') { ++y; } else { --y; }
    }
    if ((current_game->sel[0] != '0') && !possible) {
        current_game->sel[0] = '0';
        current_game->sel[1] = '0';
        tick ();
    }
    fprintf (http_out, "</table></td>\n");

    /************** chat ***************/
    fprintf (http_out, "<td valign=\"top\">\n");
    fprintf (http_out, "<pre id=\"chat\" style=\"width:%dpx; height:300px; white-space: pre-wrap; word-break: keep-all; font-size:xx-small;\">",
             (can_fics ? 520 : 320));
    if (current_game->fics) {
        int i;
        char c;
        int qstart = -1;

        for (i = 0; i < current_game->chatlen; ++i) {
            c = current_game->chat[i];

            switch (c) {
            default:
                if (qstart < 0) {
                    fputc (c, http_out);
                }
                break;
            case '\n':
                if (qstart >= 0) {
                    fprintf (http_out, "%.*s",
                             i - qstart, current_game->chat + qstart);
                    qstart = -1;
                } else {
                    fputc (c, http_out);
                }
                break;
            case '"':
                if (qstart >= 0) {
                    int j;

                    fprintf (http_out, "<a href=\"?C%c=", flip);
                    for (j = qstart + 1; j < i; ++j) {
                        c = current_game->chat[j];
                        fputc ((c == ' ' ? '+' : c), http_out);
                    }
                    fprintf (http_out, "\">%.*s</a>",
                             i - qstart + 1, current_game->chat + qstart);
                    qstart = -1;
                } else {
                    qstart = i;
                }
                break;
            }
        }
    } else {
        fprintf (http_out, "%.*s", current_game->chatlen, current_game->chat);
    }
    fprintf (http_out, "</pre></td></tr></table>\n");

    /************** bottom player ***************/
    if (flip == 'F') {
        name = print_black_name (current_game, 1);
    } else {
        name = print_white_name (current_game, 1);
    }

    /************** bottom player links ***************/
    play_anchor (prom, 'X', ((flip == 'F') ? 'N' : 'F'), 'X',
                 current_game->sel[0], current_game->sel[1], current_game->pos, "flip");
    promotion_links (flip);
    if (current_game->transcriptlen) {
        fprintf (http_out,
                 "<a href=\"%s?T\" target=\"_blank\">transcribe</a>\n",
                 current_game->name);
    }
    fprintf (http_out, "<a href=\"?C%c\">type</a>\n", flip);

    /************** that's all folks! ***************/
    fprintf (http_out, "</body></html>\n");
}

/* 0 => no match, 1 => prefix to move, -1 => opponent of prefix to move */
static int
filter_match (char *prefix, struct game *g)
{
    char *vs;

    vs = strstr (g->name, VS);
    if (!vs) {
        return (0);
    }

    if (!prefix || !prefix[0]) {
        return (1);
    }

    if (!strncasecmp (prefix, g->name, strlen (prefix))) {
        /* prefix has white pieces */
        return ((g->pos[0] == 'W') ? 1 : -1);
    }

    if (vs && !strncasecmp (prefix, vs + VSLEN, strlen (prefix))) {
        /* prefix has black pieces */
        return ((g->pos[0] == 'B') ? 1 : -1);
    }

    return (0);
}

static int
maybe_expand_game (int y, char onlymymove, int fm, time_t threshold_t,
                   struct game *g)
{
    int shown = 0;

    switch (onlymymove) {
    case 'T':
        if (fm < 0) {
            /* 'tis not my move */
            print_game_link (g, y);
            break;
        }
    default:
        if (!threshold_t || g->update_t > threshold_t) {
            ++shown;
            http_game (g, y, 'X');
        } else {
            print_game_link (g, y);
        }
        break;
    }

    return (shown);
}

static int
http_games (char *query)
{
    struct game *this_game;
    time_t now = time (NULL);
    time_t threshold_t = 0;
    int y = 0;
    int shown = 0;
    char *filter = "";
    char omm = 'T';

    if (!query || !query[0]) {
        query = "N";
    }

    switch (query[0]) {
    default:
        break;
    case 'S': //Sequence
        if (strlen (query) == 1) {
            fprintf (http_out, "%d", god_sequence);
            return (0);
        }
        break;
    case 'N': //Filter and only expand when updated after now
        threshold_t = now;
    case 'F': //Filter
        omm = 'F';
    case 'T': //Filter and only expand when my move
        filter = query + 1;
        while (isdigit (*filter)) {
            threshold_t *= 10;
            threshold_t += (*filter - '0');
            ++filter;
        }
        if (!filter[0]) {
            /* it's always someone's move! */
            omm = 'F';
        }
        break;
    }

    fprintf (http_out,
             "<html>\n"
             "  <head>\n"
             "    <title>games</title>\n"
             "    <script>\n"
             "      var timer = null;\n"
             "      var sequence = %d;\n"
             "      var first = 1;\n"
             "      var y = 0;\n"
             "      document.onkeydown = function(evt) {\n"
             "        evt = evt || window.event;\n"
             "        switch(evt.which) {\n"
             "          case 8:\n" //backspace
             "          case 46:\n" //delete
             "            window.location = '?N';\n"
             "            break;\n"
             "          case 9:\n" //tab
             "            window.location = '/players/%s';\n"
             "            return;\n"
             "          case 27:\n" //escape
             "            window.alert('"
             "a-z: filter further\\n"
             "backspace or delete: clear filter\\n"
             "space: toggle showing boards to move versus all\\n"
             "?: show boards upon next update\\n"
             "up or down arrow: change focus to a different row\\n"
             "enter or left arrow: play or kibitz game on left of focus row\\n"
             "right arrow: play or kibitz game on right of focus row\\n"
             "tab: players list');\n"
             "            break;\n"
             "          case 32:\n" //space
             "            window.location = '?%c%s';\n"
             "            break;\n"
             "          case 13:\n" //enter
             "          case 37:\n" //left arrow
             "            document.getElementById(y).click();\n"
             "            break;\n"
             "          case 38:\n" //up arrow
             "            document.getElementById(y).style.backgroundColor = 'initial';\n"
             "            if (first) {\n"
             "              first = 0;\n"
             "            } else if (y > 0) {\n"
             "              y -= 2;\n"
             "            }\n"
             "            window.location = '#'+y;\n"
             "            document.getElementById(y).style.backgroundColor = 'indigo';\n"
             "            evt.preventDefault();\n"
             "            break;\n"
             "          case 39:\n" //right arrow
             "            if (document.getElementById(y+1)) {\n"
             "              ++y;\n"
             "            }\n"
             "            document.getElementById(y).click();\n"
             "            return;\n"
             "          case 40:\n" //down arrow
             "            document.getElementById(y).style.backgroundColor = 'initial';\n"
             "            if (first) {\n"
             "              first = 0;\n"
             "            } else if (document.getElementById(y + 2)) {\n"
             "              y += 2;\n"
             "            }\n"
             "            window.location = '#'+y;\n"
             "            document.getElementById(y).style.backgroundColor = 'indigo';\n"
             "            evt.preventDefault();\n"
             "            break;\n"
             "          case 191:\n" //question
             "            window.location = '?N%s';\n"
             "            break;\n"
             "          default:\n" //0-9 and A-Z
             "            if (evt.which < 48 || evt.which > 90) {\n"
             "              return;\n"
             "            }\n"
             "            if (evt.which > 57 && evt.which < 65) {\n"
             "              return;\n"
             "            }\n"
             "            window.location = '?T%s' + String.fromCharCode(evt.which).toLowerCase();\n"
             "            break;\n"
             "        }\n"
             "      };\n"
             "      function auto_reload()\n"
             "      {\n"
             "        var xhr = new XMLHttpRequest();\n"
             "        xhr.onerror = function () {\n"
             "          timer = setTimeout('auto_reload()',1000);\n"
             "        };\n"
             "        xhr.onload = function () {\n"
             "          var ns = parseInt(xhr.response,10);\n"
             "          if (ns != sequence) {\n"
             "            window.location = '?%c%ld%s';\n"
             "          } else {\n"
             "              timer = setTimeout('auto_reload()',1000);\n"
             "          }\n"
             "        };\n"
             "        xhr.open('GET', '?S');\n"
             "        xhr.responseType = 'text';\n"
             "        xhr.send();\n"
             "      }\n"
             "    </script>\n"
             "  </head>\n"
             "  <body onload=\"timer = setTimeout('auto_reload()',1000);\" bgcolor=\"black\" style=\"color:white\">\n",
             god_sequence,
             filter, //tab
             (omm == 'T' ? 'F' : 'T'), filter, //space
             filter, //question mark
             filter,
             omm, threshold_t, filter);

    if (omm == 'T') {
        fprintf (http_out, "<a href=\"/?F%s\">showing</a> ", filter);
    } else if (!threshold_t) {
        fprintf (http_out, "<a href=\"/?N%s\">showing</a> ", filter);
    } else if (filter[0]) {
        fprintf (http_out, "<a href=\"/?T%s\">showing</a> ", filter);
    } else {
        fprintf (http_out, "<a href=\"/?F%s\">showing</a> ", filter);
    }

    fprintf (http_out, "<a href=\"/players/%s\">%s*</a> %s",
             filter, filter,
             ((omm == 'T') ? "to move"
                           : "<a href=\"/\">games</a>"));

    if (threshold_t) {
        fprintf (http_out, " on next update");
    }

    fprintf (http_out, "<hr/>\n");

    for (this_game = games; this_game; this_game = this_game->next)
    {
        this_game->seen = 0;
    }

    this_game = games;
    for (this_game = games;
         this_game && (shown < 10) && (y < 50);
         this_game = this_game->next)
    {
        int fm;

        if (this_game->seen) {
            continue;
        }
        this_game->seen = 1;

        fm = filter_match (filter, this_game);
        if (fm) {
            struct game *rg;
            rg = get_game (reversed_name (this_game));

            if (rg) {
                fprintf (http_out, "%s", "<table><tr><td width=\"650px\">\n");
            }
            shown += maybe_expand_game (y, omm, fm, threshold_t, this_game);
            if (rg) {
                fm = filter_match (filter, rg);
                fprintf (http_out, "</td><td>\n");
                shown += maybe_expand_game (y + 1, omm, fm, threshold_t, rg);
                rg->seen = 1;
                fprintf (http_out, "</td></tr></table>\n");
            }
            fprintf (http_out, "<hr/>\n");

            y += 2;
        }
    }
    fprintf (http_out, "<p style=\"padding-bottom:4in\"></body></html>\n");

    return (0);
}

static void
sanitize_name (char *new_name, char *name)
{
    int i;
    char *vs;

    for (i = 0; i < MAX_NAME - 1; ++i) {
        if (!name[i]) {
            break;
        }
        if (!isalpha(name[i])) {
            new_name[i] = '_';
        } else {
            new_name[i] = tolower (name[i]);
        }
    }

    new_name[i] = '\0';

    vs = strstr (new_name, VS);
    if (vs) {
        *vs = '\0';
        if (strcmp (new_name, vs + VSLEN)) {
            /* verified two distinct players */
            *vs = VS[0];
        }
    }
}

static int
count_games (char *player_name)
{
    struct game *g;
    char *vs;
    char buf[MAX_NAME];
    int count = 0;

    for (g = games; g; g = g->next) {
        vs = strstr (g->name, VS);
        if (vs) {
            strcpy (buf, g->name);
            buf[vs - g->name] = '\0';
            if (!strcmp (player_name, buf)) {
                ++count;
            } else if (!strcmp (player_name, vs + VSLEN)) {
                ++count;
            }
        } else if (!strcmp (player_name, g->name)) {
            ++count;
        }
    }

    return (count);
}

static int
only_game_for_player (struct game *g)
{
    char *vs;
    char buf[MAX_NAME];

    vs = strstr (g->name, VS);
    if (vs) {
        strcpy (buf, g->name);
        buf[vs - g->name] = '\0';
        if (count_games (buf) <= 1) {
            return (1);
        } else if (count_games (vs + VSLEN) <= 1) {
            return (1);
        }
    } else if (count_games (g->name) <= 1) {
        return (1);
    }

    return (0);
}

static void
purge_old_games (void)
{
    struct game *previous_game = NULL;
    struct game *this_game = games;
    time_t now = time (NULL);

    while (this_game) {
        if (!strcmp (this_game->start_pos, this_game->pos)
         && (now - this_game->update_t > 86400)
         && !only_game_for_player (this_game)) {
            struct game *temp = this_game;
            char path[MAX_PATH];

            fprintf (log_out, "%s: purge_old_games %s\n",
                     cnow (), temp->name);

            sprintf (path, ".chessd/%s", temp->name);
            unlink (path);

            this_game = this_game->next;
            if (previous_game) {
                previous_game->next = this_game;
            } else {
                games = this_game;
            }
            free (temp);
        } else {
            previous_game = this_game;
            this_game = this_game->next;
        }
    }
}

static struct game *
create_game (char *name, int create_players)
{
    struct game *g = NULL;
    char new_name[MAX_NAME];

    sanitize_name (new_name, name);
    if (!create_players && (strlen (new_name) != strlen (name))) {
        return (NULL);
    }

    if ((g = get_game (name))) {
        return (g);
    }

    if (create_players) {
        if (players_count >= MAX_PLAYERS) {
            return (NULL);
        }
    } else {
        if (!white_player (new_name) || !black_player (new_name)) {
            return (NULL);
        }
    }

    g = calloc (1, sizeof (*g));
    g->update_t = time (NULL);
    strcpy (g->name, new_name);
    g->sel[0] = '0';
    g->sel[1] = '0';
    g->unused[0] = 'X';
    g->unused[1] = 'X';
    memcpy (g->start_pos, classical_pos (), 66);
    memcpy (g->pos, g->start_pos, 66);
    g->next = games;
    games = g;
    god_sequence++;

    add_players (new_name);

    return (g);
}

static int
http_matches (char *path, char *query)
{
    struct player *p = NULL;
    struct player *p2;
    char playing_as = 'W';
    char new_name[MAX_NAME];
    char *p1_name;
    char filter[MAX_NAME];
    char game_name[MAX_NAME];
    int y;
    int sely = 0;

    filter[0] = '\0';
    if (query && query[0]) {
        char *p;
        playing_as = query[0];
        for (p = query + 1; isdigit (*p); ++p) {
            sely *= 10;
            sely += (*p - '0');
        }
        sanitize_name (filter, p);
    }

    new_name[0] = '\0';
    if (path[1]) {
        sanitize_name (new_name, path + 1);
        p = get_player (new_name);
    }

    fprintf (http_out,
             "<html>\n"
             "  <head>\n"
             "    <title>matches</title>\n"
             "    <script>\n"
             "      var timer = null;\n"
             "      var y = %d;\n"
             "      var filter = '%s';\n"
             "      var append = '';\n"
             "      function auto_reload () {\n"
             "        if (append.length) {\n"
             "          window.location = '?%c' + filter + append;\n"
             "        }\n"
             "      };\n"
             "      document.onkeydown = function(evt) {\n"
             "        evt = evt || window.event;\n"
             "        switch(evt.which) {\n"
             "          case 13:\n" //enter
             "          case 32:\n" //space
             "            document.getElementById(y).click();\n"
             "            break;\n"
             "          case 8:\n" //backspace
             "          case 46:\n" //delete
             "            if (append.length) {\n"
             "              clearTimeout(timer);\n"
             "              append = append.substring(0, append.length - 1);\n"
             "              document.getElementById('filter').innerHTML = filter + append + '*';\n"
             "              timer = setTimeout('auto_reload()',500);\n"
             "            } else {\n"
             "              window.location = '?%c';\n"
             "            }\n"
             "            break;\n"
             "          case 27:\n"
             "            window.alert('"
             "up and down arrows: navigate links\\n"
             "space or enter: follow link\\n"
             "a-z: filter opponent\\n"
             "backspace or delete: clear filter\\n"
             "tab: swap colors\\n"
             "left arrow: return to player list\\n"
             "right arrow: show games for filter');\n"
             "            break;\n"
             "          case 37:\n" //left arrow
             "            window.location = '/players/%s?%c';\n"
             "            return;\n"
             "          case 9:\n" //tab
             "            window.location = '/matches/%s?%c%d%s';\n"
             "            return;\n"
             "          case 38:\n" //up arrow
             "            if (y > 0) {\n"
             "              document.getElementById(y).style.backgroundColor = 'black';\n"
             "              --y;\n"
             "              document.getElementById(y).style.backgroundColor = 'white';\n"
             "            }\n"
             "            return;\n"
             "          case 39:\n" //right arrow
             "            window.location = '/?F%s';\n"
             "            return;\n"
             "          case 40:\n" //down arrow
             "            if (document.getElementById(y+1)) {\n"
             "              document.getElementById(y).style.backgroundColor = 'black';\n"
             "              ++y;\n"
             "              document.getElementById(y).style.backgroundColor = 'white';\n"
             "            }\n"
             "            return;\n"
             "          default:\n" //0-9 and A-Z
             "            if (evt.which < 48 || evt.which > 90) {\n"
             "              return;\n"
             "            }\n"
             "            if (evt.which > 57 && evt.which < 65) {\n"
             "              return;\n"
             "            }\n"
             "            clearTimeout(timer);\n"
             "            append += String.fromCharCode(evt.which).toLowerCase();\n"
             "            document.getElementById('filter').innerHTML = filter + append + '*';\n"
             "            timer = setTimeout('auto_reload()',500);\n"
             "            break;\n"
             "        }\n"
             "      };\n"
             "    </script>\n"
             "  </head>\n"
             "  <body bgcolor=\"black\" style=\"color:white\">\n",
             sely, filter, playing_as,
             playing_as, //backspace or delete
             new_name, ((playing_as == 'W') ? 'b' : 'c'), //left arrow
             new_name, ((playing_as == 'W') ? 'B' : 'W'), sely, filter, //tab
             new_name); //right arrow

    if (new_name[0]) {
        if (playing_as != 'W') {
            fprintf (http_out,
                     "<a id=\"filter\" name=\"filter\" href=\"/players/%s\">%s*</a>"
                     " <a href=\"?W%d%s\">vs</a> ",
                     filter, filter,
                     sely, filter);
        }

        fprintf (http_out,
                 "<a href=\"/players/%s\">%s</a>",
                 new_name, new_name);

        if (playing_as == 'W') {
            fprintf (http_out,
                     " <a href=\"?B%d%s\">vs</a> "
                     "<a id=\"filter\" name=\"filter\" href=\"/players/%s\">%s*</a>",
                     sely, filter,
                     filter, filter);
        }
    } else {
        fprintf (http_out,
                 "<a href=\"/players/%s\">%s*</a>",
                 filter, filter);
    }

    fprintf (http_out,
             "    <table border>\n"
             "    <tr>\n"
             "      <th align=\"left\">white</th>\n"
             "      <th align=\"left\">black</th>\n"
             "      <th align=\"left\">game</th>\n"
             "    </tr>\n");

    y = 0;
    for (p2 = players; p2; p2 = p2->next) {
        if (p == p2) {
            continue;
        }

        if (strncmp (filter, p2->name, strlen (filter))) {
            continue;
        }

        if (new_name[0]) {
            p1_name = new_name;
            game_name[sizeof (game_name) - 2] = '\0';
            snprintf (game_name, sizeof (game_name), "%s_vs_%s",
                      (playing_as == 'W' ? p1_name : p2->name),
                      (playing_as == 'W' ? p2->name : p1_name));
            if (game_name[sizeof (game_name) - 2]) {
                game_name[0] = '\0';
            }
        } else {
            p1_name = p2->name;
            strcpy (game_name, p1_name);
        }

        fprintf (http_out,
                 "    <tr>\n"
                 "      <th align=\"left\">%s</th>\n"
                 "      <th align=\"left\">%s</th>\n"
                 "      <td>",
                 (playing_as == 'W' ? p1_name : p2->name),
                 (playing_as == 'W' ? p2->name : p1_name));

        fprintf (http_out, "<a id=\"%d\" name=\"%d\" style=\"%s\"",
                 y, y, (y == sely ? "background-color:white; color:indigo"
                                  : "background-color:inherit; color:indigo"));

        if (game_name[0]) {
            set_current_game (game_name);

            if (current_game) {
                fprintf (http_out, " href=\"/%s\">%s</a>\n",
                         game_name, game_name);
            } else if (p || !new_name[0]) {
                fprintf (http_out, " href=\"/%s\">CREATE</a>\n",
                         game_name);
            } else {
                fprintf (http_out, " href=\"/create/%s\">CREATE</a>\n",
                         game_name);
            }
        } else {
            fprintf (http_out, " href=\"\">NAME TOO LONG</a>\n");
        }

        fprintf (http_out, "</td></tr>\n");

        ++y;
    }

    if (filter[0] && !new_name[0]) {
        strcpy (new_name, filter);
        filter[0] = '\0';
    }

    if (!p && new_name[0]) {
        if (filter[0]) {
            game_name[sizeof (game_name) - 2] = '\0';
            snprintf (game_name, sizeof (game_name), "%s_vs_%s",
                      (playing_as == 'W' ? new_name : filter),
                      (playing_as == 'W' ? filter : new_name));
            if (game_name[sizeof (game_name) - 2]) {
                game_name[0] = '\0';
            }
        } else {
            strcpy (game_name, new_name);
        }

        if (game_name[0]) {
            fprintf (http_out,
                     "    <tr>\n"
                     "      <th align=\"left\">%s</th>\n"
                     "      <th align=\"left\">%s</th>\n"
                     "      <td><a id=\"%d\" name=\"%d\" style=\"%s\"",
                     ((playing_as == 'W' || !filter[0]) ? new_name : filter),
                     ((playing_as == 'W' && filter[0]) ? filter : new_name),
                     y, y, (y == sely ? "background-color:white; color:black"
                                      : ""));

            fprintf (http_out,
                     " href=\"/create/%s\">CREATE</a></td>\n    </tr>\n",
                     game_name);
        }
    }

    fprintf (http_out,
             "    </table>\n"
             "  </body>\n"
             "  </head>\n"
             "</html>\n");

    return (0);
}

static int
http_players (char *player_path, char *query)
{
    struct player *p;
    char filter[MAX_NAME];
    int y;
    char selx = 'a';
    int sely = 0;
    char *off;
    char *on;

    filter[0] = '\0';

    if (player_path && player_path[0]) {
        sanitize_name (filter, player_path + 1);
        if (query) {
            char *option = query;
            switch (*option) {
            case 'a':
            case 'b':
            case 'c':
            case 'd':
            case 'e':
            case 'f':
                selx = *(option++);
            default:
                break;
            }
            while (isdigit (*option)) {
                sely *= 10;
                sely += (*(option++) - '0');
            }
            if (option[0] && (p = get_player (option + 1))) {
                switch (option[0]) {
                default:
                    break;
                case 'S':
                    p->show = ((p->show == 'Y') ? 'N' : 'Y');
                    break;
                case 'T':
                    p->touch = ((p->touch == 'Y') ? 'N' : 'Y');
                    break;
                }
            }
        }
    }

    fprintf (http_out,
             "<html>\n"
             "  <head>\n"
             "    <title>players</title>\n"
             "    <script>\n"
             "      var timer = null;\n"
             "      var x = '%c';\n"
             "      var y = %d;\n"
             "      var filter = '%s';\n"
             "      var append = '';\n"
             "      function auto_reload()\n"
             "      {\n"
             "        if (append.length) {\n"
             "          window.location = '/players/' + filter + append;\n"
             "        }\n"
             "      };\n"
             "      document.onkeydown = function(evt) {\n"
             "        evt = evt || window.event;\n"
             "        switch(evt.which) {\n"
             "          case 8:\n" //backspace
             "          case 46:\n" //delete
             "            if (append.length) {\n"
             "              clearTimeout(timer);\n"
             "              append = append.substring(0, append.length - 1);\n"
             "              document.getElementById('filter').innerHTML = filter + append + '*';\n"
             "              timer = setTimeout('auto_reload()',500);\n"
             "            } else {\n"
             "              window.location = '/players/';\n"
             "            }\n"
             "            break;\n"
             "          case 9:\n" //tab
             "            window.location = '/matches/%s';\n"
             "            return;\n"
             "          case 13:\n" //enter
             "          case 32:\n" //space
             "            document.getElementById(x+y).click();\n"
             "            break;\n"
             "          case 27:\n"
             "            window.alert('"
             "arrow keys: navigate links\\n"
             "space or enter: follow link\\n"
             "a-z: filter further\\n"
             "backspace or delete: clear filter\\n"
             "tab: show matches for filter');\n"
             "            break;\n"
             "          case 37:\n" //left arrow
             "            if (x == 'a') {\n"
             "              window.location = '/?N' + filter + append;\n"
             "            } else {\n"
             "              var e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'inherit';\n"
             "              x = String.fromCharCode(x.charCodeAt(0)-1);\n"
             "              e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'white';\n"
             "            }\n"
             "            break;\n"
             "          case 38:\n" //up arrow
             "            if (y > 0) {\n"
             "              var e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'inherit';\n"
             "              --y;\n"
             "              e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'white';\n"
             "            }\n"
             "            return;\n"
             "          case 39:\n" //right arrow
             "            if (x != 'f') {\n"
             "              var e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'inherit';\n"
             "              x = String.fromCharCode(x.charCodeAt(0)+1);\n"
             "              e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'white';\n"
             "            }\n"
             "            break;\n"
             "          case 40:\n" //down arrow
             "            if (document.getElementById('a'+(y+1))) {\n"
             "              var e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'inherit';\n"
             "              ++y;\n"
             "              e = document.getElementById(x+y);\n"
             "              e.style.backgroundColor = 'white';\n"
             "            }\n"
             "            break;\n"
             "          default:\n" //0-9 and A-Z
             "            if (evt.which < 48 || evt.which > 90) {\n"
             "              return;\n"
             "            }\n"
             "            if (evt.which > 57 && evt.which < 65) {\n"
             "              return;\n"
             "            }\n"
             "            clearTimeout(timer);\n"
             "            append += String.fromCharCode(evt.which).toLowerCase();\n"
             "            document.getElementById('filter').innerHTML = filter + append + '*';\n"
             "            timer = setTimeout('auto_reload()',500);\n"
             "            break;\n"
             "        }\n"
             "      };\n"
             "    </script>\n"
             "  </head>\n"
             "  <a id=\"filter\" name=\"filter\" href=\"/matches/%s\">%s*</a>\n"
             "  <body bgcolor=\"black\" style=\"color:white\">\n"
             "    <table border>\n"
             "    <tr>\n"
             "      <th align=\"left\">name</th>\n"
             "      <th>playing as</th>\n"
             "      <th>touch</th>\n"
             "      <th>show</th>\n"
             "    </tr>\n",
             selx, sely, filter,
             filter, //tab
             filter, filter);

    y = 0;
    off = "background-color:inherit; color:indigo";
    for (p = players; p; p = p->next) {
        if (strncmp (filter, p->name, strlen (filter))) {
            continue;
        }

        if (y == sely) {
            on = "background-color:white; color:indigo";
        } else {
            on = off;
        }

        fprintf (http_out,
                 "    <tr>\n"
                 "      <th align=\"left\"><a id=\"a%d\" name=\"a%d\" style=\"%s\" href=\"/?T%s\">%s</a></th>\n"
                 "      <td><a id=\"b%d\" name=\"b%d\" style=\"%s\" href=\"/matches/%s?W\">white</a>&nbsp;\n"
                 "          <a id=\"c%d\" name=\"c%d\" style=\"%s\" href=\"/matches/%s?B\">black</a>&nbsp;\n"
                 "          <a id=\"d%d\" name=\"d%d\" style=\"%s\" href=\"/%s\">both</a></td>\n"
                 "      <td align=\"center\"><a id=\"e%d\" name=\"e%d\" style=\"%s\" href=\"/players/%s?e%dT%s\">%c</a></td>\n"
                 "      <td align=\"center\"><a id=\"f%d\" name=\"f%d\" style=\"%s\" href=\"/players/%s?f%dS%s\">%c</a></td>\n"
                 "    </tr>\n",
                 y, y, (selx == 'a' ? on : off), p->name, p->name, //name
                 y, y, (selx == 'b' ? on : off), p->name, //playing as white
                 y, y, (selx == 'c' ? on : off), p->name, //playing as black
                 y, y, (selx == 'd' ? on : off), p->name, //playing as both
                 y, y, (selx == 'e' ? on : off), filter, y, p->name, p->touch,
                 y, y, (selx == 'f' ? on : off), filter, y, p->name, p->show);
        ++y;
    }

    fprintf (http_out,
             "    </table>\n"
             "  </body>\n"
             "  </head>\n"
             "</html>\n");

    return (0);
}

static int
http_create (char *path, char *query)
{
    struct game *g;
    char *msg = "nothing to do!";

    if (path[1]) {
        g = create_game (path + 1, 1);
        if (g) {
            if (g->sequence) {
                msg = "players already exist:";
            } else {
                msg = "players created:";
            }
        } else {
            msg = "too many players! no room! no room!";
        }
    }

    fprintf (http_out,
             "<html>\n"
             "  <head>\n"
             "    <title>create</title>\n"
             "  </head>\n"
             "  <body bgcolor=\"black\" style=\"color:white\">\n"
             "    %s\n",
             msg);

    if (g) {
        fprintf (http_out, "    <a href=\"/%s\">%s</a>\n", g->name, g->name);
    }

    fprintf (http_out,
             "  </body>\n"
             "  </head>\n"
             "</html>\n");

    return (0);
}

static int
http_respond (char *path, char *query)
{
    char name[MAX_NAME];

    fprintf (log_out, "%s: %s GET %s?%s\n",
             cnow (), current_ip, path, query);
    fflush (log_out);

    if (!strncmp (path, "/images/", 8)) {
        http_png (http_out, path + 1);
        return (0);
    } else if (!strcmp (path, "/favicon.ico")) {
        http_png (http_out, path + 1);
        return (0);
    } else if (!strncmp (path, "/players/", 9)) {
        fprintf (http_out, "HTTP/1.0 200 OK\nContent-Type: text/html\n\n");
        return (http_players (path + 8, query));
    } else if (!strncmp (path, "/matches/", 9)) {
        fprintf (http_out, "HTTP/1.0 200 OK\nContent-Type: text/html\n\n");
        return (http_matches (path + 8, query));
    } else if (!strncmp (path, "/create/", 8)) {
        fprintf (http_out, "HTTP/1.0 200 OK\nContent-Type: text/html\n\n");
        return (http_create (path + 7, query));
    } else if (path[0] == '/') {
        strncpy (name, path + 1, sizeof (name));
        name[sizeof (name) - 1] = '\0';
    } else {
        name[0] = '\0';
    }

    set_current_game (name);

    if (!current_game) {
        if (!name[0]) {
            fprintf (http_out, "HTTP/1.0 200 OK\nContent-Type: text/html\n\n");
            return (http_games (query));
        }
        current_game = create_game (name, 0);
        if (!current_game) {
            fclose (http_out);
            http_out = NULL;
            return (1);
        }
    }

    fprintf (http_out, "HTTP/1.0 200 OK\nContent-Type: text/html\n\n");
    http_play (query);

    return (0);
}

#define MAX_HTTP_BUFFER 10000
static int
http_client (int http_fd)
{
    char http_buffer[MAX_HTTP_BUFFER];
    char *http_buffer_end = http_buffer;
    char *path = NULL;
    char *post = http_buffer;
    int remaining = MAX_HTTP_BUFFER;
    int count;
    char *query = NULL;
    char *headers = NULL;
    char *status = NULL;
    struct timespec req;
    int flags;
    int loop = 0;

    flags = fcntl (http_fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl (http_fd, F_SETFL, flags);
    req.tv_sec = 0;
    req.tv_nsec = 1000000; /* one millisecond */

    do {
        do {
            count = read (http_fd, http_buffer_end, remaining);

            if ((count >= 0) || (errno != EAGAIN))
                break;

            nanosleep (&req, NULL);
        } while (++loop < 1000);

        if (count == 0) {
            fprintf (log_out, "%s: http_client read EOF\n", cnow ());
            return (0);
        } else if (count < 0) {
            return (1);
        }

        remaining -= count;
        if (!remaining) {
            fprintf (log_out, "%s: http_client HTTP headers too long!\n",
                     cnow ());
            return (1);
        }

        http_buffer_end += count;
        while (post < http_buffer_end) {
            ++post;
            if (!strncmp (post - 2, "\n\n", 2)) {
                remaining = 0;
                *(post - 1) = '\0';
                break;
            }
            if (!strncmp (post - 4, "\r\n\r\n", 4)) {
                remaining = 0;
                *(post - 2) = '\0';
                break;
            }
        }
    } while (remaining);

    *http_buffer_end = '\0';
    char *p;

    p = strchr (http_buffer, ' ');

    if (!p) {
        status = "501 Unsupported Method";
    } else {
        *p = '\0';
        path = ++p;
        while (p++) {
            switch (*p) {
                case '?':
                    *p = '\0';
                    query = p + 1;
                    break;
                case ' ':
                    *p = '\0';
                    if (!query) query = p;
                    break;
                case '\n':
                    headers = p;
                    p = 0;
                    break;
            }
        }

        *post = '\0';
    }

    flags &= ~O_NONBLOCK;
    fcntl (http_fd, F_SETFL, flags);
    http_out = fdopen (http_fd, "w");

    if (status) {
        fprintf (http_out, "HTTP/1.0 %s\n\n%s", status, status);
    } else if (http_respond (path, query)) {
        return (1);
    } else if (http_out) {
        fflush (http_out);
        fclose (http_out);
        http_out = NULL;
    }

    return (0);
}

void
process_fics_line (char *input)
{
    char pos[66];
    char c;
    int i;
    int efile;
    int wck;
    int wcq;
    int bck;
    int bcq;
    char *notation;
//<12> r---k--r p-pbnpp- -pnp--q- -------- B--PP--p --P-BN-P P----PP- -R-QR--K B -1 0 0 1 1 2 94 Tupangligaw sailingsoul 0 15 10 35 35 552 121 15 K/g1-h1 (2:19) Kh1 0 1 0
    if (strncmp (input, "<12> ", 5)) {
        return;
    }

    input += 5;

    i = 1;
    while (i < 65) {
        c = *(input++);
        switch (c) {
        default:
            pos[i++] = c;
            break;
        case '\0':
            chatstr ("===== ERROR: truncated position!\n");
            return;
        case ' ':
            break;
        case '-':
            pos[i++] = '+';
            break;
        }
    }

    pos[i] = '\0';
    if (6 != sscanf (input, " %c %d %d %d %d %d ",
                     &(pos[0]), &efile, &wck, &wcq, &bck, &bcq)) {
        chatstr ("===== ERROR: could not parse castle rights!\n");
        return;
    }

    if (efile >= 0) {
        if (efile > 7) {
            chatstr ("===== ERROR: en passant file too large!\n");
            return;
        }
        if (pos[0] == 'W') {
            if (((efile > 0) && pos[cti(efile - 1 + 'a','6')] == 'P')
             || ((efile < 7) && pos[cti(efile + 1 + 'a','6')] == 'P')) {
                pos[cti(efile + 'a','6')] = 'e';
            }
        } else {
            if (((efile > 0) && pos[cti(efile - 1 + 'a','3')] == 'p')
             || ((efile < 7) && pos[cti(efile + 1 + 'a','3')] == 'p')) {
                pos[cti(efile + 'a','3')] = 'E';
            }
        }
    }

    if (wck) pos[cti('h','1')] = 'O';
    if (wcq) pos[cti('a','1')] = 'O';
    if (bck) pos[cti('h','8')] = 'o';
    if (bcq) pos[cti('a','8')] = 'o';

    if (!memcmp (current_game->pos, pos, 66)) {
        return;
    }

    fprintf (log_out, "%s: was %.*s\n", cnow (), 65, current_game->pos);
    fprintf (log_out, "%s: now %.*s\n", cnow (), 65, pos);

    notation = one_move_diff (current_game->pos, pos);
    memcpy (current_game->pos, pos, 66);
    current_game->sel[0] = '0';
    current_game->sel[1] = '0';
    tick ();

    if (notation) {
        if (!current_game->movenum) {
            current_game->transcriptlen = 0;
            transcribe_fen ();
        }
        transcribe_move (notation);
    } else {
        current_game->transcriptlen = 0;
        current_game->movenum = 0;
        transcribe_fen ();
    }
}

static void
process_fics (char *buffer, int len)
{
    static char input[500];
    static int input_len = 0;

    while (len-- > 0) {
        char c = *(buffer++);

        if (c == '\r') {
            continue;
        }

        if (input_len) {
            if (input[0] != '<') {
                input[input_len] = '\0';
                if (!strcmp (input, "fics%")) {
                    if (!current_game->fics_style) {
                        if (13 == write (current_game->fics, "set style 12\n", 13)) {
                            current_game->fics_style = 12;
                        }
                    }
                    if (current_game->chatlen >= 6) {
                        current_game->chatlen -= 6;
                    }
                    input_len = 0;
                    return;
                }
                chatchar (c);
            }
        } else if (c != '<') {
            chatchar (c);
        }
        if (c == '\n') {
            input[input_len] = '\0';
            process_fics_line (input);
            input_len = 0;
        } else if (input_len + 1 < sizeof (input)) {
            input[input_len++] = c;
        }
    }
}

void
handler (int s)
{
    fprintf (log_out, "%s: Received signal %d\n",
             cnow (), s);

    switch (s) {
    case SIGTERM:
    case SIGINT:
        fprintf (log_out, "%s: Set term=1\n", cnow ());
        term = 1;
        break;
    }
}

static int
roll_log (void)
{
    static char last_path[100];
    static time_t last_t;
    char this_path[100];
    time_t now_t = time (NULL);
    struct tm *now_tm;

    if (last_t == now_t) {
        return (0);
    }
    last_t = now_t;

    now_tm = localtime (&now_t);
    sprintf (this_path, "%s.%04d%02d%02d",
             log_path, now_tm->tm_year + 1900,
             now_tm->tm_mon + 1, now_tm->tm_mday);

    if (strcmp (last_path, this_path)) {
        strcpy (last_path, this_path);

        if (log_out) {
            fclose (log_out);
        }
        log_out = fopen (this_path, "a");

        if (!log_out) {
            last_t = 0;
            last_path[0] = '\0';
            return (1);
        }
    }

    return (0);
}

int
main (int argc, char **argv)
{
    int portno;
    int sockfd;
    socklen_t clilen;
    struct sockaddr_in serv_addr;
    struct sockaddr_in cli_addr;
    int on = 1;

    if (argc != 2) {
        fprintf (stderr, "Usage: %s port\n", argv[0]);
        exit (1);
    }
    portno = atoi (argv[1]);

    if (roll_log ()) {
        return (1);
    }

    fprintf (log_out, "%s: Started as '%s %d'\n",
             cnow (), argv[0], portno);

    if (load_games ()) {
        fprintf (log_out, "%s: load_games failed!\n", cnow ());
        return (1);
    }

    clilen = sizeof (cli_addr);

    if ((sockfd = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf (log_out, "%s: Failed to create server socket\n",
                 cnow ());
        exit (1);
    }

    signal (SIGPIPE, handler);
    signal (SIGTERM, handler);
    signal (SIGINT, handler);

    if (setsockopt (sockfd, SOL_SOCKET, SO_REUSEADDR,
                    (const void *) &on, sizeof (on))) {
        fprintf (log_out, "%s: Failed to setsockopt SO_REUSEADDR\n",
                 cnow ());
    }

    if (fcntl (sockfd, F_SETFL, O_NONBLOCK) < 0) {
        fprintf (log_out, "%s: Failed to set socket to O_NONBLOCK\n",
                 cnow ());
    }

    memset (&serv_addr, 0, sizeof (serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons (portno);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind (sockfd, (struct sockaddr *) &serv_addr, sizeof (serv_addr)) < 0) {
        fprintf (log_out, "%s: Failed to bind server socket!\n",
                 cnow ());
        exit (1);
    }
    fprintf (log_out, "%s: Bound server socket to port %d\n",
             cnow (), portno);

    listen (sockfd, 5);
    fprintf (log_out, "%s: Listening on port %d\n",
             cnow (), portno);

    while (1) {
        struct pollfd fds[1];
        time_t now;

        fds[0].fd = sockfd;
        fds[0].events = POLLIN;
        http_fd = 0;

        if (poll (fds, 1, 100) > 0) {
            http_fd = accept (sockfd, (struct sockaddr *) &cli_addr, &clilen);
        }

        for (current_game = games;
             current_game;
             current_game = current_game->next) {
            if (current_game->fics > 0) {
                int result;
                fd_set readset;
                struct timeval tv;

                FD_ZERO (&readset);
                FD_SET (current_game->fics, &readset);
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                result = select (current_game->fics + 1, &readset,
                                 NULL, NULL, &tv);
                if ((result > 0) && FD_ISSET (current_game->fics, &readset)) {
                    char buffer[1000];
                    result = read (current_game->fics, buffer, sizeof (buffer));
                    if (result <= 0) {
                        chatstr ("========================== CLOSED FICS ==========================\n");
                        close (current_game->fics);
                        current_game->fics = 0;
                    } else {
                        process_fics (buffer, result);
                    }
                }
            }
        }

        if (http_fd > 0) {
            strcpy (current_ip, inet_ntoa (cli_addr.sin_addr));
            if (http_client (http_fd)) {
                int zero = 0;
                if (setsockopt (sockfd, SOL_SOCKET, SO_LINGER,
                                (const void *) &zero, sizeof (zero))) {
                }
            }
            close (http_fd);
        }

        now = time (NULL);
        if (term) {
            return (persist ());
        } else if (!save_t) {
            save_t = now;
        } else if ((now - save_t > 60) && (save_sequence != god_sequence)) {
            purge_old_games ();
            persist ();
        }

        if (roll_log ()) {
            return (1);
        }
    }

    return (0) /* Never reached */;
}
