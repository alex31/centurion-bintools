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

struct cpu
{
    const char *name;
    unsigned stub_size;
    void (*stub)(unsigned dseg, uint16_t addr);
    unsigned (*fixup)(unsigned sseg, unsigned dseg, uint16_t addr);
};

static struct cpu *cpu;
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
//    printf("addr %u:%04X\n", seg, addr);
    if (seg >= OVERLAY)
        off += 0x10000 * (seg - OVERLAY + 1);
    if (off != last + 1) {
//        printf("Seek to %ld\n", (long)off);
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

static unsigned is_overlay(unsigned seg)
{
    return seg >= OVERLAY;
}

static unsigned is_code(unsigned seg)
{
    if (seg == CODE || seg == DISCARD || seg == COMMON || seg >= OVERLAY)
        return 1;
    return 0;
}

static unsigned map_seg_to_bank(unsigned seg)
{
    if (!is_overlay(seg))
        return '0';
    if (seg >= OVERLAY && seg < PCREL)
        return '1' + seg - OVERLAY;
    /* Common calls into banks need to save/restore old bank which will
       need to be established at runtime */
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

static unsigned fixup_6803(unsigned sseg, unsigned dseg, uint16_t addr)
{
    int i;
    unsigned sm, dm;
    /* Points to high byte so back pedal for little endian */
    addr--;
    /* Look for PSHX JSR xxxx PULX */
    if (readb(sseg, addr - 2) != 0x3C ||
        readb(sseg, addr - 1) != 0xBD || readb(sseg, addr + 2) != 0x38) {
        fprintf(stderr, "Unable to auto-relocate %u:%04X\n", sseg, addr);
        for (i = -2; i <= 2; i++)
            printf("%02X ", readb(sseg, addr + i));
        printf("\n");
        exit(1);
    }
    /* Ok it's a valid block */
    writeb(sseg, addr + 2, readb(sseg, addr + 1));
    writeb(sseg, addr + 1, readb(sseg, addr));
    writeb(sseg, addr - 2, 0xBD);

    sm = map_seg_to_bank(sseg);
    dm = map_seg_to_bank(dseg);
    if (bankfunc[sm][dm] == 0)
        find_bank_function(sm, dm);
    /* 6803 is big endian */
    writeb(sseg, addr - 1, bankfunc[sm][dm] >> 8);
    writeb(sseg, addr, bankfunc[sm][dm]);
}

static void stub_6803(unsigned dseg, uint16_t addr)
{
    /* Write record */
    /* LD P2,=addr */
    writeb(stubseg, stub_next, 0xCE);
    writeb(stubseg, stub_next + 1, addr >> 8);
    writeb(stubseg, stub_next + 2, addr);
    /* JP stubfunc */
    writeb(stubseg, stub_next + 3, 0x7E);
    writeb(stubseg, stub_next + 4, stubfunc[dseg]  >> 8);
    writeb(stubseg, stub_next + 5, stubfunc[dseg]);
}

/* 6809 is much more awkward as we have no single byte push word */
static unsigned fixup_6809(unsigned sseg, unsigned dseg, uint16_t addr)
{
    int i;
    unsigned sm, dm;
    /* Points to high byte so back pedal for little endian */
    addr--;
    /* Look for PSHS X JSR PULS X over a 7 byte range*/
    if (readb(sseg, addr - 3) == 0x34 || readb(sseg, addr - 2) != 0x10 ||
        readb(sseg, addr - 1) != 0xBD ||
        readb(sseg, addr + 2) != 0x35 || readb(sseg, addr + 3) != 0x10) {
        fprintf(stderr, "Unable to auto-relocate %u:%04X\n", sseg, addr);
        for (i = -3; i <= 3; i++)
            printf("%02X ", readb(sseg, addr + i));
        printf("\n");
        exit(1);
    }
    /* Ok it's a valid block */
    /* LDX the value */
    writeb(sseg, addr -3, 0x8E);
    writeb(sseg, addr - 2, readb(sseg, addr + 1));
    writeb(sseg, addr - 1, readb(sseg, addr));
    /* JSR the bank op */
    writeb(sseg, addr, 0xBD);
    writeb(sseg, addr + 3, 0x12);

    sm = map_seg_to_bank(sseg);
    dm = map_seg_to_bank(dseg);
    if (bankfunc[sm][dm] == 0)
        find_bank_function(sm, dm);
    /* 6809 is big endian */
    writeb(sseg, addr + 1, bankfunc[sm][dm] >> 8);
    writeb(sseg, addr + 2, bankfunc[sm][dm]);
}

static void stub_6809(unsigned dseg, uint16_t addr)
{
    /* Write record */
    /* LDX #xxxx JMP func */
    writeb(stubseg, stub_next, 0x8E);
    writeb(stubseg, stub_next + 1, addr >> 8);
    writeb(stubseg, stub_next + 2, addr);
    /* JMP stubfunc */
    writeb(stubseg, stub_next + 3, 0x7E);
    writeb(stubseg, stub_next + 4, stubfunc[dseg] >> 8);
    writeb(stubseg, stub_next + 5, stubfunc[dseg]);
}

static unsigned fixup_8070(unsigned sseg, unsigned dseg, uint16_t addr)
{
    int i;
    unsigned sm, dm;
    /* Points to high byte so back pedal for little endian */
    addr--;
    /* Look for PUSH EA JSR xxxx POP EA */
    if (readb(sseg, addr - 2) != 0x08 ||
        readb(sseg, addr - 1) != 0x20 || readb(sseg, addr + 2) != 0x3A) {
        fprintf(stderr, "Unable to auto-relocate %u:%04X\n", sseg, addr);
        for (i = -2; i <= 2; i++)
            printf("%02X ", readb(sseg, addr + i));
        printf("\n");
        exit(1);
    }
    /* Ok it's a valid block */
    writeb(sseg, addr + 2, readb(sseg, addr + 1));
    writeb(sseg, addr + 1, readb(sseg, addr));
    writeb(sseg, addr - 2, 0x20);

    sm = map_seg_to_bank(sseg);
    dm = map_seg_to_bank(dseg);
    if (bankfunc[sm][dm] == 0)
        find_bank_function(sm, dm);
    /* 8070 does increment and PC fetch so the jumps point to the address
       before the target */
    writeb(sseg, addr - 1, (bankfunc[sm][dm] - 1));
    writeb(sseg, addr, (bankfunc[sm][dm] - 1) >> 8);
}

static void stub_8070(unsigned dseg, uint16_t addr)
{
    /* Write record */
    /* LD P2,=addr */
    addr--;	/* As we need to point to the byte before */
    writeb(stubseg, stub_next, 0x26);
    writeb(stubseg, stub_next + 1, addr);
    writeb(stubseg, stub_next + 2, addr >> 8);
    /* JP stubfunc */
    writeb(stubseg, stub_next + 3, 0x24);
    writeb(stubseg, stub_next + 4, stubfunc[dseg] - 1);
    writeb(stubseg, stub_next + 5, (stubfunc[dseg] - 1) >> 8);
}

static unsigned fixup_8080(unsigned sseg, unsigned dseg, uint16_t addr)
{
    int i;
    unsigned sm, dm;
    uint8_t c;
    /* Points to high byte so back pedal for little endian */
    addr--;
    c = readb(sseg, addr - 1);
    /* For 8080/8085/Z80 this should have PUSH AF CALL in front */
    if (readb(sseg, addr - 2) != 0xF5 || c != 0xCD || readb(sseg, addr + 2) != 0xF1) {
        /* Direct call - so an error ? */
        if ((c & 0xC7) == 0xC2 || (c & 0xC7) == 0xC5 ||
            c == 0xCD || c == 0xC3) {
                fprintf(stderr, "Unable to auto-relocate %u:%04X\n", sseg, addr);
                for (i = -2; i <= 2; i++)
                    printf("%02X ", readb(sseg, addr + i));
                printf("\n");
                exit(1);
        }
        return 0;
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
    return 1;
}

static void stub_8080(unsigned dseg, uint16_t addr)
{
    /* Write record */
    /* LD DE,addr */
    writeb(stubseg, stub_next, 0x11);
    writeb(stubseg, stub_next + 1, addr);
    writeb(stubseg, stub_next + 2, addr >> 8);
    /* JP stubfunc */
    writeb(stubseg, stub_next + 3, 0xC3);
    writeb(stubseg, stub_next + 4, stubfunc[dseg]);
    writeb(stubseg, stub_next + 5, stubfunc[dseg] >> 8);
}

static uint16_t find_stub(unsigned sseg, unsigned dseg, uint16_t addr)
{
    struct stub *s = stubs;
    unsigned ds;
    while(s < stubs + nstub) {
        if (s->addr == addr && s->sseg == sseg && s->dseg == dseg)
            return s->stub;
        s++;
    }
    /* No luck */
    if (stub_next + cpu->stub_size >= stub_end) {
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
    ds = map_seg_to_bank(dseg);
    if (stubfunc[ds] == 0)
        find_stub_function(ds);
    cpu->stub(ds, addr);
    nstub++;
    stub_next += cpu->stub_size;
    printf("New stub in segment %c for routine at %c:%04x is %04X\n",
      segname[sseg], segname[dseg], addr, s->stub);
    return s->stub;
}

static void insert_stub(unsigned sseg, unsigned dseg, uint16_t addr)
{
    uint16_t dp = readb(sseg, addr - 1) | (readb(sseg, addr) << 8);
    uint16_t stub = find_stub(sseg, dseg, dp);
    writeb(sseg, addr - 1, stub);
    writeb(sseg, addr, stub >> 8);
    printf("Adding stub in segment %c for routine at %c:%04x\n",
      segname[sseg], segname[dseg], addr);
}

static void process_reloc(int fd)
{
    unsigned sseg, dseg;
    uint8_t c[3];
    unsigned r;

    while(read(fd, c, 3) == 3) {
        switch(c[0]) {
        /* ZP relocations are uninteresting */
        case 0:
            break;
        case 1:
            if (is_code(sseg)) {
                /* Code calling code or referencing data in the same */
                if (sseg == dseg || (!is_overlay(sseg) && !is_overlay(dseg)))
                    continue;
                /* If the code being called is always present then we will
                   not use helpers. However this means any call out from
                   common code must use a helper when calling banked code */
                if (!is_overlay(dseg))
                    continue;
                /* Code referencing data (unbanked) */
                if (!is_code(dseg))
                    continue;
                /* Code is always mapped and our base segment */
                printf("Fixup from in seg %u ref %u\n",
                    sseg, dseg);
                r = cpu->fixup(sseg, dseg, (c[1] << 8) | c[2]);
                /* Fix up error */
                /* Was fixed up */
                if (r)
                    continue;
                /* Data reference literal in code (eg ld hl,#_funcname)
                   so stub it */
                insert_stub(sseg, dseg, (c[1] << 8) | c[2]);
            } else {
                /* Source is a data reference to something */
                if (is_overlay(dseg) && is_code(dseg))	/* Symbol reference to code */
                        insert_stub(sseg, dseg, (c[1] << 8) | c[2]);
                    /* Data to data references are unbanked and don't matter */
            }
            break;
        case 2:
            /* Sets the segmenmts for reloc 1 above */
            sseg = c[1];
            dseg = c[2];
            if (sseg > 13 || dseg > 13) {
                fprintf(stderr, "Invalid relocation segment record %02X %02X\n",
                    c[1], c[2]);
                exit(1);
            }
            continue;
        default:
            fprintf(stderr, "Unknown reloc record %u\n", c[0]);
            exit(1);
        }
    }
}

static struct cpu cputab[] = {
    { "6303", 6, stub_6803, fixup_6803 },
    { "6803", 6, stub_6803, fixup_6803 },
    { "6809", 6, stub_6809, fixup_6809 },
    { "8070", 6, stub_8070, fixup_8070 },
    { "8080", 6, stub_8080, fixup_8080 },
    { "8085", 6, stub_8080, fixup_8080 },
    { "z80", 6, stub_8080, fixup_8080 }, /* We only use 8080 ops for this */
    { NULL }
};

static struct cpu *find_cpu(const char *p)
{
    register struct cpu *cpu = cputab;
    while(cpu->name) {
        if (strcmp(cpu->name, p) == 0)
            return cpu;
        cpu++;
    }
    return NULL;
}

void usage(void)
{
    puts("bankman binary map relocs cpu");
    exit(1);
}

int main(int argc, char *argv[])
{
    int reloc;
    unsigned dummy;

    if (argc != 5)
        usage();

    cpu = find_cpu(argv[4]);
    if (cpu == NULL) {
        fprintf(stderr, "%s: unsupported CPU '%s'.\n", argv[0], argv[4]);
        return 1;
    }
    bin_fd = open(argv[1], O_RDWR);
    if (bin_fd == -1) {
        perror(argv[1]);
        return 1;
    }
    map = fopen(argv[2], "r");
    if (map == NULL) {
        perror(argv[2]);
        return 1;
    }

    reloc = open(argv[3], O_RDONLY);
    if (reloc == -1) {
        perror(argv[3]);
        return 1;
    }

    if (find_symbol("stub", &stubseg, &stub_next) == 0 ||
        find_symbol("stub_end", &dummy, &stub_end) == 0) {
        fprintf(stderr, "%s: missing stub symbols.\n", argv[2]);
        return 1;
    }
    printf("Stubs from %04X to %04X, max of %u stubs.\n",
        stub_next, stub_end, (stub_end - stub_next) / cpu->stub_size);

    process_reloc(reloc);
    printf("%u stubs used.\n", nstub);
    return 0;
}
