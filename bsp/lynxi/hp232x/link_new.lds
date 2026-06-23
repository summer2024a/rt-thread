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
     * HP232X / KA200 — two-segment IRAM layout
     *
     * IRAM0: 0x04000000 ~ 0x0407FFFF (512KB) — kernel text + data
     * IRAM1: 0x100000000 ~ 0x10007FFFF (512KB) — bss + heap + stack
     *
    * For PCIe/HAPS boot mode, bootloader expects code starting at 0x04000000
    * No 32-byte header offset needed for this boot method
     */
    . = 0x04000000;

    .text :
    {
        PROVIDE(__text_start = .);
        KEEP(*(.text.entrypoint))
        KEEP(*(.vectors))
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
