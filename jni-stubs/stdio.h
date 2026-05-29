/* JNI 交叉编译桩 —— 为 jni.h 提供最小 stdio.h
 *
 * 交叉编译到 aarch64-w64-mingw32 时，目标平台缺少 C 标准头文件。
 * 此桩提供 jni.h 所需的全部符号（FILE, NULL），不引入任何 I/O 函数。
 *
 * 使用: clang -Ijni-stubs ...
 */

#ifndef JNI_STUBS_STDIO_H
#define JNI_STUBS_STDIO_H

/* 阻止真正的 stdio.h 被二次包含 */
#ifndef _STDIO_H_
#define _STDIO_H_
#endif
#ifndef _STDIO_H
#define _STDIO_H
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* FILE —— jni.h 需要此类型，但我们不调用任何 FILE* 相关的 JNI 函数 */
typedef struct _iobuf FILE;

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* JNI_STUBS_STDIO_H */
