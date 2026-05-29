/* Minimal string.h stub for Windows ARM64 JNI cross compile.
 * Only declarations needed by this JNI wrapper / headers are provided.
 */

#ifndef JNI_STUBS_STRING_H
#define JNI_STUBS_STRING_H

#ifndef _STRING_H_
#define _STRING_H_
#endif
#ifndef _STRING_H
#define _STRING_H
#endif
#ifndef _INC_STRING
#define _INC_STRING
#endif

#ifndef JNI_STUBS_SIZE_T_DEFINED
#define JNI_STUBS_SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
size_t strlen(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* JNI_STUBS_STRING_H */