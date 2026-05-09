#define _CRT_SECURE_NO_WARNINGS

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <lmcons.h>

#define INPUT_SIZE 1024

static char *trim_whitespace(char *text)
{
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return text;
}

static void print_current_directory(void)
{
    char buffer[MAX_PATH];
    DWORD length = GetCurrentDirectoryA((DWORD)sizeof(buffer), buffer);

    if (length == 0 || length >= sizeof(buffer)) {
        fprintf(stderr, "pwd: unable to get current directory\n");
        return;
    }

    printf("%s\n", buffer);
}

static void print_current_user(void)
{
    char username[UNLEN + 1];
    DWORD size = (DWORD)sizeof(username);

    if (!GetUserNameA(username, &size)) {
        fprintf(stderr, "whoami: unable to get current user\n");
        return;
    }

    printf("%s\n", username);
}

static void list_directory(const char *path)
{
    char search_path[MAX_PATH];
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    const char *target = path;

    if (target == NULL || *target == '\0') {
        target = ".";
    }

    if (_snprintf(search_path, sizeof(search_path), "%s\\*", target) < 0) {
        fprintf(stderr, "ls: path is too long\n");
        return;
    }

    handle = FindFirstFileA(search_path, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ls: cannot access '%s'\n", target);
        return;
    }

    do {
        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }

        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            printf("[DIR]  %s\n", entry.cFileName);
        } else {
            printf("       %s\n", entry.cFileName);
        }
    } while (FindNextFileA(handle, &entry) != 0);

    FindClose(handle);
}

static void change_directory(char *path)
{
    const char *target = path;

    if (target == NULL || *target == '\0') {
        target = getenv("USERPROFILE");
        if (target == NULL || *target == '\0') {
            target = "C:\\";
        }
    }

    if (!SetCurrentDirectoryA(target)) {
        fprintf(stderr, "cd: cannot change directory to '%s'\n", target);
    }
}

int main(void)
{
    char input[INPUT_SIZE];

    puts("Simple Windows Shell");
    puts("Commands: ls [path], pwd, whoami, cd [path], exit");

    for (;;) {
        char *command;
        char *args;

        printf("shell> ");
        if (fgets(input, sizeof(input), stdin) == NULL) {
            putchar('\n');
            break;
        }

        command = trim_whitespace(input);
        if (*command == '\0') {
            continue;
        }

        args = command;
        while (*args != '\0' && !isspace((unsigned char)*args)) {
            args++;
        }

        if (*args != '\0') {
            *args = '\0';
            args = trim_whitespace(args + 1);
        } else {
            args = command + strlen(command);
        }

        if (strcmp(command, "exit") == 0) {
            break;
        }

        if (strcmp(command, "pwd") == 0) {
            print_current_directory();
        } else if (strcmp(command, "whoami") == 0) {
            print_current_user();
        } else if (strcmp(command, "ls") == 0) {
            list_directory(args);
        } else if (strcmp(command, "cd") == 0) {
            change_directory(args);
        } else {
            fprintf(stderr, "Unknown command: %s\n", command);
        }
    }

    return 0;
}
