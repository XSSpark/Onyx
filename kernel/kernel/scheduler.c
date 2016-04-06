/* Copyright 2016 Pedro Falcato

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
/**************************************************************************
 *
 *
 * File: scheduler.cpp
 *
 * Description: Contains the implementation of the kernel's thread scheduler
 *
 * Date: 4/3/2016
 *
 *
 **************************************************************************/
#include <kernel/scheduler.h>
#include <kernel/registers.h>
#include <kernel/panic.h>
#include <kernel/timer.h>
#include <kernel/compiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/mm.h>
#include <string.h>
task_t *last_thread = NULL;
void jump_userspace();
static task_t *first_task;
task_t *current_task = NULL;
void sched_create_task(task_t * task, void (*thread) (), uint32_t cs,
		       uint32_t ss);
/*	This is a version of _exit, but exits the thread instead of the process.
	It doesn't release memory, unmap memory or call destructors.
	It terminates the task from the scheduler, and yields the control
	It yields because it has no where to return to, as returning to the thread would cause disastrous consequences
*/
void _exit_task()
{
	sched_terminate_task(current_task);
	__asm__ __volatile__ ("int $0x50");	/* yield the current task */
}

void sched_create_task(task_t *task, void (*thread) (), uint32_t cs,
		       uint32_t ss)
{
	__asm__ __volatile__("cli");
	unsigned int *stack = NULL;

	task->stack = (uint32_t *)((uint32_t)valloc(2,false) + 0x2000);
	if (task->stack == (uint32_t *)0x2000)
		abort();
	printf("thread has stack %p\n",task->stack);
	if(cs == 0x1B) {
		printf("User-space thread has stack %p\n",task->stack);
		task->is_kernel = false;
	}else {
		task->is_kernel = true;
	}
	stack = task->stack;
	/* Push the return address */
	*--stack = (uint32_t) _exit_task;
	*--stack = ss;
	*--stack = (unsigned int) task->stack - 1;
	if(thread == jump_userspace) {
		*--stack = 0x02;
		task->is_kernel = false;
	}else
	*--stack = 0x0202;	/*This is EFLAGS */
	*--stack = cs;		/*This is CS, our code segment */
	*--stack = (unsigned int) thread;	/*This is EIP */

	/*Next, the stuff pushed by 'pusha' */
	*--stack = 0;		/*EDI */
	*--stack = 0;		/*ESI */
	*--stack = 0;		/*EBP */
	*--stack = 0;		/*Just an offset, no value */
	*--stack = 0;		/*EBX */
	*--stack = 0;		/*EDX */
	*--stack = 0;		/*ECX */
	*--stack = 0;		/*EAX */

	/*Now these are the data segments pushed by the IRQ handler */
	*--stack = ss;		/*DS */
	*--stack = ss;		/*ES */
	*--stack = ss;		/*FS */
	*--stack = ss;		/*GS */
	task->stack = stack;
	task->next = NULL;
	if (!first_task) {
		first_task = task;
		task->next = first_task;
	} else {
		task_t *new_task = first_task;
		task->next = first_task;
		while (new_task->next != first_task) {
			new_task = new_task->next;
		}
		new_task->next = task;
	}
	__asm__ __volatile__("sti");
}

void sched_terminate_task(task_t *task)
{
	if (task != NULL) {
		task_t *search_task = first_task;
		while (search_task->next != task) {
			search_task = search_task->next;
		}
		search_task->next = task->next;
	}
}

unsigned int sched_switch_task(uint32_t *old_esp)
{
	if (likely(current_task != NULL)) {
		/*Were we even running a task? */
		current_task->stack = old_esp;	/*Save the new esp for the thread */
		current_task = current_task->next;
	} else {
		current_task = first_task;	/*We just started multi-tasking, start with task 0 */
	}
	return (unsigned int) current_task->stack;	/*Return new stack pointer to ASM */
}