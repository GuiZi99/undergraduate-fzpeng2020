#include "bbl.h"
#include "mtrap.h"
#include "atomic.h"
#include "vm.h"
#include "bits.h"
#include "config.h"
#include <string.h>

static kernel_elf_info info;
static volatile int elf_loaded;

static void supervisor_vm_init()
{
  uintptr_t highest_va = DRAM_BASE - first_free_paddr;
  //mem_size = MIN(mem_size, highest_va - info.first_user_vaddr) & -RISCV_PGSIZE;
  mem_size = MIN(mem_size, highest_va - info.first_user_vaddr) & -MEGAPAGE_SIZE;

  pte_t* sbi_pt = (pte_t*)(info.first_vaddr_after_user + info.load_offset);
  memset(sbi_pt, 0, RISCV_PGSIZE);
  printm("info.first_user_vaddr: %x sbi_pt_addr:%x mem_size: %x\n", info.first_user_vaddr, (uint32_t)sbi_pt,mem_size);
  pte_t* middle_pt = (void*)sbi_pt + RISCV_PGSIZE;
#if __riscv_xlen == 32
  ///*
  size_t num_middle_pts = 1;
  pte_t* root_pt = middle_pt;
  memset(root_pt, 0, RISCV_PGSIZE);
#else
  //size_t num_middle_pts = (-info.first_user_vaddr - 1) / MEGAPAGE_SIZE + 1;
  printm("MEGAPAGE_SIZE: %d total_vaddr: %d \n", MEGAPAGE_SIZE, (-info.first_user_vaddr -1) );
  //pte_t* root_pt = (void*)middle_pt + num_middle_pts * RISCV_PGSIZE;
  printm("Step 1.1 begin memset size: %x \n", (num_middle_pts + 1) * RISCV_PGSIZE);
  //mset(middle_pt, 0, (num_middle_pts + 1) * RISCV_PGSIZE);
  for (size_t i = 0; i < num_middle_pts - 1; i++)
    root_pt[(1<<RISCV_PGLEVEL_BITS)-num_middle_pts+i] = ptd_create(((uintptr_t)middle_pt >> RISCV_PGSHIFT) + i);
//else
  die("not implemented!");
  size_t num_middle_pts = (-info.first_user_vaddr - 1) / GIGAPAGE_SIZE + 1;
  pte_t* root_pt = (void*)middle_pt + num_middle_pts * RISCV_PGSIZE;
  memset(middle_pt, 0, (num_middle_pts + 1) * RISCV_PGSIZE);
  for (size_t i = 0; i < num_middle_pts; i++)
    root_pt[(1<<RISCV_PGLEVEL_BITS)-num_middle_pts+i] = ptd_create(((uintptr_t)middle_pt >> RISCV_PGSHIFT) + i);
#endif
#if 0  
  for (uintptr_t vaddr = info.first_user_vaddr, paddr = vaddr + info.load_offset, end = info.first_vaddr_after_user;
       paddr < DRAM_BASE + mem_size; vaddr += RISCV_PGSIZE, paddr += RISCV_PGSIZE) {
    int l2_shift = RISCV_PGLEVEL_BITS + RISCV_PGSHIFT;
    int l1_shift = RISCV_PGSHIFT;
    size_t l2_idx = (info.first_user_vaddr >> l2_shift) & ((1 << RISCV_PGLEVEL_BITS)-1);
    l2_idx += ((vaddr - info.first_user_vaddr) >> l2_shift);
    size_t l1_idx = (info.first_user_vaddr >> l1_shift) & ((1 << RISCV_PGLEVEL_BITS)-1);
    l1_idx += ((vaddr - info.first_user_vaddr) >> l1_shift);
    middle_pt[l1_idx] = pte_create(paddr >> RISCV_PGSHIFT, PTE_G | PTE_R | PTE_W | PTE_X);

    if(vaddr == info.first_user_vaddr || paddr == DRAM_BASE + mem_size - RISCV_PGSIZE ) printm("%x %x, %x, %x %x\n", vaddr, paddr, vaddr >> 22, l2_idx, l1_idx);
  }
#else
  for (uintptr_t vaddr = info.first_user_vaddr, paddr = vaddr + info.load_offset, end = info.first_vaddr_after_user;
    paddr < DRAM_BASE + mem_size; vaddr += MEGAPAGE_SIZE, paddr += MEGAPAGE_SIZE) {
	    int l2_shift = RISCV_PGLEVEL_BITS + RISCV_PGSHIFT;
        size_t l2_idx = (info.first_user_vaddr >> l2_shift) & ((1 << RISCV_PGLEVEL_BITS)-1);
        l2_idx += ((vaddr - info.first_user_vaddr) >> l2_shift);
		middle_pt[l2_idx] = pte_create(paddr >> RISCV_PGSHIFT, PTE_G | PTE_R | PTE_W | PTE_X);
  	    printm("vaddr: %x, paddr: %x, vaddr>>22: %x, l2_idx: %x\n", vaddr, paddr, vaddr >> 22, l2_idx);
  }
#endif

  // map SBI at top of vaddr space
  extern char _sbi_end;
  uintptr_t num_sbi_pages = ((uintptr_t)&_sbi_end - DRAM_BASE - 1) / RISCV_PGSIZE + 1;
  assert(num_sbi_pages <= (1 << RISCV_PGLEVEL_BITS));
  for (uintptr_t i = 0; i < num_sbi_pages; i++) {
    uintptr_t idx = (1 << RISCV_PGLEVEL_BITS) - num_sbi_pages + i;
    sbi_pt[idx] = pte_create((DRAM_BASE / RISCV_PGSIZE) + i, PTE_G | PTE_R | PTE_X);
  }
  //pte_t* sbi_pte = /*middle_pt*/root_pt + ((/*num_middle_pts*/1 << RISCV_PGLEVEL_BITS)-1);
  pte_t* sbi_pte = middle_pt + ((num_middle_pts << RISCV_PGLEVEL_BITS) - 1);
  assert(!*sbi_pte);
  *sbi_pte = ptd_create((uintptr_t)sbi_pt >> RISCV_PGSHIFT);
//for (uint32_t i = 0; i < 1024; i++) {
//  	  printm("middle_pt[%d]:%x", i, middle_pt[i]);
//  }
  mb();
  root_page_table = root_pt;
  write_csr(sptbr, (uintptr_t)root_pt >> RISCV_PGSHIFT);
}

void boot_loader()
{
  log("machine mode: loading payload OS...");
  
  extern char _payload_start, _payload_end;
  load_kernel_elf(&_payload_start, &_payload_end - &_payload_start, &info);
  supervisor_vm_init();
#ifdef PK_ENABLE_LOGO
  //print_logo();
#endif
  mb();
  elf_loaded = 1;
  enter_supervisor_mode((void *)info.entry, 0);
}

void boot_other_hart()
{
  while (!elf_loaded)
    ;
  mb();
  enter_supervisor_mode((void *)info.entry, 0);
}