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

#define CTRL_KEY(value) ((value) & 0x1f)
#define ABUF_INIT {NULL, 0}
#define KILO_VERSION "0.0.1"

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
void editorAppendRow(char *s, size_t len);

struct erow {
    int size;
    char *chars;
};

struct editorConfig {
    int screenrows;
    int screencols;
    int cx;
    int cy;
    int numrows;
    erow *row;
    struct termios orig_termios;
};

struct abuf {
    char *buf;
    int len;
};

enum editorKey {
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

void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screencols - 1) {
                E.cx++;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenrows - 1) {
                E.cy++;
            }
            break;
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }
            break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(EXIT_SUCCESS);
            break;
        // 画面の左端か右端にカーソルを移動させる
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;
        // 画面の一番上か一番下のカーソルを移動させる
        case PAGE_UP:
        case PAGE_DOWN:
            {
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
    }
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
        if (y >= E.numrows) {
            // Welcome Messsage を描画
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcome_length = snprintf(welcome, sizeof(welcome), "Keditor -- version %s", KILO_VERSION);
                if (welcome_length > E.screencols) {
                    welcome_length = E.screencols;
                }
                int padding = (E.screencols - welcome_length) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                }
                while (padding--) {
                    abAppend(ab, " ", 1);
                }

                abAppend(ab, welcome, welcome_length);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[y].size > E.screencols ? E.screencols : E.row[y].size;
            abAppend(ab, E.row[y].chars, len);
        }

        // この行削除のエスケープシーケンスを書き込むことで、画面を消して上書きで書き込むことができる。
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
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

void editorOpen(char *filename) {
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
        editorAppendRow(line, linelen);
    }

    free(line);
    fclose(fp);
}

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(sizeof(char) * (len + 1));
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;
    if (getWindowSize(&E.screenrows, &E.screencols) < 0) {
        die("getWindowSize");
    }
}

int main(int argc, char **argv) {
    enableRauMode();
    initEditor();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (true) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return EXIT_SUCCESS;
}
