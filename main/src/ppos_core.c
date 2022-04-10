#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/time.h>

// Internal libs
#include <ppos.h>
#include <queue.h>

// Thread stack size
#define STACKSIZE 64 * 1024

// Task status
#define READY 0
#define RUNNING 1
#define SUSPENDED 2
#define TERMINATED 3

// Task priority
#define MAX_TASK_PRIO 20
#define MIN_TASK_PRIO -20
#define TASK_AGING -1
#define QUANTUM 20


// Globals
task_t *currentTask, dispatcherTask, mainTask;
task_t *readyTasksQueue;
int currentId = 1;
int userTasks = 0;
int ticks = 0;

struct sigaction action;
struct itimerval timer;

#ifdef DEBUG
void print_elem(task_t *task) {
    printf("%d(%d, %d)", task->id, task->prio, task->priod);
}
#endif

// tratador do sinal
void ticks_handler(int signum) {
#ifdef DEBUG
    printf("Recebi o sinal %d\n", signum);
#endif
    ticks -= 1;
    if (!ticks && currentTask->preemptable) {
        task_yield();
    }
}

void init_handler() {
    // registra a ação para o sinal de timer SIGALRM
    struct sigaction action;
    action.sa_handler = ticks_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGALRM, &action, 0) < 0) {
        perror("Erro em sigaction: ");
        exit(1);
    }

    // ajusta valores do temporizador inicia apos 1ms com intervalo de 1ms
    timer.it_value.tv_usec = 1000;
    timer.it_value.tv_sec = 0;
    timer.it_interval.tv_usec = 1000;
    timer.it_interval.tv_sec = 0;

    // arma o temporizador ITIMER_REAL (vide man setitimer)
    if (setitimer(ITIMER_REAL, &timer, 0) < 0) {
        perror("Erro em setitimer: ");
        exit(1);
    }
}

void append_to_ready_tasks_queue(task_t *task) {
#ifdef DEBUG
    printf("Appending task to queue: %d\n", task->id);
#endif
    queue_append((queue_t **)&readyTasksQueue, (queue_t *)task);
}

void remove_from_ready_tasks_queue(task_t *task) {
#ifdef DEBUG
    printf("Removing task from queue: %d\n", task->id);
#endif
    queue_remove((queue_t **)&readyTasksQueue, (queue_t *)task);
}

int task_create(task_t *task, void (*start_func)(void *), void *arg) {
#ifdef DEBUG
    printf("PPOS: task %d created by task %d (body function %p)\n", currentId,
           currentTask->id, start_func);
#endif
    getcontext(&(task->context));
    task->stack = malloc(STACKSIZE);

    // Atualiza os parametros do contexto com a stack alocada
    if (task->stack) {
        task->context.uc_stack.ss_sp = task->stack;
        task->context.uc_stack.ss_size = STACKSIZE;
        task->context.uc_stack.ss_flags = 0;
        task->context.uc_link = 0;
        task->id = currentId++;
        task->status = READY;
        task->preemptable = 1;
    } else {
        perror("Erro na criação da pilha: ");
        return -1;
    }
    makecontext(&(task->context), (void (*)(void))start_func, 1, arg);
    append_to_ready_tasks_queue(task);
    if (task->id != dispatcherTask.id) {
        userTasks++;
    }
    task_setprio(task, 0);
#ifdef DEBUG
    printf("Criou task %d\n", currentId - 1);
#endif
    return 0;
}

void task_exit(int exit_code) {
#ifdef DEBUG
    printf("PPOS: task %d exited with code %d\n", task_id(), exit_code);
#endif
    currentTask->status = TERMINATED;
    if (currentTask->id == dispatcherTask.id) {
        task_switch(&mainTask);
    } else {
        task_switch(&dispatcherTask);
    }
}

int task_switch(task_t *task) {
    if (currentTask->id == task->id) {
#ifdef DEBUG
        printf("Ignoring task switch because it's the same: %d -> %d\n",
               currentTask->id, task->id);
#endif
        return 0;
    }
#ifdef DEBUG
    printf("Switch task %d -> %d\n", currentTask->id, task->id);
    printf("Current Task antes -> %d \n", currentTask->id);
#endif
    ucontext_t *currentContext = &(currentTask->context);
    // return current task to ready task queue if not mainTask
    if (currentTask->id != mainTask.id && currentTask->status != TERMINATED) {
        append_to_ready_tasks_queue(currentTask);
    }

    currentTask = task;
    currentTask->status = RUNNING;
    if (currentTask->id != mainTask.id) {
        remove_from_ready_tasks_queue(currentTask);
    }
#ifdef DEBUG
    printf("Current Task depois -> %d \n", currentTask->id);
#endif
    ticks = QUANTUM;
    swapcontext(currentContext, &(task->context));
    return 0;
}

int task_id() { return currentTask->id; }

/* ======================[ Dispatcher]====================== */

void task_yield() {
#ifdef DEBUG
    printf("Suspending task -> %d \n", currentTask->id);
#endif
    // return the processor to dispatcher
    currentTask->status = SUSPENDED;
    task_switch(&dispatcherTask);
}

void task_setprio(task_t *task, int prio) {
    if (task == NULL)
        task = currentTask;

    if (prio > MAX_TASK_PRIO)
        prio = MAX_TASK_PRIO;

    if (prio < MIN_TASK_PRIO)
        prio = MIN_TASK_PRIO;

    task->prio = prio;
    task->priod = prio;
}

int task_getprio(task_t *task) {
    return task == NULL ? currentTask->prio : task->prio;
}

void age_task(task_t *task) {
    if (task->priod != MIN_TASK_PRIO)
        task->priod += TASK_AGING;
}

task_t *scheduler() {
#ifdef DEBUG
    queue_print("Scheduler", readyTasksQueue, print_elem);
    printf("readyTasksQueue: %p\n", (void *)readyTasksQueue);
#endif

    if (readyTasksQueue == NULL)
        return NULL;
    task_t *first = readyTasksQueue, *it;
    task_t *nextTask = readyTasksQueue;
    int minPrio = nextTask->priod;

    // find the task with lowest priod and age all tasks
    for (it = readyTasksQueue->next; it != first; it = it->next) {
        if (it->priod <= minPrio) {
            nextTask = it;
            minPrio = it->priod;
        }
        age_task(it);
    }
    age_task(first);
    nextTask->priod = nextTask->prio;

#ifdef DEBUG
    printf("minPrio: %d\n", nextTask->priod);
    printf("Scheduler next task: %d\n", nextTask->id);
#endif
    return nextTask;
}

void handle_task_return_to_dispatcher(task_t *task) {
    switch (task->status) {
    case READY:
        break;
    case RUNNING:
        break;
    case SUSPENDED:
        break;
    case TERMINATED:
#ifdef DEBUG
        printf("Freeing task: %d\n", task->id);
#endif
        free(task->stack);
        userTasks--;
        break;

    default:
        printf("Dispatcher handler default case, status: %d\n", task->status);
        break;
    }
}

void dispatcher() {
#ifdef DEBUG
    printf("\nPPOS: running dispatcher with %d user tasks\n\n", userTasks);
#endif
    // while there is user tasks
    while (userTasks > 0) {
        // pick the next task to execute
        task_t *nextTask = scheduler();

        // scheduler choose a task?
        if (nextTask != NULL) {
            // switch the context to the next task
            currentTask->status = SUSPENDED;
            task_switch(nextTask);
            // returning to dispatcher, handle the task acording to it state
            handle_task_return_to_dispatcher(nextTask);
        }
#ifdef DEBUG
        printf("User tasks remaining: %d\n", userTasks);
#endif
    }
    task_exit(0);
}

void ppos_init() {
    // desativa o buffer da saida padrao (stdout), usado pela função printf
    setvbuf(stdout, 0, _IONBF, 0);
    mainTask.preemptable = 0;
    dispatcherTask.preemptable = 0;
    mainTask.id = 0;         // inicializa o id da main em 0
    currentTask = &mainTask; // inicia com o contexto principal

    task_create(&dispatcherTask, dispatcher, NULL);
    init_handler();
#ifdef DEBUG
    printf("\nPPOS: system initialized\n\n");
#endif
}
