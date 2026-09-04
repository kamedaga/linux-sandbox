/* SPDX-License-Identifier: GPL-2.0-only */

/* Linux sections whose ordering or boundary symbols are part of live code. */
SECTIONS
{
	.data..percpu : ALIGN(4096)
	{
		__per_cpu_start = .;
		*(.data..percpu..page_aligned)
		. = ALIGN(64);
		__per_cpu_hot_start = .;
		*(SORT_BY_ALIGNMENT(.data..percpu..hot.*))
		__per_cpu_hot_end = .;
		. = ALIGN(64);
		*(.data..percpu..read_mostly)
		. = ALIGN(64);
		*(.data..percpu)
		*(.data..percpu..shared_aligned)
		__per_cpu_end = .;
	}

	.kobox_init_stack (NOLOAD) : ALIGN(16384)
	{
		__start_init_stack = .;
		init_thread_union = .;
		init_stack = .;
		. += 16384;
		__end_init_stack = .;
		__top_init_kernel_stack = __end_init_stack - 168;
	}

	.kobox_sched_classes : ALIGN(32)
	{
		__sched_class_highest = .;
		KEEP(*(__stop_sched_class))
		KEEP(*(__dl_sched_class))
		KEEP(*(__rt_sched_class))
		KEEP(*(__fair_sched_class))
		KEEP(*(__ext_sched_class))
		KEEP(*(__idle_sched_class))
		__sched_class_lowest = .;
	}

	.cpuidle.text : ALIGN(16)
	{
		__cpuidle_text_start = .;
		*(.cpuidle.text)
		__cpuidle_text_end = .;
	}

	.kobox_init_bounds : ALIGN(16)
	{
		__init_begin = .;
		_sinittext = .;
		_einittext = .;
		__init_end = .;
	}

	.x86_cpu_dev.init : ALIGN(8)
	{
		__x86_cpu_dev_start = .;
		__x86_cpu_dev_end = .;
	}

	runtime_ptr_USER_PTR_MAX : ALIGN(8)
	{
		__start_runtime_ptr_USER_PTR_MAX = .;
		KEEP(*(runtime_ptr_USER_PTR_MAX))
		__stop_runtime_ptr_USER_PTR_MAX = .;
	}

	runtime_ptr_dentry_hashtable : ALIGN(8)
	{
		__start_runtime_ptr_dentry_hashtable = .;
		KEEP(*(runtime_ptr_dentry_hashtable))
		__stop_runtime_ptr_dentry_hashtable = .;
	}

	runtime_shift_d_hash_shift : ALIGN(8)
	{
		__start_runtime_shift_d_hash_shift = .;
		KEEP(*(runtime_shift_d_hash_shift))
		__stop_runtime_shift_d_hash_shift = .;
	}

	.brk : ALIGN(4096)
	{
		__brk_base = .;
		*(.bss..brk)
		__brk_limit = .;
	}

	/DISCARD/ :
	{
		*(.discard.*)
		*(.export_symbol)
	}
}
INSERT AFTER .data;

_text = ADDR(.text);
_stext = ADDR(.text);
_sdata = ADDR(.data);
__start_rodata = ADDR(.rodata);
__end_rodata = ADDR(.rodata) + SIZEOF(.rodata);
__end_rodata_hpage_align = __end_rodata;
__bss_stop = ADDR(.bss) + SIZEOF(.bss);
__end_of_kernel_reserve = __bss_stop;
