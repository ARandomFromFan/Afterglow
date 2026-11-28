/* The game folder's path, for the files the game keeps there (image.bin, romfs, saves, settings, caches, logs).
   On Windows the game is bin\afterglow.exe since the launcher came: afterglow.exe in the folder is the launcher (the builder's
   setup in launch mode), the game and its DLLs are in bin\, the game's files stay in the folder. hos_game_path_a/_w answer as
   GetModuleFileName(NULL, ...) does, but with the folder's path when the exe is in a "bin" folder beside image.bin, so code that
   strips the exe's name and adds a file's finds the folder's file. Modules (DLLs) are still loaded from beside the exe itself. */
#pragma once
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static inline DWORD hos_game_path_a(char *buf, DWORD size) {
    DWORD n = GetModuleFileNameA(NULL, buf, size);
    if (!n || n >= size) return n;
    char *name = n ? buf + n : buf;
    while (name > buf && name[-1] != '\\' && name[-1] != '/') name--;
    if (name - buf < 5) return n;
    char *dir = name - 1;   /* (the separator before the exe's name) */
    char *bin = dir;
    while (bin > buf && bin[-1] != '\\' && bin[-1] != '/') bin--;
    if (dir - bin != 3 || _strnicmp(bin, "bin", 3) != 0) return n;
    char probe[MAX_PATH + 16];
    const size_t root = (size_t)(bin - buf);   /* (the folder, with its separator) */
    if (root + 10 >= sizeof probe) return n;
    memcpy(probe, buf, root);
    strcpy(probe + root, "image.bin");
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) return n;
    memmove(buf + root, name, strlen(name) + 1);   /* (folder\afterglow.exe) */
    return (DWORD)strlen(buf);
}

static inline DWORD hos_game_path_w(wchar_t *buf, DWORD size) {
    DWORD n = GetModuleFileNameW(NULL, buf, size);
    if (!n || n >= size) return n;
    wchar_t *name = buf + n;
    while (name > buf && name[-1] != L'\\' && name[-1] != L'/') name--;
    if (name - buf < 5) return n;
    wchar_t *dir = name - 1;
    wchar_t *bin = dir;
    while (bin > buf && bin[-1] != L'\\' && bin[-1] != L'/') bin--;
    if (dir - bin != 3 || _wcsnicmp(bin, L"bin", 3) != 0) return n;
    wchar_t probe[MAX_PATH + 16];
    const size_t root = (size_t)(bin - buf);
    if (root + 10 >= sizeof probe / sizeof probe[0]) return n;
    wmemcpy(probe, buf, root);
    wcscpy(probe + root, L"image.bin");
    if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) return n;
    wmemmove(buf + root, name, wcslen(name) + 1);
    return (DWORD)wcslen(buf);
}
#else
#include <unistd.h>
#include <windows.h>   /* (linux: host/posix's, for DWORD and GetModuleFileNameA) */
/* (Linux) the same bin/ layout when the folder was built by the packaged builder: afterglow in the folder is the
   launcher script, and the game is bin/afterglow. A development build has no bin/ and this is GetModuleFileName. */
static inline DWORD hos_game_path_a(char *buf, DWORD size) {
    DWORD n = GetModuleFileNameA(NULL, buf, size);
    if (!n || n >= size) return n;
    char *name = buf + n;
    while (name > buf && name[-1] != '/') name--;
    if (name - buf < 5) return n;
    char *dir = name - 1;
    char *bin = dir;
    while (bin > buf && bin[-1] != '/') bin--;
    if (dir - bin != 3 || memcmp(bin, "bin", 3) != 0) return n;
    char probe[MAX_PATH + 16];
    const size_t root = (size_t)(bin - buf);
    if (root + 10 >= sizeof probe) return n;
    memcpy(probe, buf, root);
    memcpy(probe + root, "image.bin", 10);
    if (access(probe, F_OK) != 0) return n;
    memmove(buf + root, name, strlen(name) + 1);
    return (DWORD)strlen(buf);
}
#endif

/* (the same file, a text value: the line's value as it stands, minus the newline) */
static inline int hos_ini_text(const char *key, char *out, size_t size) {
    char path[MAX_PATH];
    const DWORD n = hos_game_path_a(path, MAX_PATH);
    char *slash = n ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;
    if (!slash || size == 0) return 0;
    snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "afterglow_options.ini");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    const size_t len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, len) && line[len] == '=') {
            size_t v = strlen(line + len + 1);
            while (v && (line[len + 1 + v - 1] == '\n' || line[len + 1 + v - 1] == '\r')) v--;
            if (v >= size) v = size - 1;
            memcpy(out, line + len + 1, v), out[v] = 0, found = 1;
        }
    fclose(f);
    return found;
}

/* The Insert menu's options (afterglow_options.ini beside the executable or game folder) */
static inline int hos_ini_option(const char *key, int otherwise) {
    char path[MAX_PATH];
    const DWORD n = hos_game_path_a(path, MAX_PATH);
    char *slash = n ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;
    if (!slash) return otherwise;
    snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "afterglow_options.ini");
    FILE *f = fopen(path, "r");
    if (!f) return otherwise;
    char line[80];
    const size_t len = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, len) && line[len] == '=') otherwise = atoi(line + len + 1);
    fclose(f);
    return otherwise;
}
