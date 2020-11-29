#include "list.h"

#include "gcc-plugin.h"
#include "tree.h"

#include <stddef.h>

void list_init(struct list *l)
{
        gcc_assert(l != NULL);

        l->head = NULL;
}

void list_destroy(struct list *l)
{
        gcc_assert(l != NULL);

        struct list_node *cursor = l->head;
        while (cursor != NULL) {
                struct list_node *tmp = cursor;
                cursor = cursor->next;
                free(tmp);
        }
}

void list_add(struct list *l, tree record_type)
{
        gcc_assert(l != NULL);
        gcc_assert(TYPE_P(record_type));

        struct list_node *newnode = (struct list_node *)xmalloc(sizeof(*newnode));
        gcc_assert(newnode);

        newnode->record_type = record_type;
        newnode->next = l->head;
        l->head = newnode;
}

bool list_contains(struct list *l, tree record_type)
{
        gcc_assert(l != NULL);
        gcc_assert(TYPE_P(record_type));

        struct list_node *cursor = l->head;
        while (cursor != NULL) {
                if (cursor->record_type == record_type) {
                        return (true);
                }

                cursor = cursor->next;
        }

        return (false);
}
