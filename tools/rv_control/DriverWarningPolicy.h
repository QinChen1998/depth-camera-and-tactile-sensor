#ifndef TMF8829_WARNING_POLICY_H
#define TMF8829_WARNING_POLICY_H
#include <stdlib.h>
#include <string.h>
// Retain the current exact info-0x10 acceptance, plus the existing explicitly
// selected observe-0x08 policy. Combined or unknown warning bits remain fatal.
static inline int tmf8829_observe08(void)
{
    const char *value=getenv("CANONICAL_WARNING_POLICY");
    return value && strcmp(value,"observe-0x08")==0;
}
static inline int tmf8829_warning_fatal(unsigned warnings)
{
    return warnings!=0u && warnings!=16u && !(warnings==8u && tmf8829_observe08());
}
#endif
