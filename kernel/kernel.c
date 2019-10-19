// A simple kernel.
#include "system.h"

#include "../drivers/screen.h"

#include "idt.h"
#include "isrs.h"
#include "irq.h"

#define NUM_TEMPLATE "000000000000"

extern void enable_interrupts(void);
extern void initialize_idt(void);

void init(void) {
	init_idt();
	install_isrs();
	install_irqs();
	enable_interrupts();
}

char* kernel_load_message = "Kernel Loaded\n";

int main(void) {
	char* kernel_init_message = "Kernel initialized.\n";
	char* long_kernel_story = 
	"============================================\n"
	"This is the story of a little kernel\n"
	"============================================\n";
	print(kernel_load_message);
	init();
	print(kernel_init_message);
	print(long_kernel_story);

	// for (;;);
	return 0;
}

