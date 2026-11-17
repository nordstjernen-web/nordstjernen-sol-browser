/* Nordstjernen — HTML parser API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_HTML_H
#define NS_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

ns_node *ns_html_parse(const char *input, gssize len);
ns_node *ns_html_parse_with_scripting(const char *input, gssize len,
                                      gboolean scripting);
ns_node *ns_parse_document_for_type(const char *input, gssize len,
                                    const char *content_type,
                                    gboolean scripting);

typedef struct ns_html_stream ns_html_stream;

ns_html_stream *ns_html_stream_new(gboolean scripting);
void     ns_html_stream_feed(ns_html_stream *stream, const char *data,
                             gsize len);
ns_node *ns_html_stream_document(const ns_html_stream *stream);
ns_node *ns_html_stream_finish(ns_html_stream *stream);
void     ns_html_stream_cancel(ns_html_stream *stream);

ns_node *ns_xml_parse(const char *input, gssize len);
ns_node *ns_xml_parse_html_entities(const char *input, gssize len);
char *ns_html_named_char_ref(const char *name, gsize len);
ns_node *ns_xml_parse_reporting(const char *input, gssize len,
                                int *line, int *column);

gboolean ns_xml_well_formed(const char *input, gssize len, char **out_root_ns);

ns_node *ns_html_parse_fragment_with_scripting(const char *context_tag,
                                               const char *input, gssize len,
                                               gboolean scripting);
ns_node *ns_html_parse_fragment_in_context(const ns_node *context,
                                           const char *input, gssize len,
                                           gboolean scripting);

void ns_html_convert_declarative_shadow(ns_node *root);

gboolean ns_html_is_void(const char *tag);

gboolean ns_html_parse_float(const char *s, double *out);

gboolean ns_html_is_raw_text(const char *tag);

void ns_html_escape_append(GString *out, const char *s, gboolean escape_quotes);

char *ns_html_escape_text(const char *s);

char *ns_html_declared_charset(const char *body, gsize len,
                               const char *content_type);

char *ns_html_decode_body_full(const char *body, gsize len,
                               const char *content_type, char **charset_out);
char *ns_html_decode_body_len(const char *body, gsize len,
                              const char *content_type, char **charset_out,
                              gsize *len_out);

char *ns_html_image_document(const char *url);

char *ns_html_json_document(const char *url, const char *json, gsize len);
char *ns_html_xml_document(const char *url, const char *xml, gsize len);

struct ns_response;
void  ns_html_prepare_document_response(struct ns_response *resp);

typedef struct ns_html_load ns_html_load;

ns_html_load *ns_html_load_new(gboolean scripting);
void     ns_html_load_chunk(ns_html_load *load, const char *url,
                            const char *raw_headers, const guint8 *data,
                            gsize len);
void     ns_html_load_finish(ns_html_load *load, struct ns_response *resp);
ns_node *ns_html_take_loaded_document(struct ns_response *resp,
                                      gboolean scripting);

G_END_DECLS

#endif
