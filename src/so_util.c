/* so_util.c -- ELF shared-object loader for the Switch
 *
 * Derived from so_util in fgsfdsfgs/max_nx and NaGaa95/swordigo_nx
 * (Copyright (C) 2021 Andy Nguyen, fgsfds), MIT licensed.
 *
 * Supported: an arbitrary number of PT_LOAD segments, RELA and packed
 * RELR/ANDROID_RELR relocations, cross-module symbol resolution (needed here
 * because Inotia ships libHercules.so alongside libgame.so).
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "so_util.h"
#include "util.h"
#include "error.h"

#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

static so_module *so_list = NULL;

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_flush_caches(so_module *mod) {
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
}

/* hbloader hands the NRO its own process handle through the launch config, but
 * an emulator booting the NRO straight from a file provides no config at all,
 * and envGetOwnProcessHandle() then reports INVALID_HANDLE. The kernel resolves
 * the CUR_PROCESS_HANDLE pseudo-handle for any syscall that takes a process, so
 * that is a complete stand-in. */
static Handle g_proc = INVALID_HANDLE;

Handle so_process_handle(void) {
  if (g_proc != INVALID_HANDLE)
    return g_proc;

  Handle h = envGetOwnProcessHandle();
  if (h == INVALID_HANDLE) {
    h = CUR_PROCESS_HANDLE;
    debugPrintf("so: no launch config, using the pseudo-handle for our process\n");
  }
  g_proc = h;
  return g_proc;
}

/* svcMapProcessCodeMemory hands the staging pages over to the code region, so
 * after the call the staging copy is no longer readable. Anything that searches
 * the symbol table later -- input_bind, a hook finder, a debug helper -- would
 * then be walking unmapped memory, which on hardware is an instant data abort
 * (and on an emulator just works, which is why this is easy to miss). Move the
 * two tables the lookups need out of the staging area first. */
static void move_symtab_out_of_staging(so_module *mod) {
  const int n = mod->num_syms;
  if (!mod->syms || n <= 0)
    return;

  Elf64_Sym *syms = malloc((size_t)n * sizeof(Elf64_Sym));
  if (!syms)
    return;
  memcpy(syms, mod->syms, (size_t)n * sizeof(Elf64_Sym));

  size_t strsize = 1;
  for (int i = 0; i < n; i++) {
    const char *s = mod->dynstrtab + syms[i].st_name;
    const size_t end = (size_t)syms[i].st_name + strlen(s) + 1;
    if (end > strsize)
      strsize = end;
  }
  char *strs = malloc(strsize);
  if (!strs) {
    free(syms);
    return;
  }
  memcpy(strs, mod->dynstrtab, strsize);

  mod->syms = syms;
  mod->dynstrtab = strs;
}

void so_finalize(so_module *mod) {
  move_symtab_out_of_staging(mod);

  Result rc = svcMapProcessCodeMemory(so_process_handle(),
                                      (u64)mod->load_virtbase,
                                      (u64)mod->load_base, mod->load_size);
  if (R_FAILED(rc))
    fatal_error("svcMapProcessCodeMemory failed:\n%08x", rc);

  /* The kernel refuses a W->X transition on code memory, so every page that
   * ends up executable has to be set RX first, straight from the freshly
   * mapped (RW == isolated) state; the rest becomes RW afterwards. Build a
   * page map so each permission is emitted exactly once per contiguous run. */
  const size_t num_pages = mod->load_size / 0x1000;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page)
    fatal_error("out of memory in so_finalize");

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / 0x1000;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < num_pages) {
      if (is_x_page[pg] != want_x) {
        pg++;
        continue;
      }
      size_t run_end = pg;
      while (run_end < num_pages && is_x_page[run_end] == want_x)
        run_end++;
      const u64 addr = (u64)mod->load_virtbase + pg * 0x1000;
      const u64 size = (run_end - pg) * 0x1000;
      rc = svcSetProcessMemoryPermission(so_process_handle(), addr, size,
                                         want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc))
        fatal_error("could not map %u bytes of %s memory at %p:\n%08x",
                    (u32)size, want_x ? "RX" : "RW", (void *)addr, rc);
      pg = run_end;
    }
  }

  free(is_x_page);
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;

  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);

  FILE *fd = fopen(filename, "rb");
  if (fd == NULL)
    return -1;

  fseek(fd, 0, SEEK_END);
  mod->so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    fclose(fd);
    return -2;
  }

  if (fread(mod->so_base, mod->so_size, 1, fd) != 1) {
    fclose(fd);
    res = -2;
    goto err_free_so;
  }
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  if (mod->elf_hdr->e_machine != EM_AARCH64) {
    debugPrintf("%s: not an AArch64 object\n", filename);
    res = -1;
    goto err_free_so;
  }

  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base +
                           mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("%s: too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4;
    goto err_free_so;
  }

  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {
    debugPrintf("%s: needs %u KB, only %u KB free\n", filename,
                (u32)(mod->load_size / 1024), (u32)(max_size / 1024));
    res = -3;
    goto err_free_so;
  }

  mod->load_base = base;
  if (!mod->load_base) {
    res = -3;
    goto err_free_so;
  }
  memset(mod->load_base, 0, mod->load_size);

  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->load_memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();

  debugPrintf("%s: load base = %p, virt = %p, size = %u KB\n", filename,
              mod->load_base, mod->load_virtbase, (u32)(mod->load_size / 1024));

  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset), p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    /* section headers are optional for a stripped .so; fall back to the
     * PT_DYNAMIC tags the linker always keeps. libgame.so ships its section
     * headers today, but this keeps us honest if that ever changes. */
    if (!so_load_from_dynamic(mod)) {
      debugPrintf("%s: no .dynsym/.dynstr\n", filename);
      res = -2;
      goto err_free_load;
    }
  }

  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }

  return 0;

err_free_load:
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;
  return res;
}

/* locate .dynsym/.dynstr through PT_DYNAMIC (used when section headers are gone) */
int so_load_from_dynamic(so_module *mod) {
  Elf64_Addr symtab = 0, strtab = 0, strsz = 0;
  size_t syment = sizeof(Elf64_Sym);
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type != PT_DYNAMIC)
      continue;
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base +
                                              mod->phdr[i].p_offset);
    for (; dyn->d_tag != DT_NULL; dyn++) {
      switch (dyn->d_tag) {
        case DT_SYMTAB: symtab = dyn->d_un.d_ptr; break;
        case DT_STRTAB: strtab = dyn->d_un.d_ptr; break;
        case DT_STRSZ:  strsz  = dyn->d_un.d_val; break;
        case DT_SYMENT: syment = dyn->d_un.d_val; break;
        default: break;
      }
    }
  }
  (void)syment;
  if (!symtab || !strtab || !strsz)
    return 0;
  mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + symtab);
  mod->dynstrtab = (char *)((uintptr_t)mod->load_base + strtab);
  /* no reliable symbol count without .hash/.gnu.hash; 0 means "scan until the
   * name pointer leaves the string table", which resolve does below */
  mod->num_syms = -1;
  return 1;
}

static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base +
                                                 mod->phdr[i].p_offset);
      for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag)
          return dyn->d_un.d_val;
    }
  }
  return 0;
}

static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      where = (uintptr_t)entry;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit))
          *(uint64_t *)((uintptr_t)mod->load_base + where + (bit - 1) * 8) +=
              (uint64_t)mod->load_virtbase;
      }
      where += 63 * 8;
    }
  }
}

int so_relocate(so_module *mod) {
  int deferred_abs64 = 0;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") != 0 && strcmp(sh_name, ".rela.plt") != 0)
      continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
      uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
      Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      int type = ELF64_R_TYPE(rels[j].r_info);
      switch (type) {
        case R_AARCH64_ABS64:
          if (sym->st_shndx == SHN_UNDEF) {
            /* imported absolute relocations (RTTI/vtable refs) resolve later */
            *ptr = rels[j].r_addend;
            deferred_abs64++;
          } else {
            *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
          }
          break;
        case R_AARCH64_RELATIVE:
          *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
          break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
          if (sym->st_shndx != SHN_UNDEF)
            *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
          break;
        case R_AARCH64_NONE:
          break;
        default:
          fatal_error("unknown relocation type %x in %s", type, mod->name);
          break;
      }
    }
  }

  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && relr_size) {
    debugPrintf("%s: %u bytes of RELR relocations\n", mod->name, (u32)relr_size);
    so_process_relr(mod, (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off),
                    relr_size);
  }

  if (deferred_abs64)
    debugPrintf("%s: deferred %d ABS64 imports\n", mod->name, deferred_abs64);
  return 0;
}

static int so_sym_count(so_module *mod) {
  if (mod->num_syms >= 0)
    return mod->num_syms;
  return 0;
}

static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  /* bounded scan: without .gnu.hash we walk the dynsym until the entry's
   * st_name no longer lands inside the module's own string table */
  uintptr_t limit = 0;
  for (int i = 0; i < mod->phnum; i++)
    if (mod->phdr[i].p_type == PT_LOAD && mod->phdr[i].p_vaddr > limit)
      limit = mod->phdr[i].p_vaddr + mod->phdr[i].p_memsz;

  for (int i = 0; i < so_sym_count(mod); i++) {
    Elf64_Sym *s = &mod->syms[i];
    if (s->st_shndx == SHN_UNDEF && s->st_name == 0)
      break;
    if (s->st_shndx == SHN_UNDEF || s->st_name == 0)
      continue;
    if (ELF64_ST_BIND(s->st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + s->st_name;
    if ((uintptr_t)sname >= limit)
      break;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + s->st_value;
  }
  return 0;
}

/* Overrides let the port replace a function inside the module itself.

 * Engine-internal calls to exported symbols go through the PLT, so the GOT slot
 * is the one place that has to change -- no code patching, no instruction cache
 * flushing, and it works even though the code pages are read-only. The DRM in
 * libgame (Protection::*) is neutralised this way. */
/* several independent tables (DRM, resource probes, ...) can be installed.
 * The count has to cover every so_set_overrides() call site in main.c -- and
 * it did not: adding one more table pushed the DRM set past the limit, where
 * so_set_overrides() silently dropped it and the game booted into a black
 * screen. Keep headroom, and let the setter complain instead of ignoring. */
#define SO_MAX_OVERRIDE_SETS 24
static const so_override *g_sets[SO_MAX_OVERRIDE_SETS];
static int g_set_counts[SO_MAX_OVERRIDE_SETS];
static int g_num_sets;

void so_set_overrides(const so_override *list, int count) {
  if (g_num_sets >= SO_MAX_OVERRIDE_SETS) {
    /* Silent truncation here is invisible until something a later table was
     * supposed to fix breaks far away from the cause. */
    debugPrintf("so: override set table full (%d), dropping %d entries\n",
                SO_MAX_OVERRIDE_SETS, count);
    return;
  }
  g_sets[g_num_sets] = list;
  g_set_counts[g_num_sets] = count;
  g_num_sets++;
}

static void *so_find_override(const char *name) {
  for (int s = 0; s < g_num_sets; s++)
    for (int i = 0; i < g_set_counts[s]; i++)
      if (g_sets[s][i].name && strcmp(g_sets[s][i].name, name) == 0)
        return g_sets[s][i].fn;
  return NULL;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs,
                                   int num_funcs, const char *name) {
  for (int k = 0; k < num_funcs; k++)
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;

  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }
  return 0;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs,
               int taint_missing_imports) {
  int missing = 0, resolved_abs64 = 0, self_resolved = 0;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") != 0 && strcmp(sh_name, ".rela.plt") != 0)
      continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    for (int j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
      uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
      Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      const int type = ELF64_R_TYPE(rels[j].r_info);
      if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
          type != R_AARCH64_JUMP_SLOT)
        continue;

      char *name = mod->dynstrtab + sym->st_name;

      /* checked before the SHN_UNDEF test: the interesting overrides are
       * symbols the module defines itself and calls through its own PLT */
      void *ov = so_find_override(name);
      if (ov) {
        *ptr = (uintptr_t)ov;
        continue;
      }
      if (sym->st_shndx != SHN_UNDEF) {
        /* The library is linked with BIND_NOW, so a call to one of its own
         * exported functions still goes through a PLT stub that loads this GOT
         * slot. In the file the slot is 0 -- the real dynamic linker fills it
         * with the definition it finds in this module -- and skipping these
         * made every such call branch to address 0. That is what swallowed the
         * engine at Protection::isRunningMemoryHack and friends.
         * 5716 slots in libgame.so depend on this. */
        if (type == R_AARCH64_JUMP_SLOT || type == R_AARCH64_GLOB_DAT) {
          *ptr = (uintptr_t)mod->load_virtbase + sym->st_value +
                 rels[j].r_addend;
          self_resolved++;
          if (strcmp(name, "jWD_GetIsTerminateIfCracked") == 0)
            debugPrintf("so: %s slot 0x%x = %p\n", name,
                        (unsigned)rels[j].r_offset, (void *)*ptr);
        }
        continue;
      }

      uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
      if (addr) {
        *ptr = addr + rels[j].r_addend;
        if (type == R_AARCH64_ABS64)
          resolved_abs64++;
      } else {
        missing++;
        debugPrintf("%s: UNRESOLVED import: %s\n", mod->name, name);
        if (taint_missing_imports)
          *ptr = rels[j].r_offset;  /* branch to a bogus address: obvious crash site */
      }
    }
  }

  debugPrintf("%s: %d missing imports, %d ABS64, %d self-resolved PLT slots\n",
              mod->name, missing, resolved_abs64, self_resolved);
  return missing;
}

void so_execute_init_array(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") != 0)
      continue;
    int (**init_array)(void) = (void *)((uintptr_t)mod->load_virtbase +
                                        mod->sec_hdr[i].sh_addr);
    for (size_t j = 0; j < mod->sec_hdr[i].sh_size / 8; j++)
      if (init_array[j] != 0)
        init_array[j]();
  }
}

uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  for (int i = 0; i < so_sym_count(mod); i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
  }
  fatal_error("could not find symbol:\n%s", symbol);
  return 0;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < so_sym_count(mod); i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("could not find symbol:\n%s", symbol);
  return addr;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL)
    return -1;
  if (mod->so_base)
    so_free_temp(mod);

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    const u64 seg_start = ((u64)mod->load_virtbase + p->p_vaddr) & ~0xFFFull;
    const u64 seg_end = ALIGN_MEM((u64)mod->load_virtbase + p->p_vaddr + p->p_memsz, 0x1000);
    svcSetProcessMemoryPermission(so_process_handle(), seg_start,
                                  seg_end - seg_start, Perm_Rw);
  }
  svcUnmapProcessCodeMemory(so_process_handle(), (u64)mod->load_virtbase,
                            (u64)mod->load_base, mod->load_size);

  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();

  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next)
      if (m->next == mod) { m->next = mod->next; break; }
  }
  return 0;
}

/* layout matches the bionic/libunwind expectation */
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr;
    info.dlpi_phnum = mod->phnum;
    int ret = callback(&info, sizeof(info), data);
    if (ret)
      return ret;
  }
  return 0;
}
