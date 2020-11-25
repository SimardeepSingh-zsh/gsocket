#ifndef __GS_LIST_H__
#define __GS_LIST_H__ 1

typedef struct
{
	void *next;
	void *prev;
	void *gsl;   // Pointer to GS_LIST context
	uint64_t id;
	int add_id;
	int is_calloc;
	void *data;
} GS_LIST_ITEM;

typedef struct
{
	GS_LIST_ITEM *head;
	GS_LIST_ITEM *tail;
	int n_items;
	int add_count;
	int opt;
} GS_LIST;


int GS_LIST_init(GS_LIST *gsl, int opt);
GS_LIST_ITEM *GS_LIST_add(GS_LIST *gsl, GS_LIST_ITEM *src_li, void *data, uint64_t id);
int GS_LIST_del(GS_LIST_ITEM *li);
GS_LIST_ITEM *GS_LIST_next(GS_LIST *gsl, GS_LIST_ITEM *li);
GS_LIST_ITEM *GS_LIST_by_pos(GS_LIST *gsl, int pos);
void GS_LIST_relink(GS_LIST_ITEM *li, uint64_t id);


#endif /* !__GS_LIST_H__ */