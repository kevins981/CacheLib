#include <stdlib.h> 
#include <stdio.h>
#include <time.h> 

int main() {
  const int size = 500*1024*1024; // Allocate 50M. L3 is 20MB
  char *c = (char *)malloc(size);
  srand (time(NULL));

  for (int j = 0; j < size; j++) {
    c[j] = rand();
  } 

  free(c);
}
