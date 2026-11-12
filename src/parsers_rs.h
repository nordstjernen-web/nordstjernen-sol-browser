/* Nordstjernen — C ABI of the in-tree Rust URL and HTML5 parsers (src/parsers_rs).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_PARSERS_RS_H
#define NS_PARSERS_RS_H

#include <glib.h>

#include "dom.h"
#include "net.h"

G_BEGIN_DECLS

char *ns_rs_url_resolve(const char *base, size_t base_len,
                        const char *href, size_t href_len);
char *ns_rs_url_set_component(const char *href, size_t href_len,
                              const char *component, size_t component_len,
                              const char *value, size_t value_len);
char *ns_rs_url_origin(const char *url, size_t len, int default_port);
char *ns_rs_url_host(const char *url, size_t len);
int   ns_rs_url_parts(const char *url, size_t len, ns_url_parts *out);

typedef struct ns_h5_sink ns_h5_sink;

int   ns_rs_html_parse_document(const char *data, size_t len, int scripting,
                                ns_node *document, ns_h5_sink *sink);
void  ns_rs_html_parse_fragment(const char *context, size_t context_len,
                                int namespace_id, const char *data, size_t len,
                                int scripting, ns_node *document,
                                ns_h5_sink *sink);
char *ns_rs_html_named_char_ref(const char *name, size_t len);

ns_node *ns_h5_element(ns_h5_sink *sink, const char *name, size_t len,
                       int borrowed, int line, int column);
void     ns_h5_attribute(ns_h5_sink *sink, ns_node *element,
                         const char *name, size_t name_len,
                         const char *value, size_t value_len, int namespace_id,
                         size_t prefix_len, int only_if_missing);
void     ns_h5_set_namespace(ns_node *element, int namespace_id);
ns_node *ns_h5_comment(ns_h5_sink *sink, const char *text, size_t len);
ns_node *ns_h5_processing_instruction(ns_h5_sink *sink,
                                      const char *target, size_t target_len,
                                      const char *data, size_t data_len);
void     ns_h5_doctype(ns_h5_sink *sink, ns_node *document,
                       const char *name, size_t name_len,
                       const char *public_id, size_t public_len,
                       const char *system_id, size_t system_len);
void     ns_h5_append(ns_node *parent, ns_node *child);
void     ns_h5_insert_before(ns_node *sibling, ns_node *child);
void     ns_h5_append_text(ns_h5_sink *sink, ns_node *parent,
                           const char *text, size_t len);
void     ns_h5_insert_text_before(ns_h5_sink *sink, ns_node *sibling,
                                  const char *text, size_t len);
ns_node *ns_h5_parent(ns_node *node);
void     ns_h5_remove(ns_node *node);
void     ns_h5_reparent_children(ns_node *from, ns_node *to);
ns_node *ns_h5_template_contents(ns_node *template_element);
void     ns_h5_discard(ns_node *node);

G_END_DECLS

#endif
