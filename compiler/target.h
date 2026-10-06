#ifndef FORGE_TARGET_H
#define FORGE_TARGET_H

#include <stdbool.h>

typedef enum { FORGE_ARCH_X86_64, FORGE_ARCH_AARCH64, FORGE_ARCH_ARMV7 } ForgeArch;
typedef enum { FORGE_OS_LINUX, FORGE_OS_MACOS, FORGE_OS_WINDOWS } ForgeOS;
typedef enum { FORGE_ABI_SYSV, FORGE_ABI_DARWIN, FORGE_ABI_WINDOWS_MSVC,
               FORGE_ABI_WINDOWS_GNU, FORGE_ABI_LINUX_ARMHF,
               FORGE_ABI_LINUX_ARMEL } ForgeABI;
typedef enum { FORGE_OBJECT_ELF, FORGE_OBJECT_MACHO, FORGE_OBJECT_COFF } ForgeObjectFormat;
typedef enum { FORGE_LIBC_NONE, FORGE_LIBC_GNU, FORGE_LIBC_MUSL } ForgeLibC;

typedef struct {
    ForgeArch arch;
    ForgeOS os;
    ForgeABI abi;
    ForgeObjectFormat object_format;
    ForgeLibC libc;
    unsigned pointer_bits;
} ForgeTarget;

bool forge_target_parse(const char *triple, ForgeTarget *out);
const char *forge_target_arch_name(ForgeArch arch);
const char *forge_target_os_name(ForgeOS os);
const char *forge_target_abi_name(ForgeABI abi);
const char *forge_target_object_name(ForgeObjectFormat format);
const char *forge_target_libc_name(ForgeLibC libc);

#endif
