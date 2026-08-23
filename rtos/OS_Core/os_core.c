#include "os_core.h"
#include "RP2040.h"       /* NVIC / SCB / SysTick / PendSV_IRQn */
#include <stdio.h>
#include <string.h>

os_list_node_t os_delay_list_head;
os_list_node_t os_global_task_list;
os_tcb_t os_idle_task_tcb;
uint32_t os_idle_task_stack[128];          /* 加大,避免中断压栈溢出 */
uint32_t os_timer_task_stack[128];         /* 加大,避免 os_delay/os_sched 栈溢出 */
os_tcb_t os_timer_task_tcb;
os_list_node_t os_ready_queue[32];
uint32_t os_ready_bitmap  = 0x00;
os_tcb_t *os_current_task = NULL;
os_tcb_t *os_next_task    = NULL;

volatile uint32_t os_idle_count = 0;
volatile uint8_t os_cpu_usage   = 0;
volatile uint32_t os_idle_max   = 0;
volatile uint8_t os_is_calibrated = 0;

volatile uint32_t os_sys_tick = 0;

/* EXC_RETURN:从 Handler 返回到 Thread mode,使用 PSP。
 * 由 PendSV_Handler 的汇编部分读取,避开 GAS "lr,=imm" 解析问题。 */
volatile uint32_t os_exc_return = 0xFFFFFFFDu;

os_list_node_t os_timer_list;

/*空闲任务
 *
 * 关键:idle_task 必须空转累加 idle_count,否则校准分母为 0。
 * 用 wfi 暂停 CPU 后,SysTick 唤醒时 idle_count 几乎为 0,
 * idle_max 校准失败,CPU% 永远 0%。
 *
 * 空转让 idle_count 每秒累加几十万次,反映 CPU 的真实空闲速度。
 */
void os_idle_task(void *param)
{
    (void)param;
    while (1) {
        os_idle_count++;
    }
}

/* 开机手动校准
 *
 * 注意:os_sched_init 阶段 RTOS 还没启动,idle_task 在 ready 队列但没运行。
 * 因此我们**手动模拟 idle_task** 的累加行为(空循环 + os_idle_count++)
 * 来估算 idle_max。
 *
 * 这是估算而非精确测量,适合 boot 阶段快速校准。如果要精确,
 * 应该在 RTOS 运行后用 calibration task 测量。
 */

#include "hardware/structs/timer.h"
static void os_run_calibration(void)
{
    os_idle_count = 0;

    // 1. 记录起始硬件微秒时间戳
    uint32_t t_start = timer_hw->timerawl;

    // 2. 纯净空转 1,000,000 次 (与 idle_task 的汇编指令完全一致，不夹带任何外设读取)
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        os_idle_count++;
    }

    // 3. 计算 100 万次消耗的精确微秒数 (dt)
    uint32_t dt_us = timer_hw->timerawl - t_start;
    if (dt_us == 0) dt_us = 1;

    // 4. 精确换算 1 秒 (1,000,000 us) 内该循环能跑的最大次数 (标尺)
    // 通常在 125MHz 下结果在 25,000,000 ~ 31,000,000 之间
    os_idle_max = (uint32_t)(((uint64_t)1000000 * 1000000) / dt_us);
    if (os_idle_max < 100) os_idle_max = 100;

    os_is_calibrated = 1;
    os_idle_count = 0;
}
uint32_t os_tick_get(){
return os_sys_tick;
}
/*任务创建*/
void os_task_create(os_tcb_t *tcb, const char *name, void (*task_func)(void *), void *param, uint32_t *stack_base, uint32_t stack_size, uint8_t prio, uint32_t time_slice)
{
    tcb->priority       = prio;
    tcb->ticks_to_delay = 0;
    tcb->state          = OS_TASK_STATE_SUSPENDED;
    os_list_init(&tcb->list_node);
    uint32_t *sp = stack_base + stack_size;
    sp -= 16;
    uint32_t *paint_ptr = stack_base;
    while (paint_ptr < sp) {
        *paint_ptr = 0xDEADBEEF;
        paint_ptr++;
    }
    sp[15]                 = (0x01 << 24);
    sp[14]                 = (uint32_t)task_func;
    sp[13]                 = 0x14141414;
    sp[8]                  = (uint32_t)param;
    tcb->stack_ptr         = sp;
    tcb->time_slice_reload = time_slice;
    tcb->time_slice        = time_slice;

    tcb->stack_base = stack_base;
    tcb->stack_size = stack_size;

    int i = 0;
    while (name[i] != '\0' && i < 14) {
        tcb->name[i] = name[i];
        i++;
    }
    while (i < 14) {
        tcb->name[i] = ' ';
        i++;
    }
    tcb->name[14] = '\0';
    os_list_init(&tcb->global_node);
    os_list_add(&os_global_task_list, &tcb->global_node);
}
/*初始化调度器*/
void os_sched_init(void)
{
    /* 不在这里启动 SysTick,放到 os_start() 中。
     * 避免在数据结构未完全初始化时,1ms SysTick 触发 os_tick_handler
     * 时访问未初始化的全局状态。 */
    for (int i = 0; i < 32; i++) {
        os_list_init(&os_ready_queue[i]);
    }
    os_list_init(&os_delay_list_head);
    os_list_init(&os_global_task_list);
    os_task_create(&os_idle_task_tcb, "IDLE", os_idle_task, NULL, os_idle_task_stack, 128, 0, 0);
    os_task_ready(&os_idle_task_tcb);
    os_timer_init();

    /* 开机校准:100ms 内只有 idle 任务在跑,测量 idle_count 增长速度。
     * 用 busy_wait_us 计时,不依赖 SysTick(还没启动)。
     * 这 100ms 是阻塞的,boot 阶段会卡 100ms。 */
    os_run_calibration();
}

void os_task_ready(os_tcb_t *tcb)
{
    if (tcb->state == OS_TASK_STATE_READY)
        return;
    tcb->state = OS_TASK_STATE_READY;
    os_list_add(&os_ready_queue[tcb->priority], &tcb->list_node);
    os_ready_bitmap |= (0x01 << (tcb->priority));
}
/*任务调度*/
void os_sched(void)
{
    // uint8_t highest_prio       = 31 - __clz(os_ready_bitmap);
    uint8_t highest_prio       = OS_PORT_GET_HIGHEST_PRI(os_ready_bitmap);
    os_list_node_t *first_node = os_ready_queue[highest_prio].next;
    os_tcb_t *next_task        = OS_TCB_FROM_NODE(first_node);
    if (next_task == os_current_task)
        return;
    if (os_current_task != NULL) {
        if (os_current_task->state == OS_TASK_STATE_RUNNING) {
            os_current_task->state = OS_TASK_STATE_READY;
        }
    }
    next_task->state = OS_TASK_STATE_RUNNING;
    os_next_task     = next_task;
    // SCB->ICSR |= (1 << 28); // 触发上下文切换
    OS_PORT_YIELD();

    // while(1);
}

void os_start(void)
{
    /* Cortex-M0+ 只有 0~3 四级优先级(2 位),3 =最低优先级 */
    NVIC_SetPriority(PendSV_IRQn, 3);
    NVIC_SetPriority(SysTick_IRQn, 3);

    /* PendSV 优先级应该比 SysTick 低(数值更大),这样 SysTick
     * 能抢占 PendSV 完成上下文切换?实际上要让 PendSV 优先级最低,
     * SysTick 不打断 PendSV。M0+ 上数值越大优先级越低,
     * PendSV IRQ number = -2 < SysTick IRQ number = -1,本身就 PendSV 优先 */
    __set_PSP(0);

    /* 所有数据结构就绪后才启动 SysTick,避免被打断 */
    os_port_systick_init();

    os_sched();
    while (1);
}

void os_delay(uint32_t ticks)
{
    if (ticks == 0)
        return;
    os_current_task->ticks_to_delay = ticks;
    os_current_task->state          = OS_TASK_STATE_BLOCKED;

    os_list_remove(&os_current_task->list_node);
    if (os_ready_queue[os_current_task->priority].next == &os_ready_queue[os_current_task->priority]) {
        os_ready_bitmap &= ~(0x01 << os_current_task->priority);
    }
    os_list_add(&os_delay_list_head, &os_current_task->list_node);
    os_sched();
}

void os_tick_handler(void)
{
    static uint32_t tick_counter = 0;
    os_sys_tick++;
    tick_counter++;
  if (tick_counter >= 500) {
        tick_counter = 0;

        if (os_is_calibrated && os_idle_max > 0) {
            // 500ms 对应的标尺是 1秒标尺的一半
            uint32_t half_idle_max = os_idle_max / 2;

            if (os_idle_count > half_idle_max) {
                half_idle_max = os_idle_count;
                os_idle_max = half_idle_max * 2;
            }

            // 计算最近 500ms 的瞬时占用率
            uint32_t instant_usage = 100u - (uint32_t)((uint64_t)os_idle_count * 100u / half_idle_max);
            if (instant_usage > 100) instant_usage = 100;

            // 一阶低通滤波 (EMA)：消除采样相位错位引起的抖动
            if (os_cpu_usage == 0) {
                os_cpu_usage = (uint8_t)instant_usage;
            } else {
                os_cpu_usage = (uint8_t)((os_cpu_usage * 7 + instant_usage * 3) / 10);
            }
        } else {
            os_cpu_usage = 0;
        }
        os_idle_count = 0;
    }
    if (os_current_task == NULL)
        return;
    os_list_node_t *curr = os_delay_list_head.next;
    os_list_node_t *next_node;
    if (os_current_task->time_slice_reload > 0)
        if (os_current_task->time_slice > 0) {
            os_current_task->time_slice--;
            if (os_current_task->time_slice == 0) {
                os_current_task->time_slice = os_current_task->time_slice_reload;
                os_list_remove(&os_current_task->list_node);
                os_list_add(&os_ready_queue[os_current_task->priority], &os_current_task->list_node);
            }
        }
    while (curr != &os_delay_list_head) {
        next_node     = curr->next;
        os_tcb_t *tcb = OS_TCB_FROM_NODE(curr);
        if (tcb->ticks_to_delay > 0) {
            tcb->ticks_to_delay--;
            if (tcb->ticks_to_delay == 0) {
                os_list_remove(curr);
                os_task_ready(tcb);
            }
        }
        curr = next_node;
    }

    os_sched();
}

// ==================== 信号量 ====================

void os_sem_init(os_sem_t *sem, uint8_t value)
{
    sem->count = value;
    os_list_init(&sem->wait_node);
}

void os_sem_take(os_sem_t *sem)
{
    if (sem->count > 0) {
        sem->count--;
    } else {
        os_current_task->state = OS_TASK_STATE_BLOCKED;
        os_list_remove(&os_current_task->list_node);

        if (os_list_is_empty(&os_ready_queue[os_current_task->priority]))
            os_ready_bitmap &= ~(0x01 << os_current_task->priority);
        os_list_add(&sem->wait_node, &os_current_task->list_node);

        os_sched();
    }
}

void os_sem_give(os_sem_t *sem)
{
    if (!(os_list_is_empty(&sem->wait_node))) {
        os_list_node_t *node = sem->wait_node.next;
        os_list_remove(node);
        os_tcb_t *task = container_of(node, os_tcb_t, list_node);
        os_task_ready(task);

        os_sched();
    } else {
        sem->count = 1;
    }
}

// ==================== 互斥锁 ====================

void os_mutex_init(os_mutex_t *mutex)
{
    mutex->lock         = 0;
    mutex->owner        = NULL;
    mutex->original_pri = 0;
    os_list_init(&mutex->wait_node);
}

void os_mutex_take(os_mutex_t *mutex)
{
    if (mutex->lock == 0) {
        mutex->lock         = 1;
        mutex->owner        = os_current_task;
        mutex->original_pri = os_current_task->priority;
    } else {

        if (os_current_task->priority > mutex->owner->priority) {
            uint8_t old_pri = mutex->owner->priority;
            os_list_remove(&mutex->owner->list_node);
            if (os_list_is_empty(&os_ready_queue[old_pri]))
                os_ready_bitmap &= ~(0x01 << old_pri);
            mutex->owner->priority = os_current_task->priority;
            os_list_add(&os_ready_queue[mutex->owner->priority], &mutex->owner->list_node);
            os_ready_bitmap |= (0x01 << mutex->owner->priority);
        }

        os_current_task->state = OS_TASK_STATE_BLOCKED;
        os_list_remove(&os_current_task->list_node);
        if (os_list_is_empty(&os_ready_queue[os_current_task->priority]))
            os_ready_bitmap &= ~(0x01 << os_current_task->priority);
        os_list_add(&mutex->wait_node, &os_current_task->list_node);
        os_sched();
    }
}

// void os_mutex_give(os_mutex_t *mutex)
// {
//     if (os_current_task->priority != mutex->original_pri) { // if(mutex->owner->priority!=mutex->original_pri)
//         os_list_remove(&os_current_task->list_node);
//         if (os_list_is_empty(&os_ready_queue[os_current_task->priority]))
//             os_ready_bitmap &= ~(0x01 << os_current_task->priority);
//         os_current_task->priority = mutex->original_pri;
//         os_list_add(&os_ready_queue[os_current_task->priority], &os_current_task->list_node);
//         os_ready_bitmap |= (0x01 << os_current_task->priority);
//     }
//     if (!(os_list_is_empty(&mutex->wait_node))) {
//         os_list_node_t *node = mutex->wait_node.next;
//         os_list_remove(node);
//         os_tcb_t *task = container_of(node, os_tcb_t, list_node);
//         os_task_ready(task);
//         mutex->owner        = task;
//         mutex->original_pri = task->priority;
//         os_sched();
//     } else {
//         mutex->lock = 0;
//     }
// }
void os_mutex_give(os_mutex_t *mutex)
{
    // 安全校验：只有持有锁的任务才有资格释放[cite: 7]
    if (mutex->owner != os_current_task) {
        return;
    }

    if (os_current_task->priority != mutex->original_pri) {
        os_list_remove(&os_current_task->list_node);
        if (os_list_is_empty(&os_ready_queue[os_current_task->priority]))
            os_ready_bitmap &= ~(0x01 << os_current_task->priority);
        os_current_task->priority = mutex->original_pri;
        os_list_add(&os_ready_queue[os_current_task->priority], &os_current_task->list_node);
        os_ready_bitmap |= (0x01 << os_current_task->priority);
    }

    if (!(os_list_is_empty(&mutex->wait_node))) {
        os_list_node_t *node = mutex->wait_node.next;
        os_list_remove(node);
        os_tcb_t *task = container_of(node, os_tcb_t, list_node);
        os_task_ready(task);
        mutex->owner        = task;
        mutex->original_pri = task->priority;
        os_sched();
    } else {
        mutex->lock  = 0;
        mutex->owner = NULL;
    }
}
// ==================== 消息队列 ====================

void os_msg_init(os_msg_queue_t *msg, uint8_t *pool, uint16_t msg_size, uint16_t max_msg)
{
    msg->msg_pool = pool;
    msg->msg_size = msg_size;
    msg->max_msg  = max_msg;

    msg->msg_count = 0;
    msg->head      = 0;
    msg->tail      = 0;
    os_list_init(&msg->wait_node);
}
void os_msg_send(os_msg_queue_t *msg, void *data)
{
    if (msg->msg_count >= msg->max_msg)
        return;

    uint8_t *dest = msg->msg_pool + (msg->tail * msg->msg_size);

    memcpy(dest, data, msg->msg_size);

    msg->tail = (msg->tail + 1) % msg->max_msg;

    msg->msg_count++;
    if (!os_list_is_empty(&msg->wait_node)) {
        os_list_node_t *node = msg->wait_node.next;
        os_list_remove(node);
        os_tcb_t *task = container_of(node, os_tcb_t, list_node);
        os_task_ready(task);
        os_sched();
    }
}

void os_msg_recv(os_msg_queue_t *msg, void *data)
{
    if (msg->msg_count == 0) {
        os_current_task->state = OS_TASK_STATE_BLOCKED;
        os_list_remove(&os_current_task->list_node);
        if (os_list_is_empty(&os_ready_queue[os_current_task->priority]))
            os_ready_bitmap &= ~(0x01 << os_current_task->priority);
        os_list_add(&msg->wait_node, &os_current_task->list_node);
        os_sched();
    }

    uint8_t *src = msg->msg_pool + (msg->head * msg->msg_size);

    memcpy(data, src, msg->msg_size);
    msg->head = (msg->head + 1) % msg->max_msg;
    msg->msg_count--;
}
// ==================== 系统信息 ====================

void os_system_info(void)
{
    printf("\x1B[2J\x1B[H");
    printf("========================================================\n");
    printf("                MiniRTOS System Info                    \n");
    printf("========================================================\n");
    printf("Task Name       | State | Prio | Stack Used / Total (Words)\n");
    printf("--------------------------------------------------------\n");

    os_list_node_t *node = os_global_task_list.next;

    while (node != &os_global_task_list) {
        os_tcb_t *tcb         = container_of(node, os_tcb_t, global_node);
        const char *state_str = "UNK";
        if (tcb == os_current_task)
            state_str = "RUN";
        else if (tcb->state == OS_TASK_STATE_READY)
            state_str = "RDY";
        else if (tcb->state == OS_TASK_STATE_BLOCKED)
            state_str = "BLK";
        else if (tcb->state == OS_TASK_STATE_SUSPENDED)
            state_str = "SUS";

        uint32_t unused_words = 0;
        uint32_t *stack_ptr   = tcb->stack_base;
        while (stack_ptr < (tcb->stack_base + tcb->stack_size)) {
            if (*stack_ptr == 0xDEADBEEF) {
                unused_words++;
                stack_ptr++;
            } else
                break;
        }
        uint32_t used_words = tcb->stack_size - unused_words;
        printf("%s |  %s  |  %02d  |  %03lu / %03lu\n",
               tcb->name, state_str, tcb->priority,
               (unsigned long)used_words, (unsigned long)tcb->stack_size);

        node = node->next;
    }
    printf("========================================================\n");
    printf("CPU Usage: %d%%\n", os_cpu_usage);
    printf("========================================================\n");
    printf("idlemax:%ld\n",os_idle_max);
}

uint32_t os_task_stack_peak(const os_tcb_t *tcb)
{
    if (!tcb || !tcb->stack_base || tcb->stack_size == 0) return 0;
    /* 与 os_system_info 同样的扫描逻辑:从栈底向上找第一个非 0xDEADBEEF 的位置 */
    uint32_t unused = 0;
    uint32_t *p = tcb->stack_base;
    const uint32_t *end = tcb->stack_base + tcb->stack_size;
    while (p < end) {
        if (*p == 0xDEADBEEF) { unused++; p++; }
        else break;
    }
    return tcb->stack_size - unused;
}

// ==================== 软件定时 ====================

void os_timer_start(os_timer_t *timer, uint32_t delay, uint32_t period)
{
    timer->period       = period;
    timer->state        = 1;
    timer->timeout_tick = os_sys_tick + delay;
    os_list_add(&os_timer_list, &timer->list_node);
}

void os_timer_task(void *param)
{
    while (1) {
        uint32_t next_delay  = 0xFFFFFFFF;
        os_list_node_t *node = os_timer_list.next;

        while (node != &os_timer_list) {
            os_timer_t *timer = container_of(node, os_timer_t, list_node);
            if (timer->state == 1) {
                if (os_sys_tick >= timer->timeout_tick) {
                    timer->cb(timer->arg);
                    if (timer->period == 0)
                        timer->state = 0;
                    else
                        timer->timeout_tick += timer->period;
                    }
                    if (timer->state == 1) {
                      next_delay= ((timer->timeout_tick-os_sys_tick)<next_delay) ? timer->timeout_tick-os_sys_tick :next_delay;
                        }
                }
                node =node->next;
            }
            os_delay(next_delay);
        }
    }


void os_timer_init()
{
    os_list_init(&os_timer_list);
    os_task_create(&os_timer_task_tcb,
                   "TimerTask",
                   os_timer_task,
                   NULL,
                   os_timer_task_stack,
                   128,             /* 与全局数组大小匹配 */
                   31,
                   1);
    os_task_ready(&os_timer_task_tcb);

}

// ==================== 中断 ====================
// 中断专用的信号量释放接口
void os_sem_give_from_isr(os_sem_t *sem)
{
    if (!(os_list_is_empty(&sem->wait_node))) {
        os_list_node_t *node = sem->wait_node.next;
        os_list_remove(node);
        os_tcb_t *task = container_of(node, os_tcb_t, list_node);
        os_task_ready(task);

        // 挂起 PendSV，待 ISR 退出后统一执行调度[cite: 7]
        SCB->ICSR = (1 << 28);
    } else {
        sem->count = 1;
    }
}


