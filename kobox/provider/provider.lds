/* SPDX-License-Identifier: GPL-2.0-only */

/* Linux sections whose ordering or boundary symbols are part of live code. */
SECTIONS
{
	.ltext : ALIGN(16)
	{
		*(.ltext)
		*(.ltext.*)
	}

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

	.altinstructions : ALIGN(8)
	{
		__alt_instructions = .;
		KEEP(*(.altinstructions))
		__alt_instructions_end = .;
	}

	.altinstr_replacement : ALIGN(1)
	{
		KEEP(*(.altinstr_replacement))
	}

	.smp_locks : ALIGN(4096)
	{
		__smp_locks = .;
		KEEP(*(.smp_locks))
		. = ALIGN(4096);
		__smp_locks_end = .;
	}

	__ex_table : ALIGN(8)
	{
		__start___ex_table = .;
		KEEP(*(__ex_table))
		__stop___ex_table = .;
	}

	__param : ALIGN(8)
	{
		__start___param = .;
		KEEP(*(__param))
		__stop___param = .;
	}

	.init.setup : ALIGN(8)
	{
		__setup_start = .;
		KEEP(*(.init.setup))
		__setup_end = .;
	}

	.kobox_initcalls : ALIGN(8)
	{
		__initcall_start = .;
		KEEP(*(.initcallearly.init))
		__initcall0_start = .;
		KEEP(*(.initcall0.init))
		KEEP(*(.initcall0s.init))
		__initcall1_start = .;
		KEEP(*(.initcall1.init))
		KEEP(*(.initcall1s.init))
		__initcall2_start = .;
		KEEP(*(.initcall2.init))
		KEEP(*(.initcall2s.init))
		__initcall3_start = .;
		KEEP(*(.initcall3.init))
		KEEP(*(.initcall3s.init))
		__initcall4_start = .;
		KEEP(*(.initcall4.init))
		KEEP(*(.initcall4s.init))
		__initcall5_start = .;
		KEEP(*(.initcall5.init))
		KEEP(*(.initcall5s.init))
		__initcallrootfs_start = .;
		KEEP(*(.initcallrootfs.init))
		KEEP(*(.initcallrootfss.init))
		__initcall6_start = .;
		KEEP(*(.initcall6.init))
		KEEP(*(.initcall6s.init))
		__initcall7_start = .;
		KEEP(*(.initcall7.init))
		KEEP(*(.initcall7s.init))
		__initcall_end = .;
	}

	.con_initcall.init : ALIGN(8)
	{
		__con_initcall_start = .;
		KEEP(*(.con_initcall.init))
		__con_initcall_end = .;
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
	_brk_end = __brk_limit;

	/DISCARD/ :
	{
		*(.discard.*)
		*(.export_symbol)
	}
}
INSERT AFTER .data;

_text = ADDR(.ltext);
_stext = ADDR(.ltext);
_etext = ADDR(.ltext) + SIZEOF(.ltext);
_sdata = ADDR(.data);
_edata = ADDR(.data) + SIZEOF(.data);
__start_rodata = ADDR(.rodata);
__end_rodata = ADDR(.rodata) + SIZEOF(.rodata);
__end_rodata_hpage_align = __end_rodata;
__bss_stop = ADDR(.bss) + SIZEOF(.bss);
_end = __bss_stop;
__end_of_kernel_reserve = __bss_stop;
const_current_task = current_task;
