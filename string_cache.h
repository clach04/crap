#ifndef STRING_CACHE_H
#define STRING_CACHE_H

#include <stddef.h>

/** Cache unique copy of a string.  */
const char * cache_string (const char * str);

/** Cache unique copy of a string.  */
const char * cache_string_n (const char * str, size_t len); 

/** Hash function.  */
unsigned long string_hash_func (const char * str, size_t len);

/** Look-up hash of cached string.  */
unsigned long string_hash_get (const char * str);



/** Support for hashes indexed by a cached string.  */
typedef struct string_hash_head {
    const char * string;                /* Must be cached.  */
    struct string_hash_head * next;
} string_hash_head_t;


typedef struct string_hash {
    size_t num_entries;
    size_t num_buckets;                 /* Always a power of two.  */
    string_hash_head_t ** buckets;
} string_hash_t;


void string_hash_init (string_hash_t * hash);
void string_hash_destroy (string_hash_t * hash);

/* Returns existing bucket if found, otherwise inserts new bucket and returns
 * pointer to it.  The pointers should be to objects starting with the
 * struct string_hash_head.  */
void * string_hash_insert (string_hash_t * hash,
                           const void * entry, size_t entry_size);
/* String need not be cached.  */
void * string_hash_find (const string_hash_t * hash, const char * string);

void * string_hash_begin (const string_hash_t * hash);
void * string_hash_next (const string_hash_t * hash, void * i);


#endif
