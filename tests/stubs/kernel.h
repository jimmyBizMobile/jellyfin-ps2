#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
typedef unsigned char u8;
/* Only declarations: unused PS2 thread code is discarded by the host linker. */
typedef struct { int init_count, max_count; } ee_sema_t;
typedef struct {
	void (*func)(void *);
	void *stack, *gp_reg;
	int stack_size, initial_priority;
} ee_thread_t;
int CreateSema(ee_sema_t *);
int CreateThread(ee_thread_t *);
int StartThread(int, void *);
int WaitSema(int);
int SignalSema(int);
#endif
