/* Nordstjernen — compression dictionary transport (RFC 9842): the store of dictionaries a response offered and their matching. */

#ifndef NS_COMPRESSION_DICTIONARY_H
#define NS_COMPRESSION_DICTIONARY_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_DICTIONARY_HASH_LEN 32

typedef struct ns_dictionary ns_dictionary;

void           ns_dictionary_store(const char *url, const char *use_as_dictionary,
                                   const char *cache_control,
                                   const guint8 *body, gsize len);
ns_dictionary *ns_dictionary_match(const char *url, const char *destination);
void           ns_dictionary_unref(ns_dictionary *dictionary);
const char    *ns_dictionary_available_value(const ns_dictionary *dictionary);
const char    *ns_dictionary_id(const ns_dictionary *dictionary);
const guint8  *ns_dictionary_hash(const ns_dictionary *dictionary);
const guint8  *ns_dictionary_data(const ns_dictionary *dictionary, gsize *len);
void           ns_dictionary_clear_all(void);

G_END_DECLS

#endif
