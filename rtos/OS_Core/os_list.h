#ifndef OS_LIST_H
#define OS_LIST_H
#include <stdint.h>
#include <stddef.h>
// 任务节点
typedef struct os_list_node {
    struct os_list_node *prev;
    struct os_list_node *next;
} os_list_node_t;

static inline void os_list_init(os_list_node_t *node)
{
    node->prev = node;
    node->next = node;
}
static inline void os_list_remove(os_list_node_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next       = node;
    node->prev       = node;
}

static inline void os_list_add(os_list_node_t *head, os_list_node_t *node)
{
    node->next=head;
    node->prev=head->prev;

    head->prev->next=node;
    head->prev=node;
}
static inline uint8_t os_list_is_empty(os_list_node_t *head)
{
    return head->next==head;
}

#endif // !OS_LIST_H
