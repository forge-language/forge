#include "forge/fs.h"
#include "forge/os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(void) {
    const char *path = fr_os_temp_file();
    CHECK(path && fr_fs_exists(path));
    CHECK(fr_fs_write(path, "first"));
    CHECK(fr_fs_append(path, " second"));
    char *data = fr_fs_read(path);
    CHECK(data && strcmp(data, "first second") == 0);
    free(data);
    CHECK(fr_fs_write(path, ""));
    data = fr_fs_read(path);
    CHECK(data && data[0] == '\0');
    free(data);
    CHECK(fr_fs_remove(path));
    CHECK(!fr_fs_read(path));
    CHECK(!fr_fs_write(NULL, "data"));
#ifdef __linux__
    CHECK(!fr_fs_write("/dev/full", "buffered"));
    CHECK(!fr_fs_append("/dev/full", "buffered"));
#endif
    puts("filesystem tests passed");
    return 0;
}
