#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "loader.h"
#include "elf_reader.h"

int glob_my = 123;

void *f(const char *n)
{
  if (!strcmp("glob", n))
    return &glob_my;
  else if (!strcmp("dupa", n))
    return malloc;
  else
    return NULL;
}

int main()
{
  struct library* lib = library_load("elf.pic", f);
  printf ("start\n");

  int (*ptr)();
  ptr = library_getsym(lib, "fun");
  /*
  printf ("tutaj: %d\n", (int)f("dupa"));
  printf ("tutaj: %d\n", (int)malloc);
  */
  int a = ptr();

  int *ccc = library_getsym(lib, "c");
  printf("c: %d!\n", *ccc);

  return 0;
}