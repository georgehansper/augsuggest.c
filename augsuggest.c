/*      vim: expandtab:softtabstop=2:tabstop=2:shiftwidth=2
 *
 *      Copyright (C) 2022 George Hansper <george@hansper.id.au>
 *
 * This tool produces output similar to the augeas 'print' statement
 *
 * Where augeas print may produce a set of paths and values like this:
 *
 * /files/some/path/label[1]/tail_a value_1a
 * /files/some/path/label[1]/tail_b value_1b
 * /files/some/path/label[2]/tail_a value_2a
 * /files/some/path/label[2]/tail_b value_2b
 *
 * or
 *
 * /files/some/path/1/tail_a value_1a
 * /files/some/path/1/tail_b value_1b
 * /files/some/path/2/tail_a value_2a
 * /files/some/path/2/tail_b value_2b
 *
 *
 * This tool replaces the abosolute 'position' (1, 2, 3,..) with a path-expression that matches the position
 * where the values are used to identify the position
 *
 * /files/some/path/label[tail_a = value_1a]/tail_a value_1a
 * /files/some/path/label[tail_a = value_1a]/tail_b value_1b
 * /files/some/path/label[tail_a = value_2a]/tail_a value_2a
 * /files/some/path/label[tail_a = value_2a]/tail_b value_2b
 *
 * Terms used within:
 *
 * /files/some/path/label[1]/tail_a value_1a
 * `--------------------' \ `-----' `------'
 *         `--- head       \    \       `--- value
 *                          \    `--- tail
 *                           `-- position
 *
 * For more complex paths
 * /files/some/path/1/segement/label[1]/tail_a value_1a
 *                    `----------------------'
 *                             |
 *                             v
 *                   /segement/label/tail_a
 *                   `--------------------'
 *                             `--- simple_tail
 */

#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>        /* for exit, strtoul */
#include <getopt.h>
#include <string.h>
#include <augeas.h>
#include "augsuggest.h"
#include <malloc.h>
#include <sys/param.h>     /* for MIN() MAX() */

#define CHECK_OOM(condition, action, arg)         \
    do {                                          \
        if (condition) {                          \
            out_of_memory|=1;                     \
            action(arg);                          \
        }                                         \
    } while (0)

#define MAX_PRETTY_WIDTH 30

static augeas *aug = NULL;
static unsigned int flags = AUG_NONE;
static unsigned int num_groups = 0;
static struct group **all_groups=NULL;
static char **all_matches;
static struct augeas_path_value **all_augeas_paths; /* array of pointers */

int out_of_memory=0;
int verbose=0;
int debug=0;
int pretty=0;
int compact=0;
int noseq=0;
int help=0;
int use_regexp=0;
char *lens = NULL;
char *loadpath = NULL;

char *str_next_pos(char *start, char **head_end, unsigned long *pos);
char *str_simplified_tail(char *tail_orig);
void add_segment_to_group(struct path_segment *segment, struct augeas_path_value *);
char *quote_value(char *);
char *regexp_value(char *, int);


void exit_oom(char *msg) {
  if( out_of_memory ) {
    fprintf(stderr, "Out of memory%s%s\n", msg ? " ": "", msg);
  }
  exit(1);
}

/* ----- split_path() str_next_pos() str_simplified_tail() add_segment_to_group() ----- */
/* split_path()
 * Break up a path like this
 *   /head/label_a[123]/middle/label_b[456]/tail
 *
 * into (struct path_segment) segments like this
 *
 * head     = "/head/label_a"
 * segement = "/head/label_a"
 * position = 123
 * simplified_tail = "/middle/label_b/tail"
 *
 * head     = "/head/label_a[123]/middle/label_b"
 * segement =                   "/middle/label_b"
 * position = 456
 * simplified_tail = "/tail"
 *
 * head     = "/head/label_a[123]/middle/label_b[456]/tail"
 * segement =                   "/tail"
 * position = ULONG_MAX (-1)
 * simplified_tail = ""
 *
 * If label_a or label_b is absent, use seq::* or * instead in the simplified tail, head is unaffected
 */
struct path_segment *split_path(struct augeas_path_value *path_value) {
  char *path = path_value->path;
  struct path_segment *first_segment = NULL;
  struct path_segment *this_segment = NULL;
  struct path_segment **next_segment = &first_segment;
  unsigned long position;
  char *head_end;
  char *path_seg_start=path;
  char *path_seg_end;

  while(*path_seg_start) {
    this_segment = malloc(sizeof(struct path_segment));
    CHECK_OOM(! this_segment, exit_oom, "split_path() allocating struct path_segment");

    *next_segment  = this_segment;
    path_seg_end   = str_next_pos(path_seg_start, &head_end, &position);
    this_segment->head     = strndup(path, (head_end-path));
    this_segment->segment  = (this_segment->head) + (path_seg_start-path);
    this_segment->position = position;
    this_segment->simplified_tail = str_simplified_tail(path_seg_end);
    path_seg_start = path_seg_end;
    this_segment->next     = NULL;
    next_segment = &(this_segment->next);
    if ( position != ULONG_MAX ) {
      add_segment_to_group(this_segment, path_value);
    } else {
      this_segment->group = NULL;
    }
    if(debug) fprintf(stderr,"head = '%s', segment = '%s' group = %lx path_seg_start = %s\n", this_segment->head, this_segment->segment, (unsigned long) this_segment->group, path_seg_start);
    if(debug) fprintf(stderr,"first_segment = '%s', this_segment = '%s'\n", first_segment->segment, this_segment->segment);
    if(debug) fprintf(stderr,"first_segment = %lx, this_segment = %lx\n", (unsigned long) first_segment, (unsigned long) this_segment);
    if(debug) fprintf(stderr,"split_path() head = %s\n",this_segment->head);
  }
  return(first_segment);
}

/*
 * str_next_pos() scans a string from (char *)start, and finds the next occurance
 * of the substring '[123]' or '/123/' where 123 is a decimal number
 * (int *)pos is set to the value of 123
 * if [123] is found,
 *   - head_end points to the character '['
 *   - returns a  pointer to the character after the ']'
 * if /123/  or /123\0 is found
 *   - head_end points to character after the first '/'
 *   - returns a pointer to the second '/' or '\0'
 * if none of the above are found
 *   - head_end points to the terminating '\0'
 *   - returns a pointer to the terminating '\0' (same as head_end)
 * ie. look for [123] or /123/ or /123\0, set int pos to 123 or ULONG_MAX, set head_end to char before '[' or before 1st digit; return pointer to trailing / or \0
*/
char *str_next_pos(char *start, char **head_end, unsigned long *pos) {
  char *endptr=NULL;
  char *s=start;
  *pos=ULONG_MAX;
  while(*s) {
    if( *s=='[' && *(s+1) >= '0' && *(s+1) <= '9' ) {
        *pos = strtoul(s+1, &endptr, 10);
        if ( *endptr == ']' ) {
          /* success */
          *head_end = s;
          return(endptr+1);
        }
    } else if ( *s == '/' && *(s+1) >= '0' && *(s+1) <= '9' ) {
        *pos = strtoul(s+1, &endptr, 10);
        if ( *endptr == '\0' || *endptr == '/' ) {
          /* success */
          *head_end = s+1;
          return(endptr);
        }
    }
    s++;
  }
  *head_end=s;
  return(s);
}

char *str_simplified_tail(char *tail_orig) {
  int tail_len=0;
  char *tail;
  char *from, *to, *scan;
  char *simple;
  /* first work out how much space we will need to allocate */
  tail=tail_orig;
  while(*tail) {
    if( *tail == '[' && *(tail+1) >= '0' && *(tail+1) <= '9' ) {
      /* Look for matching ']' */
      scan = tail;
      scan++;
      while (*scan >= '0' && *scan <= '9')
        scan++;
      if(*scan == ']') {
        tail=scan+1;
        continue;
      }
    } else if ( *tail == '/' && *(tail+1) >= '0' && *(tail+1) <= '9' ) {
      /* Look for next '/' or '\0' */
      scan = tail;
      scan++;
      while (*scan >= '0' && *scan <= '9')
        scan++;
      if(*scan == '/' || *scan == '\0' ) {
        tail=scan;
        tail_len += 7; /* allow for /seq::* */
        continue;
      }
    }
    tail_len++;
    tail++;
  }
  simple = (char *) malloc( sizeof(char) * (tail_len+1));
  CHECK_OOM( ! simple, exit_oom, "allocating simple_tail in str_simplified_tail()");

  from=tail_orig;
  to=simple;
  while(*from) {
    if( *from == '[' && *(from+1) >= '0' && *(from+1) <= '9' ) {
      /* skip over  [123] */
      scan = from;
      scan++;
      while (*scan >= '0' && *scan <= '9')
        scan++;
      if(*scan == ']') {
        from=scan+1;
        continue;
      }
    } else if ( *from == '/' && *(from+1) >= '0' && *(from+1) <= '9' ) {
      /* replace /123 with /seq::*  */
      scan = from;
      scan++;
      while (*scan >= '0' && *scan <= '9')
        scan++;
      if(*scan == '/' || *scan == '\0' ) {
        from=scan;
        if ( noseq ) {
          strcpy(to,"/*");
          to += 2;
        } else {
          strcpy(to,"/seq::*");
          to += 7; /* len("/seq::*") */
        }
        continue;
      }
    }
    *to++ = *from++; /* copy */
  }
  *to='\0';
  if(debug && *simple != '\0' ) fprintf(stderr,"simplified_tail: %s\n",simple);
  return(simple);
}

/* Compare two values (char *), subject to use_regexp
 * If both pointers are NULL, return 1 (true)
 * If only one pointer is NULL, return 0 (false)
 * set *matched to the number of characters in common
 * return 1 (true) if the strings match, otherwise 0 (false)
 */
int value_cmp(char *v1, char *v2, unsigned int *matched) {
  char *s1, *s2;
  if( v1 == NULL && v2 == NULL ) {
    *matched = 0;
    return(1);
  }
  if( v1 == NULL || v2 == NULL ) {
    *matched = 0;
    return(0);
  }
  s1 = v1;
  s2 = v2;
  *matched = 0;
  if( use_regexp ) {
    /* Compare values, allowing for the fact that ']' is replaced with '.' */
    while( *s1 || *s2 ) {
      if( *s1 != *s2 ) {
        if( *s1 =='\0' || *s2 == '\0')
          return(0);
        if( *s1 != ']' && *s2 != ']' )
          return(0);
      }
      s1++; s2++; (*matched)++;
    }
    return(1);
  } else {
    while( *s1 == *s2 ) {
      if( *s1 == '\0' ) {
        return(1);
      }
      s1++; s2++; (*matched)++;
    }
    return(0);
  }
  return(1); /* unreachable */
}

/* Find an existing group with the same 'head'
 * If no such group exists, create a new one
 * Update the size of all_groups array if required
 */
struct group *find_or_create_group(char *head) {
  unsigned long ndx;
  struct group **all_groups_realloc;
  unsigned int num_groups_newsize;
  struct group *group = NULL;
  if(debug) fprintf(stderr,"find_or_create_group(%s)\n",head);
  /* Look for an existing group with group->head matching path_seg->head */
  for(ndx=0; ndx < num_groups; ndx++) {
    if( strcmp(head, all_groups[ndx]->head) == 0 ) {
      group = all_groups[ndx];
      return(group);
    }
  }
  /* Group not found - create a new one */
  /* First, grow all_groups[] array if required */
  if ( num_groups % 32 == 0 ) {
      num_groups_newsize = (num_groups)/32*32+32;
      all_groups_realloc = reallocarray(all_groups, sizeof(struct group *), num_groups_newsize);
      CHECK_OOM( ! all_groups_realloc, exit_oom, "in find_or_create_group()");
      if(debug) fprintf(stderr,"Increased all_groups to %u members (num_groups=%u)\n", num_groups_newsize, num_groups);

      all_groups=all_groups_realloc;
  }
  /* Create new group */
  group = malloc(sizeof(struct group));
  CHECK_OOM( ! group, exit_oom, "allocating struct group in find_or_create_group()");

  all_groups[num_groups++] = group;
  group->head = head;
  group->all_tails = NULL;
  group->position_array_size = 0;
  group->tails_at_position = NULL;
  group->chosen_tail = NULL;
  group->chosen_tail_state = NULL;
  group->first_tail = NULL;
  group->position_array_size = 0;
  group->max_position = 0;
  group->subgroups = NULL;  /* subgroups are only created if we need to use our 3rd preference */
  group->subgroup_position = NULL;
  /* for --pretty */
  group->pretty_width_ct = NULL;
  /* for --regexp */
  group->re_width_ct = NULL;
  group->re_width_ft = NULL;

  return(group);
}

/* Find a matching tail+value within group->all_tails linked list
 * If no such tail exists, append a new (struct tail) list item
 * Return the tail found, or the new fail
 */
struct tail *find_or_create_tail(struct group *group, struct path_segment *path_seg, struct augeas_path_value *path_value) {
  /* Scan for a matching simplified tail+value in group->all_tails */
  struct tail *tail;
  struct tail *found_tail_value=NULL;
  struct tail *found_tail=NULL;
  struct tail **all_tails_end;
  unsigned int tail_found_this_pos=1;
  unsigned int match_length;
  if(debug) fprintf(stderr,"find_or_create_tail(tail=%s, position=%lu) value=%s\n",path_seg->simplified_tail, path_seg->position,path_value->value_qq);
  all_tails_end =&(group->all_tails);
  found_tail_value=NULL;
  for( tail = group->all_tails; tail != NULL; tail=tail->next ) {
    if( strcmp(path_seg->simplified_tail, tail->simple_tail) == 0 ) {
      /* found matching simple_tail - increment counters */
      tail->tail_found_map[path_seg->position]++;
      tail_found_this_pos = tail->tail_found_map[path_seg->position];
#if 0
      if ( ( value == NULL && tail->value == NULL )
           || ( value != NULL && tail->value != NULL && strcmp(tail->value,value) == 0 )) {
#else
      if ( value_cmp(tail->value, path_value->value, &match_length ) ) {
#endif
        /* matching tail+value found, increment tail_value_found */
        tail->tail_value_found_map[path_seg->position]++;
        tail->tail_value_found++;
        found_tail_value=tail;
      }
      found_tail=tail;
    }
    all_tails_end=&tail->next;
  }
  if ( found_tail_value == NULL ) {
    /* matching tail+value not found, create a new one */
    tail = malloc(sizeof(struct tail));
    CHECK_OOM( ! tail, exit_oom, "in find_or_create_tail()");

    tail->tail_found_map       = reallocarray(NULL, sizeof(unsigned int), group->position_array_size);
    CHECK_OOM( ! tail->tail_found_map, exit_oom, "in find_or_create_tail()");

    tail->tail_value_found_map = reallocarray(NULL, sizeof(unsigned int), group->position_array_size);
    CHECK_OOM( ! tail->tail_value_found_map, exit_oom, "in find_or_create_tail()");


    for(unsigned int i=0; i<group->position_array_size; i++) {
      tail->tail_found_map[i]=0;
      tail->tail_value_found_map[i]=0;
    }

    if ( found_tail ) {
      for( unsigned long ndx=0; ndx<=group->max_position; ndx++ ) {
        tail->tail_found_map[ndx] = found_tail->tail_found_map[ndx];
      }
    }
    tail->tail_found_map[path_seg->position]=tail_found_this_pos;
    tail->tail_value_found_map[path_seg->position]=1;
    tail->tail_value_found = 1;
    tail->simple_tail = path_seg->simplified_tail;
    tail->value       = path_value->value;
    tail->value_qq    = path_value->value_qq;
#if 0
    tail->value_re    = path_value->value_re;
#endif
    tail->next        = NULL;
    *all_tails_end = tail;
    return(tail);
  } else {
    return(found_tail_value);
  }
}

/* Append a (struct tail_stub) to the linked list group->tails_at_position[position] */
void append_tail_stub(struct group *group, struct tail *tail, unsigned long position) {
  struct tail_stub **tail_stub_pp;
  if(debug) fprintf(stderr,"append_tail_stub() position=%lu size=%lu tail=%s value=%s\n",position, group->position_array_size, tail->simple_tail, tail->value_qq);

  for( tail_stub_pp=&(group->tails_at_position[position]); *tail_stub_pp != NULL; tail_stub_pp=&(*tail_stub_pp)->next ) {
    if(debug) fprintf(stderr,"  append_tail_stub() %s=%s\n",(*tail_stub_pp)->tail->simple_tail, (*tail_stub_pp)->tail->value_qq);
  }
  *tail_stub_pp = malloc(sizeof(struct tail_stub));
  CHECK_OOM( ! *tail_stub_pp, exit_oom, "in append_tail_stub()");

  (*tail_stub_pp)->tail     = tail;
  (*tail_stub_pp)->next     = NULL;
}

/* Grow memory structures within the group record and associated tail records
 * to accommodate additional positions
 */
void grow_position_arrays(struct group *group, unsigned long new_max_position) {
  struct tail_stub **tails_at_position_realloc;
  struct tail     **chosen_tail_realloc;
  struct tail     **first_tail_realloc;
  unsigned int     *chosen_tail_state_realloc;
  unsigned int     *pretty_width_ct_realloc;
  unsigned int     *re_width_ct_realloc;
  unsigned int     *re_width_ft_realloc;
  if( new_max_position != ULONG_MAX && new_max_position >= group->position_array_size ) {
    unsigned long old_size = group->position_array_size;
    unsigned long new_size = (new_max_position+1) / 8 * 8 + 8;
    if(debug) fprintf(stderr, "--- grow_position_arrays() group=%s position = %lu ), new_size = %lu\n", group->head, new_max_position, new_size);

    /* Grow arrays within struct group */
    tails_at_position_realloc = reallocarray(group->tails_at_position,  sizeof(struct tail_stub *),  new_size);
    chosen_tail_realloc       = reallocarray(group->chosen_tail,        sizeof(struct tail *),       new_size);
    first_tail_realloc        = reallocarray(group->first_tail,         sizeof(struct tail *),       new_size);
    chosen_tail_state_realloc = reallocarray(group->chosen_tail_state,  sizeof(chosen_tail_state_t), new_size);
    pretty_width_ct_realloc   = reallocarray(group->pretty_width_ct,    sizeof(unsigned int),        new_size);
    re_width_ct_realloc       = reallocarray(group->re_width_ct,        sizeof(unsigned int),        new_size);
    re_width_ft_realloc       = reallocarray(group->re_width_ft,        sizeof(unsigned int),        new_size);
    CHECK_OOM( ! tails_at_position_realloc || ! chosen_tail_realloc || ! chosen_tail_state_realloc ||
               ! pretty_width_ct_realloc   || ! re_width_ct_realloc || ! re_width_ft_realloc       ||
               ! first_tail_realloc, exit_oom, "in grow_position_arrays()");

    /* initialize array entries between old size to new_size */
    unsigned long ndx;
    for( ndx=old_size; ndx < new_size; ndx++) {
      tails_at_position_realloc[ndx]=NULL;
      chosen_tail_realloc[ndx]=NULL;
      first_tail_realloc[ndx]=NULL;
      chosen_tail_state_realloc[ndx] = NOT_DONE;
      pretty_width_ct_realloc[ndx] = 0;
      re_width_ct_realloc[ndx] = 0;
      re_width_ft_realloc[ndx] = 0;
    }
    group->tails_at_position = tails_at_position_realloc;
    group->chosen_tail = chosen_tail_realloc;
    group->first_tail  = first_tail_realloc;
    group->chosen_tail_state = chosen_tail_state_realloc;
    group->pretty_width_ct = pretty_width_ct_realloc;
    group->re_width_ct = re_width_ct_realloc;
    group->re_width_ft = re_width_ft_realloc;
    /* Grow arrays in all_tails */
    struct tail *tail;
    for( tail = group->all_tails; tail != NULL; tail=tail->next ) {
      unsigned int *tail_found_map_realloc;
      unsigned int *tail_value_found_map_realloc;
      tail_found_map_realloc       = reallocarray(tail->tail_found_map,       sizeof(unsigned int), new_size);
      tail_value_found_map_realloc = reallocarray(tail->tail_value_found_map, sizeof(unsigned int), new_size);
      CHECK_OOM( ! tail_found_map_realloc || ! tail_value_found_map_realloc, exit_oom, "in grow_position_arrays()");

      /* initialize array entries between old size to new_size */
      unsigned long ndx;
      for( ndx=old_size; ndx < new_size; ndx++) {
        tail_found_map_realloc[ndx]=0;
        tail_value_found_map_realloc[ndx]=0;
      }
      tail->tail_found_map = tail_found_map_realloc;
      tail->tail_value_found_map = tail_value_found_map_realloc;
    }
    group->position_array_size = new_size;
  }
}

void add_segment_to_group(struct path_segment *path_seg, struct augeas_path_value *path_value) {
  struct group *group = NULL;
  struct tail  *tail;
  group = find_or_create_group(path_seg->head);

  /* group is our new or matching group for this segment->head */
  path_seg->group = group;
  if( path_seg->position != ULONG_MAX && path_seg->position > group->max_position ) {
    group->max_position = path_seg->position;
    if( group->max_position >= group->position_array_size ) {
      /* grow arrays in group */
      grow_position_arrays(group, group->max_position);
    }
  }
  tail = find_or_create_tail(group, path_seg, path_value);

  /* Append a tail_stub record to the linked list @ group->tails_at_position[position] */
  append_tail_stub(group, tail, path_seg->position);
}

/* find_or_create_subgroup()
 * This is called from choose_tail(), and is only used if we need to go to our 3rd Preference
 */
struct subgroup *find_or_create_subgroup(struct group *group, struct tail *first_tail) {
  struct subgroup *subgroup_ptr;
  struct subgroup **sg_pp;
  for( sg_pp=&(group->subgroups); *sg_pp != NULL; sg_pp=&(*sg_pp)->next) {
    if( (*sg_pp)->first_tail == first_tail ) {
      return(*sg_pp);
    }
  }
  /* Create and populate subgroup */
  subgroup_ptr = (struct subgroup *) malloc( sizeof(struct subgroup));
  CHECK_OOM( ! subgroup_ptr, exit_oom, "in find_or_create_subgroup()");

  subgroup_ptr->next=NULL;
  subgroup_ptr->first_tail=first_tail;
  /* positions are 1..max_position, +1 for the terminating 0=end-of-list */
  subgroup_ptr->matching_positions = malloc( (group->max_position+1) * sizeof( unsigned long ));
  CHECK_OOM( ! subgroup_ptr->matching_positions, exit_oom, "in find_or_create_subgroup()");

  /* malloc group->subgroup_position if not already done */
  if ( ! group->subgroup_position ) {
    group->subgroup_position = malloc( (group->max_position+1) * sizeof( unsigned long ));
    CHECK_OOM( ! group->subgroup_position, exit_oom, "in find_or_create_subgroup()");

  }
  *sg_pp = subgroup_ptr; /* Append new subgroup record to list */
  /* populate matching_positions */
  unsigned long pos_ndx;
  unsigned long ndx = 0;
  for(pos_ndx=1; pos_ndx <= group->max_position; pos_ndx++ ){
    /* save the position if this tail+value exists for this position - not necessarily the first tail, we need to check all tails at this position */
    struct tail_stub *tail_stub_ptr;
    for( tail_stub_ptr = group->tails_at_position[pos_ndx]; tail_stub_ptr != NULL; tail_stub_ptr=tail_stub_ptr->next ) {
      if( tail_stub_ptr->tail == first_tail ) {
        subgroup_ptr->matching_positions[ndx++] = pos_ndx;
        group->subgroup_position[pos_ndx]=ndx; /* yes, we want ndx+1, because matching_positions index starts at 0, where as the fallback position starts at 1 */
        break;
      }
    }
  }
  subgroup_ptr->matching_positions[ndx] = 0;  /* 0 = end of list */
  return(subgroup_ptr);
}

/* str_ischild()
 * compare 2 strings which are  of the form simple_tail
 * return true(1)  if parent == /path and child == /path/tail
 * return false(0) if child == /pathother or child == /pat or anything else
 */
int str_ischild(char *parent, char *child) {
  while( *parent ) {
    if( *parent != *child ) {
      return(0);
    }
    parent++;
    child++;
  }
  if( *child == '/' ) {
    return(1);
  } else {
    return(0);
  }
}

/* Find the first tail in the linked-list that is not NULL, or has no child nodes
   * eg for paths starting with /head/123/... ignore the entry:
   *    /head/123 (null)
   * and any further paths like this
   *   head/123/tail (null)
   *   head/123/tail/child (null)
   * stop when we encounter a value, or find a tail that has no child nodes,
   * if the next tail is eg
   *   head/123/tail2
   * then head/123/tail/child is significant, and that becomes the first_tail
   */
struct tail_stub *find_first_tail(struct tail_stub *tail_stub_ptr) {
  if( tail_stub_ptr == NULL )
    return(NULL);
  for( ; tail_stub_ptr->next != NULL; tail_stub_ptr=tail_stub_ptr->next ) {
    if ( tail_stub_ptr->tail->value != NULL && tail_stub_ptr->tail->value[0] != '\0' ) {
      break;
    }
    if( ! str_ischild( tail_stub_ptr->tail->simple_tail, tail_stub_ptr->next->tail->simple_tail) ) {
      /* the next tail is not a child-node of this tail */
      break;
    }
  }
  return(tail_stub_ptr);
}

struct tail *choose_tail(struct group *group, unsigned long position ) {
  struct tail_stub *first_tail;
  struct tail_stub *tail_stub_ptr;
  unsigned int ndx;

  if( group->tails_at_position[position] == NULL ) {
    /* first_tail == NULL
     * this does not happen, because every position gets at least one tail of ""
     * eg, even if the value is NULL.
     *   /head/1   (null)
     *   ...simple_tail ""
     *   ...value NULL
     * paths without a position ( /head/tail ) are not added to any group
     * We can't do anything with this, use seq::* or [*] only (no value) */
    fprintf(stderr,"# choose_tail() %s[%lu] first_tail is NULL (internal error)\n", group->head, position);
    group->chosen_tail_state[position] = NO_CHILD_NODES;
    return(NULL);
  }

  /* find first "significant" tail. see find_first_tail() */
  first_tail = find_first_tail(group->tails_at_position[position]);
  group->first_tail[position] = first_tail->tail;
  if(debug) fprintf(stderr,"# choose_tail() %s[%lu] first_tail = %s\n", group->head, position, first_tail->tail->simple_tail);

  /* First preference - if the first-tail+value is unique, use that */
  if( first_tail->tail->tail_value_found == 1 ) {
      group->chosen_tail_state[position] = FIRST_TAIL;
  }
  if ( group->chosen_tail_state[position] == FIRST_TAIL ) {
    if(debug) fprintf(stderr, "# choose_tail() [%lu] 1st preference: using first tail %s[%lu] %s=%s\n",position, group->head,position,first_tail->tail->simple_tail, first_tail->tail->value_qq);
    return(first_tail->tail);
  }

  /* Second preference - find a unique tail+value that has only one value for this position and has the tail existing for all other positions */
  for( tail_stub_ptr=first_tail; tail_stub_ptr!=NULL; tail_stub_ptr=tail_stub_ptr->next) {
    if( tail_stub_ptr->tail->tail_value_found == 1 ) { /* tail_stub_ptr->tail->value can be NULL, just needs to be unique */
      int found=1;
      if (debug) fprintf(stderr, "# choose_tail() [%lu] found %s at", position, tail_stub_ptr->tail->simple_tail);
      for( ndx=1; ndx <= group->max_position; ndx++ ) {
        if(debug) fprintf(stderr, " %d", ndx);
        if( tail_stub_ptr->tail->tail_found_map[ndx] == 0 ) {
          /* tail does not exist for every position within this group */
          found=0;
          break;
        }
      }
      if(debug) fprintf(stderr, "\n");
      if ( found ) {
        /* This works only if chosen_tail->simple_tail is the first appearance of simple_tail at this position */
        struct tail_stub *tail_check_ptr;
        for( tail_check_ptr=first_tail; tail_check_ptr != tail_stub_ptr; tail_check_ptr=tail_check_ptr->next) {
          if( strcmp(tail_check_ptr->tail->simple_tail, tail_stub_ptr->tail->simple_tail ) == 0 ) {
            found=0;
          }
        }
      }
      if ( found ) {
        if (debug) fprintf(stderr, "# choose_tail() [%lu] 2nd preference first_tail: %s=%s found: %s = %s\n", position, first_tail->tail->simple_tail, first_tail->tail->value_qq,tail_stub_ptr->tail->simple_tail, tail_stub_ptr->tail->value_qq);
        group->chosen_tail_state[position] = CHOSEN_TAIL_START;
        return(tail_stub_ptr->tail);
      }
    } /* if ... tail_value_found == 1 */
  }

  /* Third preference - first tail is not unique but could make a unique combination with another tail */
  struct subgroup *subgroup_ptr = find_or_create_subgroup(group, first_tail->tail);
  for( tail_stub_ptr=first_tail->next; tail_stub_ptr!=NULL; tail_stub_ptr=tail_stub_ptr->next) {
    /* for each tail at this position (other than the first) */
    /* Find a tail at this position where:
     * a) tail+value is unique within this subgroup
     * b) tail exists at all positions within this subgroup
     */
    int found=1;
    if (debug) fprintf(stderr, "choose_tail() [%lu] 3rd preference: first_tail: %s=%s, candidate: %s=%s\n", position, first_tail->tail->simple_tail, first_tail->tail->value_qq, tail_stub_ptr->tail->simple_tail, tail_stub_ptr->tail->value_qq);
    for(ndx=0; subgroup_ptr->matching_positions[ndx] != 0; ndx++ ) {
      int pos=subgroup_ptr->matching_positions[ndx];
      if ( pos == position ) continue;
      if( tail_stub_ptr->tail->tail_value_found_map[pos] != 0 ) {
        /* tail+value is not unique within this subgroup */
        found=0;
        break;
      }
      if( tail_stub_ptr->tail->tail_found_map[pos] == 0 ) {
        /* tail does not exist for every position within this subgroup */
        found=0;
        break;
      }
    }
    if ( found ) {
      /* This works only if chosen_tail->simple_tail is the first appearance of simple_tail at this position */
      struct tail_stub *tail_check_ptr;
      for( tail_check_ptr=first_tail; tail_check_ptr != tail_stub_ptr; tail_check_ptr=tail_check_ptr->next) {
        if( strcmp(tail_check_ptr->tail->simple_tail, tail_stub_ptr->tail->simple_tail ) == 0 ) {
          found=0;
        }
      }
    }
    if ( found ) {
      if (debug) fprintf(stderr, "choose_tail() [%lu] 3rd preference: first_tail: %s=%s, candidate: %s=%s\n", position, first_tail->tail->simple_tail, first_tail->tail->value_qq, tail_stub_ptr->tail->simple_tail, tail_stub_ptr->tail->value_qq);
      group->chosen_tail_state[position] = CHOSEN_TAIL_PLUS_FIRST_TAIL_START;
      return(tail_stub_ptr->tail);
    }
  }
  /* Fourth preference (fallback) - use first_tail PLUS the position with the subgroup */
  if (debug) fprintf(stderr, "choose_tail() 4th preference: first_tail: %s=%s, position=%lu\n", first_tail->tail->simple_tail, first_tail->tail->value_qq, position);
  group->chosen_tail_state[position] = FIRST_TAIL_PLUS_POSITION;
  return(first_tail->tail);
}

/* simple_tail_expr()
 * given a simple_tail of the form "/path" or ""
 * return "path" or "."
 */
char *simple_tail_expr(char *simple_tail) {
  if( *simple_tail == '/' ) {
    /* usual case - .../123/... or /label[123]/... */
    return(simple_tail+1);
  } else if ( *simple_tail == '\0' ) {
    /* path ending in /123 or /label[123] */
    return(".");
  } else {
    /* unreachabe ? */
    return(simple_tail);
  }
}

/* Write out the path-segment, up to and including the [ expr ] (if required) */
void output_segment(struct path_segment *ps_ptr, struct augeas_path_value *path_value_seg) {
  char *last_c;
  struct group *group;
  struct tail *chosen_tail;
  unsigned long position;
  chosen_tail_state_t     chosen_tail_state;
  struct tail_stub *first_tail;

  char *value_qq = path_value_seg->value_qq;

  /* print segment possibly followed by * or seq::* */
  for(char *s=ps_ptr->segment; *s; last_c=s++);
  if(*last_c=='/') {
    /* sequential position .../123 */
    if ( noseq )
      printf("%s*", ps_ptr->segment);
    else
      printf("%sseq::*", ps_ptr->segment);
  } else {
    /* label with a position .../label[123], or no position ... /last */
    printf("%s", ps_ptr->segment);
  }
  group = ps_ptr->group;
  if( group == NULL ) {
    /* last segment .../last_tail No position, nothing else to print */
    return;
  }

  /* apply "chosen_tail" criteria here */
  position = ps_ptr->position;
  chosen_tail = group->chosen_tail[position];
  if( chosen_tail == NULL ) {
    /* This should not happen */
    fprintf(stderr,"chosen_tail==NULL ???\n");
  }

  first_tail = find_first_tail(group->tails_at_position[position]);
  chosen_tail_state = group->chosen_tail_state[position];

  if( debug ) fprintf(stderr,"   output_segment() head=%s, simple_tail=%s chosen_tail=%s chosen_tail_state=%d\n",ps_ptr->head, ps_ptr->simplified_tail, chosen_tail->simple_tail, chosen_tail_state);

  switch( chosen_tail_state ) {
    case CHOSEN_TAIL_START:
      group->chosen_tail_state[position] = CHOSEN_TAIL_WIP;
      /* drop through */
    case FIRST_TAIL:
    case CHOSEN_TAIL_DONE:
    case FIRST_TAIL_PLUS_POSITION:
      if ( chosen_tail->value == NULL ) {
        printf("[%s]", simple_tail_expr(chosen_tail->simple_tail));
      } else if ( use_regexp ) {
        printf("[%s=~regexp(%*s)]",
          simple_tail_expr(chosen_tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          chosen_tail->value_re
          );
      } else {
        printf("[%s=%*s]",
          simple_tail_expr(chosen_tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          chosen_tail->value_qq
          );
      }
      if ( chosen_tail_state == FIRST_TAIL_PLUS_POSITION ) {
        /* no unique tail+value - duplicate or overlapping positions */
        printf("[%lu]", group->subgroup_position[position] );
      }
      break;
    case CHOSEN_TAIL_WIP:
      if ( chosen_tail->value == NULL ) {
        /* theoretically possible - how to test? */
        printf("[%s or count(%s)=0]",
          simple_tail_expr(chosen_tail->simple_tail),
          simple_tail_expr(chosen_tail->simple_tail));
      } else if ( use_regexp ) {
        printf("[%s=~regexp(%*s) or count(%s)=0]",
          simple_tail_expr(chosen_tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          chosen_tail->value_re,
          simple_tail_expr(chosen_tail->simple_tail));
      } else {
        printf("[%s=%*s or count(%s)=0]",
          simple_tail_expr(chosen_tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          chosen_tail->value_qq,
          simple_tail_expr(chosen_tail->simple_tail));
      }
      if ( strcmp(chosen_tail->simple_tail, ps_ptr->simplified_tail) == 0 && strcmp(chosen_tail->value_qq, value_qq) == 0 ) {
        group->chosen_tail_state[position] = CHOSEN_TAIL_DONE;
      }
      break;
    case CHOSEN_TAIL_PLUS_FIRST_TAIL_START:
      if ( first_tail->tail->value == NULL && use_regexp ) {
        /* test with /etc/sudoers */
        printf("[%s and %s=~regexp(%s)]",
          simple_tail_expr(first_tail->tail->simple_tail),
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_re
          );

      } else if ( first_tail->tail->value == NULL && ! use_regexp ) {
        /* test with /etc/sudoers */
        printf("[%s and %s=%s]",
          simple_tail_expr(first_tail->tail->simple_tail),
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_qq
          );
      } else if ( use_regexp ) {
        printf("[%s=~regexp(%*s) and %s=~regexp(%s)]",
          simple_tail_expr(first_tail->tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          first_tail->tail->value_re,
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_re );
      } else {
        printf( "[%s=%*s and %s=%s]",
          simple_tail_expr(first_tail->tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          first_tail->tail->value_qq,
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_qq );
      }
      group->chosen_tail_state[position] = CHOSEN_TAIL_PLUS_FIRST_TAIL_WIP;
      break;
    case CHOSEN_TAIL_PLUS_FIRST_TAIL_WIP:
      if ( first_tail->tail->value == NULL && use_regexp ) {
        printf("[%s and ( %s=~regexp(%s) or count(%s)=0 )]",
          simple_tail_expr(first_tail->tail->simple_tail),
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_re,
          simple_tail_expr(chosen_tail->simple_tail)
          );
      } else if ( first_tail->tail->value == NULL && ! use_regexp ) {
        printf("[%s and ( %s=%s or count(%s)=0 )]",
          simple_tail_expr(first_tail->tail->simple_tail),
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_qq,
          simple_tail_expr(chosen_tail->simple_tail)
          );
      } else if ( use_regexp ) {
        printf("[%s=~regexp(%*s) and ( %s=~regexp(%s) or count(%s)=0 ) ]",
          simple_tail_expr(first_tail->tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          first_tail->tail->value_re,
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_re,
          simple_tail_expr(chosen_tail->simple_tail)
          );
      } else {
        printf("[%s=%*s and ( %s=%s or count(%s)=0 ) ]",
          simple_tail_expr(first_tail->tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          first_tail->tail->value_qq,
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_qq,
          simple_tail_expr(chosen_tail->simple_tail)
          );
      }
      if ( strcmp(chosen_tail->simple_tail, ps_ptr->simplified_tail) == 0 && strcmp(chosen_tail->value_qq, value_qq) == 0 ) {
        group->chosen_tail_state[position] = CHOSEN_TAIL_PLUS_FIRST_TAIL_DONE;
      }
      break;
    case CHOSEN_TAIL_PLUS_FIRST_TAIL_DONE:
      if ( first_tail->tail->value == NULL && use_regexp ) {
        printf("[%s and %s=~regexp(%s)]",
          simple_tail_expr(first_tail->tail->simple_tail),
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_re
          );
      } else if ( first_tail->tail->value == NULL && ! use_regexp ) {
        printf("[%s and %s=%s]",
          simple_tail_expr(first_tail->tail->simple_tail),
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_qq
          );
      } else if ( use_regexp ) {
        printf("[%s=~regexp(%*s) and %s=~regexp(%s)]",
          simple_tail_expr(first_tail->tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          first_tail->tail->value_re,
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_re
          );
      } else {
        printf("[%s=%*s and %s=%s]",
          simple_tail_expr(first_tail->tail->simple_tail),
          -(group->pretty_width_ct[position]),        /* minimum field width */
          first_tail->tail->value_qq,
          simple_tail_expr(chosen_tail->simple_tail),
          chosen_tail->value_qq
          );
      }
      break;
    case NO_CHILD_NODES:
      if(*last_c!='/') {
        printf("[*]"); /* /head/label with no child nodes */
      }
      break;
    default:
      /* unreachable */
      printf("[ %s=%s ]", simple_tail_expr(chosen_tail->simple_tail),chosen_tail->value_qq);
  }
}

void output_path(struct augeas_path_value *path_value_seg) {
  struct path_segment *ps_ptr;
  printf("set ");
  for( ps_ptr=path_value_seg->segments; ps_ptr != NULL; ps_ptr=ps_ptr->next) {
    output_segment(ps_ptr, path_value_seg);
  }
  if( path_value_seg->value_qq != NULL ) {
    printf(" %s\n", path_value_seg->value_qq);
  } else {
    printf("\n");
  }
}

void output(int num_matched, struct augeas_path_value **all_augeas_paths ) {
  int ndx;   /* index to matches() */
  struct augeas_path_value  *path_value_seg;
  char *value;
  for( ndx=0; ndx<num_matched; ndx++) {
    path_value_seg = all_augeas_paths[ndx];
    value = path_value_seg->value;
    if( value != NULL && *value == '\0' )
      value = NULL;
    if(verbose) {
      if ( value == NULL )
        fprintf(stdout,"#   %s\n", path_value_seg->path);
      else
        fprintf(stdout,"#   %s  %s\n", path_value_seg->path, path_value_seg->value_qq);
    }
    if ( debug ) fprintf(stderr, "#%3d %s %s\n",ndx, path_value_seg->path, path_value_seg->value_qq);
    /* weed out null paths here, eg
     *   /head/123 (null)
     *   /head/123/tail (null)
     *   /head/path (null)
     * ie. if value==NULL AND this node has child nodes
     * does not apply if there is no
     *   /head/path/tail
     */
    if ( value == NULL && ndx < num_matched-1 ) {
      if(str_ischild(all_augeas_paths[ndx]->path, all_augeas_paths[ndx+1]->path)) {
        if(debug) fprintf(stderr," # %s (null) (skipped)\n", all_augeas_paths[ndx]->path);
        continue;
      }
    }
    output_path(path_value_seg);
    if( pretty ) {
      if( ndx < num_matched-1 ) {
        /* fixme - do we just need to compare the position? */
        struct group *this_group, *next_group;
        this_group = all_augeas_paths[ndx]->segments->group;
        next_group = all_augeas_paths[ndx+1]->segments->group;
        if ( this_group != next_group
          || ( this_group != NULL && all_augeas_paths[ndx]->segments->position != all_augeas_paths[ndx+1]->segments->position )
          ) {
          /* New group, put in a newline for visual seperation */
          printf("\n");
        }
      }
    }
  }
}

void choose_re_width(struct group *group) {
  unsigned long position;
  /* For each position, compare the value of chosen_tail with 
   * all other matching simple_tails in the group, to find the minimum
   * required length of the RE
   */
  for(position=1; position<=group->max_position; position++) {
    unsigned int max_re_width_ct=0;
    unsigned int max_re_width_ft=0;
    unsigned int re_width;
    struct tail *chosen_tail = group->chosen_tail[position];
    struct tail *first_tail  = group->first_tail[position];
    struct tail *tail_ptr;
    for(tail_ptr = group->all_tails; tail_ptr != NULL; tail_ptr = tail_ptr->next) {
      if ( tail_ptr != chosen_tail ) {
        if( strcmp(tail_ptr->simple_tail, chosen_tail->simple_tail) == 0 ) {
          value_cmp(tail_ptr->value, chosen_tail->value, &re_width);
          if( re_width > max_re_width_ct ) {
            max_re_width_ct = re_width;
          }
        }
      }
      if (debug) fprintf(stderr, "chosen_tail_state = %d\n", group->chosen_tail_state[position] );
      if( group->chosen_tail_state[position] == CHOSEN_TAIL_PLUS_FIRST_TAIL_START && chosen_tail != first_tail ) {
        /* 3rd preference, we need an re_width for both the chosen_tail and the first_tail */
        /* In theory, the first_tail of this position may be present in other positions, but may not be first */
        if ( tail_ptr != first_tail ) {
          if( strcmp(tail_ptr->simple_tail, first_tail->simple_tail) == 0 ) {
            value_cmp(tail_ptr->value, first_tail->value, &re_width);
            if( re_width > max_re_width_ft ) {
              max_re_width_ft = re_width;
            }
          }
        }
      } /* If 3rd preference */
    } /* for each tail in group->all_tails */
    if(debug) fprintf(stderr,"\n");
    max_re_width_ct = MAX(max_re_width_ct,use_regexp);
    max_re_width_ft = MAX(max_re_width_ft,use_regexp);
    group->re_width_ct[position] = max_re_width_ct;
    group->re_width_ft[position] = max_re_width_ft;
    chosen_tail->value_re = regexp_value( chosen_tail->value, max_re_width_ct );
    if ( group->chosen_tail_state[position] == CHOSEN_TAIL_PLUS_FIRST_TAIL_START ) {
      /* otherwise, max_re_width_ft=0, and we don't need first_tail->value_re at all */
      if ( chosen_tail == first_tail ) {
        /* if chosen_tail == first_tail, we would overwrite chosen_tail->value_re */
        first_tail->value_re = chosen_tail->value_re;
      } else {
        first_tail->value_re  = regexp_value( first_tail->value,  max_re_width_ft );
      }
    }
    if(debug) fprintf(stderr,"# %s[%lu] chosen_tail=%-20s %u %s\n", group->head, position, chosen_tail->simple_tail, max_re_width_ct, chosen_tail->value_re);
    if(debug) fprintf(stderr,"# %s[%lu]  first_tail=%-20s %u %s\n", group->head, position,  first_tail->simple_tail, max_re_width_ft,  first_tail->value_re);
  } /* for position 1..max_position */
}

void choose_pretty_width(struct group *group) {
  unsigned long position;
  int value_len;
  for(position=1; position<=group->max_position; position++) {
    struct tail *pretty_tail;
    if( group->chosen_tail_state[position] == CHOSEN_TAIL_PLUS_FIRST_TAIL_START ) {
      pretty_tail = group->first_tail[position];
    } else {
      pretty_tail = group->chosen_tail[position];
    }
    if( use_regexp ) {
      value_len = pretty_tail->value_re == NULL ? 0 : strlen(pretty_tail->value_re);
    } else {
      value_len = pretty_tail->value_qq == NULL ? 0 : strlen(pretty_tail->value_qq);
    }
    group->pretty_width_ct[position] = value_len;
  }
  /* find the highest pretty_width_ct for each unique chosen_tail->simple_tail in the group */
  for(position=1; position<=group->max_position; position++) {
    unsigned int max_width=0;
    unsigned long pos_search;
    char *chosen_simple_tail = group->chosen_tail[position]->simple_tail;
    for(pos_search=position; pos_search <= group->max_position; pos_search++) {
      if(strcmp( group->chosen_tail[pos_search]->simple_tail, chosen_simple_tail) == 0 ) {
        value_len = group->pretty_width_ct[pos_search];
        if( value_len <= MAX_PRETTY_WIDTH ) {
          /* If we're already over the limit, do not pad everything else out too */
          max_width = MAX(max_width, value_len);
        }
        group->pretty_width_ct[pos_search] = max_width; /* so we can start at position+1 */
      }
    }
    max_width = MIN(max_width,MAX_PRETTY_WIDTH);
    group->pretty_width_ct[position] = max_width;
  } /* for position 1..max_position */
}

/* populate group->chosen_tail[] and group->first_tail[] arrays */
/* Also call choose_re_width() and choose_pretty_width() to populate group->re_width_ct[] ..->re_width_ft[] and ..->pretty_width_ft[] */
void choose_all_tails() {
  int ndx;   /* index to all_groups() */
  unsigned long position;
  struct group *group;
  for(ndx=0; ndx<num_groups; ndx++) {
    group=all_groups[ndx];
    for(position=1; position<=group->max_position; position++) {
      group->chosen_tail[position] = choose_tail(group, position);
    }
    if( use_regexp ) {
      choose_re_width(group);
    }
    if( pretty ) {
      choose_pretty_width(group);
    }
  }
}

/* Create a quoted value from the value, using single quotes if possible
 * Quotes are not strictly require for the value, but they _are_ required
 * for values within the path-expressions
 */
char *quote_value(char *value) {
  char *s, *t, *value_qq, quote;
  int len=0;
  int has_q=0;
  int has_qq=0;
  int has_special=0;
  int has_nl=0;
  int new_len;
  if(value==NULL)
    return(NULL);
  for(s = value, len=0; *s; s++, len++) {
    switch(*s) {
      case '"': has_qq++; break;
      case '\'': has_q++; break;
      case ' ':
      case '/':
      case '*':
      case '.':
      case ':':
        has_special++; break;
      case '\n':
      case '\t':
      case '\\':
        has_nl++; break;
      default:
    }
  }
  if( has_q == 0 ) {
    /* Normal case, no single-quotes within the value */
    new_len = len+2+has_nl;
    quote='\'';
  } else if ( has_qq == 0 ) {
    new_len = len+2+has_nl;
    quote='"';
  } else {
    /* This needs a bugfix in augeas */
    new_len = len+2+has_q+has_nl;
    quote='\'';
  }
  value_qq = malloc( sizeof(char) * ++new_len); /* don't forget the \0 */
  CHECK_OOM( ! value_qq, exit_oom, "in quote_value()");

  t=value_qq;
  *t++ = quote;
  for(s = value; *s; s++, t++) {
    if ( *s == quote ) {
      *t++ = '\\';
      *t =quote;
      continue;
    }  else if ( *s == '\n' ) {
      *t++ = '\\';
      *t = 'n';
      continue;
    }  else if ( *s == '\t' ) {
      *t++ = '\\';
      *t = 't';
      continue;
    }  else if ( *s == '\\' ) {
      *t++ = '\\';
      *t = '\\';
      continue;
    }
    *t = *s;
  }
  *t++ = quote;
  *t++ = '\0';
  return(value_qq);
}

/* Create a quoted regular expression from the value, using single quotes if possible
 */
char *regexp_value(char *value, int max_len) {
  char *s, *t, *value_re, quote;
  int len=0;
  int has_q=0;
  int has_qq=0;
  int has_special=0;
  int has_nl=0;
  int new_len;
  if(value==NULL)
    return(NULL);
  for(s = value, len=0; *s; s++, len++) {
    switch(*s) {
      case '"':  has_qq++; break;
      case '\'': has_q++; break;
      case '*':
      case '?':
      case '.':
      case '[':
      case ']':
      case '(':
      case ')':
      case '^':
      case '$':
      case '|':
        has_special++; break;
      case '\n':
      case '\t':
        has_nl++; break;
      case '\\':
        has_special+=2; break;
      default:
    }
  }
  len++;  /* don't forget the \0 */
  if( has_q == 0 ) {
    /* Normal case, no single-quotes within the value */
    new_len = len+2+has_nl+has_special*2;
    quote='\'';
  } else if ( has_qq == 0 ) {
    new_len = len+2+has_nl+has_special*2;
    quote='"';
  } else {
    /* This needs a bugfix in augeas */
    new_len = len+2+has_q+has_nl+has_special*2;
    quote='\'';
  }
  value_re = malloc( sizeof(char) * new_len);
  CHECK_OOM( ! value_re, exit_oom, "in regexp_value()");

  t=value_re;
  *t++ = quote;
  for(s = value; *s; s++, t++) {
    if ( *s == quote ) {
      *t++ = '\\';
      *t =quote;
      continue;
    }  else if ( *s == '\n' ) {
      *t++ = '\\';
      *t = 'n';
      continue;
    }  else if ( *s == '\t' ) {
      *t++ = '\\';
      *t = 't';
      continue;
    }  else if ( *s == '\\' || *s == ']' ) {
      *t = '.';
      continue;
    }
    switch(*s) {
      /* Special handling for ] */
      case ']':
        *t = '.'; continue;
      case '[':
        *t++ = '\\';
        break;
      case '*':
      case '?':
      case '.':
      case '(':
      case ')':
      case '^':
      case '$':
      case '|':
        *t++ = '\\';
        *t++ = '\\';
        break;
      case '\\':
      case '\n':
      case '\t':
        break;  /* already dealt with above */
      default:
    }
    *t = *s;
    if( ( s - value ) >= max_len  && *(s+1)!='\0' && *(s+2)!='\0' && *(s+3)!='\0' ) {
      /* don't append .* if there are only one or two chars left in the string */
      t++;
      *t++='.';
      *t++='*';
      break;
    }
  }
  *t++ = quote;
  *t++ = '\0';
  return(value_re);
}

void usage(char *progname) {
  if(progname == NULL)
    progname = "augsuggest";
  fprintf(stdout, "Usage:\n\t%s [--target=realname] [--lens=Lensname] [--pretty] [--regexp[=n]] [--noseq] /path/filename\n\n",progname);
  fprintf(stdout, "\t    --target ... use this as the filename in the output set-commands\n");
  fprintf(stdout, "\t                 this filename also implies the default lens to use\n");
  fprintf(stdout, "\t    --lens   ... override the default lens and target and use this one\n");
  fprintf(stdout, "\t    --pretty ... make the output more readable\n");
  fprintf(stdout, "\t    --regexp ... use regexp() in path-expressions instead of absolute values\n");
  fprintf(stdout, "\t                 if followed by number, this is the minimum length of the regexp to use\n");
  fprintf(stdout, "\t    --noseq  ... use * instead of seq::* (useful for compatability with augeas < 1.13.0)\n");
  fprintf(stdout, "\t    --help   ... this message\n");
  fprintf(stdout, "\t    /path/filename   ... full pathname to the file being analysed (required)\n\n");
  fprintf(stdout, "%s will generate a script of augtool set-commands suitable for rebuilding the file specified\n", progname);
  fprintf(stdout, "If --target is specified, then the lens associated with the target will be use to parse the file\n");
  fprintf(stdout, "If --lens is specified, then the given lens will be used, overriding the default, and --target\n\n");
  fprintf(stdout, "Examples:\n");
  fprintf(stdout, "\t%s --target=/etc/squid/squid.conf /etc/squid/squid.conf.new\n", progname);
  fprintf(stdout, "\t\tOutput an augtool script for re-creating /etc/squid/squid.conf.new at /etc/squid/squid.conf\n\n");
  fprintf(stdout, "\t%s --lens=simplelines /etc/hosts\n", progname);
  fprintf(stdout, "\t\tOutput an augtool script for /etc/hosts using the lens simplelines instead of the default for /etc/hosts\n\n");
  fprintf(stdout, "\t%s --regexp=12 /etc/hosts\n", progname);
  fprintf(stdout, "\t\tUse regular expressions in the resulting augtool script, each being at least 12 chars long\n");
  fprintf(stdout, "\t\tIf the value is less than 12 chars, use the whole value in the expression\n");
  fprintf(stdout, "\t\tLonger regexp values may be output, if the resulting regexp would be ambiguous\n");
}

int main(int argc, char **argv) {
  int opt;
  char *inputfile = NULL;
  char *target_file = NULL;

  while (1) {
    int option_index = 0;
    static struct option long_options[] = {
        {"help",    no_argument,       &help,       1 },
        {"verbose", no_argument,       &verbose,    1 },
        {"debug",   no_argument,       &debug,      1 },
        {"lens",    required_argument, 0,           0 },
        {"noseq",   no_argument,       &noseq,      1 },
        {"seq",     no_argument,       &noseq,      0 },
        {"target",  required_argument, 0,           0 },
        {"pretty",  no_argument,       &pretty,     1 },
        {"regexp",  optional_argument, &use_regexp, 1 },
        {0,         0,                 0,           0 } /* marker for end of data */
      };

    opt = getopt_long(argc, argv, "vdhl:sSrpt:", long_options, &option_index);
    if (opt == -1)
       break;

    switch (opt) {
      case 0:
        if(debug) {
          fprintf(stderr,"option %d %s", option_index, long_options[option_index].name);
          if(optarg) fprintf(stderr," with arg %s", optarg);
          fprintf(stderr,"\n");
        }
        if (strcmp(long_options[option_index].name, "lens") == 0 ) {
          lens = optarg;
          flags |= AUG_NO_MODL_AUTOLOAD;
          if(debug) fprintf(stderr,"Lens=%s\n",lens);
        } else if (strcmp(long_options[option_index].name, "target") == 0) {
          target_file = optarg;
          if( *target_file != '/' ) {
            fprintf(stderr,"Error: target \"%s\" must be an absolute path\neg.\n\t--target=/etc/%s\n", target_file, target_file);
            exit(1);
          }
          if(debug) fprintf(stderr,"target_file=%s\n",target_file);
        } else if (strcmp(long_options[option_index].name, "regexp") == 0) {
          if(optarg) {
            int optarg_int = strtol(optarg, NULL, 0);
            if(optarg_int > 0)
              use_regexp = optarg_int;
          } else {
            use_regexp = 8;
          }
          if(debug) fprintf(stderr,"regexp=%d\n",use_regexp);
        }
        break;

      case 'h':
        help=1;
        break;
      case 'v':
        verbose=1;
        break;
      case 'd':
        debug=1;
        fprintf(stderr,"option d with value '%s'\n", optarg);
        break;
      case 'S':
        noseq=0;
        if(debug) fprintf(stderr,"noseq=%d\n",noseq);
        break;
      case 's':
        noseq=1;
        if(debug) fprintf(stderr,"noseq=%d\n",noseq);
        break;
      case 'l':
        lens = optarg;
        if(debug) fprintf(stderr,"Lens=%s\n", optarg);
        break;
      case 'r':
        use_regexp = use_regexp ? use_regexp : 8;
        break;

      case '?':    /* unknown option */
        break;

      default:
        fprintf(stderr,"?? getopt returned character code 0x%x ??\n", opt);
    }
  }

  if( help ) {
    usage(argv[0]);
    exit(0);
  }
  if (optind == argc-1) {
    /* We need exactly one non-option argument - the input filename */
    if( *argv[optind] == '/' ) {
      /* filename is an absolute path - use it verbatim */
      inputfile = argv[optind];
    } else {
      /* filename is a relative path - prepend the current PWD */
      int result = asprintf(&inputfile, "%s/%s", getenv("PWD"), argv[optind] );
      CHECK_OOM( result < 0, exit_oom, NULL);
    }
    if(debug) {
      fprintf(stderr,"non-option ARGV-elements: ");
      while (optind < argc)
        fprintf(stderr,"%s ", argv[optind++]);
      fprintf(stderr,"\n");
    }
  } else if( optind == argc ) {
    /* No non-option args given (missing inputfile) */
    fprintf(stderr,"Missing command-line argument\nPlease specify a filename to read eg.\n\t%s %s\n", argv[0], "/etc/hosts");
    fprintf(stderr, "\nTry '%s --help' for more information.\n", argv[0]);
    exit(1);
  } else {
    /* Too many args - we only want one */
    fprintf(stderr,"Too many command-line arguments\nPlease specify only one filename to read eg.\n\t%s %s\n", argv[0], "/etc/hosts");
    fprintf(stderr, "\nTry '%s --help' for more information.\n", argv[0]);
    exit(1);
  }

  if(debug) fprintf(stderr,"%s: AUGEAS_ROOT=%s, Inputfile: %s\n", argv[0], getenv("AUGEAS_ROOT"), inputfile);

  aug = aug_init(NULL, loadpath, flags|AUG_NO_ERR_CLOSE|AUG_NO_LOAD);

  if ( target_file != NULL && lens == NULL ) {
    /* Infer the lens which applies to the --target_file option */
    char *aug_load_path = NULL;
    char **matching_lenses;
    int  num_lenses, result, ndx;
    char *target_file_tail;
    target_file_tail = target_file;
    for (char *s1 = target_file; *s1; s1++ ) {
       if ( *s1 == '/' )
         target_file_tail = s1+1;
    }
    result = asprintf(&aug_load_path, "/augeas/load/*['%s' =~ glob(incl)]['%s' !~ glob(excl)]['%s' !~ glob(excl)]", target_file, target_file, target_file_tail);
    CHECK_OOM( result < 0, exit_oom, NULL);

    if(debug) {
      fprintf(stderr,"path expr: %s\n",aug_load_path);
      aug_print(aug, stderr, aug_load_path);
    }
    num_lenses = aug_match( aug, aug_load_path, &matching_lenses);
    if ( num_lenses == 0 ) {
      fprintf(stderr, "Aborting - no lens applies for target: %s\n", target_file);
      exit(1);
    }
    lens = matching_lenses[0] + 13; /* skip over /augeas/load */

    if ( num_lenses > 1 ) {
      /* Should never happen */
      for( ndx=0; ndx<num_lenses;ndx++) {
        fprintf(stderr,"Found lens: %s\n", matching_lenses[ndx]);
      }
      fprintf(stderr, "Warning: multiple lenses apply to target %s - using %s\n", target_file, lens);
    }
  }

  if ( lens != NULL ) {
    /* Explict lens given, or inferred from --target */
    if(debug) fprintf(stderr,"Adding transform lens: %s   file: %s\n", lens, inputfile);
    if ( target_file ) {
      /* There's no need to output a transform, because we just searched for it anyway */
      if (verbose) printf("transform %s incl %s\n", lens, target_file);
    } else {
      printf("transform %s incl %s\n", lens, inputfile);
    }

    if ( aug_transform(aug, lens, inputfile, 0) != 0 ) {
      fprintf(stderr, "%s\n", aug_error_details(aug));
      exit(1);
    }
  }

  if ( aug_load_file(aug, inputfile) != 0 ) {
    const char *msg;
    fprintf(stderr, "Failed to load file %s\n", inputfile);
    msg = aug_error_details(aug);
    if(msg) {
      fprintf(stderr,"%s\n",msg);
    } else {
      msg = aug_error_message(aug);
      if(msg)
        fprintf(stderr,"%s\n",msg);
      msg = aug_error_minor_message(aug);
      if(msg)
        fprintf(stderr,"%s\n",msg);
    }
    exit(1);
  }

  if ( target_file ) {
    /* Rename the tree from inputfile to target_file, if specified */
    char *files_inputfile;
    char *files_targetfile;
    int  result;
    result = asprintf(&files_inputfile, "/files%s", inputfile );
    CHECK_OOM( result < 0, exit_oom, NULL);

    result = asprintf(&files_targetfile, "/files%s", target_file );
    CHECK_OOM( result < 0, exit_oom, NULL);

    aug_mv(aug, files_inputfile, files_targetfile);
    if( debug ) aug_print(aug, stderr, "/files");
  }

  int num_matched;
  char *value;
  /* There is a subtle difference between "/files//(star)" and "/files/descendant::(star)" in the order that matches appear */
  /* descendant::* is better suited, as it allows us to prune out intermediate nodes with null values (directory-like nodes) */
  /* These would be created implicity by "set" */
  num_matched = aug_match(aug, "/files/descendant::*", &all_matches);
  all_augeas_paths = (struct augeas_path_value **) malloc( sizeof(struct augeas_path_value *) * num_matched);
  CHECK_OOM( all_augeas_paths == NULL, exit_oom, NULL);

  for (int ndx=0; ndx < num_matched; ndx++) {
    all_augeas_paths[ndx] = (struct augeas_path_value *) malloc( sizeof(struct augeas_path_value));
    CHECK_OOM( all_augeas_paths[ndx] == NULL, exit_oom, NULL);
    all_augeas_paths[ndx]->path = all_matches[ndx];
    aug_get(aug, all_matches[ndx], (const char **) &value );
    if (debug) fprintf(stderr,"%s %s\n", all_augeas_paths[ndx]->path, value);
    all_augeas_paths[ndx]->value    = value;
    all_augeas_paths[ndx]->value_qq = quote_value(value);
    all_augeas_paths[ndx]->segments = split_path(all_augeas_paths[ndx]);
  }
  choose_all_tails(num_matched, all_augeas_paths);
  output(num_matched, all_augeas_paths);

  exit(0);
}
