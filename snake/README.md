# Snake

A simple console Snake game for Windows written in C.

## Build

Use a Visual Studio Developer PowerShell:

```powershell
cl /O2 /Fe:snake.exe snake.c /link ntdll.lib
```

## Run

```powershell
.\snake.exe
```

## Controls

- `W`, `A`, `S`, `D`
- Arrow keys
- `Esc` to quit

The snake starts still and begins moving when you press a direction key.

## How to Play

Eat the `*` food to grow and increase your score.

Avoid:

- The walls
- Your own body

When the snake crashes, the game shows the final score and exits after a short delay.

## How It Works

The game keeps the snake body as positions in a fixed-size array and updates the head and tail each frame. Food is placed at random inside the border, but never on the snake.

Rendering is done in the console with ANSI escape sequences. The program redraws the board, snake, food, and score in a timed loop, while keyboard input is checked every frame for direction changes.
