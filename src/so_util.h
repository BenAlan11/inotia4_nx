/* so_util.h -- ELF shared-object loader for the Switch
 *
 * Derived from so_util in fgsfdsfgs/max_nx and NaGaa95/swordigo_nx
 * (Copyright (C) 2021 Andy Nguyen, fgsfds), MIT licensed.
 */
#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <switch.h>
#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#define SO_MAX_SEGMENTS 8
#define SO_MAX_MODULES  4

typedef struct so_module so_module;

struct so_module {
  char name[256];

  void *so_base;      /* file image, freed once loading is done        */
  size_t so_size;

  Elf64_Ehdr *elf_hdr;
  Elf64_Phdr *prog_hdr;   /* mutable: vaddrs are rebased to load_virtbase */
  Elf64_Shdr *sec_hdr;
  char *shstrtab;

  Elf64_Phdr phdr[SO_MAX_SEGMENTS * 2];  /* pristine copy, for dl_iterate_phdr */

  void *load_base;        /* anonymous RW staging copy                    */
  void *load_virtbase;    /* code-memory alias handed out to the game     */
  size_t load_size;
  VirtmemReservation *load_memrv;

  Elf64_Sym *syms;
  char *dynstrtab;
  int num_syms;

  int phnum;
  so_module *next;
};

typedef struct {
  char symbol[64];
  uintptr_t func;
} DynLibFunction;

/* loads the ELF image and reserves virtual address space; returns <0 on error */
int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
/* fallback for a .so whose section headers were stripped */
int so_load_from_dynamic(so_module *mod);
/* applies the internal (RELATIVE/GLOB_DAT/...) relocations in place */
int so_relocate(so_module *mod);
/* patches imports using the table, then falls back to other loaded modules */
int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
/* maps the staging copy into code memory and applies final protections */
void so_finalize(so_module *mod);

/* Replace a symbol the module defines itself; the PLT slot is what gets
 * rewritten, so this must run before so_resolve(). */
typedef struct {
  const char *name;
  void *fn;
} so_override;

void so_set_overrides(const so_override *list, int count);

/* our own process handle, with a fallback for loaders that provide no config */
Handle so_process_handle(void);
void so_flush_caches(so_module *mod);
void so_execute_init_array(so_module *mod);
void so_free_temp(so_module *mod);
int so_unload(so_module *mod);

/* link-time address (usable for data before so_finalize) */
uintptr_t so_find_addr(so_module *mod, const char *symbol);
/* runtime address (load_virtbase-relative; the only valid form after finalize) */
uintptr_t so_find_addr_rx(so_module *mod, const char *symbol);
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);

void hook_arm64(uintptr_t addr, uintptr_t dst);

/* bionic-compatible dl_iterate_phdr over the modules loaded here */
int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

#endif
