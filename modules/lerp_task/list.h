/**
 * @file leos_list.h
 * @author Lee Essen (lee.essen@nowonline.co.uk)
 * @brief 
 * @version 0.1
 * @date 2022-04-15
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#ifndef _LEOS_LIST_H
#define _LEOS_LIST_H
        
#define DEFINE_LIST(name, type)           struct { type *head; type *tail; } name = { .head=NULL, .tail=NULL };   
#define LIST_REF(name, type)              type name##_next; type name##_prev
    
//
// Add to the end of a given list (using 'ref' next/prev items)
//
#define LIST_ADD(list,item,ref) \
                            (item)->ref##_prev = (list)->tail; \
                            if ((list)->tail) { \
                                (list)->tail->ref##_next = (item); \
                            } \
                            (list)->tail = (item); \
                            if (!(list)->head) (list)->head = (item);

//
// Add to a list, before the given item (if item is NULL then at tail)
//
#define LIST_ADD_BEFORE(list,item,ref,other) \
                            (item)->ref##_next = (other); \
                            if (other) { \
                                (item)->ref##_prev = (other)->ref##_prev; \
                            } else { \
                                (item)->ref##_prev = (list)->tail; \
                            } \
                            if ((item)->ref##_prev) { \
                                (item)->ref##_prev->ref##_next = (item); \
                            } else { \
                                (list)->head = (item); \
                            } \
                            if ((item)->ref##_next) { \
                                (item)->ref##_next->ref##_prev = (item); \
                            } else { \
                                (list)->tail = (item); \
                            }

//
// Add to the front of a given list (using 'ref' next/prev items)
//
#define LIST_PUSH(list,item,ref) \
                            (item)->ref##_next = (list)->head; \
                            if ((list)->head) { \
                                (list)->head->ref##_prev = (item); \
                            } \
                            (list)->head = (item); \
                            if (!(list)->tail) (list)->tail = (item);
    

//
// Pop the first item from the list (need to provide type for this)
//
#define LIST_POP(list,ref,type)  ({ \
                            type v = (list)->head; \
                            if (v) { \
                                (list)->head = v->ref##_next; \
                                if ((list)->head) { \
                                    (list)->head->ref##_prev = NULL; \
                                } else { \
                                    (list)->tail = NULL; \
                                } \
                                v->ref##_next = NULL; \
                            }  v; })
                               
//
// Remove an item from the list
//
#define LIST_REMOVE(list,item,ref) \
                            if ((item) == (list)->head) { \
                                (list)->head = (item)->ref##_next; \
                            } \
                            if ((item) == (list)->tail) { \
                                (list)->tail = (item)->ref##_prev; \
                            } \
                            if ((item)->ref##_next) { \
                                (item)->ref##_next->ref##_prev = (item)->ref##_prev; \
                            } \
                            if ((item)->ref##_prev) { \
                                (item)->ref##_prev->ref##_next = (item)->ref##_next; \
                            } \
                            (item)->ref##_next = NULL; \
                            (item)->ref##_prev = NULL;
    

//
// Add an item to a list taking into account a field (time) that represents a time
// delta. So the item goes in the correct place and the timings are updated accordingly.
//
#define LIST_ADD_TO_TIMED(list, item, ref, time) \
                            { \
                                typeof(item) e = (list)->head; \
                                while (e) { \
                                    if (e->time > (item)->time) break; \
                                   (item)->time -= e->time; \
                                    e = e->ref##_next; \
                                } \
                                LIST_ADD_BEFORE(list, item, ref, e); \
                                if (e) e->time -= (item)->time; \
                            }

//
// Remove an item from the timed list, ensuring any remaining time is added back
// to the next item.
//
#define LIST_REMOVE_FROM_TIMED(list, item, ref, time) \
                            if ((item)->time && (item)->ref##_next) { \
                                (item)->ref##_next->time += (item)->time; \
                            } \
                            LIST_REMOVE(list, item, ref);

//
// Reduce the time on the list by a given amount, may involve changing multiple
// records but won't take time below zero
//
#define LIST_UPDATE_TIMED(list, ref, time, delta) \
                            { \
                                uint32_t d = (delta); \
                                typeof((list)->head) e = (list)->head; \
                                while (e) { \
                                    if (e->time < d) { \
                                        d -= e->time; \
                                        e->time = 0; \
                                        e = e->ref##_next; \
                                        continue; \
                                    } \
                                    e->time -= (delta); \
                                    break; \
                                } \
                            }

    
#endif     // _LIST_H