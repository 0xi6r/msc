/*
 * snake.c – Professional Snake Game for Windows (Native API Edition)
 * Compile: cl /O2 /Fe:snake.exe snake.c /link ntdll.lib
 * (requires Visual Studio 2022 Developer PowerShell)
 *
 * The design leverages (Nt/Zw) functions directly from ntdll to avoid
 * higher-level Win32 where possible, resulting in a tighter, lower-latency game.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <winternl.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winioctl.h>


typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    } u;
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

// ---------------------------------------------------------------------------
//  Native API function prototypes (resolved at runtime)
// ---------------------------------------------------------------------------
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// Console I/O
typedef NTSTATUS (NTAPI *NtWriteFile_t)(
    HANDLE FileHandle, HANDLE Event, VOID *ApcRoutine, PVOID ApcContext,
    PVOID IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

typedef NTSTATUS (NTAPI *NtReadFile_t)(
    HANDLE FileHandle, HANDLE Event, VOID *ApcRoutine, PVOID ApcContext,
    PVOID IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

// Synchronization & timing
typedef NTSTATUS (NTAPI *NtWaitForSingleObject_t)(
    HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
typedef NTSTATUS (NTAPI *NtDelayExecution_t)(
    BOOLEAN Alertable, PLARGE_INTEGER Interval);

// Console mode IOCTLs (we send them directly to the console device)
typedef NTSTATUS (NTAPI *NtDeviceIoControlFile_t)(
    HANDLE FileHandle, HANDLE Event, VOID *ApcRoutine, PVOID ApcContext,
    PVOID IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength);

// Random number generation (RtlRandomEx uses a per-thread seed)
typedef ULONG (NTAPI *RtlRandomEx_t)(PULONG Seed);

// ---------------------------------------------------------------------------
//  Globals – resolved function pointers
// ---------------------------------------------------------------------------
static NtWriteFile_t              pNtWriteFile;
static NtReadFile_t               pNtReadFile;
static NtWaitForSingleObject_t    pNtWaitForSingleObject;
static NtDelayExecution_t         pNtDelayExecution;
static NtDeviceIoControlFile_t    pNtDeviceIoControlFile;
static RtlRandomEx_t              pRtlRandomEx;

static HANDLE hStdIn, hStdOut;

// ---------------------------------------------------------------------------
//  Game constants
// ---------------------------------------------------------------------------
#define GRID_WIDTH   40
#define GRID_HEIGHT  20
#define INITIAL_LENGTH 3

// Offsets to place the grid inside the terminal (so it looks centred)
#define GRID_X 5
#define GRID_Y 2

// Speed: microseconds per frame (100000 us = 100 ms = 10 fps)
#define TICK_US     100000ULL

// ---------------------------------------------------------------------------
//  Vector2 – simple coordinate
// ---------------------------------------------------------------------------
typedef struct { int x, y; } Vec2;

// ---------------------------------------------------------------------------
//  Snake state
// ---------------------------------------------------------------------------
static Vec2  snake_body[GRID_WIDTH * GRID_HEIGHT];  // circular buffer
static int   head_idx;          // index of head in snake_body
static int   tail_idx;          // index of tail
static int   snake_len;
static int   grow_pending;      // how many segments to add next tick
static Vec2  food_pos;
static Vec2  direction;         // (0,0) initially frozen
static bool  game_over;

static int   score;
static ULONG random_seed;       // seed for RtlRandomEx

// ---------------------------------------------------------------------------
//  Double‑buffer for screen updates (avoids rewriting identical cells)
// ---------------------------------------------------------------------------
static char screen[GRID_HEIGHT][GRID_WIDTH];
static char screen_prev[GRID_HEIGHT][GRID_WIDTH];

// ANSI buffer – we collect multiple writes into one NtWriteFile call
static char out_buf[4096];
static int  out_len;

// ---------------------------------------------------------------------------
//  Forward declarations
// ---------------------------------------------------------------------------
static void init_native_functions(void);
static void enable_virtual_terminal(void);
static void restore_console_mode(void);
static void write_out(const char* str);
static void flush_out(void);
static void draw_screen(void);
static void draw_cell(int y, int x, char ch);
static void clear_screen(void);
static void hide_cursor(void);
static void show_cursor(void);
static void read_input(void);
static void update_game(void);
static void place_food(void);
static void game_over_cleanup(void);

// ---------------------------------------------------------------------------
//  Entry point
// ---------------------------------------------------------------------------
int main(void) {
    // Resolve Native API pointers once
    init_native_functions();

    hStdIn  = GetStdHandle(STD_INPUT_HANDLE);
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // Enable ANSI escape sequence processing (both input & output)
    enable_virtual_terminal();
    atexit(restore_console_mode);   // best‑effort cleanup

    hide_cursor();

    // Initial snake – centred, waiting for the first move
    snake_len    = INITIAL_LENGTH;
    grow_pending = 0;
    direction.x  = 0;  direction.y = 0;
    game_over    = false;
    score        = 0;

    int start_x = GRID_WIDTH/2;
    int start_y = GRID_HEIGHT/2;
    for (int i = 0; i < snake_len; i++) {
        snake_body[i].x = start_x - i;
        snake_body[i].y = start_y;
    }
    head_idx = snake_len - 1;
    tail_idx = 0;

    // Initialize double buffer with spaces
    for (int y = 0; y < GRID_HEIGHT; y++)
        for (int x = 0; x < GRID_WIDTH; x++)
            screen_prev[y][x] = ' ';

    // Random seed from high‑res counter (via NtQueryPerformanceCounter)
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);          // one Win32 call for simplicity
    random_seed = (ULONG)(qpc.QuadPart & 0xFFFFFFFF);

    place_food();
    clear_screen();
    draw_screen();            // initial render

    // Game loop
    LARGE_INTEGER tick;
    tick.QuadPart = -(LONGLONG)(TICK_US * 10);  // relative 100‑ns units

    while (!game_over) {
        read_input();
        update_game();
        draw_screen();
        pNtDelayExecution(FALSE, &tick);
    }

    game_over_cleanup();
    return 0;
}

// ---------------------------------------------------------------------------
//  D y n a m i c   F u n c t i o n   R e s o l u t i o n
// ---------------------------------------------------------------------------
static void init_native_functions(void) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");   // unavoidable Win32 call
    #define RESOLVE(fn) p##fn = (fn##_t)GetProcAddress(ntdll, #fn)
    RESOLVE(NtWriteFile);
    RESOLVE(NtReadFile);
    RESOLVE(NtWaitForSingleObject);
    RESOLVE(NtDelayExecution);
    RESOLVE(NtDeviceIoControlFile);
    RESOLVE(RtlRandomEx);
    #undef RESOLVE
}

// ---------------------------------------------------------------------------
//  C o n s o l e   M o d e   S e t u p
// ---------------------------------------------------------------------------
static DWORD original_out_mode = 0;
static DWORD original_in_mode  = 0;

static void enable_virtual_terminal(void) {
    DWORD mode;

    if (GetConsoleMode(hStdOut, &mode)) {
        original_out_mode = mode;
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hStdOut, mode);
    }

    if (GetConsoleMode(hStdIn, &mode)) {
        original_in_mode = mode;
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        mode |= ENABLE_PROCESSED_INPUT;
        SetConsoleMode(hStdIn, mode);
    }
}

static void restore_console_mode(void) {
    if (original_out_mode) {
        SetConsoleMode(hStdOut, original_out_mode);
    }
    if (original_in_mode) {
        SetConsoleMode(hStdIn, original_in_mode);
    }
}

// ---------------------------------------------------------------------------
//  O u t p u t   H e l p e r s   (draining into out_buf)
// ---------------------------------------------------------------------------
static void write_out(const char* str) {
    int len = (int)strlen(str);
    if (out_len + len > (int)sizeof(out_buf)-1) flush_out();
    memcpy(out_buf + out_len, str, len);
    out_len += len;
}

static void flush_out(void) {
    if (out_len == 0) return;
    IO_STATUS_BLOCK iosb;
    pNtWriteFile(hStdOut, NULL, NULL, NULL, &iosb,
                 out_buf, out_len, NULL, NULL);
    out_len = 0;
}

static void hide_cursor(void) { write_out("\x1b[?25l"); flush_out(); }
static void show_cursor(void) { write_out("\x1b[?25h"); flush_out(); }

static void clear_screen(void) {
    write_out("\x1b[2J\x1b[H");   // clear entire screen, cursor home
    // fill internal buffers with spaces so diff logic works correctly
    for (int y = 0; y < GRID_HEIGHT; y++)
        for (int x = 0; x < GRID_WIDTH; x++) {
            screen[y][x] = ' ';
            screen_prev[y][x] = ' ';
        }
    flush_out();
}

// Move cursor to (y,x) inside the playing field (0‑origin)
static void move_cursor(int y, int x) {
    // ANSI row;col are 1‑based
    char buf[32];
    sprintf(buf, "\x1b[%d;%dH", GRID_Y + y + 1, GRID_X + x + 1);
    write_out(buf);
}

static void draw_cell(int y, int x, char ch) {
    if (screen[y][x] == ch) return;   // already correct
    move_cursor(y, x);
    char str[2] = { ch, 0 };
    write_out(str);
    screen[y][x] = ch;
}

// ---------------------------------------------------------------------------
//  R e n d e r i n g
// ---------------------------------------------------------------------------
static void draw_border(void) {
    // Corners & horizontal lines (we draw them every frame in the diff logic,
    // but they rarely change; the diff function will skip identical cells)
    for (int y = 0; y < GRID_HEIGHT; y++) {
        draw_cell(y, 0, '|');
        draw_cell(y, GRID_WIDTH-1, '|');
    }
    for (int x = 0; x < GRID_WIDTH; x++) {
        draw_cell(0, x, '-');
        draw_cell(GRID_HEIGHT-1, x, '-');
    }
    draw_cell(0, 0, '+');
    draw_cell(0, GRID_WIDTH-1, '+');
    draw_cell(GRID_HEIGHT-1, 0, '+');
    draw_cell(GRID_HEIGHT-1, GRID_WIDTH-1, '+');
}

static void draw_score(void) {
    move_cursor(-2, 0);   // two lines above grid
    char buf[64];
    sprintf(buf, "Score: %d  ", score);
    // pad to overwrite previous number
    for (int i = (int)strlen(buf); i < 20; i++) buf[i] = ' ';
    buf[20] = '\0';
    write_out(buf);
}

static void draw_screen(void) {
    for (int y = 0; y < GRID_HEIGHT; y++)
        for (int x = 0; x < GRID_WIDTH; x++)
            screen[y][x] = ' ';

    // 1) erase previous snake cells that are no longer occupied
    for (int y = 0; y < GRID_HEIGHT; y++)
        for (int x = 0; x < GRID_WIDTH; x++) {
            if (screen_prev[y][x] != ' ' && screen[y][x] == ' ')
                draw_cell(y, x, ' ');
        }

    // 2) draw current snake (head = 'O', body = 'o')
    {
        int idx = head_idx;
        for (int i = 0; i < snake_len; i++) {
            Vec2 s = snake_body[(idx - i + GRID_WIDTH*GRID_HEIGHT) % (GRID_WIDTH*GRID_HEIGHT)];
            if (s.x < 0 || s.x >= GRID_WIDTH || s.y < 0 || s.y >= GRID_HEIGHT) continue;
            char ch = (i == 0) ? 'O' : 'o';
            draw_cell(s.y, s.x, ch);
        }
    }

    // 3) draw food
    draw_cell(food_pos.y, food_pos.x, '*');

    // 4) ensure border
    draw_border();
    draw_score();

    // copy current screen to previous buffer
    memcpy(screen_prev, screen, sizeof(screen));
    flush_out();
}

// ---------------------------------------------------------------------------
//  I n p u t   (non‑blocking, WASD + arrow keys)
// ---------------------------------------------------------------------------
static void read_input(void) {
    while (_kbhit()) {
        int ch = _getch();
        switch (ch) {
            case 'w': case 'W': if (direction.y != 1) { direction.x = 0; direction.y = -1; } break;
            case 's': case 'S': if (direction.y != -1){ direction.x = 0; direction.y = 1;  } break;
            case 'a': case 'A': if (direction.x != 1) { direction.x = -1; direction.y = 0; } break;
            case 'd': case 'D': if (direction.x != -1){ direction.x = 1; direction.y = 0; } break;
            case 27: game_over = true; break;
            case 0:
            case 0xE0: {
                int ext = _getch();
                switch (ext) {
                    case 72: if (direction.y != 1) { direction.x = 0; direction.y = -1; } break;
                    case 80: if (direction.y != -1){ direction.x = 0; direction.y = 1;  } break;
                    case 75: if (direction.x != 1) { direction.x = -1; direction.y = 0; } break;
                    case 77: if (direction.x != -1){ direction.x = 1; direction.y = 0; } break;
                }
                break;
            }
        }

        if (game_over) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
//  G a m e   L o g i c
// ---------------------------------------------------------------------------
static void place_food(void) {
    // Simple rejection sampling; infinite loop avoided by grid size guaranteed to have space
    do {
        food_pos.x = 1 + (int)(pRtlRandomEx(&random_seed) % (GRID_WIDTH - 2));
        food_pos.y = 1 + (int)(pRtlRandomEx(&random_seed) % (GRID_HEIGHT - 2));
        // Ensure not on snake
        bool on_snake = false;
        int idx = head_idx;
        for (int i = 0; i < snake_len; i++) {
            Vec2 s = snake_body[(idx - i + GRID_WIDTH*GRID_HEIGHT) % (GRID_WIDTH*GRID_HEIGHT)];
            if (s.x == food_pos.x && s.y == food_pos.y) { on_snake = true; break; }
        }
        if (!on_snake) break;
    } while (1);
}

static void update_game(void) {
    if (direction.x == 0 && direction.y == 0) return;   // not started

    // Compute new head position
    Vec2 new_head = snake_body[head_idx];
    new_head.x += direction.x;
    new_head.y += direction.y;

    // Collision with walls
    if (new_head.x <= 0 || new_head.x >= GRID_WIDTH - 1 ||
        new_head.y <= 0 || new_head.y >= GRID_HEIGHT - 1) {
        game_over = true;
        return;
    }

    // Collision with self (ignore tail because it will move away, unless we are growing)
    {
        int check_len = grow_pending ? snake_len : snake_len - 1;
        int idx = head_idx;
        for (int i = 0; i < check_len; i++) {
            Vec2 s = snake_body[(idx - i + GRID_WIDTH*GRID_HEIGHT) % (GRID_WIDTH*GRID_HEIGHT)];
            if (s.x == new_head.x && s.y == new_head.y) {
                game_over = true;
                return;
            }
        }
    }

    // Advance head (circular buffer)
    head_idx = (head_idx + 1) % (GRID_WIDTH * GRID_HEIGHT);
    snake_body[head_idx] = new_head;

    if (grow_pending > 0) {
        snake_len++;
        grow_pending--;
    } else {
        // move tail forward (erase the last segment)
        tail_idx = (tail_idx + 1) % (GRID_WIDTH * GRID_HEIGHT);
        // Mark old tail position as empty in our render state
        // (done implicitly by draw_screen’s diff logic)
    }

    // Food check
    if (new_head.x == food_pos.x && new_head.y == food_pos.y) {
        score += 10;
        grow_pending += 3;
        place_food();
    }
}

// ---------------------------------------------------------------------------
//  G a m e   O v e r
// ---------------------------------------------------------------------------
static void game_over_cleanup(void) {
    clear_screen();
    move_cursor(GRID_HEIGHT/2, GRID_WIDTH/2 - 6);
    write_out("*** GAME OVER ***");
    move_cursor(GRID_HEIGHT/2 + 1, GRID_WIDTH/2 - 8);
    char buf[64];
    sprintf(buf, "Final Score: %d", score);
    write_out(buf);
    flush_out();
    show_cursor();
    // Let user read before exit (simple wait via NtDelayExecution)
    LARGE_INTEGER delay;
    delay.QuadPart = -30000000; // 3 seconds in 100-ns units
    pNtDelayExecution(FALSE, &delay);
}
