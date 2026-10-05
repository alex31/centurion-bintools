/*
 *	Do template substitutions on an input file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

void badmarker(char c)
{
    fprintf(stderr, "Missing %% marker '%c'\n", c);
    exit(1);
}

void badsize(unsigned n, unsigned max)
{
    fprintf(stderr, "Substitution %%%u out of range (max %%%u)\n", n, max);
    exit(1);
}

void badeof(void)
{
    fputs("Unexpected EOF\n", stderr);
    exit(1);
}

void badquote(char c)
{
    fputs("Unexpected quoting '", stderr);
    fputc(c, stderr);
    fputs("'\n", stderr);
    exit(1);
}

void process_sub(int argc, char *argv[])
{
    int n = 0;
    char *p;
    int c;
    c = getchar();
    if (c == EOF)
        badeof();
    if (c == '%') {
        putchar(c);
        return;
    }
    while(isdigit(c)) {
        n *= 10;
        n += (c - '0');
        c = getchar();
    }
    if (c != '%')
        badmarker(c);
    if (n < 1 || n >= argc)
        badsize(n, argc - 1);

    p = argv[n];
    while(*p) {
        if (*p != '\\')
            putchar(*p);
        else switch(*++p) {
        case '\\':
            putchar(*p);
            break;
        case 'n':
            putchar('\n');
            break;
        case 'r':
            putchar('\r');
            break;
        case 't':
            putchar('\t');
            break;
        default:
            badquote(*p);
        }
        p++;
    }
}

int main(int argc, char *argv[])
{
    int c;

    while((c = getchar()) != EOF) {
        if (c == '%')
            process_sub(argc, argv);
        else
            putchar(c);
    }
}
