#include "mm.h"

#include "string.h"
#include "task.h"

#include "print.h"

// System memory map as told by BIOS.
extern unsigned int mem_map_buf_addr;
extern unsigned int mem_map_buf_entry_count;

// Kernel uninitialized data start and end.
extern int _bss_start;
extern int _bss_end;
extern int _text_start;
extern int _text_end;
extern int _data_start;
extern int _data_end;

// TSS for kernel and user tasks.
extern tss kernel_tss __attribute__((aligned(0x1000)));
extern tss user_tss __attribute__((aligned(0x1000)));

// Memory management structures for the kernel's use.
static uint32_t kernel_mem_bitmap;
static struct bios_mem_map *bmm;

extern void *APP_START_PHY_ADDR;
extern void *APP_START_VIRT_ADDR;
extern int APP_STACK_SIZE;
extern int APP_HEAP_SIZE;

#define NUM_GDT_ENTRIES 8
// Kernel structures for segmentation.
struct gdt_entry pm_gdt[NUM_GDT_ENTRIES];
struct gdt_info pm_gdt_info = {
	.len = sizeof(pm_gdt),
	.addr = (unsigned int) pm_gdt,
};

// Kernel structures for paging.
// We have 1024 page directory entries -> 1024 page tables and each page table has 1024 ptes.
unsigned int kernel_page_directory[1024]__attribute__((aligned(0x1000)));
unsigned int kernel_page_tables[1024][1024]__attribute__((aligned(0x1000)));

unsigned int user_page_directory[USER_PAGE_DIR_SIZE]__attribute__((aligned(0x1000)));
unsigned int user_page_tables[USER_PAGE_DIR_SIZE][USER_PAGE_TABLE_SIZE]__attribute__((aligned(0x1000)));

extern void enable_paging(void);

void init_mm(void) {
  	print_string("mem_map_buf_addr=");			print_int32(mem_map_buf_addr);
	print_string("\n");
	print_string("mem_map_buf_entry_count="); 	print_int32(mem_map_buf_entry_count);
	print_string("\n");

	bmm = (struct bios_mem_map*) mem_map_buf_addr;

	for (int i = 0; i < mem_map_buf_entry_count; i++) {
		print_string("entry "); 		print_int32(i); 			print_string(" has base "); print_int32(bmm[i].base);
    	print_string(" and length "); 	print_int32(bmm[i].length);
		print_string(" (avail="); 		print_int32(bmm[i].type); 	print_string(")\n");
	}

	kernel_mem_bitmap = ~((unsigned int) 0);
	print_string("kernel_mem_bitmap="); print_int32(kernel_mem_bitmap); print_string("\n");

	print_string("_bss_start=");	print_int32((unsigned int) &_bss_start);	print_string("\n");
	print_string("_bss_end="); 		print_int32((unsigned int) &_bss_end);		print_string("\n");
	print_string("_text_start="); 	print_int32((unsigned int) &_text_start);	print_string("\n");
	print_string("_text_end="); 	print_int32((unsigned int) &_text_end);		print_string("\n");
	print_string("_data_start="); 	print_int32((unsigned int) &_data_start);	print_string("\n");
	print_string("_data_end=");		print_int32((unsigned int) &_data_end);		print_string("\n");

	setup_tss();
	setup_pm_gdt();
	setup_and_enable_paging();
}

void setup_and_enable_paging(void) {
	setup_page_directory_and_page_tables();
	enable_paging();
}

void setup_page_directory_and_page_tables(void) {
	va_range_sz_t region_length;
	unsigned int i, page_frame;
	int num_app_pages = 2;
	va_t region_start;
	unsigned int addr;

	print_string("kernel_page_directory="); 	print_int32((unsigned int) kernel_page_directory); 	print_string("\n");
	print_string("user_page_directory="); 		print_int32((unsigned int) user_page_directory); 	print_string("\n");
	print_string("kernel_page_tables="); 		print_int32((unsigned int) kernel_page_tables); 	print_string("\n");
	print_string("user_page_tables="); 			print_int32((unsigned int) user_page_tables); 		print_string("\n");

	// Setup kernel paging.
	addr = (unsigned int) &kernel_page_tables[0];
	for (i = 0; i < 1024; i++) {
		// Set all PDEs.
		kernel_page_directory[i] = (addr & 0xfffff000) | 0x3;
		addr += 0x1000;
	}

	{
		region_length = (va_range_sz_t)0x100000000ULL;
		region_start = 0x0;
		page_frame = 0x0;
		map_va_range_to_pa_range(kernel_page_tables, /*va=*/(va_t)region_start, /*size=*/region_length, /*pa=*/page_frame,
								 /*access_flags=*/0x3);
	}

	// Setup user paging.
	addr = (unsigned int) &user_page_tables[0];
	for (i = 0; i < USER_PAGE_DIR_SIZE; i++) {
		// Set all PDEs.
		user_page_directory[i] = (addr & 0xfffff000) | 0x3;
		addr += 0x1000;
	}

	{
		/**
		 * Identity map the kernel into the process's virtual address space.
		 * There are 2 usable portions of physical memory:
		 * 		1. [0x0, 0x9fc00)
		 * 		2. [0x100000, 0xbffe0000)
		 * Only the kernel is allowed in the first part.
		 * We map this portion of the kernel here.
		 * VA: 0x0 -> PAGE_ALIGN(0x9fc00), PA: 0x0 -> PAGE_ALIGN(0x9fc00)
		 */
		region_length = (unsigned int)0x9fc00;
		region_start = 0x0;
		page_frame = 0x0;
		map_va_range_to_pa_range(user_page_tables, /*va=*/(va_t)region_start, /*size=*/region_length, /*pa=*/page_frame,
								 /*access_flags=*/0x1);
	}

	{
		/**
		 * We map the part of the second portion of usable physical memory which the kernel uses here.
		 * We know that the kernel uses only [0x100000, _bss_end) because that is what is specified in kernel.ld.
		 * Should that change, this should probablky change too.
		 */
		region_length = (unsigned int)&_bss_end - 0x100000;
		region_start = 0x100000;
		page_frame = 0x100000;
		map_va_range_to_pa_range(user_page_tables, /*va=*/(va_t)region_start, /*size=*/region_length, /*pa=*/page_frame,
								 /*access_flags=*/0x1);
	}

	{
		// Map user virtual address arbitrarily ->
		//	-> VA: APP_VIRT_ADDR -> PAGE_ALIGN(APP_VIRT_ADDR + APP_SIZE), PA: 0x20000000 -> PAGE_ALIGN(0x20000000 + APP_SIZE)
		region_length = (va_range_sz_t)(num_app_pages * PAGE_SIZE + APP_STACK_SIZE + APP_HEAP_SIZE);
		region_start = (unsigned int)APP_START_VIRT_ADDR;
		page_frame = (unsigned int)APP_START_PHY_ADDR;
		map_va_range_to_pa_range(user_page_tables, /*va=*/(va_t)region_start, /*size=*/(va_range_sz_t)region_length, /*pa=*/page_frame,
								 /*access_flags=*/0x7);
	}
	return;
}

/**
 * map_va_range_to_pa_range - Map a contiguos block of virtual memory to a contiguous block of
 * physical memory.
 * 
 * @page_tables_ptr: A list of page tables where mapping are stored.
 * @va: The base address of the block of virtual memory to be mapped.
 * @range_size_bytes: The length of the block of virtual memory to be mapped.
 * @pa: The base address of the block of physical memory to be mapped to.
 */
int map_va_range_to_pa_range(unsigned int page_tables_ptr[][USER_PAGE_TABLE_SIZE],
							 va_t va, va_range_sz_t range_size_bytes, unsigned int pa,
							 unsigned int access_flags) {
	unsigned int va_aligned = PAGE_ALIGN(va), pa_aligned = PAGE_ALIGN(pa);
	unsigned int i_start, j_start;
	unsigned int i_end, j_end;
	unsigned int i, j;

	i_end = (va_aligned + range_size_bytes) / (USER_PAGE_TABLE_SIZE * PAGE_SIZE);
	j_end = ((va_aligned + range_size_bytes) / PAGE_SIZE) % USER_PAGE_TABLE_SIZE;
	i_start = va_aligned / (USER_PAGE_TABLE_SIZE * PAGE_SIZE);
	j_start = (va_aligned / PAGE_SIZE) % USER_PAGE_TABLE_SIZE;
	i = i_start, j = j_start;

	if (j_end)
		i_end++;

	for (; i < i_end; i++) {
		int tmp_j_end = USER_PAGE_TABLE_SIZE;

		if (i == i_end - 1)
			tmp_j_end = j_end;

		for (; j < tmp_j_end; j++) {
			page_tables_ptr[i][j] = pa_aligned | access_flags;
			pa_aligned += PAGE_SIZE;
		}
		j = 0;
	}

	return 0;
}

void setup_pm_gdt(void) {
	pm_gdt_info.len = sizeof(pm_gdt);
	pm_gdt_info.addr = (long unsigned int) pm_gdt;

	/* make_gdt_entry(gdt_entry*, limit, base, type, flags) */
	/* In x86 (who knows about elsewhere) first GDT entry should be null. */
	make_gdt_entry(&pm_gdt[0], 0x0, 0x0, 0x0, 0x0);
	// KERNEL_CODE_SEGMENT
	// Presently, it is very important that the c kernel code segment be
	// the second entry (offset of 8 bytes from the start.) If you change this,
	// make sure to also change pm_jump() in kernel_entry.asm to use the right
	// segment selector so that CS register is updated correctly.
	make_gdt_entry(&pm_gdt[1], 0xfffff, 0x0, 0xa, 0xc9);
	// KERNEL_DATA_SEGMENT
	make_gdt_entry(&pm_gdt[2], 0xfffff, 0x0, 0x2, 0xc9);
	// USER_CODE_SEGMENT
	make_gdt_entry(&pm_gdt[3], 0x0, 0x0, 0x0, 0x0);
	// USER_DATA_SEGMENT
	make_gdt_entry(&pm_gdt[4], 0x0, 0x0, 0x0, 0x0);

	// Add entry for kernel task (TSS descriptor).
	make_gdt_entry(&pm_gdt[5], sizeof(kernel_tss), (unsigned int) &kernel_tss, 0x9, 0x18);
	// Add entry for user task (TSS descriptor).
	make_gdt_entry(&pm_gdt[6], sizeof(user_tss), (unsigned int) &user_tss, 0x9, 0x1e);
	// Add task gate for user TSS.
	make_gdt_entry(&pm_gdt[7], sizeof(user_tss), 0x30, 0x5, 0xe);

	show_gdt(pm_gdt, NUM_GDT_ENTRIES - 1);
	load_pm_gdt();
}

void show_gdt(struct gdt_entry* gdt, int num_entries) {
	unsigned int limit = 0, base = 0, flags = 0, type = 0;
	int i = 0;

	for (; i < num_entries; i++) {
		limit |= gdt[i].limit0_15;
		limit |= ((gdt[i].limit16_19_avl_l_db_g & 0x0f) << 16);

		base |= gdt[i].base0_15;
		base |= gdt[i].base16_23 << 16;
		base |= gdt[i].base24_31 << 24;

		flags |= (gdt[i].type_s_dpl_p & 0xf0) >> 4;
		flags |= (gdt[i].limit16_19_avl_l_db_g & 0xf0);

		type |= (gdt[i].type_s_dpl_p & 0xf);

		print_string("limit="); print_int32(limit); print_string(",");
		print_string("base="); 	print_int32(base); 	print_string(",");
		print_string("type="); 	print_int32(type); 	print_string(",");
		print_string("flags="); print_int32(flags); print_string("\n");

		(limit = 0, base = 0, flags = 0, type = 0);
	}	
}

extern void pm_jump(void);
void load_pm_gdt(void) {
	asm volatile("lgdt %0" : : "m"(pm_gdt_info));
	pm_jump();
}

void make_gdt_entry(struct gdt_entry* entry,
					unsigned int limit,
					unsigned int base,
					char type,
					/*flags format: S_DPL_P_AVL_L_DB_G*/
					/*bits:         1_2___1_1___1_1__1*/
					char flags) {

	// Set lower 16 bits of limit.
	entry->limit0_15 = limit & 0xffff;
	// Set lower 16 bits of base.
	entry->base0_15 = base & 0xffff;
	// Set bits 16 to 13 of base.
	entry->base16_23 = (base >> 16) & 0xff;
	// Set bits 24 to 31 of base.
	entry->base24_31 = (base >> 24) & 0xff;
	// Set upper 4 bits of 20 bit limit.
	entry->limit16_19_avl_l_db_g = (limit >> 16) & 0xf;
	// Set 4 bits of type.
	entry->type_s_dpl_p = type & 0xf;
	// Set S_DPL_P flags (lower 4 bits of 8 bit flags).
	entry->type_s_dpl_p |= (flags & 0xf) << 4;
	// Set AVL_L_DB_G flags (upper 4 bits of 8 bit flags).
	entry->limit16_19_avl_l_db_g |= flags & 0xf0;
}
