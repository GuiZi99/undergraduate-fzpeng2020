use riscv::register::{
    sstatus::Sstatus,
    scause::Scause,
};
use riscv::register::sstatus;
use core::mem::zeroed;

#[repr(C)]
#[derive(Debug)]
pub struct TrapFrame {
    pub x: [usize; 32], // General registers
    pub sstatus: Sstatus, // Supervisor Status Register
    pub sepc: usize, // Supervisor exception program counter
    pub stval: usize, // Supervisor trap value
    pub scause: Scause, // Scause register: record the cause of exception/interrupt/trap
}

#[repr(C)]
pub struct Context {
    pub content_addr: usize,
}


impl Context {
    #[naked]
    #[inline(never)]
    pub unsafe extern "C" fn switch(&mut self, target: &mut Context) {
        asm!(include_str!("process/switch.asm") :::: "volatile");
    }

	pub fn null() -> Context {
        Context { content_addr: 0, }
    }

	pub unsafe fn new_kernel_thread(
        entry: usize,
        kstack_top: usize,
        satp: usize
        ) -> Context {

        ContextContent::new_kernel_thread(entry, kstack_top, satp).push_at(kstack_top)
    }

    pub unsafe fn append_initial_arguments(&self, args: [usize; 3]) {
        let contextContent = &mut *(self.content_addr as *mut ContextContent);
        contextContent.tf.x[10] = args[0];
        contextContent.tf.x[11] = args[1];
        contextContent.tf.x[12] = args[2];
    }

}

#[repr(C)]
pub struct ContextContent {
    pub ra: usize,
    satp: usize,
    s: [usize; 12],
    tf: TrapFrame,
}

extern "C" {
	fn __trapret();
}

impl ContextContent {
    fn new_kernel_thread(
        entry: usize,
        kstack_top: usize,
        satp: usize,
        ) -> ContextContent {

        let mut content = ContextContent {
            ra: __trapret as usize,
            satp,
            s: [0; 12],
            tf: {
                let mut tf: TrapFrame = unsafe { zeroed() };
                tf.x[2] = kstack_top;
                tf.sepc = entry;
                tf.sstatus = sstatus::read();
                tf.sstatus.set_spp(sstatus::SPP::Supervisor);
                tf.sstatus.set_spie(true);
                tf.sstatus.set_sie(false);
                tf
            }
        };
        content
    }

	unsafe fn push_at(self, stack_top: usize) -> Context {
        let ptr = (stack_top as *mut ContextContent).sub(1);
        *ptr = self;
        Context { content_addr: ptr as usize }
    }
}
