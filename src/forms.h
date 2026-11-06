/* Nordstjernen — HTML form validation, serialization, and submission helpers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_FORMS_H
#define NS_FORMS_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

gboolean ns_form_is_submit_trigger(const ns_node *n);
gboolean ns_form_is_reset_trigger(const ns_node *n);
const ns_node *ns_form_default_button(const ns_node *form, const ns_node *doc);

void ns_form_collect_inputs(const ns_node *form, const ns_node *n,
                            const ns_node *doc, GString *query,
                            gboolean *first, const ns_node *submitter);

const ns_node *ns_form_first_invalid(const ns_node *form, const ns_node *n,
                                     const ns_node *doc);

G_END_DECLS

#endif
