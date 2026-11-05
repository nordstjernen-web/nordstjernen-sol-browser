/* Nordstjernen — microphone capture (SDL) for getUserMedia audio + Web Audio. */

#ifndef NS_MIC_H
#define NS_MIC_H

#include <glib.h>

G_BEGIN_DECLS

void     ns_mic_preload(void);
gboolean ns_mic_acquire(void);
void     ns_mic_release(void);

void ns_mic_fill_time_domain(guint8 *out, int n);
void ns_mic_fill_frequency(guint8 *out, int n);

G_END_DECLS

#endif
