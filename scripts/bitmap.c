
#include <stdio.h>


unsigned long bitmap; /* support at least up to 32 config entries */
int conf_n;
char **conf_list;

static int print_conf(void) {
    unsigned long b;
    int i;
    
    for (b = bitmap, i = conf_n - 1; i >= 0; i--) {
	if (b & 1)
	    printf("-D%s ", conf_list[i]);
	b >>= 1;
    }
    printf("\n");
    if (b)
	/* overflow : we have finished */
	return 0;
    return 1;
}
    
int main(int argc, char *argv[]) {

    conf_n = --argc;
    conf_list = ++argv; 

    while (++bitmap) {
	if (print_conf() == 0)
	    return 0;
    }
    
    return 1;
}

