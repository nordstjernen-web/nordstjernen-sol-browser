/* Nordstjernen — decoding of JSON string literals, shared by the engine, the shell and the IPC framing. */

#ifndef NS_JSON_STRING_H
#define NS_JSON_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

long ns_json_decode_string(const char *s, const char *end, char *out,
                           const char **stop, int strict);

#ifdef __cplusplus
}
#endif

#endif
