//
// Created by root on 11/29/25.
//

#ifndef SYSTEMS_H
#define SYSTEMS_H

#if defined(_MSC_VER)
#define URPC_ALWAYS_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define URPC_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define URPC_ALWAYS_INLINE inline
#endif

#endif //SYSTEMS_H
