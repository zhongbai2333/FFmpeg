/* Minimal stdlib.h stub for Windows ARM64 JNI cross compile.
 * Only declarations used by eac3_jni.c are provided.
 */

#ifndef JNI_STUBS_STDLIB_H
#define JNI_STUBS_STDLIB_H

#ifndef _STDLIB_H_
#define _STDLIB_H_
#endif
#ifndef _STDLIB_H
#define _STDLIB_H
#endif
#ifndef _INC_STDLIB
#define _INC_STDLIB
#endif

#ifndef JNI_STUBS_SIZE_T_DEFINED
#define JNI_STUBS_SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *calloc(size_t count, size_t size);
void free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* JNI_STUBS_STDLIB_H */