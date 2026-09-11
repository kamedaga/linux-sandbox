// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <asm/futex.h>
#include <asm/ptrace.h>
#include "../arch/x86_64/user_layout.h"

long __x64_sys_mprotect(const struct pt_regs *regs);

/* Run inside a real Linux user task. Native process pointers must fail even
 * when their numeric address is readable/writable in the runtime process.
 */
static int exercise(unsigned long base, unsigned int *line)
{
	char input[16] = "user page bytes", output[16];
	u64 value = 0, sentinel = 0xaabbccddeeff0011ULL;
	u32 old;
	unsigned long alias;
	int result, previous;
	struct pt_regs regs = {.di = base, .si = 3 * PAGE_SIZE, .dx = PROT_READ};

#define CHECK(condition) do { \
	if (!(condition)) { \
		*line = __LINE__; \
		return -EINVAL; \
	} \
} while (0)

	/* No-fault access must not populate an absent anonymous page. */
	pagefault_disable();
	result = get_user(value, (u64 __user *)base);
	pagefault_enable();
	CHECK(result == -EFAULT && !value);
	CHECK(!put_user(sentinel, (u64 __user *)base));
	pagefault_disable();
	result = get_user(value, (const u64 __user *)base);
	pagefault_enable();
	CHECK(!result && value == sentinel);
	CHECK(!copy_to_user((void __user *)(base + PAGE_SIZE - 8), input, sizeof(input)));
	CHECK(!copy_from_user(output, (void __user *)(base + PAGE_SIZE - 8), sizeof(output)));
	CHECK(!memcmp(input, output, sizeof(input)));
	CHECK(!copy_from_user_inatomic_nontemporal(output,
			(void __user *)(base + PAGE_SIZE - 8), sizeof(output)));
	CHECK(!memcmp(input, output, sizeof(input)));
	CHECK(!copy_from_user_flushcache(output,
			(void __user *)(base + PAGE_SIZE - 8), sizeof(output)));
	CHECK(!memcmp(input, output, sizeof(input)));

	memset(output, 0x55, sizeof(output));
	CHECK(raw_copy_from_user(output, (void __user *)&sentinel, sizeof(sentinel)) == sizeof(sentinel));
	CHECK(output[0] == 0x55);
	CHECK(copy_from_user(output, (void __user *)&sentinel, sizeof(sentinel)) == sizeof(sentinel));
	CHECK(!memchr_inv(output, 0, sizeof(sentinel)));
	CHECK(put_user(0, (u64 __user *)&sentinel) == -EFAULT);
	CHECK(sentinel == 0xaabbccddeeff0011ULL);
	CHECK(get_user(value, (u64 __user *)~0UL) == -EFAULT && !value);
	CHECK(raw_copy_to_user((void __user *)(TASK_SIZE - 4), input, 8) == 8);
	/* Native bootstrap pointers can pass the CPU's canonical-address
	 * clamp. That must never grant a client access beyond its own MM.
	 */
	CHECK(TASK_SIZE == KOBOX_X86_USER_END);
	CHECK(get_user(value, (u64 __user *)KOBOX_X86_USER_END) == -EFAULT);
	CHECK(copy_from_user(output, (void __user *)KOBOX_X86_USER_END,
			     sizeof(output)) == sizeof(output));
	CHECK(!memchr_inv(output, 0, sizeof(output)));
	CHECK(strncpy_from_user(output, (void __user *)KOBOX_X86_USER_END,
			       sizeof(output)) == -EFAULT);
	CHECK(!strnlen_user((void __user *)KOBOX_X86_USER_END, sizeof(output)));
	alias = vm_mmap(NULL, KOBOX_X86_USER_END, PAGE_SIZE,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
	CHECK(IS_ERR_VALUE(alias));
	/* The same numeric VA can name different memory in client and core.
	 * A valid client mapping must never turn into a native-pointer copy.
	 */
	alias = vm_mmap(NULL, (unsigned long)&sentinel & PAGE_MASK, PAGE_SIZE,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
	CHECK(!IS_ERR_VALUE(alias));
	result = put_user(42ULL, (u64 __user *)&sentinel);
	if (!result)
		result = get_user(value, (const u64 __user *)&sentinel);
	CHECK(!vm_munmap(alias, PAGE_SIZE));
	CHECK(!result && value == 42 && sentinel == 0xaabbccddeeff0011ULL);

	CHECK(!put_user(5, (u32 __user *)base));
	pagefault_disable();
	result = futex_atomic_cmpxchg_inatomic(&old, (u32 __user *)base, 5, 9);
	pagefault_enable();
	CHECK(!result && old == 5);
	pagefault_disable();
	result = futex_atomic_cmpxchg_inatomic(&old, (u32 __user *)base, 5, 13);
	pagefault_enable();
	CHECK(!result && old == 9);
	pagefault_disable();
	result = arch_futex_atomic_op_inuser(FUTEX_OP_ADD, 7, &previous, (u32 __user *)base);
	pagefault_enable();
	CHECK(!result && previous == 9);
	CHECK(!get_user(old, (u32 __user *)base) && old == 16);
	CHECK(futex_atomic_cmpxchg_inatomic(&old, (u32 __user *)(base + 2 * PAGE_SIZE), 0, 1) == -EFAULT);
	CHECK(!get_user(old, (u32 __user *)(base + 2 * PAGE_SIZE)) && !old);
	CHECK(!put_user(77, (u32 __user *)(base + 2 * PAGE_SIZE)));
	CHECK(!get_user(old, (u32 __user *)(base + 2 * PAGE_SIZE)) && old == 77);

	CHECK(!__x64_sys_mprotect(&regs));
	CHECK(put_user(7, (u32 __user *)base) == -EFAULT);
	CHECK(!get_user(old, (u32 __user *)base) && old == 16);
	CHECK(clear_user((void __user *)base, 4) == 4);
	regs.dx = PROT_READ | PROT_WRITE;
	CHECK(!__x64_sys_mprotect(&regs));
	CHECK(!vm_munmap(base + PAGE_SIZE, PAGE_SIZE));
	CHECK(copy_to_user((void __user *)(base + PAGE_SIZE - 8), input, sizeof(input)) == 8);
	memset(output, 0x55, sizeof(output));
	CHECK(copy_from_user(output, (void __user *)(base + PAGE_SIZE - 8), sizeof(output)) == 8);
	CHECK(!memcmp(input, output, 8) && !memchr_inv(output + 8, 0, 8));
	value = ~0ULL;
	CHECK(get_user(value, (u64 __user *)(base + PAGE_SIZE - 4)) == -EFAULT && !value);
	CHECK(clear_user((void __user *)(base + PAGE_SIZE - 4), 8) == 4);
	CHECK(!put_user(0x00636261U, (u32 __user *)(base + PAGE_SIZE - 4)));
	CHECK(strncpy_from_user(output, (const char __user *)(base + PAGE_SIZE - 4), sizeof(output)) == 3);
	CHECK(!strcmp(output, "abc"));
	CHECK(strnlen_user((const char __user *)(base + PAGE_SIZE - 4), sizeof(output)) == 4);
	CHECK(strncpy_from_user(output, (const char __user *)&sentinel, sizeof(output)) == -EFAULT);
	CHECK(!strnlen_user((const char __user *)&sentinel, sizeof(output)));
	return 0;
#undef CHECK
}

int kobox_linux_uaccess_probe(unsigned int *line)
{
	unsigned long base;
	int result, cleanup;

	base = vm_mmap(NULL, 0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(base))
		return (long)base;
	result = exercise(base, line);
	cleanup = vm_munmap(base, 3 * PAGE_SIZE);
	return result ?: cleanup;
}
