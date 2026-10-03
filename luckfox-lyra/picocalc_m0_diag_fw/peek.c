/* peek ADDR [WORDS]  /  peek -w ADDR VALUE : read or write 32-bit words through /dev/mem */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf jb;
static void onbus(int s) { (void)s; siglongjmp(jb, 1); }

int main(int argc, char **argv)
{
	int wr = argc > 1 && !strcmp(argv[1], "-w");
	unsigned long addr, n, val, i;
	volatile uint32_t *p;
	int fd;

	if (argc < (wr ? 4 : 2)) {
		fprintf(stderr, "usage: peek ADDR [WORDS]\n       peek -w ADDR VALUE\n");
		return 1;
	}
	addr = strtoul(argv[wr ? 2 : 1], 0, 0);
	n = wr ? 1 : (argc > 2 ? strtoul(argv[2], 0, 0) : 8);
	val = wr ? strtoul(argv[3], 0, 0) : 0;
	fd = open("/dev/mem", (wr ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	p = mmap(0, 0x4000, wr ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, addr & ~0xFFFUL);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	p = (volatile uint32_t *)((char *)p + (addr & 0xFFF));
	signal(SIGBUS, onbus);
	signal(SIGSEGV, onbus);
	if (sigsetjmp(jb, 1)) { printf("\nBUS ERROR\n"); return 2; }
	if (wr) { p[0] = val; printf("%08lx <- %08lx, reads back %08x\n", addr, val, p[0]); return 0; }
	for (i = 0; i < n; i++) {
		if (i % 8 == 0) printf("%s%08lx:", i ? "\n" : "", addr + 4 * i);
		printf(" %08x", p[i]);
	}
	printf("\n");
	return 0;
}
