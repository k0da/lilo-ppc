/*  $Id$ */
/*
 * Copyright (C) Paul Mackerras 1997.
 *
 * Updates for PPC64 by Todd Inglett, Dave Engebretsen & Peter Bergner.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <stdarg.h>
#include <stddef.h>
#include <elf.h>
#include <page.h>
#include <string.h>
#include <stdio.h>
#include <prom.h>
#include "zlib.h"

static void do_gunzip(void *, int, unsigned char *, int *);
extern void flush_cache(void *, unsigned long);


/* Value picked to match that used by yaboot */
#define PROG_START	0x01400000
#define RAM_END		(256<<20) // Fixme: use OF */

static char *avail_ram;
static char *begin_avail, *end_avail;
static char *avail_high;
static unsigned int heap_use;
static unsigned int heap_max;

extern char _start[];
extern char _vmlinux_start[];
extern char _vmlinux_end[];
extern char _initrd_start[];
extern char _initrd_end[];
void *_vmlinux_filesize;
void *_vmlinux_memsize;

struct addr_range {
	unsigned long addr;
	unsigned long size;
	unsigned long memsize;
	unsigned long offset;
};
static struct addr_range vmlinux = {0, 0, 0};
static struct addr_range vmlinuz = {0, 0, 0};
static struct addr_range initrd  = {0, 0, 0};

static char scratch[128<<10];	/* 128kB of scratch space for gunzip */

typedef void (*kernel_entry_t)( unsigned long,
                                unsigned long,
                                void *,
				void *);


#undef DEBUG

#define cmdline_start_string   "cmd_line_start"
#define cmdline_end_string     "cmd_line_end"
struct _builtin_cmd_line {
	unsigned char prefer;
	unsigned char cmdling_start_flag[sizeof(cmdline_start_string)-1]; /* without trailing zero */
	unsigned char string[512]; /* COMMAND_LINE_SIZE */
	unsigned char cmdline_end_flag[sizeof(cmdline_end_string)]; /* with trailing zero */
} __attribute__ ((__packed__));

struct _builtin_cmd_line  __attribute__ ((__section__ (".kernel:cmdline"))) _builtin_cmd_line = {
	.prefer = '0',
	.cmdling_start_flag = cmdline_start_string,
	.string = "",
	.cmdline_end_flag = cmdline_end_string,
};

static int check_elf64(void *p)
{
	Elf64_Ehdr *elf64 = p;
	Elf64_Phdr *elf64ph;
	int i;

	if (elf64->e_ident[EI_MAG0] != ELFMAG0 ||
	    elf64->e_ident[EI_MAG1] != ELFMAG1 ||
	    elf64->e_ident[EI_MAG2] != ELFMAG2 ||
	    elf64->e_ident[EI_MAG3] != ELFMAG3 ||
	    elf64->e_ident[EI_CLASS] != ELFCLASS64 ||
	    elf64->e_ident[EI_DATA] != ELFDATA2MSB ||
	    elf64->e_type != ET_EXEC || elf64->e_machine != EM_PPC64)
		return 0;

	elf64ph = (Elf64_Phdr *) ((unsigned long)elf64 +
				  (unsigned long)elf64->e_phoff);

	for (i = 0; i < (unsigned int)elf64->e_phnum; i++, elf64ph++)
		if (elf64ph->p_type == PT_LOAD && elf64ph->p_offset != 0)
			break;

	vmlinux.memsize = (unsigned long)elf64ph->p_memsz;
	vmlinux.offset = (unsigned long)elf64ph->p_offset;

#ifdef DEBUG
	printf("PPC64 ELF file, ph %d\n\r", i);
	printf("p_type   0x%08x\n\r", elf64ph->p_type);
	printf("p_flags  0x%08x\n\r", elf64ph->p_flags);
	printf("p_offset 0x%08Lx\n\r", elf64ph->p_offset);
	printf("p_vaddr  0x%08Lx\n\r", elf64ph->p_vaddr);
	printf("p_paddr  0x%08Lx\n\r", elf64ph->p_paddr);
	printf("p_filesz 0x%08Lx\n\r", elf64ph->p_filesz);
	printf("p_memsz  0x%08Lx\n\r", elf64ph->p_memsz);
	printf("p_align  0x%08Lx\n\r", elf64ph->p_align);
	printf("... skipping 0x%lx bytes of ELF header\n\r",
	       (unsigned long)elf64ph->p_offset);
#endif

	return 64;
}

static unsigned long claim_base = PROG_START;

static unsigned long try_claim(unsigned long size)
{
	unsigned long addr = 0;

	for(; claim_base < RAM_END; claim_base += 0x100000) {
#ifdef DEBUG
		printf("    trying: 0x%08lx\n\r", claim_base);
#endif
		addr = (unsigned long)claim(claim_base, size, 0);
		if ((void *)addr != (void *)-1)
			break;
	}
	if (addr == 0)
		return 0;
	claim_base = PAGE_ALIGN(claim_base + size);
	return addr;
}

static void gunzip(unsigned long dest, int destlen,
		   unsigned long src, int srclen, const char *what)
{
	int len;
	avail_ram = scratch;
	begin_avail = avail_high = avail_ram;
	end_avail = scratch + sizeof(scratch);
	printf("gunzipping %s (0x%lx:0x%lx <- 0x%lx:0x%0lx) using 0x%p:%0xl...",
	       what, dest, destlen, src, srclen, scratch, sizeof(scratch));
	len = srclen;
	heap_use = heap_max = 0;
	do_gunzip((void *)dest, destlen, (unsigned char *)src, &len);
	printf("done 0x%lx bytes\n\r", len);
	printf("0x%x bytes of heap consumed, max in use 0x%x\n\r",
	       (unsigned)(avail_high - begin_avail), heap_max);
}


void start(unsigned long a1, unsigned long a2, void *promptr)
{
	void *bootcpu_phandle;
	unsigned long vmlinux_memsize, vmlinux_filesize;
	kernel_entry_t kernel_entry;
	int cputype;

	prom = (int (*)(void *)) promptr;
	chosen_handle = finddevice("/chosen");
	if (chosen_handle == (void *) -1)
		exit();
	if (getprop(chosen_handle, "stdout", &stdout, sizeof(stdout)) != 4)
		exit();
	stderr = stdout;
	if (getprop(chosen_handle, "stdin", &stdin, sizeof(stdin)) != 4)
		exit();

	printf("\n\rzImage starting: loaded at 0x%x (0x%lx/0x%lx/0x%p)\n\r", (unsigned)_start,a1,a2,promptr);

	if (getprop(chosen_handle, "mmu", &mmu, sizeof(mmu)) != 4)
		exit();

	if (getprop(chosen_handle, "cpu", &bootcpu, sizeof(bootcpu)) == 4) {
		bootcpu_phandle = instance_to_package(bootcpu);
		if (bootcpu_phandle == (void *)-1)
			exit();

		if (getprop(bootcpu_phandle, "64-bit", NULL, 0) != -1)
			cputype = 64;
		else
			cputype = 32;
	} else
		cputype = 0;
	/*
	 * Now we try to claim some memory for the kernel itself
	 * our "vmlinux_memsize" is the memory footprint in RAM, _HOWEVER_, what
	 * our Makefile stuffs in is an image containing all sort of junk including
	 * an ELF header. We need to do some calculations here to find the right
	 * size... In practice we add 1Mb, that is enough, but we should really
	 * consider fixing the Makefile to put a _raw_ kernel in there !
	 */
	vmlinux_memsize = (unsigned long) &_vmlinux_memsize;
	vmlinux_filesize = (unsigned long) &_vmlinux_filesize;
	vmlinux_memsize += 0x100000;
	printf("Allocating 0x%lx bytes for kernel ...\n\r", vmlinux_memsize);
	vmlinux.addr = try_claim(vmlinux_memsize);
	if (vmlinux.addr == 0) {
		printf("Can't allocate memory for kernel image !\n\r");
		exit();
	}
	vmlinuz.addr = (unsigned long)_vmlinux_start;
	vmlinuz.size = (unsigned long)(_vmlinux_end - _vmlinux_start);
	vmlinux.size = PAGE_ALIGN(vmlinux_filesize);
	vmlinux.memsize = vmlinux_memsize;

	/*
	 * Now we try to claim memory for the initrd (and copy it there)
	 */
	initrd.size = (unsigned long)(_initrd_end - _initrd_start);
	initrd.memsize = initrd.size;
	if ( initrd.size > 0 ) {
		printf("Allocating 0x%lx bytes for initrd ...\n\r", initrd.size);
		initrd.addr = try_claim(initrd.size);
		if (initrd.addr == 0) {
			printf("Can't allocate memory for initial ramdisk !\n\r");
			exit();
		}
		a1 = initrd.addr;
		a2 = initrd.size;
		printf("initial ramdisk moving 0x%lx <- 0x%lx (0x%lx bytes)\n\r",
		       initrd.addr, (unsigned long)_initrd_start, initrd.size);
		memmove((void *)initrd.addr, (void *)_initrd_start, initrd.size);
		printf("initrd head: 0x%lx\n\r", *((unsigned long *)initrd.addr));
	}

	/* Eventually gunzip the kernel */
	if (*(unsigned short *)vmlinuz.addr == 0x1f8b)
		gunzip(vmlinux.addr, vmlinux.size, vmlinuz.addr, vmlinuz.size,
		       "kernel");
	else
		memmove((void *)vmlinux.addr, (void *)vmlinuz.addr,
			vmlinuz.size);

	if ( _builtin_cmd_line.prefer && _builtin_cmd_line.prefer != '0' ) {
		int l = strlen (_builtin_cmd_line.string)+1;
		printf("copy built-in cmdline(%d) %s\n\r",l,_builtin_cmd_line.string);
		l = (int)setprop( chosen_handle, "bootargs", _builtin_cmd_line.string, l);
		printf ("setprop bootargs: %d\n\r",l);
	}

	if (!check_elf64((void *)vmlinux.addr)) {
		printf("Error: not a valid PPC64 ELF file!\n\r");
		exit();
	}

	flush_cache((void *)vmlinux.addr, vmlinux.size);

	kernel_entry = (kernel_entry_t)(vmlinux.addr + vmlinux.offset);
#ifdef DEBUG
	printf( "kernel:\n\r"
		"        entry addr = 0x%lx\n\r"
		"        a1         = 0x%lx,\n\r"
		"        a2         = 0x%lx,\n\r"
		"        prom       = 0x%lx,\n\r"
		"        bi_recs    = 0x%lx,\n\r",
		(unsigned long)kernel_entry, a1, a2,
		(unsigned long)prom, NULL);
#endif

	kernel_entry( a1, a2, prom, NULL );

	printf("Error: Linux kernel returned to zImage bootloader!\n\r");

	exit();
}

struct memchunk {
	unsigned int size;
	unsigned int pad;
	struct memchunk *next;
};

static struct memchunk *freechunks;

void *zalloc(void *x, unsigned items, unsigned size)
{
	void *p;
	struct memchunk **mpp, *mp;

	size *= items;
	size = _ALIGN(size, sizeof(struct memchunk));
	heap_use += size;
	if (heap_use > heap_max)
		heap_max = heap_use;
	for (mpp = &freechunks; (mp = *mpp) != 0; mpp = &mp->next) {
		if (mp->size == size) {
			*mpp = mp->next;
			return mp;
		}
	}
	p = avail_ram;
	avail_ram += size;
	if (avail_ram > avail_high)
		avail_high = avail_ram;
	if (avail_ram > end_avail) {
		printf("oops... out of memory\n\r");
		pause();
	}
	return p;
}

void zfree(void *x, void *addr, unsigned nb)
{
	struct memchunk *mp = addr;

	nb = _ALIGN(nb, sizeof(struct memchunk));
	heap_use -= nb;
	if (avail_ram == addr + nb) {
		avail_ram = addr;
		return;
	}
	mp->size = nb;
	mp->next = freechunks;
	freechunks = mp;
}

#define HEAD_CRC	2
#define EXTRA_FIELD	4
#define ORIG_NAME	8
#define COMMENT		0x10
#define RESERVED	0xe0

#define DEFLATED	8

static void do_gunzip(void *dst, int dstlen, unsigned char *src, int *lenp)
{
	z_stream s;
	int r, i, flags;

	/* skip header */
	i = 10;
	flags = src[3];
	if (src[2] != DEFLATED || (flags & RESERVED) != 0) {
		printf("bad gzipped data\n\r");
		exit();
	}
	if ((flags & EXTRA_FIELD) != 0)
		i = 12 + src[10] + (src[11] << 8);
	if ((flags & ORIG_NAME) != 0)
		while (src[i++] != 0)
			;
	if ((flags & COMMENT) != 0)
		while (src[i++] != 0)
			;
	if ((flags & HEAD_CRC) != 0)
		i += 2;
	if (i >= *lenp) {
		printf("gunzip: ran out of data in header\n\r");
		exit();
	}

	s.zalloc = zalloc;
	s.zfree = zfree;
	r = inflateInit2(&s, -MAX_WBITS);
	if (r != Z_OK) {
		printf("inflateInit2 returned %d\n\r", r);
		exit();
	}
	s.next_in = src + i;
	s.avail_in = *lenp - i;
	s.next_out = dst;
	s.avail_out = dstlen;
	r = inflate(&s, Z_FINISH);
	if (r != Z_OK && r != Z_STREAM_END) {
		printf("inflate returned %d msg: %s\n\r", r, s.msg);
		exit();
	}
	*lenp = s.next_out - (unsigned char *) dst;
	inflateEnd(&s);
}

