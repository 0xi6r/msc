#define _CRT_SECURE_NO_WARNINGS

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#define CTRL_KEY(k) ((k) & 0x1f)

#define EDITOR_VERSION "0.1"
#define EDITOR_TAB_STOP 4
#define EDITOR_QUIT_TIMES 2
#define EDITOR_STATUS_SIZE 160

enum editorKey {
    BACKSPACE_KEY = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorMode {
    MODE_NORMAL = 0,
    MODE_INSERT
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

struct editorSyntax {
    const char *filetype;
    const char **filematch;
    const char **keywords;
    const char *singleline_comment_start;
    const char *multiline_comment_start;
    const char *multiline_comment_end;
    int flags;
};

struct editorConfig {
    int cx;
    int cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int textrows;
    int textcols;
    int numrows;
    int dirty;
    int quit_times;
    int mode;
    int pending_g;
    erow *row;
    char *filename;
    char statusmsg[EDITOR_STATUS_SIZE];
    DWORD statusmsg_time;
    struct editorSyntax *syntax;
    HANDLE heap;
    HANDLE hstdin;
    HANDLE hstdout;
    DWORD original_in_mode;
    DWORD original_out_mode;
} E;

static const char *C_HL_extensions[] = {
    ".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".java", ".js", ".ts", NULL
};

static const char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "short|", "const|", "volatile|", "size_t|", "ssize_t|",
    "uint8_t|", "uint16_t|", "uint32_t|", "uint64_t|", "int8_t|",
    "int16_t|", "int32_t|", "int64_t|", "bool|", NULL
};

static struct editorSyntax HLDB[] = {
    {
        "c-family",
        C_HL_extensions,
        C_HL_keywords,
        "//",
        "/*",
        "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    }
};

#define HLDB_ENTRIES ((int)(sizeof(HLDB) / sizeof(HLDB[0])))

static void editorDie(const char *message);
static void editorRefreshScreen(void);

static void *editorAlloc(size_t size)
{
    void *ptr;

    if (size == 0) {
        size = 1;
    }

    ptr = HeapAlloc(E.heap, HEAP_ZERO_MEMORY, size);
    if (ptr == NULL) {
        editorDie("HeapAlloc");
    }

    return ptr;
}

static void *editorRealloc(void *ptr, size_t size)
{
    void *new_ptr;

    if (size == 0) {
        size = 1;
    }

    if (ptr == NULL) {
        return editorAlloc(size);
    }

    new_ptr = HeapReAlloc(E.heap, HEAP_ZERO_MEMORY, ptr, size);
    if (new_ptr == NULL) {
        editorDie("HeapReAlloc");
    }

    return new_ptr;
}

static void editorFree(void *ptr)
{
    if (ptr != NULL) {
        HeapFree(E.heap, 0, ptr);
    }
}

static char *editorStrdup(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = editorAlloc(length);
    memcpy(copy, text, length);
    return copy;
}

static void editorSetStatusMessage(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, args);
    va_end(args);
    E.statusmsg_time = GetTickCount();
}

static int editorIsSeparator(int c)
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];{}", c) != NULL;
}

static WORD editorSyntaxToColor(int hl)
{
    switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT:
        return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case HL_KEYWORD1:
        return FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case HL_KEYWORD2:
        return FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_INTENSITY;
    case HL_STRING:
        return FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY;
    case HL_NUMBER:
        return FOREGROUND_BLUE | FOREGROUND_RED;
    case HL_MATCH:
        return BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_INTENSITY |
               FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    default:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}

static void editorSelectSyntaxHighlight(void);

static int editorRowCxToRx(const erow *row, int cx)
{
    int j;
    int rx = 0;

    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
        }
        rx++;
    }

    return rx;
}

static int editorRowRxToCx(const erow *row, int rx)
{
    int cur_rx = 0;
    int cx;

    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (EDITOR_TAB_STOP - 1) - (cur_rx % EDITOR_TAB_STOP);
        }
        cur_rx++;

        if (cur_rx > rx) {
            return cx;
        }
    }

    return cx;
}

static void editorUpdateSyntax(erow *row)
{
    int i;
    int prev_sep = 1;
    int in_string = 0;
    int in_comment;
    const char **keywords;
    const char *scs;
    const char *mcs;
    const char *mce;
    int scs_len;
    int mcs_len;
    int mce_len;

    row->hl = editorRealloc(row->hl, (size_t)row->rsize);
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    if (E.syntax == NULL) {
        row->hl_open_comment = 0;
        return;
    }

    keywords = E.syntax->keywords;
    scs = E.syntax->singleline_comment_start;
    mcs = E.syntax->multiline_comment_start;
    mce = E.syntax->multiline_comment_end;
    scs_len = scs ? (int)strlen(scs) : 0;
    mcs_len = mcs ? (int)strlen(mcs) : 0;
    mce_len = mce ? (int)strlen(mce) : 0;
    in_comment = (row > E.row && (row - 1)->hl_open_comment);

    i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, (size_t)scs_len)) {
                memset(&row->hl[i], HL_COMMENT, (size_t)(row->rsize - i));
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, (size_t)mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, (size_t)mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                i++;
                continue;
            }

            if (!strncmp(&row->render[i], mcs, (size_t)mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, (size_t)mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    prev_sep = 1;
                    continue;
                }
                if (c == in_string) {
                    in_string = 0;
                }
                i++;
                prev_sep = 1;
                continue;
            }

            if (c == '"' || c == '\'') {
                in_string = c;
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit((unsigned char)c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int j;

            for (j = 0; keywords[j] != NULL; j++) {
                int klen = (int)strlen(keywords[j]);
                int kw2 = (keywords[j][klen - 1] == '|');

                if (kw2) {
                    klen--;
                }

                if (!strncmp(&row->render[i], keywords[j], (size_t)klen) &&
                    editorIsSeparator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, (size_t)klen);
                    i += klen;
                    break;
                }
            }

            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = editorIsSeparator(c);
        i++;
    }

    {
        int changed = (row->hl_open_comment != in_comment);
        row->hl_open_comment = in_comment;
        if (changed && row + 1 < E.row + E.numrows) {
            editorUpdateSyntax(row + 1);
        }
    }
}

static void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j;
    int idx = 0;

    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    editorFree(row->render);
    row->render = editorAlloc((size_t)(row->size + tabs * (EDITOR_TAB_STOP - 1) + 1));

    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % EDITOR_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
    editorUpdateSyntax(row);
}

static void editorInsertRow(int at, const char *s, size_t len)
{
    erow *row;

    if (at < 0 || at > E.numrows) {
        return;
    }

    E.row = editorRealloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (size_t)(E.numrows - at));

    row = &E.row[at];
    row->size = (int)len;
    row->chars = editorAlloc(len + 1);
    memcpy(row->chars, s, len);
    row->chars[len] = '\0';
    row->rsize = 0;
    row->render = NULL;
    row->hl = NULL;
    row->hl_open_comment = 0;
    editorUpdateRow(row);
    E.numrows++;
    E.dirty++;
}

static void editorFreeRow(erow *row)
{
    editorFree(row->render);
    editorFree(row->chars);
    editorFree(row->hl);
}

static void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows) {
        return;
    }

    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (size_t)(E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

static void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size) {
        at = row->size;
    }

    row->chars = editorRealloc(row->chars, (size_t)row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at + 1));
    row->size++;
    row->chars[at] = (char)c;
    editorUpdateRow(row);
    E.dirty++;
}

static void editorRowAppendString(erow *row, const char *s, size_t len)
{
    row->chars = editorRealloc(row->chars, (size_t)row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

static void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size) {
        return;
    }

    memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

static void editorInsertChar(int c)
{
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

static void editorInsertNewline(void)
{
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], (size_t)(row->size - E.cx));
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }

    E.cy++;
    E.cx = 0;
}

static void editorDelChar(void)
{
    erow *row;

    if (E.cy == E.numrows) {
        return;
    }

    if (E.cx == 0 && E.cy == 0) {
        return;
    }

    row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, (size_t)row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

static void editorOpen(const char *filename)
{
    HANDLE file;
    LARGE_INTEGER file_size;
    char *buffer;
    DWORD bytes_read;
    DWORD total_size;
    char *line_start;
    char *cursor;

    editorFree(E.filename);
    E.filename = editorStrdup(filename);
    editorSelectSyntaxHighlight();

    file = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            E.dirty = 0;
            return;
        }
        editorDie("CreateFileA");
    }

    if (!GetFileSizeEx(file, &file_size)) {
        CloseHandle(file);
        editorDie("GetFileSizeEx");
    }

    if (file_size.QuadPart > 0x7ffffffe) {
        CloseHandle(file);
        editorDie("File too large for this build");
    }

    total_size = (DWORD)file_size.QuadPart;
    buffer = editorAlloc((size_t)total_size + 1);

    if (total_size > 0 && !ReadFile(file, buffer, total_size, &bytes_read, NULL)) {
        editorFree(buffer);
        CloseHandle(file);
        editorDie("ReadFile");
    }

    CloseHandle(file);
    buffer[total_size] = '\0';

    line_start = buffer;
    cursor = buffer;
    while (*cursor != '\0') {
        if (*cursor == '\n') {
            size_t len = (size_t)(cursor - line_start);
            if (len > 0 && line_start[len - 1] == '\r') {
                len--;
            }
            editorInsertRow(E.numrows, line_start, len);
            line_start = cursor + 1;
        }
        cursor++;
    }

    if (cursor != line_start) {
        size_t len = (size_t)(cursor - line_start);
        if (len > 0 && line_start[len - 1] == '\r') {
            len--;
        }
        editorInsertRow(E.numrows, line_start, len);
    }

    editorFree(buffer);
    E.dirty = 0;
}

static char *editorPrompt(const char *prompt)
{
    size_t bufsize = 128;
    size_t buflen = 0;
    char *buffer = editorAlloc(bufsize);

    buffer[0] = '\0';

    for (;;) {
        int c;

        editorSetStatusMessage(prompt, buffer);

        {
            INPUT_RECORD record;
            DWORD count;

            for (;;) {
                if (!ReadConsoleInputA(E.hstdin, &record, 1, &count)) {
                    editorDie("ReadConsoleInputA");
                }

                if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                    CONSOLE_SCREEN_BUFFER_INFO info;
                    if (GetConsoleScreenBufferInfo(E.hstdout, &info)) {
                        E.screencols = info.srWindow.Right - info.srWindow.Left + 1;
                        E.screenrows = info.srWindow.Bottom - info.srWindow.Top + 1;
                        E.textrows = E.screenrows - 2;
                    }
                    continue;
                }

                if (record.EventType == KEY_EVENT &&
                    record.Event.KeyEvent.bKeyDown) {
                    KEY_EVENT_RECORD key = record.Event.KeyEvent;
                    if (key.wVirtualKeyCode == VK_ESCAPE) {
                        c = 27;
                    } else if (key.wVirtualKeyCode == VK_BACK) {
                        c = BACKSPACE_KEY;
                    } else if (key.wVirtualKeyCode == VK_RETURN) {
                        c = '\r';
                    } else {
                        c = key.uChar.AsciiChar;
                    }
                    break;
                }
            }
        }

        if (c == 27) {
            editorSetStatusMessage("");
            editorFree(buffer);
            return NULL;
        }

        if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                return buffer;
            }
        } else if (c == BACKSPACE_KEY) {
            if (buflen != 0) {
                buffer[--buflen] = '\0';
            }
        } else if (!iscntrl((unsigned char)c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buffer = editorRealloc(buffer, bufsize);
            }
            buffer[buflen++] = (char)c;
            buffer[buflen] = '\0';
        }

        {
            int saved_mode = E.mode;
            E.mode = MODE_NORMAL;
            editorRefreshScreen();
            E.mode = saved_mode;
        }
    }
}

static void editorSave(void)
{
    HANDLE file;
    DWORD bytes_written;
    int i;
    DWORD total = 0;

    if (E.filename == NULL) {
        char *name = editorPrompt("Save as: %s (Enter to confirm, Esc to cancel)");
        if (name == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        E.filename = name;
        editorSelectSyntaxHighlight();
    }

    file = CreateFileA(E.filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        editorSetStatusMessage("Save failed: %lu", GetLastError());
        return;
    }

    for (i = 0; i < E.numrows; i++) {
        erow *row = &E.row[i];
        DWORD line_length = (DWORD)row->size;
        if (line_length > 0 &&
            !WriteFile(file, row->chars, line_length, &bytes_written, NULL)) {
            CloseHandle(file);
            editorSetStatusMessage("Save failed: %lu", GetLastError());
            return;
        }
        total += line_length;

        if (!WriteFile(file, "\r\n", 2, &bytes_written, NULL)) {
            CloseHandle(file);
            editorSetStatusMessage("Save failed: %lu", GetLastError());
            return;
        }
        total += 2;
    }

    CloseHandle(file);
    E.dirty = 0;
    editorSetStatusMessage("%lu bytes written to disk", total);
}

static int editorRowsDigits(void)
{
    int digits = 1;
    int lines = (E.numrows > 0) ? E.numrows : 1;

    while (lines >= 10) {
        digits++;
        lines /= 10;
    }

    return digits;
}

static void editorScroll(void)
{
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.textrows) {
        E.rowoff = E.cy - E.textrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.textcols) {
        E.coloff = E.rx - E.textcols + 1;
    }
}

static void editorClearBuffer(CHAR_INFO *buffer)
{
    int count = E.screencols * E.screenrows;
    int i;

    for (i = 0; i < count; i++) {
        buffer[i].Char.AsciiChar = ' ';
        buffer[i].Attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}

static void editorDrawTextRows(CHAR_INFO *buffer)
{
    int y;
    int gutter_width = editorRowsDigits() + 2;

    E.textcols = E.screencols - gutter_width;
    if (E.textcols < 8) {
        E.textcols = 8;
    }

    for (y = 0; y < E.textrows; y++) {
        int filerow = y + E.rowoff;
        int x;
        CHAR_INFO *line = &buffer[y * E.screencols];

        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.textrows / 3) {
                char welcome[80];
                int len = snprintf(welcome, sizeof(welcome),
                                   "6r editor %s | i: insert | Ctrl-S: save | Ctrl-Q: quit",
                                   EDITOR_VERSION);
                if (len < 0) {
                    len = 0;
                }
                if (len > E.textcols) {
                    len = E.textcols;
                }

                line[0].Char.AsciiChar = '~';
                line[0].Attributes = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                for (x = 0; x < len; x++) {
                    int col = gutter_width + x;
                    if (col < E.screencols) {
                        line[col].Char.AsciiChar = welcome[x];
                    }
                }
            } else {
                line[0].Char.AsciiChar = '~';
                line[0].Attributes = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            }
            continue;
        }

        {
            char lineno[32];
            int lineno_len = snprintf(lineno, sizeof(lineno), "%*d ",
                                      gutter_width - 1, filerow + 1);
            WORD gutter_color = (filerow == E.cy)
                ? (FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY)
                : (FOREGROUND_BLUE | FOREGROUND_INTENSITY);

            if (lineno_len < 0) {
                lineno_len = 0;
            }
            if (lineno_len > gutter_width) {
                lineno_len = gutter_width;
            }

            for (x = 0; x < lineno_len; x++) {
                line[x].Char.AsciiChar = lineno[x];
                line[x].Attributes = gutter_color;
            }
        }

        {
            erow *row = &E.row[filerow];
            int len = row->rsize - E.coloff;
            if (len < 0) {
                len = 0;
            }
            if (len > E.textcols) {
                len = E.textcols;
            }

            for (x = 0; x < len; x++) {
                int render_index = x + E.coloff;
                char c = row->render[render_index];
                WORD color = editorSyntaxToColor(row->hl[render_index]);
                int col = gutter_width + x;

                if ((unsigned char)c < 32) {
                    c = '?';
                    color = BACKGROUND_BLUE | FOREGROUND_RED |
                            FOREGROUND_GREEN | FOREGROUND_BLUE;
                }

                if (col >= E.screencols) {
                    break;
                }

                line[col].Char.AsciiChar = c;
                line[col].Attributes = color;
            }
        }
    }
}

static void editorDrawStatusBar(CHAR_INFO *buffer)
{
    int y = E.screenrows - 2;
    CHAR_INFO *line = &buffer[y * E.screencols];
    char status[EDITOR_STATUS_SIZE];
    char rstatus[EDITOR_STATUS_SIZE];
    const char *name = E.filename ? E.filename : "[No Name]";
    const char *mode = (E.mode == MODE_INSERT) ? "INSERT" : "NORMAL";
    int len = snprintf(status, sizeof(status), "%.48s - %d lines %s",
                       name, E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                        mode, E.cy + 1, E.numrows ? E.numrows : 1);
    int x;

    if (len < 0) {
        len = 0;
    }
    if (rlen < 0) {
        rlen = 0;
    }

    for (x = 0; x < E.screencols; x++) {
        line[x].Attributes = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
        line[x].Char.AsciiChar = ' ';
    }

    if (len > E.screencols) {
        len = E.screencols;
    }
    for (x = 0; x < len; x++) {
        line[x].Char.AsciiChar = status[x];
        line[x].Attributes |= FOREGROUND_BLUE;
    }

    if (rlen < E.screencols) {
        int start = E.screencols - rlen;
        for (x = 0; x < rlen; x++) {
            line[start + x].Char.AsciiChar = rstatus[x];
            line[start + x].Attributes |= FOREGROUND_BLUE;
        }
    }
}

static void editorDrawMessageBar(CHAR_INFO *buffer)
{
    int y = E.screenrows - 1;
    CHAR_INFO *line = &buffer[y * E.screencols];
    int len = (int)strlen(E.statusmsg);
    int x;

    for (x = 0; x < E.screencols; x++) {
        line[x].Attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        line[x].Char.AsciiChar = ' ';
    }

    if (len > E.screencols) {
        len = E.screencols;
    }

    if (len > 0 && GetTickCount() - E.statusmsg_time < 5000) {
        for (x = 0; x < len; x++) {
            line[x].Char.AsciiChar = E.statusmsg[x];
            line[x].Attributes = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        }
    }
}

void editorRefreshScreen(void)
{
    static CHAR_INFO *buffer = NULL;
    static size_t buffer_capacity = 0;
    CONSOLE_SCREEN_BUFFER_INFO info;
    COORD buffer_size;
    COORD buffer_coord = {0, 0};
    SMALL_RECT write_region;
    COORD cursor;
    size_t required;
    CONSOLE_CURSOR_INFO cursor_info;
    int gutter_width;

    if (!GetConsoleScreenBufferInfo(E.hstdout, &info)) {
        editorDie("GetConsoleScreenBufferInfo");
    }

    E.screencols = info.srWindow.Right - info.srWindow.Left + 1;
    E.screenrows = info.srWindow.Bottom - info.srWindow.Top + 1;
    E.textrows = E.screenrows - 2;
    if (E.textrows < 1) {
        E.textrows = 1;
    }

    editorScroll();

    required = (size_t)E.screencols * (size_t)E.screenrows;
    if (required > buffer_capacity) {
        buffer = editorRealloc(buffer, required * sizeof(CHAR_INFO));
        buffer_capacity = required;
    }

    editorClearBuffer(buffer);
    editorDrawTextRows(buffer);
    editorDrawStatusBar(buffer);
    editorDrawMessageBar(buffer);

    buffer_size.X = (SHORT)E.screencols;
    buffer_size.Y = (SHORT)E.screenrows;
    write_region.Left = 0;
    write_region.Top = 0;
    write_region.Right = (SHORT)(E.screencols - 1);
    write_region.Bottom = (SHORT)(E.screenrows - 1);

    if (!WriteConsoleOutputA(E.hstdout, buffer, buffer_size, buffer_coord, &write_region)) {
        editorDie("WriteConsoleOutputA");
    }

    gutter_width = editorRowsDigits() + 2;
    cursor.X = (SHORT)(gutter_width + (E.rx - E.coloff));
    cursor.Y = (SHORT)(E.cy - E.rowoff);

    if (cursor.X < gutter_width) {
        cursor.X = (SHORT)gutter_width;
    }
    if (cursor.X >= E.screencols) {
        cursor.X = (SHORT)(E.screencols - 1);
    }
    if (cursor.Y < 0) {
        cursor.Y = 0;
    }
    if (cursor.Y >= E.textrows) {
        cursor.Y = (SHORT)(E.textrows - 1);
    }

    cursor_info.dwSize = 20;
    cursor_info.bVisible = TRUE;
    SetConsoleCursorInfo(E.hstdout, &cursor_info);
    SetConsoleCursorPosition(E.hstdout, cursor);
}

static void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        E.pending_g = 0;
        break;
    case ARROW_RIGHT:
        if (row != NULL && E.cx < row->size) {
            E.cx++;
        } else if (row != NULL && E.cx == row->size && E.cy + 1 < E.numrows) {
            E.cy++;
            E.cx = 0;
        }
        E.pending_g = 0;
        break;
    case ARROW_UP:
        if (E.cy != 0) {
            E.cy--;
        }
        E.pending_g = 0;
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows) {
            E.cy++;
        }
        E.pending_g = 0;
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    if (row != NULL && E.cx > row->size) {
        E.cx = row->size;
    }
}

static void editorFind(void)
{
    char *query = editorPrompt("Search: %s (Enter to jump, Esc to cancel)");
    int i;

    if (query == NULL || query[0] == '\0') {
        editorFree(query);
        editorSetStatusMessage("Search canceled");
        return;
    }

    for (i = 0; i < E.numrows; i++) {
        erow *row = &E.row[i];
        char *match = strstr(row->render, query);
        if (match != NULL) {
            E.cy = i;
            E.cx = editorRowRxToCx(row, (int)(match - row->render));
            E.rowoff = E.numrows;
            E.coloff = 0;
            editorSetStatusMessage("Found \"%s\"", query);
            editorFree(query);
            return;
        }
    }

    editorSetStatusMessage("No match for \"%s\"", query);
    editorFree(query);
}

static void editorSelectSyntaxHighlight(void)
{
    int j;

    E.syntax = NULL;
    if (E.filename == NULL) {
        return;
    }

    for (j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *syntax = &HLDB[j];
        int i;

        for (i = 0; syntax->filematch[i] != NULL; i++) {
            const char *match = syntax->filematch[i];
            size_t len = strlen(match);
            size_t name_len = strlen(E.filename);

            if (name_len >= len &&
                strcmp(E.filename + name_len - len, match) == 0) {
                int filerow;
                E.syntax = syntax;
                for (filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
        }
    }
}

static int editorReadKey(void)
{
    INPUT_RECORD record;
    DWORD count;

    for (;;) {
        if (!ReadConsoleInputA(E.hstdin, &record, 1, &count)) {
            editorDie("ReadConsoleInputA");
        }

        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            editorRefreshScreen();
            continue;
        }

        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
            continue;
        }

        {
            KEY_EVENT_RECORD key = record.Event.KeyEvent;
            char c = key.uChar.AsciiChar;

            switch (key.wVirtualKeyCode) {
            case VK_LEFT:
                return ARROW_LEFT;
            case VK_RIGHT:
                return ARROW_RIGHT;
            case VK_UP:
                return ARROW_UP;
            case VK_DOWN:
                return ARROW_DOWN;
            case VK_DELETE:
                return DEL_KEY;
            case VK_HOME:
                return HOME_KEY;
            case VK_END:
                return END_KEY;
            case VK_PRIOR:
                return PAGE_UP;
            case VK_NEXT:
                return PAGE_DOWN;
            case VK_BACK:
                return BACKSPACE_KEY;
            case VK_RETURN:
                return '\r';
            case VK_ESCAPE:
                return 27;
            default:
                break;
            }

            if (c != 0) {
                return (unsigned char)c;
            }
        }
    }
}

static void editorProcessInsertMode(int c)
{
    switch (c) {
    case 27:
        E.mode = MODE_NORMAL;
        E.pending_g = 0;
        editorSetStatusMessage("-- NORMAL --");
        break;
    case CTRL_KEY('q'):
        if (E.dirty && E.quit_times > 0) {
            editorSetStatusMessage("Unsaved changes. Press Ctrl-Q %d more time%s to quit.",
                                   E.quit_times,
                                   E.quit_times == 1 ? "" : "s");
            E.quit_times--;
            return;
        }
        ExitProcess(0);
    case CTRL_KEY('s'):
        editorSave();
        break;
    case CTRL_KEY('f'):
        editorFind();
        break;
    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if (E.cy < E.numrows) {
            E.cx = E.row[E.cy].size;
        }
        break;
    case PAGE_UP:
    case PAGE_DOWN:
    {
        int times = E.textrows;
        if (c == PAGE_UP) {
            E.cy = E.rowoff;
        } else {
            E.cy = E.rowoff + E.textrows - 1;
            if (E.cy > E.numrows) {
                E.cy = E.numrows;
            }
        }
        while (times--) {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    }
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    case DEL_KEY:
        editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;
    case BACKSPACE_KEY:
    case CTRL_KEY('h'):
        editorDelChar();
        break;
    case '\r':
        editorInsertNewline();
        break;
    default:
        if (!iscntrl((unsigned char)c) && c < 128) {
            editorInsertChar(c);
        }
        break;
    }
}

static void editorProcessNormalMode(int c)
{
    switch (c) {
    case CTRL_KEY('q'):
        if (E.dirty && E.quit_times > 0) {
            editorSetStatusMessage("Unsaved changes. Press Ctrl-Q %d more time%s to quit.",
                                   E.quit_times,
                                   E.quit_times == 1 ? "" : "s");
            E.quit_times--;
            return;
        }
        ExitProcess(0);
    case CTRL_KEY('s'):
        editorSave();
        break;
    case CTRL_KEY('f'):
        editorFind();
        break;
    case 'i':
        E.mode = MODE_INSERT;
        E.pending_g = 0;
        editorSetStatusMessage("-- INSERT --");
        break;
    case 'a':
        if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
            E.cx++;
        }
        E.mode = MODE_INSERT;
        E.pending_g = 0;
        editorSetStatusMessage("-- INSERT --");
        break;
    case 'A':
        if (E.cy < E.numrows) {
            E.cx = E.row[E.cy].size;
        }
        E.mode = MODE_INSERT;
        E.pending_g = 0;
        editorSetStatusMessage("-- INSERT --");
        break;
    case 'o':
        if (E.cy >= E.numrows) {
            editorInsertRow(E.numrows, "", 0);
            E.cy = E.numrows - 1;
        } else {
            editorInsertRow(E.cy + 1, "", 0);
            E.cy++;
        }
        E.cx = 0;
        E.mode = MODE_INSERT;
        E.pending_g = 0;
        editorSetStatusMessage("-- INSERT --");
        break;
    case 'O':
        if (E.cy < E.numrows) {
            editorInsertRow(E.cy, "", 0);
        } else {
            editorInsertRow(E.numrows, "", 0);
            E.cy = E.numrows - 1;
        }
        E.cx = 0;
        E.mode = MODE_INSERT;
        E.pending_g = 0;
        editorSetStatusMessage("-- INSERT --");
        break;
    case 'x':
        if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
            E.cx++;
            editorDelChar();
        }
        E.pending_g = 0;
        break;
    case 'h':
        editorMoveCursor(ARROW_LEFT);
        break;
    case 'j':
        editorMoveCursor(ARROW_DOWN);
        break;
    case 'k':
        editorMoveCursor(ARROW_UP);
        break;
    case 'l':
        editorMoveCursor(ARROW_RIGHT);
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    case PAGE_UP:
    case PAGE_DOWN:
    {
        int times = E.textrows;
        if (c == PAGE_UP) {
            E.cy = E.rowoff;
        } else {
            E.cy = E.rowoff + E.textrows - 1;
            if (E.cy > E.numrows) {
                E.cy = E.numrows;
            }
        }
        while (times--) {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    }
        break;
    case HOME_KEY:
    case '0':
        E.cx = 0;
        E.pending_g = 0;
        break;
    case END_KEY:
    case '$':
        if (E.cy < E.numrows) {
            E.cx = E.row[E.cy].size;
        }
        E.pending_g = 0;
        break;
    case 'G':
        E.cy = E.numrows ? E.numrows - 1 : 0;
        if (E.cy < E.numrows) {
            E.cx = E.row[E.cy].size;
        } else {
            E.cx = 0;
        }
        E.pending_g = 0;
        break;
    case 'g':
        if (E.pending_g) {
            E.cy = 0;
            E.cx = 0;
            E.pending_g = 0;
        } else {
            E.pending_g = 1;
            editorSetStatusMessage("g pressed, press g again for top of file");
        }
        break;
    default:
        E.pending_g = 0;
        break;
    }
}

static void editorProcessKeypress(void)
{
    int c = editorReadKey();

    if (c != CTRL_KEY('q')) {
        E.quit_times = EDITOR_QUIT_TIMES;
    }

    if (E.mode == MODE_INSERT) {
        editorProcessInsertMode(c);
    } else {
        editorProcessNormalMode(c);
    }
}

static void editorDisableRawMode(void)
{
    CONSOLE_CURSOR_INFO cursor_info;
    COORD home = {0, 0};
    DWORD written;
    CONSOLE_SCREEN_BUFFER_INFO info;

    SetConsoleMode(E.hstdin, E.original_in_mode);
    SetConsoleMode(E.hstdout, E.original_out_mode);

    if (GetConsoleScreenBufferInfo(E.hstdout, &info)) {
        DWORD cells = (DWORD)(info.dwSize.X * info.dwSize.Y);
        FillConsoleOutputCharacterA(E.hstdout, ' ', cells, home, &written);
        FillConsoleOutputAttribute(E.hstdout,
                                   FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
                                   cells, home, &written);
        SetConsoleCursorPosition(E.hstdout, home);
    }

    cursor_info.dwSize = 20;
    cursor_info.bVisible = TRUE;
    SetConsoleCursorInfo(E.hstdout, &cursor_info);
}

static void editorEnableRawMode(void)
{
    DWORD in_mode;
    DWORD out_mode;

    if (!GetConsoleMode(E.hstdin, &E.original_in_mode)) {
        editorDie("GetConsoleMode stdin");
    }
    if (!GetConsoleMode(E.hstdout, &E.original_out_mode)) {
        editorDie("GetConsoleMode stdout");
    }

    in_mode = E.original_in_mode;
    in_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    in_mode &= ~ENABLE_QUICK_EDIT_MODE;
    in_mode |= ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;

    out_mode = E.original_out_mode;

    if (!SetConsoleMode(E.hstdin, in_mode)) {
        editorDie("SetConsoleMode stdin");
    }
    if (!SetConsoleMode(E.hstdout, out_mode)) {
        editorDie("SetConsoleMode stdout");
    }
}

static void editorInit(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    memset(&E, 0, sizeof(E));
    E.heap = GetProcessHeap();
    E.hstdin = GetStdHandle(STD_INPUT_HANDLE);
    E.hstdout = GetStdHandle(STD_OUTPUT_HANDLE);
    E.quit_times = EDITOR_QUIT_TIMES;
    E.mode = MODE_NORMAL;

    if (E.hstdin == INVALID_HANDLE_VALUE || E.hstdout == INVALID_HANDLE_VALUE) {
        editorDie("GetStdHandle");
    }

    editorEnableRawMode();
    atexit(editorDisableRawMode);

    if (!GetConsoleScreenBufferInfo(E.hstdout, &info)) {
        editorDie("GetConsoleScreenBufferInfo");
    }

    E.screencols = info.srWindow.Right - info.srWindow.Left + 1;
    E.screenrows = info.srWindow.Bottom - info.srWindow.Top + 1;
    E.textrows = E.screenrows - 2;
    if (E.textrows < 1) {
        E.textrows = 1;
    }

    editorSetStatusMessage("-- NORMAL --");
}

static void editorDie(const char *message)
{
    DWORD error = GetLastError();
    char system_message[256] = {0};

    editorDisableRawMode();
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, error, 0, system_message, sizeof(system_message), NULL);
    fprintf(stderr, "%s: %s\n", message, system_message[0] ? system_message : "unknown error");
    ExitProcess(1);
}

int main(int argc, char **argv)
{
    int running = 1;

    editorInit();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (running) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
