/* zso1 jk320790 */

#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

#include "elf_reader.h"

#define MAX_PHNUM 100
#define NONSFI_PAGE_SIZE 0x1000
#define NONSFI_PAGE_MASK (NONSFI_PAGE_SIZE - 1)


static uintptr_t page_size_round_down(uintptr_t addr) {
  return addr & ~NONSFI_PAGE_MASK;
}

static uintptr_t page_size_round_up(uintptr_t addr) {
  return page_size_round_down(addr + NONSFI_PAGE_SIZE - 1);
}

static int elf_flags2mmap_flags(int pflags) {
  return ((pflags & PF_X) != 0 ? PROT_EXEC : 0) |
    ((pflags & PF_R) != 0 ? PROT_READ : 0) |
    ((pflags & PF_W) != 0 ? PROT_WRITE : 0);
}

int is_image_valid(Elf32_Ehdr *hdr)
{
  unsigned int ok = 1;
  /* i386 */
  if (hdr->e_machine != EM_386)
    ok = 0;
  /* ELF type */
  if (hdr->e_type != ET_DYN)
    ok = 0;

  if (!ok)
    errno = EINVAL;
  return ok;
}

size_t map_elf(const char* name, void **mapped_load, struct protect **protections, int *number_of_protections)
{
  FILE* elf = fopen(name, "rb");
  if (!elf)
    return -1;
  int fd = fileno(elf);

  size_t span;

  /* Read ELF file headers. */
  Elf32_Ehdr ehdr;
  ssize_t bytes_read = pread(fd, &ehdr, sizeof(ehdr), 0);
  if (bytes_read != sizeof(ehdr)) {
    errno = EINVAL;
    return -1;
  }

  if (!is_image_valid(&ehdr))
    return -1;
  if (ehdr.e_phnum > MAX_PHNUM) {
    errno = EINVAL;
    return -1;
  }
  Elf32_Phdr phdr[MAX_PHNUM];
  ssize_t phdrs_size = sizeof(phdr[0]) * ehdr.e_phnum;
  bytes_read = pread(fd, phdr, phdrs_size, ehdr.e_phoff);
  if (bytes_read != phdrs_size) {
    errno = EINVAL;
  }

  /* Find the first PT_LOAD segment. */
  size_t phdr_index = 0;
  while (phdr_index < ehdr.e_phnum && phdr[phdr_index].p_type != PT_LOAD)
    ++phdr_index;
  if (phdr_index == ehdr.e_phnum) {
    errno = EINVAL;
    return -1;
  }
  /*
   * ELF requires that PT_LOAD segments be in ascending order of p_vaddr.
   * Find the last one to calculate the whole address span of the image.
   */
  Elf32_Phdr *first_load = &phdr[phdr_index];
  Elf32_Phdr *last_load = &phdr[ehdr.e_phnum - 1];
  while (last_load > first_load && last_load->p_type != PT_LOAD)
    --last_load;
  if (first_load->p_vaddr != 0) {
    errno = EINVAL;
    return -1;
  }
  span = last_load->p_vaddr + last_load->p_memsz;

  /* Reserve address space. */
  void *mapping = mmap(NULL, span, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (mapping == MAP_FAILED) {
    return -1;
  }
  uintptr_t load_bias = (uintptr_t) mapping;

  /* Map the PT_LOAD segments. */
  uintptr_t prev_segment_end = 0;
  int entry_point_is_valid = 0;

  *protections = (struct protect*)malloc((last_load - first_load + 1) * sizeof(struct protect));
  *number_of_protections = 0;
  struct protect *protection;

  Elf32_Phdr *ph;
  for (ph = first_load, protection = *protections; ph <= last_load; ph++, protection++) {
    if (ph->p_type != PT_LOAD)
      continue;
    /* protection lvl will be set when relocations are complete */
    protection->prot = elf_flags2mmap_flags(ph->p_flags);
    (*number_of_protections)++;
    uintptr_t segment_start = page_size_round_down(ph->p_vaddr);
    uintptr_t segment_end = page_size_round_up(ph->p_vaddr + ph->p_memsz);
    if (segment_start < prev_segment_end) {
      errno = EINVAL;
      munmap(mapping, span);
      return -1;
    }
    prev_segment_end = segment_end;
    void *segment_addr = (void *) (load_bias + segment_start);
    protection->addr = segment_addr;
    protection->size = segment_end - segment_start;
    /* protection lvl of this segments will be changed after relocations are done */
    void *map_result = mmap((void *) segment_addr,
        segment_end - segment_start,
        (PROT_READ | PROT_WRITE), MAP_PRIVATE | MAP_FIXED, fd,
        page_size_round_down(ph->p_offset));
    if (map_result != segment_addr) {
      errno = EINVAL;
      munmap(mapping, span);
      return -1;
    }
    if ((ph->p_flags & PF_X) != 0 &&
        ph->p_vaddr <= ehdr.e_entry &&
        ehdr.e_entry < ph->p_vaddr + ph->p_filesz) {
      entry_point_is_valid = 1;
    }

    if (ph->p_memsz > ph->p_filesz) {
      if ((ph->p_flags & PF_W) == 0) {
        errno = EINVAL;
        munmap(mapping, span);
        return -1;
      }
      uintptr_t bss_start = ph->p_vaddr + ph->p_filesz;
      uintptr_t bss_map_start = page_size_round_up(bss_start);
      memset((void *) (load_bias + bss_start), 0, bss_map_start - bss_start);
      if (bss_map_start < segment_end) {
        void *map_addr = (void *) (load_bias + bss_map_start);
        map_result = mmap(map_addr, segment_end - bss_map_start,
            elf_flags2mmap_flags(ph->p_flags), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
            -1, 0);
        if (map_result != map_addr) {
          errno = EINVAL;
          munmap(mapping, span);
          return -1;
        }
      }
    }
  }

  if (close(fd) != 0) {
    errno = EIO;
    munmap(mapping, span);
    return -1;
  }

  *mapped_load = (void*)load_bias;
  return span;
}


void* get_dyn_segment( char *elf)
{
  Elf32_Ehdr *hdr = (Elf32_Ehdr*) elf;
  Elf32_Phdr *phdr = (Elf32_Phdr *)(elf + hdr->e_phoff);

  int i;
  for(i=0; i < hdr->e_phnum; ++i) {
    if(phdr[i].p_type == PT_DYNAMIC) {
      return elf + phdr[i].p_vaddr;
    }
  }
  errno = EINVAL;
  return (void*)NULL;
}


int get_symbols(char* elf_start, Elf32_Dyn *dyn, Elf32_Sym **symbols, char **strtab)
{
  *symbols = NULL;
  *strtab = NULL;

  Elf32_Word *hash;
  Elf32_Word number_of_symbols;
  while(dyn->d_tag != DT_NULL)
  {
    if (dyn->d_tag == DT_HASH)
    {
      hash = (Elf32_Word*)(elf_start + dyn->d_un.d_ptr);
      number_of_symbols = hash[1];
    }
    else if (dyn->d_tag == DT_STRTAB)
    {
      *strtab = elf_start + dyn->d_un.d_ptr;
    }
    else if (dyn->d_tag == DT_SYMTAB)
    {
      *symbols = (Elf32_Sym*)(elf_start + dyn->d_un.d_ptr);
    }
    dyn++;
  }
  if (symbols && strtab)
    return number_of_symbols;
  return -1;
}


int32_t get_offset_of_declared_symbol(const int number_of_symbols, Elf32_Sym *sym, char *strtab, const char *name)
{
  Elf32_Word sym_index;
  for (sym_index = 0; sym_index < number_of_symbols; sym_index++)
  {
    unsigned char sym_type = ELF32_ST_TYPE(sym[sym_index].st_info);
    if (sym_type != STT_OBJECT
        && sym_type != STT_FUNC
        && sym_type != STT_NOTYPE)
      continue;

    char* sym_name = &strtab[sym[sym_index].st_name];
    if (!strcmp(name, sym_name)) {
      if (sym_type == STT_NOTYPE) return -1;
      return sym[sym_index].st_value;
    }
  }
  return -1;
}


int do_relocation(char *elf_start, Elf32_Rel *rel, int32_t addr)
{
  Elf32_Word rel_type = ELF32_R_TYPE(rel->r_info);

  int32_t *rel_point = (int32_t*)(elf_start + rel->r_offset);
  switch (ELF32_R_TYPE(rel->r_info)) {
    case R_386_32:
      *rel_point += addr;
      break;
    case R_386_PC32:
      *rel_point = *rel_point + addr - (int32_t)rel_point;
      break;
    case R_386_JMP_SLOT:
      /* only relative relocation - name resolving is lazy */
      *rel_point += (int32_t)elf_start;
      break;
    case R_386_GLOB_DAT:
      *rel_point = addr;
      break;
    case R_386_RELATIVE:
      *rel_point += (int32_t)elf_start;
      break;
    default:
      errno = EINVAL;
      return -1;
  }
  return 0;
}

int32_t resolve_relocation(char *elf_start, int number_of_symbols, Elf32_Sym *symbols, char *strtab,
    const char* sym_name, void *(*getsym)(const char *name))
{
  int32_t offset = get_offset_of_declared_symbol(number_of_symbols, symbols, strtab, sym_name);
  if (offset < 0)
    return (int32_t)getsym(sym_name);
  return (int32_t)(elf_start + offset);
}

void* do_lazy_relocation(struct elf_ptrs *elf_ptrs, int offset)
{
  const char *sym_name;
  int32_t addr;
  Elf32_Rel *relocation = (Elf32_Rel*)((void*)(elf_ptrs->plt_relocations) + offset);
  int sym_index = ELF32_R_SYM(relocation->r_info);
  sym_name = elf_ptrs->strtab + elf_ptrs->symbols[sym_index].st_name;
  addr = resolve_relocation(elf_ptrs->elf_start, 1, elf_ptrs->symbols,
      elf_ptrs->strtab, sym_name, elf_ptrs->getsym);
  *(int32_t*)(elf_ptrs->elf_start + elf_ptrs->plt_relocations[offset].r_offset) = (int32_t)(addr);
  return (void*)addr;
}

void start_lazy_relocation();
asm(".local start_lazy_relocation;"
    "start_lazy_relocation:"
    /* save registers on stack */
    " pusha;"

    /* allocate place for 2 coppied arguments and registers */
    " sub $8, %esp;"
    /* copy arguments to top of the stack */
    " movl 44(%esp), %eax;"
    " movl %eax, 4(%esp);"
    " movl 40(%esp), %eax;"
    " movl %eax, (%esp);"
    /* first argument is pointer to the structure,
       where first entry is address to do_lazy_relocation */
    " movl (%eax), %eax;"
    " call *%eax;"

    /* pop copied arguments from the stack */
    " add $8, %esp;"
    /* move resolved address to 2. of the old arguments */
    " movl %eax, 36(%esp);"

    /* restore registers */
    " popa;"

    /* pop first old argument from the stack */
    " add $4, %esp;"
    " ret;");

int do_relocations(char *elf_start, Elf32_Dyn *dyn_start, void *(*getsym)(const char *name))
{
  size_t number_of_rel_relocs = 0;
  size_t number_of_jmp_relocs = 0;
  size_t relent_size = 8; /* assuming 8 according to task description */
  Elf32_Rel *rel_rel = NULL;
  Elf32_Rel *plt_rel = NULL;

  struct elf_ptrs* elf_ptrs = malloc(sizeof(struct elf_ptrs));

  Elf32_Dyn *dyn = dyn_start;
  while(dyn->d_tag != DT_NULL)
  {
    switch (dyn->d_tag) {
      case DT_RELENT:
        relent_size = dyn->d_un.d_val;
        assert(relent_size == 8);
        break;
      case DT_RELSZ:
        number_of_rel_relocs = dyn->d_un.d_val;
        break;
      case DT_PLTRELSZ:
        number_of_jmp_relocs = dyn->d_un.d_val;
        break;
      case DT_REL:
        rel_rel = (Elf32_Rel*)(elf_start + dyn->d_un.d_ptr);
        break;
      case DT_JMPREL:
        plt_rel = (Elf32_Rel*)(elf_start + dyn->d_un.d_ptr);
        break;
      case DT_PLTGOT:
        *(int32_t*)(elf_start + dyn->d_un.d_ptr + 4) = (int32_t)elf_ptrs;
        elf_ptrs->r = (void (*)())do_lazy_relocation;
        *(int32_t*)(elf_start + dyn->d_un.d_ptr + 8) =
          (int32_t)start_lazy_relocation;
        break;
    }
    dyn++;
  }
  elf_ptrs->elf_start = elf_start;
  elf_ptrs->dyn_section = dyn_start;
  elf_ptrs->plt_relocations = plt_rel;

  Elf32_Sym *sym;
  char* strtab;
  int number_of_symbols = get_symbols(elf_start, dyn_start, &sym, &strtab);
  if (number_of_symbols < 0)
    return -1;

  elf_ptrs->symbols = sym;
  elf_ptrs->strtab = strtab;
  elf_ptrs->getsym = getsym;

  Elf32_Rel *rel;
  const char *sym_name;
  int32_t addr;
  int i;
  for (i=0; i < number_of_rel_relocs / relent_size; i++, rel_rel++) {
    sym_name = strtab + sym[ELF32_R_SYM(rel_rel->r_info)].st_name;
    addr = resolve_relocation(elf_start, number_of_rel_relocs, sym, strtab, sym_name, getsym);
    if (do_relocation(elf_start, rel_rel, addr) < 0)
      return -1;
  }

  /* for dynamic binding I have to relocate GOT.PLT entities */
  for (i=0; i < number_of_jmp_relocs / relent_size; i++, plt_rel++) {
    if (do_relocation(elf_start, plt_rel, 0) < 0)
      return -1;
  }
  return (number_of_rel_relocs + number_of_jmp_relocs) / relent_size;
}
