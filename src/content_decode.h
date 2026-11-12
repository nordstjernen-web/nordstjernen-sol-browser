/* Nordstjernen — HTTP Content-Encoding decoder that passes unknown codings through.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_CONTENT_DECODE_H
#define NS_CONTENT_DECODE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_content_decoder ns_content_decoder;

const char *ns_content_decode_accept_encoding(void);
ns_content_decoder *ns_content_decoder_new(const char *content_encoding);
ns_content_decoder *ns_content_decoder_new_with_dictionary(
    const char *content_encoding, const guint8 *dictionary,
    gsize dictionary_len, const guint8 *dictionary_sha256);
const char *ns_content_decode_dictionary_codings(void);
gboolean ns_content_decoder_feed(ns_content_decoder *dec,
                                 const guint8 *data, gsize len,
                                 gsize limit, GByteArray *out);
gboolean ns_content_decoder_over_limit(const ns_content_decoder *dec);
void ns_content_decoder_free(ns_content_decoder *dec);

G_END_DECLS

#endif
