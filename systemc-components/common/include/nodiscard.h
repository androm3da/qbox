#pragma once

#if __cplusplus >= 201703L
#define QBOX_NODISCARD [[nodiscard]]
#else
#define QBOX_NODISCARD __attribute__((warn_unused_result))
#endif
