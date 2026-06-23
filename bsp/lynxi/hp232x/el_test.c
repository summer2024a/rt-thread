/*
 * EL3→EL2→EL1 transition test for HP232X
 * Based on bootloader bootwrapper reference
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>

#define TEST_UART_BASE 0x10006000
#define TEST_UART_LSR  0x14
#define TEST_UART_THR  0x00

static void test_putc(char c) {
    volatile uint32_t *uart = (volatile uint32_t *)TEST_UART_BASE;
    while (!(uart[TEST_UART_LSR / sizeof(uint32_t)] & 0x60));
    uart[TEST_UART_THR / sizeof(uint32_t)] = c;
}

static void test_debug(force_type, const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    for (int i = 0; buf[i] != '\0'; i++) {
        test_putc(buf[i]);
    }
    test_putc('\n');
    test_putc('\r');
}

static void el_check(void) {
    uint64_t currentel;
    
    /* Get current EL - requires reading CurrentEL */
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(currentel));
    
    uint8_t el = ((currentel >> 2) & 0x3);
    
    test_debug("Current EL: %d (CurrentEL=0x%llx)", el, currentel);
}

void el_test_entry(void) {
    test_putc('T');  /* Test entry point reached */
    
    el_check();
    
    test_putc('D');  /* Debug done */
    
    while (1) {
        __asm__ volatile("wfi");
    }
}