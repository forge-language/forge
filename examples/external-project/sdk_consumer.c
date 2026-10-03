#include "forge/fs.h"
#include "forge/threading.h"

int main(void) {
    char *path = fr_fs_temp_path("forge-sdk", ".tmp");
    if (!path) return 1;
    int failed = !fr_fs_is_file(path) || fr_threading_cpu_count() < 1;
    if (!fr_fs_remove(path)) return 2;
    return failed;
}
