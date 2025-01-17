//                                                                                                                                                                                                               //
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define CTRL_KEY(value) ((value) & 0x1f)
#define ABUF_INIT {NULL, 0}
#define KEDITOR_VERSION "0.0.1"
#define KEDITOR_TAB_STOP 8
#define KEDITOR_QUIT_TIMES 2

typedef struct editorConfig editorConfig;
typedef struct abuf abuf;
typedef struct erow erow;

void enableRauMode();
void disableRauMode();
void die(const char *msg);
int editorReadKey();
void editorProcessKeypress();
void editorRefreshScreen();
void editorDrawRows();
void initEditor();
int getCursorPosition(int *rows, int *cols);
void abAppend(abuf *ab, char *s, int len);
void abFree(abuf *ab);
void editorMoveCursor(int key);
void editorOpen(char *filename);
void editorAppendRow(int at, char *s, size_t len);
void editorScroll();
void editorUpdateRow(erow *row);
int editorRowCxtoRx(erow *row, int cx);
void editorDrawStatusBar(abuf *ab);
void editorSetStatusMessage(const char *fmt, ...);
void editorDrawMessageBar(abuf *ab);
void editorRowInsertChar(erow *row, int at, int c);
void editorInsertChar(int c);
char *editorRowsToString(int *buflen);
void editorSave();
void editorInsertChar(int c);
void editorDeleteChar();
void editorFreeRow(erow *row);
void editorDeleteRow(int at);
void editorRowAppendString(erow *row, char *c, size_t len);
void editorInsertNewLine();
void *editorPrompt(char *prompt);

struct erow {
    int size;
    char *chars;
    int rsize;
    char *render;
};

struct editorConfig {
    int screenrows;
    int screencols;
    int cx;
    int cy;
    int rx;
    int numrows;
    erow *row;
    int rowoff;
    int coloff;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    int dirty;
    struct termios orig_termios;
};

struct abuf {
    char *buf;
    int len;
};

enum editorKey {
    BACKSPACE = 127,
    ARROW_UP = 1000,
    ARROW_RIGHT,
    ARROW_DOWN,
    ARROW_LEFT,
    DELETE_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

editorConfig E;

void enableRauMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRauMode);

    struct termios raw = E.orig_termios;
    // Ctrl + S, Ctrl + Q を無効化
    // Ctrl + J が 10 を取っているので、Enter と Ctrl + M を 10 から 13 に移行させる。
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    // キャリッジリターンなどの出力処理機能を無効化
    // これを無効化すると、出力が段々になる。
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CS8);
    // エコーバッグを無効化 (その役割を果たす bit を 0 にするための操作。)
    // カノニカルモードを無効化
    // Ctrl + C, Ctrl + Z, Ctrl + Z を無効化
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // The TCSAFLUSH argument specifies when to apply the change
    raw.c_cc[VMIN] = 0;
    // この値を 0 にすると、とめどなく画面が流れる。値を受け取る周期を設定する。
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

void disableRauMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void die(const char *msg) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(msg);
    exit(EXIT_FAILURE);
}

/// 入力キーを変換する関数
// 無限ループで入力を待ち受けるが、Enter を押すと、処理が終了する。
// read() == 0 としている時にこれが生じる。
// read() は読み込んだ Byte 数を返すので、この条件式だと一文字読み込んでバッファに書き込んだ時点で while が終了する。
// デフォルトではカノニカルモード (cooked mode) なので、Enter が押されるまでプログラムにキー入力が受け渡されない。
// この際、標準入力から受け取った値は、バッファに溜められ、Enter が押されると吐き出される。
// したがって、エディタの機能を実装するためには、非カノニカルモード (raw mode) を実現する必要がある。
int editorReadKey() {
    int nread;
    char c;
    // not= 1 にしないとマルチバイトに対応できない。
    // この処理がイマイチ納得いっていない。
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread < 0 && errno != EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                // page キーと Delete キーの parse
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY; // 他プラットフォーム対応
                        case '4': return END_KEY; // 他プラットフォーム対応
                        case '3':
                            return DELETE_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7': return HOME_KEY; // 他プラットフォーム対応
                        case '8': return END_KEY; // 他プラットフォーム対応
                    }
                }
            } else {
                switch (seq[1]) {
                    // 矢印キーの parse
                    case 'A':
                        return ARROW_UP;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'B':
                        return ARROW_DOWN;
                    case 'D':
                        return ARROW_LEFT;
                    // Home, End キーの parse
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY; // 他プラットフォーム対応
                case 'F': return END_KEY; // 他プラットフォーム対応
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

/// カーソルの座標を表す変数を変更する関数
void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                // E.cx > 0 の評価式にしないとファイルの一番先頭にカーソルがある状態で Left Arrow を押すと、異常終了してしまう。
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
    }

    // 行末から行末の短い行に移動した時に、カーソルが行末に移動するようなロジック
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

/// 入力キーを変換して、それに対応する処理を呼び出す関数
void editorProcessKeypress() {
    static int quit_times = KEDITOR_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        // TODO
        case '\r':
            editorInsertNewLine();
            break;
        case CTRL_KEY('q'):
            if (E.dirty > 0 && quit_times > 0) {
                editorSetStatusMessage(
                    "WARNING!!! File has unsaved changes. "
                    "Press Ctrl-Q %d more times to quit.",
                    quit_times
                );
                quit_times--;
                return;
            }

            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(EXIT_SUCCESS);
            break;
        // 保存
        case CTRL_KEY('s'):
            editorSave();
            break;
        // 画面の左端か右端にカーソルを移動させる
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cx < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;
        // TODO
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DELETE_KEY:
            // delete キーが押された時はカーソルを右にずらしてバックスペースで削除する実装にする。
            // すなはちカーソルにある文字を削除する。
            if (c == DELETE_KEY) {
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDeleteChar();
            break;
        // 画面の一番上か一番下のカーソルを移動させる
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                }
                if (E.cy > E.numrows) {
                    E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--) {
                    editorMoveCursor(c == PAGE_DOWN ? ARROW_DOWN : ARROW_UP);
                }
            }
            break;
        case ARROW_UP:
        case ARROW_RIGHT:
        case ARROW_DOWN:
        case ARROW_LEFT:
            editorMoveCursor(c);
            break;
        // TODO
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KEDITOR_QUIT_TIMES;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

int getCursorPosition(int *rows, int *cols) {
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    char buf[32];
    unsigned int i = 0;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

void editorDrawRows(abuf *ab) {
    int y = 0;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            // Welcome Messsage を描画
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcome_length = snprintf(welcome, sizeof(welcome), "Keditor -- version %s", KEDITOR_VERSION);
                if (welcome_length > E.screencols) {
                    welcome_length = E.screencols;
                }
                int padding = (E.screencols - welcome_length) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    abAppend(ab, " ", 1);
                }

                abAppend(ab, welcome, welcome_length);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screencols) {
                len = E.screencols;
            }
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        // この行削除のエスケープシーケンスを書き込むことで、画面を消して上書きで書き込むことができる。
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

// E.cx を E.rx に変換する関数
// Tab 文字が存在するときは、
int editorRowCxtoRx(erow *row, int cx) {
    //
    int rx = 0;
    for (int i = 0; i < cx; i++) {
        if (row->chars[i] == '\t') {
            rx += (KEDITOR_TAB_STOP - 1) - (rx % KEDITOR_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxtoRx(&E.row[E.cy], E.cx);
    }

    // y 方向
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.screenrows + E.rowoff) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    // x 方向
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.screencols + E.coloff) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawStatusBar(abuf *ab) {
    abAppend(ab, "\x1b[46m", 5);
    // 左端に出すメッセージ
    char status[80];
    int len = snprintf(
        status,
        sizeof(status),
        "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]" ,
        E.numrows,
        E.dirty > 0 ? "( modified )" : ""
    );
    abAppend(ab, status, len);
    len = len > E.screencols ? E.screencols : len;
    // 右端に出すメッセージ
    char rstatus[80];
    int rlen = snprintf(
        rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows
    );
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

void editorDrawMessageBar(abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) {
        msglen = E.screencols;
    }
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() {
    editorScroll();

    abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    // 絶対値 (E.cy) から相対値 (原点がウィンドウ) に変更する必要がある。
    // この処理ではバグる。
    // snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1 > E.numrows ? E.numrows : (E.cy - E.rowoff) + 1 , (E.rx - E.coloff) + 1);
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    abFree(&ab);
}

void abAppend(abuf *ab, char *s, int len) {
    char *new = realloc(ab->buf, ab->len + len);

    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->buf = new;
    ab->len += len;
}

void abFree(abuf *ab) {
    free(ab->buf);
}

char *editorRowsToString(int *buflen) {
    int total_length = 0;
    int i = 0;
    for (i = 0; i < E.numrows; i++) {
        total_length += (E.row[i].size + 1);
    }
    *buflen = total_length;

    char *buf = malloc(sizeof(char) * total_length);
    char *head = buf;
    for (i = 0; i < E.numrows; i++) {
        memcpy(head, E.row[i].chars, E.row[i].size);
        head += E.row[i].size;
        *head++ = '\n';
    }

    return buf;
}

void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as : %s");
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                E.dirty = 0;
                close(fd);
                free(buf);
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("editorOpen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    // hogehoge != 1 にしていたため、改行があると、それ移行描画されない Bug が生じていた。
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        // E,row[hoge].chars に改行やキャリッジリターンを格納しない。
        while (linelen > 0 && (line[linelen - 1] == '\r' || line[linelen - 1] == '\n')) {
            linelen--;
        }
        editorAppendRow(E.numrows, line, linelen);
    }

    E.dirty = 0;

    free(line);
    fclose(fp);
}

void *editorPrompt(char *prompt) {
    size_t bufsize = 128;
    char *buf = malloc(sizeof(char) * bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (true) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DELETE_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) {
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, sizeof(char) * bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

// ファイルから読み込んだ実体を表示用に変換する
void editorUpdateRow(erow *row) {
    free(row->render);
    int tabs = 0;
    for (int i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') {
            tabs++;
        }
    }
    row->render = malloc(sizeof(char) * (row->size + (KEDITOR_TAB_STOP - 1) * tabs + 1));

    int j = 0;
    int index = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[index++] = ' ';
            while (index % KEDITOR_TAB_STOP != 0) {
                row->render[index++] = ' ';
            }
        } else {
            row->render[index++] = row->chars[j];
        }
    }
    row->render[index] = '\0';
    row->rsize = index;
}

// 行を追加する関数
void editorAppendRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) {
        return;
    }

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(sizeof(char) * (len + 1));
    // ファイルの中身をグローバル変数 (E.row[at].chars) に格納する処理の実装
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].render = NULL;
    E.row[at].rsize = 0;

    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorInsertNewLine() {
    if (E.cx == 0) {
        editorAppendRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorAppendRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        // editorAppendRow 内で realloc() が呼出されるので、E.row に割り当てられるアドレスが変更される可能性がある。
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

/* Row Operations */

// 文字を挿入する関数
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }
    row->chars = realloc(row->chars, sizeof(char) * (row->size + 2));
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->chars[at] = c;
    row->size++;
    editorUpdateRow(row);
    E.dirty++;
}

// 文字を削除刷る関数
void editorRowDeleteChar(erow *row, int at) {
    if (at < 0 || at > row->size) {
        return;
    }
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->chars);
    free(row->render);
}

void editorDeleteRow(int at) {
    if (at < 0 || at >= E.numrows) {
        return;
    }
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

/* Editor Operations */

// 文字の挿入とカーソルの移動
void editorInsertChar(int c) {
    if (E.cy ==  E.numrows) {
        editorAppendRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

// カーソルの右側にある文字を削除し、カーソルを移動する関数
void editorDeleteChar() {
    if (E.cy == E.numrows) {
        return;
    }
    if (E.cx == 0 && E.cy == 0) {
        return;
    }

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDeleteChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDeleteRow(E.cy);
        E.cy--;
    }
}

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoff = 0;
    E.coloff = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.dirty = 0;
    if (getWindowSize(&E.screenrows, &E.screencols) < 0) {
        die("getWindowSize");
    }
    E.screenrows -= 2;
}

int main(int argc, char **argv) {
    enableRauMode();
    initEditor();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q = quit | Ctrl-S = save");

    while (true) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return EXIT_SUCCESS;
}