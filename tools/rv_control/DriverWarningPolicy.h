#ifndef TMF8829_WARNING_POLICY_H
#define TMF8829_WARNING_POLICY_H
#include <stdlib.h>
#include <string.h>
// The product's observe-0x08 policy permits both information bits together.
// Any bit outside the selected policy remains fatal.
static inline int tmf8829_observe08(void)
{
    const char *value=getenv("CANONICAL_WARNING_POLICY");
    return value && strcmp(value,"observe-0x08")==0;
}
static inline int tmf8829_warning_fatal(unsigned warnings)
{
    const unsigned allowed = 0x10u | (tmf8829_observe08() ? 0x08u : 0u);
    return (warnings & ~allowed) != 0u;
}
#endif
