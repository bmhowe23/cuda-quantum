// gcc -shared -o benlib2.so -fPIC benlib2.c
#include <stdio.h>
void ben_dependent_hello()
{
    printf("Hello from benlib2.c!\n");
}
