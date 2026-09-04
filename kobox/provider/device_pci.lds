/* SPDX-License-Identifier: GPL-2.0-only */

SECTIONS
{
	.kobox_pci_initcalls : ALIGN(4)
	{
		__kobox_pci_initcall2_start = .;
		KEEP(*(.initcall2.init))
		__kobox_pci_initcall2_end = .;
	}

	.kobox_pci_fixups : ALIGN(8)
	{
		__start_pci_fixups_early = .;
		KEEP(*(.pci_fixup_early))
		__end_pci_fixups_early = .;
		__start_pci_fixups_header = .;
		KEEP(*(.pci_fixup_header))
		__end_pci_fixups_header = .;
		__start_pci_fixups_final = .;
		KEEP(*(.pci_fixup_final))
		__end_pci_fixups_final = .;
		__start_pci_fixups_enable = .;
		KEEP(*(.pci_fixup_enable))
		__end_pci_fixups_enable = .;
		__start_pci_fixups_resume = .;
		KEEP(*(.pci_fixup_resume))
		__end_pci_fixups_resume = .;
		__start_pci_fixups_resume_early = .;
		KEEP(*(.pci_fixup_resume_early))
		__end_pci_fixups_resume_early = .;
		__start_pci_fixups_suspend = .;
		KEEP(*(.pci_fixup_suspend))
		__end_pci_fixups_suspend = .;
		__start_pci_fixups_suspend_late = .;
		KEEP(*(.pci_fixup_suspend_late))
		__end_pci_fixups_suspend_late = .;
	}

	/DISCARD/ :
	{
		*(.discard.*)
		*(.export_symbol)
	}
}
INSERT AFTER .rodata;
