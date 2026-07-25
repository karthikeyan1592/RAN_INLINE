/* BPF types header: only defines pt_regs if vmlinux.h hasn't already. */
#ifndef BPF_TYPES_H
#define BPF_TYPES_H

#ifndef __VMLINUX_H__
/* Minimal x86_64 pt_regs for BPF uprobe programs (standalone, no vmlinux.h) */
struct pt_regs {
	unsigned long r15;
	unsigned long r14;
	unsigned long r13;
	unsigned long r12;
	unsigned long bp;
	unsigned long bx;
	unsigned long r11;
	unsigned long r10;
	unsigned long r9;
	unsigned long r8;
	unsigned long ax;
	unsigned long cx;
	unsigned long dx;
	unsigned long si;
	unsigned long di;
	unsigned long rdi;
	unsigned long orig_ax;
	unsigned long ip;
	unsigned long cs;
	unsigned long flags;
	unsigned long sp;
	unsigned long ss;
};
#endif /* __VMLINUX_H__ */

#endif /* BPF_TYPES_H */
