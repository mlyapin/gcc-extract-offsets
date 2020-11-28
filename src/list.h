#ifndef LIST_H
#define LIST_H

#include "gcc-plugin.h"

#include <stddef.h>

struct list_node {
        struct list_node *next;
        tree record_type;
};

struct list {
        struct list_node *head;
};

void list_init(struct list *);

void list_destroy(struct list *);

void list_add(struct list *, tree record_type);

bool list_contains(struct list *, tree record_type);

#endif /* LIST_H */
