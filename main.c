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

void enableRauMode();
void disableRauMode();
void die(const char *msg);
char editorReadKey();
void editorProcessKeypress();
void editorRefreshScreen();
void editorDrawRows();
void initEditor();
int getCursorPosition(int *rows, int *cols);
void abAppend(abuf *ab, char *s, int len);
void abFree(abuf *ab);

struct editorConfig {
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct abuf {
    char *buf;
    int len;
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
char editorReadKey() {
    int nread;
    char c;
    // not= 1 にしないとｍマルチバイトに対応できない。
    // この処理がイマイチ納得いっていない。
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread < 0 && errno != EAGAIN) {
            die("read");
        }
    }
    return c;
}

void editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(EXIT_SUCCESS);
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
        // Welcome Messsage を描画
        if (y == E.screenrows / 3) {
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

        // この行削除のエスケープシーケンスを書き込むことで、画面を消して上書きで書き込むことができる。
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    abAppend(&ab, "\x1b[H", 3);

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

void initEditor() {
    if (getWindowSize(&E.screenrows, &E.screencols) < 0) {
        die("getWindowSize");
    }
}

int main(void) {
    enableRauMode();
    initEditor();

    while (true) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return EXIT_SUCCESS;
}
