# PCIe mode linker script for HP232X KA200
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
    . = 0x04000000;  /* PCIe boot expects start at 0x04000000, NO header offset */
    .text : {
        PROVIDE(__text_start = .);
        KEEP(*(.text.entrypoint))
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(16);
        PROVIDE(__text_end = .);
    } :text
    .data : {
        *(.data*)
    } :data
    .bss 0x100040000 : {
        *(.bss.noclean.*)
        *(.bss*)
        *(COMMON)
    } :data
    _end = .;
}
