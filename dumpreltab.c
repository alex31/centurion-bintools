/*
 *	Debug aid: decode the relocation file from the linker
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "obj.h"

void dodump(int fd)
{
    uint8_t c[3];
    while(read(fd, c, 3) == 3) {
        switch(c[0]) {
        case 0:
            printf("Z ");
            break;
        case 1:
            printf("R ");
            break;
        case 2:
            printf("SB %c DB %c\n", SEGNAME[c[1]], SEGNAME[c[2]]);
            continue;
        default:
            printf("? ");
        }
        printf("%04X\n", (c[1] << 8) | c[2]);
    }
}

int main(int argc, char *argv[])
{
    int fd;
    argv++;
    if (*argv == NULL)
        dodump(0);
    else while(*argv) {
        fd = open(*argv, O_RDONLY);
        if (fd == -1) {
            perror(*argv);
            return 1;
        }
        printf("%s:\n", *argv);
        dodump(fd);
        close(fd);
        argv++;
    }
    return 0;
}
