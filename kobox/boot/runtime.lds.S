/* SPDX-License-Identifier: GPL-2.0-only */

/* Shared-object placement; Linux owns the boot tables and their ordering. */
#define LOAD_OFFSET 0
#define RO_EXCEPTION_TABLE_ALIGN 16
/* These pages become read-only at the architecture boot-end boundary. */
#define RO_AFTER_INIT_DATA

#include <asm-generic/vmlinux.lds.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/page_types.h>
#include <asm/cache.h>

/* Program-header permissions are explicit. Native RO_DATA contains relocated
 * pointers; the loader needs write access until the boot-end protection hook.
 * Inheriting permissions from individual orphan sections can remap one page
 * read-only over a neighbouring relocation target during dlopen().
 */
#undef NOTES_HEADERS
#define NOTES_HEADERS :rodata :note
#undef NOTES_HEADERS_RESTORE
#define NOTES_HEADERS_RESTORE __restore_ph : { *(.__restore_ph) } :rodata

/* The hosted compiler uses function/data sections, including large-model
 * sections. Do not let the ELF linker's orphan placement split Linux ranges.
 */
#undef TEXT_MAIN
#define TEXT_MAIN .text .text.[0-9a-zA-Z_]* .ltext .ltext.*
#undef DATA_MAIN
#define DATA_MAIN .data .data.[0-9a-zA-Z_]* .data.rel.* .ldata .ldata.*
#undef BSS_MAIN
#define BSS_MAIN .bss .bss.[0-9a-zA-Z_]* .lbss .lbss.*

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kobox_linux_boot_start)

PHDRS
{
	headers PT_LOAD FILEHDR PHDRS FLAGS(4);
	text PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(6);
	data PT_LOAD FLAGS(6);
	inittext PT_LOAD FLAGS(5);
	initdata PT_LOAD FLAGS(6);
	loader PT_LOAD FLAGS(6);
	zerodata PT_LOAD FLAGS(6);
	dynamic PT_DYNAMIC FLAGS(6);
	tls PT_TLS FLAGS(4);
	note PT_NOTE FLAGS(4);
	stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
	. = SIZEOF_HEADERS;
	/* The host aliases every PT_LOAD byte, including ELF headers, to RAM. */
	_text = __ehdr_start;
	__pi__text = _text;
	.kobox.image : { BYTE(0); } :headers
	/* Keep loader metadata out of executable pages: overlapping PT_LOAD
	 * permissions on one page would make the text non-executable at dlopen.
	 */
	.dynsym : { *(.dynsym) }
	.gnu.hash : { *(.gnu.hash) }
	.hash : { *(.hash) }
	.dynstr : { *(.dynstr) }
	.gnu.version : { *(.gnu.version) }
	.gnu.version_d : { *(.gnu.version_d) }
	.gnu.version_r : { *(.gnu.version_r) }
	.rela.dyn : { *(.rela.dyn) }
	.rela.plt : { *(.rela.plt) }
	.relr.dyn : { *(.relr.dyn) }
	.text : ALIGN(PAGE_SIZE) {
		_stext = .;
		ENTRY_TEXT
		TEXT_TEXT
		SCHED_TEXT
		LOCK_TEXT
		KPROBES_TEXT
		SOFTIRQENTRY_TEXT
		STATIC_CALL_TEXT
	} :text
	.plt : { *(.plt .iplt) }
	.altinstr_replacement : { *(.altinstr_replacement) }
	_etext = .;

	.lrodata : ALIGN(PAGE_SIZE) {
		*(.lrodata .lrodata.*)
	} :rodata
	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame : { *(.eh_frame) }
	RO_DATA(PAGE_SIZE)
	/* Include large-model constants in Linux's read-only-data range. */
	__start_rodata = ADDR(.lrodata);
	__end_rodata_hpage_align = __end_rodata;
	__end_rodata_aligned = __end_rodata;

	.data.begin : ALIGN(PAGE_SIZE) { BYTE(0); } :data
	PERCPU_SECTION(L1_CACHE_BYTES)
	.data : ALIGN(PAGE_SIZE) {
		_sdata = .;
		INIT_TASK_DATA(THREAD_SIZE)
		__top_init_kernel_stack = __end_init_stack -
			TOP_OF_KERNEL_STACK_PADDING - PTREGS_SIZE;
		PAGE_ALIGNED_DATA(PAGE_SIZE)
		CACHE_HOT_DATA(L1_CACHE_BYTES)
		CACHELINE_ALIGNED_DATA(L1_CACHE_BYTES)
		READ_MOSTLY_DATA(INTERNODE_CACHE_BYTES)
		DATA_DATA
		CONSTRUCTORS
		_edata = .;
	}
	BUG_TABLE

	.data..ro_after_init : ALIGN(PAGE_SIZE) {
		__start_ro_after_init = .;
		*(.data..ro_after_init)
		JUMP_TABLE_DATA
		STATIC_CALL_DATA
		. = ALIGN(PAGE_SIZE);
		__end_ro_after_init = .;
	}

	.altinstructions : ALIGN(8) {
		__alt_instructions = .;
		KEEP(*(.altinstructions))
		__alt_instructions_end = .;
	}
	.apicdrivers : ALIGN(8) {
		__apicdrivers = .;
		KEEP(*(.apicdrivers))
		__apicdrivers_end = .;
	}
	.smp_locks : ALIGN(PAGE_SIZE) {
		__smp_locks = .;
		KEEP(*(.smp_locks))
		. = ALIGN(PAGE_SIZE);
		__smp_locks_end = .;
	}

	/* A real, page-aligned init range, not empty fixture boundary symbols. */
	.init.begin : ALIGN(PAGE_SIZE) {
		__init_begin = .;
	} :inittext
	INIT_TEXT_SECTION(PAGE_SIZE)
	.init.data : ALIGN(PAGE_SIZE) {
		INIT_DATA
		INIT_SETUP(8)
		INIT_CALLS
		CON_INITCALL
		INIT_RAM_FS
		__x86_cpu_dev_start = .;
		KEEP(*(.x86_cpu_dev.init))
		__x86_cpu_dev_end = .;
		. = ALIGN(PAGE_SIZE);
		__init_end = .;
	} :initdata

	.dynamic : ALIGN(PAGE_SIZE) { *(.dynamic) } :loader :dynamic
	.got : { *(.got .got.plt) } :loader
	.tdata : ALIGN(16) { *(.tdata .tdata.*) } :loader :tls
	.tbss : ALIGN(16) { *(.tbss .tbss.*) } :loader :tls
	/* Keep the native early scratch reservation, not empty bound symbols.
	 * Place it before BSS so the existing RAM image bound includes it.
	 */
	.brk (NOLOAD) : ALIGN(PAGE_SIZE) {
		__brk_base = .;
		. += 64 * 1024;
		*(.bss..brk)
		__brk_limit = .;
	} :zerodata
	BSS_SECTION(PAGE_SIZE, PAGE_SIZE, PAGE_SIZE)
	_end = .;
	__pi__end = _end;
	__end_of_kernel_reserve = .;

	/* Unlike a native vmlinux, retain ELF dynamic-link/version metadata. */
	/DISCARD/ : {
		EXIT_TEXT
		EXIT_DATA
		EXIT_CALL
		*(.discard .discard.*)
		*(.export_symbol .no_trim_symbol)
		*(.note.GNU-stack)
	}
}

jiffies = jiffies_64;
const_current_task = current_task;
const_cpu_current_top_of_stack = cpu_current_top_of_stack;
