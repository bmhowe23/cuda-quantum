// gcc -shared -o benlib1.so -fPIC benlib1.c
#include <stdio.h>
void ben_dependent_hello()
{
    printf("Hello from benlib1.c!\n");
}
