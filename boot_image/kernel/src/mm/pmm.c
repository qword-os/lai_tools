#include <stdint.h>
#include <stddef.h>
#include <mm.h>
#include <klib.h>
#include <bit.h>

#include <stivale.h>

#define MEMORY_BASE 0x1000000
#define BITMAP_BASE (MEMORY_BASE / PAGE_SIZE)

#define BMREALLOC_STEP 1

static volatile uint32_t *mem_bitmap = (uint32_t *)(MEM_PHYS_OFFSET + MEMORY_BASE);
static volatile uint32_t *tmp_bitmap;

// the initial bitmap is 1 page aka PAGE_SIZE * 8 entries
static size_t bitmap_entries = PAGE_SIZE * 8;

__attribute__((always_inline)) static inline int read_bitmap(size_t i) {
    i -= BITMAP_BASE;

    return test_bit(mem_bitmap, i);
}

__attribute__((always_inline)) static inline void set_bitmap(size_t i, size_t count) {
    i -= BITMAP_BASE;

    size_t f = i + count;
    for (size_t j = i; j < f; j++)
        set_bit(mem_bitmap, j);
}

__attribute__((always_inline)) static inline void unset_bitmap(size_t i, size_t count) {
    i -= BITMAP_BASE;

    size_t f = i + count;
    for (size_t j = i; j < f; j++)
        reset_bit(mem_bitmap, j);
}

/* Populate bitmap using memmap data. */
void init_pmm(struct stivale_mmap_entry *memmap, size_t memmap_entries) {
    // clear initial bitmap
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint32_t); i++)
        mem_bitmap[i] = 0xffffffff;

    kprint(KPRN_INFO, "pmm: Mapping memory as specified by the memmap...");

    /* For each region specified by the memmap, iterate over each page which
       fits in that region and if the region type indicates the area itself
       is usable, write that page as free in the bitmap. Otherwise, mark the page as used. */
    for (size_t i = 0; i < memmap_entries; i++) {
        if (memmap[i].type != 1)
            continue;

        size_t aligned_base;
        if (memmap[i].base % PAGE_SIZE)
            aligned_base = memmap[i].base + (PAGE_SIZE - (memmap[i].base % PAGE_SIZE));
        else
            aligned_base = memmap[i].base;
        size_t aligned_length = (memmap[i].length / PAGE_SIZE) * PAGE_SIZE;
        if ((memmap[i].base % PAGE_SIZE) && aligned_length) aligned_length -= PAGE_SIZE;

        for (size_t j = 0; j * PAGE_SIZE < aligned_length; j++) {
            size_t addr = aligned_base + j * PAGE_SIZE;

            size_t page = addr / PAGE_SIZE;

            if (addr < (MEMORY_BASE + PAGE_SIZE /* bitmap */))
                continue;

            if (addr >= (MEMORY_BASE + bitmap_entries * PAGE_SIZE)) {
                /* Reallocate bitmap */
                size_t cur_bitmap_size_in_pages = ((bitmap_entries / 32) * sizeof(uint32_t)) / PAGE_SIZE;
                size_t new_bitmap_size_in_pages = cur_bitmap_size_in_pages + BMREALLOC_STEP;
                if (!(tmp_bitmap = pmm_allocz(new_bitmap_size_in_pages))) {
                    kprint(KPRN_ERR, "pmm_alloc failure in init_pmm(). Halted.");
                    for (;;);
                }
                tmp_bitmap = (uint32_t *)((size_t)tmp_bitmap + MEM_PHYS_OFFSET);
                /* Copy over previous bitmap */
                for (size_t i = 0;
                     i < (cur_bitmap_size_in_pages * PAGE_SIZE) / sizeof(uint32_t);
                     i++)
                    tmp_bitmap[i] = mem_bitmap[i];
                /* Fill in the rest */
                for (size_t i = (cur_bitmap_size_in_pages * PAGE_SIZE) / sizeof(uint32_t);
                     i < (new_bitmap_size_in_pages * PAGE_SIZE) / sizeof(uint32_t);
                     i++)
                    tmp_bitmap[i] = 0xffffffff;
                bitmap_entries += ((PAGE_SIZE / sizeof(uint32_t)) * 32) * BMREALLOC_STEP;
                uint32_t *old_bitmap = (uint32_t *)((size_t)mem_bitmap - MEM_PHYS_OFFSET);
                mem_bitmap = tmp_bitmap;
                pmm_free(old_bitmap, cur_bitmap_size_in_pages);
            }

            unset_bitmap(page, 1);
        }
    }
}

void *pmm_alloc(size_t pg_count) {
    size_t pg_cnt = pg_count;

    size_t i;
    for (i = BITMAP_BASE; i < BITMAP_BASE + bitmap_entries; ) {
        if (!read_bitmap(i++)) {
            if (!--pg_cnt)
                goto found;
        } else {
            pg_cnt = pg_count;
        }
    }

    return NULL;

found:;
    size_t start = i - pg_count;
    set_bitmap(start, pg_count);

    // Return the physical address that represents the start of this physical page(s).
    return (void *)(start * PAGE_SIZE);
}

/* Allocate physical memory and zero it out. */
void *pmm_allocz(size_t pg_count) {
    void *ptr = pmm_alloc(pg_count);
    if (!ptr)
        return NULL;

    uint64_t *pages = (uint64_t *)(ptr + MEM_PHYS_OFFSET);

    for (size_t i = 0; i < (pg_count * PAGE_SIZE) / sizeof(uint64_t); i++)
        pages[i] = 0;

    return ptr;
}

/* Release physical memory. */
void pmm_free(void *ptr, size_t pg_count) {
    size_t start = (size_t)ptr / PAGE_SIZE;

    unset_bitmap(start, pg_count);
}
