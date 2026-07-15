OUTPUT_FORMAT("elf64-littleaarch64", "elf64-littleaarch64", "elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    stack PT_LOAD FLAGS(6);
}
SECTIONS
{
    /*
     * HP232X / KA200 - two-segment IRAM layout (Memory Constraints Applied)
     *
     * IRAM0: 0x04000000 ~ 0x0403FFFF (first 256KB)
     *   BL2_BOOT:  kernel text + data + mmu_table (IRAM0_KERNEL_BASE)
     *   BL3_BOOT:  RESERVED (bootwrapper + SPL space)
     * IRAM0: 0x04040000 ~ 0x0407FFFF (last 256KB)
     *   BL2_BOOT:  RESERVED
     *   BL3_BOOT:  kernel text + data (IRAM0_KERNEL_BASE)
     *
     * IRAM1: 0x100000000 ~ 0x10003FFFF (first 256KB reserved, unusable)
     * IRAM1: 0x100040000 ~ 0x10007FFFF (last 256KB usable, bss + heap + stack)
     *
     * For EVB PCIe boot mode (BL2_BOOT):
     * - Bootcode loads image at 0x04000000
     * - 32-byte header for bootcode metadata/checks at 0x04000000
     * - Actual code entry at 0x04000020
     * - bootcode reads header, verifies, jumps to 0x04000020
     */

    /* Kernel base address: IRAM0_KERNEL_BASE passed via --defsym from build system.
     * IRAM0_KERNEL_BASE already includes the 32-byte bootcode header offset (+0x20). */
    . = IRAM0_KERNEL_BASE;
    .head :
    {
        KEEP(*(.text.entrypoint))
        . = ALIGN(8);
    } :text

    /* Reserved space for bootcode header in EVB mode
     * Skip 32 bytes (0x20) to allow bootcode to place header at kernel base
     * Actual code starts at KERNEL_VADDR_START for EVB/PCIe boot */
    . = . + 0x20;

    .text :
    {
        PROVIDE(__text_start = .);
        *(.vectors)
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        *(.gnu.linkonce.t*)
        . = ALIGN(8);
        PROVIDE(__rt_utest_tc_tab_start = .);
        KEEP(*(UtestTcTab))
        PROVIDE(__rt_utest_tc_tab_end = .);
        . = ALIGN(8);
        PROVIDE(__fsymtab_start = .);
        KEEP(*(FSymTab))
        PROVIDE(__fsymtab_end = .);
        . = ALIGN(8);
        PROVIDE(__vsymtab_start = .);
        KEEP(*(VSymTab))
        PROVIDE(__vsymtab_end = .);
        . = ALIGN(8);
        PROVIDE(__rtmsymtab_start = .);
        KEEP(*(RTMSymTab))
        PROVIDE(__rtmsymtab_end = .);
        . = ALIGN(8);
        PROVIDE(__rt_init_start = .);
        KEEP(*(SORT(.rti_fn*)))
        PROVIDE(__rt_init_end = .);
        . = ALIGN(16);
        PROVIDE(__text_end = .);
    } :text

    /* HP232X BL22 mode: .early_hp232x not needed (bootwrapper handles EL transition) */
    /* HP232X BL21 mode: spin_table/stack will be in .bss if pre_entry.S compiled */

    .eh_frame_hdr :
    {
        *(.eh_frame_hdr)
        *(.eh_frame_entry)
    }
    .eh_frame : ONLY_IF_RO { KEEP (*(.eh_frame)) }
    . = ALIGN(8);
    .data :
    {
        *(.data)
        *(.data.*)
        *(.data1)
        *(.data1.*)
        . = ALIGN(16);
        _gp = ABSOLUTE(.);
        *(.sdata)
        *(.sdata.*)
        *(.rel.local)
    } :data
    . = ALIGN(8);
    .ctors :
    {
        PROVIDE(__ctors_start__ = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE(__ctors_end__ = .);
    }
    .dtors :
    {
        PROVIDE(__dtors_start__ = .);
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
        PROVIDE(__dtors_end__ = .);
    }

    . = ALIGN(16);
    /*
     * IRAM1 DMA nocache arena @ 0x100040000 (64KB, hp640 CONFIG_SPL_BSS).
     * MMU: ATTR_NORMAL_NC. Hardware DMA buffers only — keep WB bss separate.
     */
    .dma_nocache IRAM1_DMA_NC_START (NOLOAD) :
    {
        PROVIDE(__dma_nocache_start = .);
        KEEP(*(.bss.dma_nocache))
        KEEP(*(.bss.dma_nocache.*))
        . = ALIGN(64);
        PROVIDE(__dma_nocache_end = .);
    } :data

    /* IRAM1: noclean (stacks, early page, MMU tables) + zeroed bss/heap/stack */
    .bss IRAM1_BASE :
    {
        PROVIDE(__bss_noclean_start = .);
        KEEP(*(.bss.noclean.log_ring))
        *(.bss.noclean.cpus_stack)
        *(.bss.noclean.early_page)
        *(.bss.noclean.hp232x_spin)
        PROVIDE(__mmu_table_start = .);
        KEEP(*(.bss.noclean.mmu_table))
        . = ALIGN(4096);
        PROVIDE(__mmu_table_end = .);
        PROVIDE(__bss_noclean_end = .);
        . = ALIGN(8);
        PROVIDE(__bss_start = .);
        *(.bss)
        *(.bss.*)
        *(.dynbss)
        *(COMMON)
        . = ALIGN(8);
        PROVIDE(__bss_end = .);
    } :data
    _end = .;
    .stab 0 : { *(.stab) }
    .stabstr 0 : { *(.stabstr) }
    .stab.excl 0 : { *(.stab.excl) }
    .stab.exclstr 0 : { *(.stab.exclstr) }
    .stab.index 0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment 0 : { *(.comment) }
    .debug 0 : { *(.debug) }
    .line 0 : { *(.line) }
    .debug_srcinfo 0 : { *(.debug_srcinfo) }
    .debug_sfnames 0 : { *(.debug_sfnames) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_line 0 : { *(.debug_line) }
    .debug_frame 0 : { *(.debug_frame) }
    .debug_str 0 : { *(.debug_str) }
    .debug_loc 0 : { *(.debug_loc) }
    .debug_macinfo 0 : { *(.debug_macinfo) }
    .debug_weaknames 0 : { *(.debug_weaknames) }
    .debug_funcnames 0 : { *(.debug_funcnames) }
    .debug_typenames 0 : { *(.debug_typenames) }
    .debug_varnames 0 : { *(.debug_varnames) }
    __data_size = SIZEOF(.data);
    __bss_size = SIZEOF(.bss);
}
