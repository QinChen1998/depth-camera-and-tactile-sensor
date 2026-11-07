#pragma once
namespace LiteSrBoardStatus {
// Preserve validity and exact raw-status/warning agreement. No bit masking.
inline bool accept(unsigned raw,unsigned warnings) {
 return (raw==1u&&warnings==0u)||(raw==9u&&warnings==8u)||(raw==17u&&warnings==16u);
}
}
