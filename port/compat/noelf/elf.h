/* The part of <elf.h> the ELF loader uses (Windows and macOS have no <elf.h>). */
#pragma once
#include <stdint.h>
typedef uint32_t Elf32_Addr, Elf32_Off, Elf32_Word;
typedef uint16_t Elf32_Half;
typedef struct {
    unsigned char e_ident[16];
    Elf32_Half e_type, e_machine; Elf32_Word e_version;
    Elf32_Addr e_entry; Elf32_Off e_phoff, e_shoff; Elf32_Word e_flags;
    Elf32_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;
typedef struct {
    Elf32_Word p_type; Elf32_Off p_offset; Elf32_Addr p_vaddr, p_paddr;
    Elf32_Word p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;
#define PT_LOAD 1
