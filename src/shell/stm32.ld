/* 
 * linker script
 *
 * Memory layout on my STM32F103 is pretty much like this:
 *
 * 0x00000000 - 0x07ffffff - aliased to flash or sys memory depending on BOOT jumpers
 * 0x08000000 - 0x0800ffff - Flash (64K ... or 128K)
 * 0x1ffff000 - 0x1ffff7ff - Boot firmware in system memory
 * 0x1ffff800 - 0x1fffffff - option bytes
 * 0x20000000 - 0x20004fff - SRAM (20k)
 * 0x40000000 - 0x40023400 - peripherals
 *
 *  flash(RX)  : ORIGIN = 0x00000000, LENGTH = 128K
 */

ENTRY (__main)

MEMORY {
   flash (RX)  : ORIGIN = 0x08000000, LENGTH = 128K
   sram (WAIL)  : ORIGIN = 0x20000000, LENGTH = 20K   /* read/write, allocatable, initialized */
}

SECTIONS {
	.text :	{
                . = ALIGN(4);
                __text_start = .;
        	KEEP(*(.vect))     /* Vector table */
                *(.text*)          /* Program code */
                __data_start = .;
                *(.rodata*)        /* Read only data */
		. = ALIGN(4);
                __data_end = .;
	} > flash
	.bss :	{
                 __bss_start = . ;        
		. = ALIGN(4);
		*(.bss*)       /* Read-write zero initialized data */
		*(COMMON)
		. = ALIGN(4);
                __bss_end = . ;
	} > sram
        .ARM.exidx : {
                 __exidx_start = .;
                 *(.ARM.exidx* .gnu.linkonce.armexidx.*)
                __exidx_end = .;
        } > sram
}
