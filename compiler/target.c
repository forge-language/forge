#include "target.h"
#include <string.h>

static bool contains(const char *s, const char *part) { return strstr(s,part)!=NULL; }

bool forge_target_parse(const char *triple, ForgeTarget *out) {
    if(!triple || !out) return false;
    ForgeTarget t={0};
    if(!strncmp(triple,"x86_64-",7) || !strncmp(triple,"amd64-",6)) {
        t.arch=FORGE_ARCH_X86_64; t.pointer_bits=64;
    } else if(!strncmp(triple,"aarch64-",8) || !strncmp(triple,"arm64-",6)) {
        t.arch=FORGE_ARCH_AARCH64; t.pointer_bits=64;
    } else if(!strncmp(triple,"armv7-",6) || !strncmp(triple,"armv7l-",7)) {
        t.arch=FORGE_ARCH_ARMV7; t.pointer_bits=32;
    } else return false;

    if(contains(triple,"linux")) {
        t.os=FORGE_OS_LINUX; t.object_format=FORGE_OBJECT_ELF;
        if(t.arch==FORGE_ARCH_ARMV7) {
            if(contains(triple,"gnueabihf")) t.abi=FORGE_ABI_LINUX_ARMHF;
            else if(contains(triple,"gnueabi")) t.abi=FORGE_ABI_LINUX_ARMEL;
            else return false;
        } else {
            if(contains(triple,"musl")) t.libc=FORGE_LIBC_MUSL;
            else if(contains(triple,"gnu")) t.libc=FORGE_LIBC_GNU;
            else return false;
            t.abi=FORGE_ABI_SYSV;
        }
    } else if(contains(triple,"darwin") || contains(triple,"macos")) {
        if(t.arch==FORGE_ARCH_ARMV7) return false;
        t.os=FORGE_OS_MACOS; t.abi=FORGE_ABI_DARWIN; t.object_format=FORGE_OBJECT_MACHO;
    } else if(contains(triple,"windows") || contains(triple,"mingw")) {
        t.os=FORGE_OS_WINDOWS; t.object_format=FORGE_OBJECT_COFF;
        if(contains(triple,"msvc")) t.abi=FORGE_ABI_WINDOWS_MSVC;
        else if(contains(triple,"gnu") || contains(triple,"mingw")) t.abi=FORGE_ABI_WINDOWS_GNU;
        else return false;
    } else return false;
    *out=t; return true;
}

const char *forge_target_arch_name(ForgeArch a) {
    switch(a) { case FORGE_ARCH_X86_64:return "x86_64"; case FORGE_ARCH_AARCH64:return "aarch64"; case FORGE_ARCH_ARMV7:return "armv7"; }
    return "unknown";
}
const char *forge_target_os_name(ForgeOS os) {
    switch(os) { case FORGE_OS_LINUX:return "linux"; case FORGE_OS_MACOS:return "macos"; case FORGE_OS_WINDOWS:return "windows"; }
    return "unknown";
}
const char *forge_target_abi_name(ForgeABI a) {
    switch(a) { case FORGE_ABI_SYSV:return "sysv"; case FORGE_ABI_DARWIN:return "darwin"; case FORGE_ABI_WINDOWS_MSVC:return "msvc"; case FORGE_ABI_WINDOWS_GNU:return "gnu"; case FORGE_ABI_LINUX_ARMHF:return "gnueabihf"; case FORGE_ABI_LINUX_ARMEL:return "gnueabi"; }
    return "unknown";
}
const char *forge_target_object_name(ForgeObjectFormat f) {
    switch(f) { case FORGE_OBJECT_ELF:return "ELF"; case FORGE_OBJECT_MACHO:return "Mach-O"; case FORGE_OBJECT_COFF:return "COFF"; }
    return "unknown";
}
const char *forge_target_libc_name(ForgeLibC l) {
    switch(l) { case FORGE_LIBC_NONE:return "none"; case FORGE_LIBC_GNU:return "gnu"; case FORGE_LIBC_MUSL:return "musl"; }
    return "unknown";
}
