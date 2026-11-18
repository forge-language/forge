#include "greeting_ext.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *message = frlib_greeting_ext_hello("C");
    if (!message || strcmp(message, "Hello from an external project, C") != 0)
        return 1;
    puts(message);
    return 0;
}
