/* Minimal errno.h stub for Windows ARM64 JNI cross compile.
 * FFmpeg's libavutil/error.h needs POSIX-like errno values for AVERROR().
 */

#ifndef JNI_STUBS_ERRNO_H
#define JNI_STUBS_ERRNO_H

#ifndef _ERRNO_H_
#define _ERRNO_H_
#endif
#ifndef _ERRNO_H
#define _ERRNO_H
#endif
#ifndef _INC_ERRNO
#define _INC_ERRNO
#endif

#ifndef EPERM
#define EPERM 1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif

#endif /* JNI_STUBS_ERRNO_H */