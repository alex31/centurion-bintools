/*
 *	Convert a binary, map and relocation file from ld into a
 *	C automatically banking binary
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "obj.h"

struct stub {
    unsigned sseg;
    unsigned dseg;
    uint16_t addr;
    uint16_t stub;
};

#define NSTUB		512
#define STUB_SIZE	6

static struct stub stubs[NSTUB];
static unsigned nstub;
static uint16_t stub_next;
static uint16_t stub_end;
static FILE *map;
static int bin_fd;
static char buf[256];
static char segname[] = SEGNAME;
static uint16_t bankfunc[5][5];
static uint16_t stubfunc[5];
static unsigned stubseg;

static void bad_entry(void)
{
    fprintf(stderr, "bad map entry: %s", buf);
}

static unsigned find_symbol(const char *p, unsigned *seg,  uint16_t *addr)
{
    char segc;
    char name[15];
    rewind(map);
    while(fgets(buf, 255, map)) {
        if (sscanf(buf, "%hx %c %15s", addr, &segc, name) != 3) {
            bad_entry();
            continue;
        }
        if (strcmp(name, p) == 0) {
            char *p = strchr(segname, segc);
            if (p == NULL) {
                bad_entry();
                continue;
            }
            *seg = p - segname;
            return 1;
        }
    }
    return 0;
}

static void seek_to(unsigned seg, uint16_t addr)
{
    static off_t last = 0xFFFFFFFF;
    off_t off = addr;
    printf("addr %u:%04X\n", seg, addr);
    if (seg >= OVERLAY)
        off += 0x10000 * (seg - OVERLAY + 1);
    if (off != last + 1) {
        printf("Seek to %ld\n", (long)off);
        if (lseek(bin_fd, off, SEEK_SET) < 0) {
            perror("lseek");
            exit(1);
        }
    }
    last = off;
}

static uint8_t readb(unsigned seg, uint16_t addr)
{
    uint8_t c;

    seek_to(seg, addr);
    if (read(bin_fd, &c, 1) != 1) {
        fprintf(stderr, "readb: unable to read byte.\n");
        exit(1);
    }
    return c;
}

static void writeb(unsigned seg, uint16_t addr, uint8_t c)
{
    seek_to(seg, addr);
    if (write(bin_fd, &c, 1) != 1) {
        fprintf(stderr, "writeb: unable to write byte.\n");
        exit(1);
    }
}

static unsigned is_code(unsigned seg)
{
    if (seg == CODE || seg == COMMON || seg >= OVERLAY)
        return 1;
    return 0;
}

static unsigned map_seg_to_bank(unsigned seg)
{
    if (seg == CODE)
        return '0';
    if (seg >= OVERLAY && seg < PCREL)
        return '1' + seg - OVERLAY;
    /* Common calls into banks need to save/restore old bank which will
       need to be established at runtime */
    if (seg == COMMON)
        return 'C';
    fprintf(stderr, "Segment %c has no bank mapping.\n", segname[seg]);
    exit(1);
}

static void find_bank_function(unsigned sbank, unsigned dbank)
{
    unsigned s;
    char name[16];
    snprintf(name, 16, "__bank_%c_%c", (char)sbank, (char)dbank);
    if (find_symbol(name, &s, &bankfunc[sbank][dbank]) == 0) {
        fprintf(stderr, "Cannot find needed bank function %s\n", name);
        exit(1);
    }
    if (s >= OVERLAY) {
        fprintf(stderr, "Bank function %s needs to be fixed space.\n", name);
        exit(1);
    }
}

static void find_stub_function(unsigned dbank)
{
    unsigned s;
    char name[16];
    snprintf(name, 16, "__stub_%c", (char)dbank);
    if (find_symbol(name, &s, &stubfunc[dbank]) == 0) {
        fprintf(stderr, "Cannot find needed stub function %s\n", name);
        exit(1);
    }
    if (s >= OVERLAY) {
        fprintf(stderr, "Stub function %s needs to be fixed space.\n", name);
        exit(1);
    }
}

static void fixup_code(unsigned sseg, unsigned dseg, uint16_t addr)
{
    int i;
    unsigned sm, dm;
    /* Points to high byte so back pedal for little endian */
    addr--;
    /* For 8080/8085/Z80 this should have PUSH AF CALL in front */
    if (readb(sseg, addr - 2) != 0xF5 ||
        readb(sseg, addr - 1) != 0xCD || readb(sseg, addr + 2) != 0xF1) {
        fprintf(stderr, "Unable to auto-relocate %u:%04X\n", sseg, addr);
        for (i = -2; i <= 2; i++)
            printf("%02X ", readb(sseg, addr + i));
        printf("\n");
        exit(1);
    }
    /* Ok it's a valid block */
    writeb(sseg, addr + 2, readb(sseg, addr + 1));
    writeb(sseg, addr + 1, readb(sseg, addr));
    writeb(sseg, addr - 2, 0xCD);

    sm = map_seg_to_bank(sseg);
    dm = map_seg_to_bank(dseg);
    if (bankfunc[sm][dm] == 0)
        find_bank_function(sm, dm);
    writeb(sseg, addr - 1, bankfunc[sm][dm]);
    writeb(sseg, addr, bankfunc[sm][dm] >> 8);
}


static uint16_t find_stub(unsigned sseg, unsigned dseg, uint16_t addr)
{
    struct stub *s = stubs;
    while(s < stubs + nstub) {
        if (s->addr == addr && s->sseg == sseg && s->dseg == dseg)
            return s->stub;
    }
    /* No luck */
    if (stub_next + STUB_SIZE >= stub_end) {
        fprintf(stderr, "stub: too many stubs for stub area.\n");
        exit(1);
    }
    /* s now points to next record */
    if (s == stubs + NSTUB) {
        fprintf(stderr, "stub: bankman table too small (%d records).\n", NSTUB);
        exit(1);
    }
    s->addr = addr;
    s->sseg = sseg;
    s->dseg = dseg;
    s->stub = stub_next;
    dseg = map_seg_to_bank(dseg);
    if (stubfunc[dseg] == 0)
        find_stub_function(dseg);
    /* Write record */
    /* LD DE,addr */
    writeb(stubseg, stub_next, 0x11);
    writeb(stubseg, stub_next + 1, addr);
    writeb(stubseg, stub_next + 2, addr >> 8);
    /* JP stubfunc */
    writeb(stubseg, stub_next + 3, 0xC3);
    writeb(stubseg, stub_next + 4, stubfunc[dseg]);
    writeb(stubseg, stub_next + 5, stubfunc[dseg] >> 8);
    nstub++;
    stub_next += STUB_SIZE;
    return s->stub;
}

static void insert_stub(unsigned sseg, unsigned dseg, uint16_t addr)
{
    uint16_t dp = readb(sseg, addr - 1) | (readb(sseg, addr) << 8);
    uint16_t stub = find_stub(sseg, dseg, dp);
    writeb(sseg, addr - 1, stub);
    writeb(sseg, addr, stub >> 8);
}

void usage(void)
{
    puts("usage");
    exit(1);
}

static void process_reloc(int fd)
{
    unsigned sseg, dseg;
    uint8_t c[3];

    while(read(fd, c, 3) == 3) {
        switch(c[0]) {
        /* ZP relocations are uninteresting */
        case 0:
            break;
        case 1:
            if (is_code(sseg)) {
                /* Code calling code or referencing data in the same */
                if (sseg == dseg)
                    continue;
                /* Code referencing data (unbanked) */
                if (!is_code(dseg))
                    continue;
                fixup_code(sseg, dseg, (c[1] << 8) | c[2]);
            } else {
                if (is_code(dseg))	/* Symbol reference to code */
                    insert_stub(sseg, dseg, (c[1] << 8) | c[2]);
                /* Data to data references are unbanked and don't matter */
            }
            break;
        case 2:
            /* Sets the segmenmts for reloc 1 above */
            sseg = c[1];
            dseg = c[2];
            continue;
        default:
            fprintf(stderr, "Unknown reloc record %u\n", c[0]);
            exit(1);
        }
    }
}

int main(int argc, char *argv[])
{
    int reloc;
    unsigned dummy;

    if (argc != 4)
        usage();
    bin_fd = open(argv[1], O_RDWR);
    if (bin_fd == -1) {
        perror(argv[1]);
        exit(1);
    }
    map = fopen(argv[2], "r");
    if (map == NULL) {
        perror(argv[2]);
        exit(1);
    }

    reloc = open(argv[3], O_RDONLY);
    if (reloc == -1) {
        perror(argv[3]);
        exit(1);
    }

    if (find_symbol("stub", &stubseg, &stub_next) == 0 ||
        find_symbol("stub_end", &dummy, &stub_end) == 0) {
        fprintf(stderr, "%s: missing stub symbols.\n", argv[2]);
        exit(1);
    }

    process_reloc(reloc);
    return 0;
}
