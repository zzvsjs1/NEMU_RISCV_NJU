#include <am.h>
#include <x86/x86.h>
#include <klib.h>

#define NR_IRQ 256 // IDT size
#define SEG_KCODE 1
#define SEG_KDATA 2
#define SEG_UCODE 3
#define SEG_UDATA 4
#define SEG_TSS 5
#define NR_SEG 6

static Context *(*user_handler)(Event, Context *) = NULL;
static SegDesc gdt[NR_SEG] = {};
static TSS32 tss = {};

void __am_irq0();
void __am_vecsys();
void __am_vectrap();
void __am_vecpf();
void __am_vecnull();
void __am_kcontext_start();
void __am_panic_on_return();
void __am_iret(Context *ctx);
void __am_switch(Context *c);

struct trap_frame
{
    Context saved_context;
    uint32_t irq, errcode;
    uint32_t eip, cs, eflags, esp, ss;
};

void __am_irq_handle(struct trap_frame *tf)
{
    Context *c = &tf->saved_context;

    c->eip = tf->eip;
    c->cs = tf->cs;
    c->eflags = tf->eflags;
    c->esp0 = tss.esp0;
    c->ss3 = USEL(SEG_UDATA);
    c->esp = ((tf->cs & DPL_USER) == DPL_USER) ? tf->esp : (uintptr_t)(tf + 1) - 8;
    c->cr3 = ((tf->cs & DPL_USER) == DPL_USER) ? (void *)get_cr3() : NULL;

    if (user_handler)
    {
        Event ev = {0};
        switch (tf->irq)
        {
        case 32:
            ev.event = EVENT_IRQ_TIMER;
            break;
        case 0x80:
            ev.event = EVENT_SYSCALL;
            break;
        case 0x81:
            ev.event = EVENT_YIELD;
            break;
        case 14:
            ev.event = EVENT_PAGEFAULT;
            ev.cause = (tf->errcode & 0x2) ? MMAP_WRITE : MMAP_READ;
            ev.ref = get_cr2();
            break;
        default:
            ev.event = EVENT_ERROR;
            break;
        }

        c = user_handler(ev, c);
        assert(c != NULL);
    }

    __am_switch(c);
    if (c->cr3 != NULL)
    {
        tss.ss0 = KSEL(SEG_KDATA);
        tss.esp0 = c->esp0;
    }

    __am_iret(c);
    assert(0);
}

bool cte_init(Context *(*handler)(Event, Context *))
{
    static GateDesc32 idt[NR_IRQ];

    gdt[SEG_KCODE] = SEG32(STA_X | STA_R, 0, 0xffffffff, DPL_KERN);
    gdt[SEG_KDATA] = SEG32(STA_W, 0, 0xffffffff, DPL_KERN);
    gdt[SEG_UCODE] = SEG32(STA_X | STA_R, 0, 0xffffffff, DPL_USER);
    gdt[SEG_UDATA] = SEG32(STA_W, 0, 0xffffffff, DPL_USER);
    gdt[SEG_TSS] = SEG16(STS_T32A, &tss, sizeof(tss) - 1, DPL_KERN);
    set_gdt(gdt, sizeof(gdt));

    tss.ss0 = KSEL(SEG_KDATA);
    set_tr(KSEL(SEG_TSS));

    // initialize IDT
    for (unsigned int i = 0; i < NR_IRQ; i++)
    {
        idt[i] = GATE32(STS_TG, KSEL(SEG_KCODE), __am_vecnull, DPL_KERN);
    }

    // ----------------------- interrupts ----------------------------
    idt[32] = GATE32(STS_IG, KSEL(SEG_KCODE), __am_irq0, DPL_KERN);
    // ----------------------- exceptions ----------------------------
    idt[14] = GATE32(STS_IG, KSEL(SEG_KCODE), __am_vecpf, DPL_KERN);
    // ---------------------- system call ----------------------------
    idt[0x80] = GATE32(STS_TG, KSEL(SEG_KCODE), __am_vecsys, DPL_USER);
    idt[0x81] = GATE32(STS_TG, KSEL(SEG_KCODE), __am_vectrap, DPL_KERN);

    set_idt(idt, sizeof(idt));

    // register event handler
    user_handler = handler;

    return true;
}

Context *kcontext(Area kstack, void (*entry)(void *), void *arg)
{
    Context *ctx = kstack.end - sizeof(Context);
    *ctx = (Context){0};

    ctx->ds = KSEL(SEG_KDATA);
    ctx->cs = KSEL(SEG_KCODE);
    ctx->eip = (uintptr_t)__am_kcontext_start;
    ctx->eflags = FL_IF;
    ctx->esp = (uintptr_t)kstack.end;
    ctx->GPR1 = (uintptr_t)arg;
    ctx->GPR2 = (uintptr_t)entry;

    return ctx;
}

void yield()
{
    asm volatile("int $0x81");
}

bool ienabled()
{
    return (get_efl() & FL_IF) != 0;
}

void iset(bool enable)
{
    if (enable)
        sti();
    else
        cli();
}

void __am_panic_on_return()
{
    panic("kernel context returns");
}
