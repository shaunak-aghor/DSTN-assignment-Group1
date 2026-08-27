#ifndef MAIN_MEMORY_H
#define MAIN_MEMORY_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define PAGE_SIZE 1024u
#define MAIN_MEMORY_SIZE (32u * 1024u * 1024u)
#define MM_FRAME_COUNT (MAIN_MEMORY_SIZE / PAGE_SIZE)
#define PREPAGED_PAGES 2u

typedef uint16_t Process_Id;

typedef struct {
	uint32_t valid : 1;
	uint32_t resident : 1;
	uint32_t dirty : 1;
	uint32_t referenced : 1;
	uint32_t frame_number : 15;
} Page_Table_Entry;

typedef struct {
	uint32_t valid : 1;
	uint32_t page_table : 1;
	uint32_t dirty : 1;
	uint32_t referenced : 1;
	uint32_t frequency : 8;
	uint32_t aging_value : 8;
	Process_Id owner_pid;
	uint32_t virtual_page;
} Frame_Info;

typedef struct {
	Process_Id pid;
	uint32_t virtual_page_count;
	uint32_t page_table_base_frame;
	uint32_t page_table_frame_count;
	uint32_t resident_page_count;
	uint32_t lower_page_limit;
	uint32_t upper_page_limit;
	FILE *backing_store;
} Process_Memory;

typedef struct {
	FILE *memory_file;
	Frame_Info frames[MM_FRAME_COUNT];
	Process_Memory *processes;
	size_t process_count;
	size_t process_capacity;
	uint32_t aging_epoch;
} Main_Memory;

#endif
