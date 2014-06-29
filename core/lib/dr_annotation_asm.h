/* ******************************************************
 * Copyright (c) 2014 Google, Inc.  All rights reserved.
 * ******************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef _DYNAMORIO_ANNOTATION_ASM_H_
#define _DYNAMORIO_ANNOTATION_ASM_H_ 1

#include <stddef.h>

#ifndef DYNAMORIO_ANNOTATIONS_X64
# ifdef _MSC_VER
#  ifdef _WIN64
#   define DYNAMORIO_ANNOTATIONS_X64 1
#  endif
# else
#  ifdef __LP64__
#   define DYNAMORIO_ANNOTATIONS_X64 1
#  endif
# endif
#endif

#ifdef _MSC_VER /* Microsoft Visual Studio */
# define PASTE1(x, y) x##y
# define PASTE(x, y) PASTE1(x, y)

# ifdef DYNAMORIO_ANNOTATIONS_X64
/* The value of this intrinsic is assumed to be non-zero. In the rare case of
 * annotations occurring within managed code (i.e., C# or VB), it may not properly be
 * the address of the return address on the stack, but it still should never be zero. */
#  pragma intrinsic(_AddressOfReturnAddress, __debugbreak, __int2c, _m_prefetchw)
#  define GET_RETURN_PTR _AddressOfReturnAddress
#  define DR_DEFINE_ANNOTATION_LABELS(annotation) \
    const char *annotation##_expression_label = "dynamorio-annotation:expression:"#annotation; \
    const char *annotation##_statement_label = "dynamorio-annotation:statement:"#annotation;
#  define DR_ANNOTATION_OR_NATIVE(annotation, native_version, ...) \
do { \
    if ((unsigned __int64) GET_RETURN_PTR() > (0xfffffffffffffff1 - (2 * __LINE__))) { \
        extern const char *annotation##_statement_label; \
        __int2c(); \
        _m_prefetchw(annotation##_statement_label); \
        __debugbreak(); \
        annotation(__VA_ARGS__); \
    } else { \
        native_version; \
    } \
} while ((unsigned __int64) GET_RETURN_PTR() > (0xfffffffffffffff0 - (2 * __LINE__)))
#  define DR_ANNOTATION_FUNCTION(annotation, body) \
    if (GET_RETURN_PTR() == (void *) 0) { \
        extern const char *annotation##_expression_label; \
        __int2c(); \
        _m_prefetchw(annotation##_expression_label); \
        __debugbreak(); \
    } else { \
        body; \
    }
# else
#  define DR_DEFINE_ANNOTATION_LABELS(annotation) \
    const char *annotation##_label = "dynamorio-annotation:"#annotation;
#  define DR_ANNOTATION_OR_NATIVE_INSTANCE(unique_id, annotation, native_version, ...) \
    { \
        extern const char *annotation##_label; \
        __asm { \
            __asm _emit 0xeb \
            __asm _emit 0x06 \
            __asm mov eax, annotation##_label \
            __asm pop eax \
            __asm jmp PASTE(native_run, unique_id) \
        } \
        annotation(__VA_ARGS__); \
        goto PASTE(native_end_marker, unique_id); \
        PASTE(native_run, unique_id): native_version; \
        PASTE(native_end_marker, unique_id): ; \
    }
#  define DR_ANNOTATION_OR_NATIVE(annotation, native_version, ...) \
    DR_ANNOTATION_OR_NATIVE_INSTANCE(__COUNTER__, annotation, native_version, __VA_ARGS__)
#  define DR_ANNOTATION_FUNCTION_INSTANCE(unique_id, annotation, body) \
    __asm { \
        __asm _emit 0xeb \
        __asm _emit 0x06 \
        __asm mov eax, annotation##_label \
        __asm nop \
        __asm jmp PASTE(native_run, unique_id) \
    } \
    goto PASTE(native_end_marker, unique_id); \
    PASTE(native_run, unique_id): body; \
    PASTE(native_end_marker, unique_id): ;
#  define DR_ANNOTATION_FUNCTION(annotation, body) \
    DR_ANNOTATION_FUNCTION_INSTANCE(__COUNTER__, annotation, body)
# endif
# define DR_DECLARE_ANNOTATION(return_type, annotation, parameters) \
    return_type __fastcall annotation parameters
# define DR_DEFINE_ANNOTATION(return_type, annotation, parameters, body) \
    DR_DEFINE_ANNOTATION_LABELS(annotation) \
    return_type __fastcall annotation parameters \
    { \
        DR_ANNOTATION_FUNCTION(annotation, body) \
    }
#else /* GCC or Intel (Unix or Windows) */
/* Each reference to _GLOBAL_OFFSET_TABLE_ is adjusted by the linker to be
 * XIP-relative, and no relocations are generated for the operand. */
# ifdef DYNAMORIO_ANNOTATIONS_X64
#  define LABEL_REFERENCE_LENGTH "0x11"
#  define LABEL_REFERENCE_REGISTER "%rax"
#  define ANNOTATION_FUNCTION_CLOBBER_LIST "%rax","%rcx","%rdx","%rsi","%rdi","%r8","%r9"
#  define _CALL_TYPE
# else
#  define LABEL_REFERENCE_LENGTH "0xc"
#  define LABEL_REFERENCE_REGISTER "%eax"
#  define ANNOTATION_FUNCTION_CLOBBER_LIST "%rax","%rcx","%rdx"
#  define _CALL_TYPE , fastcall
# endif
# define DR_ANNOTATION_ATTRIBUTES \
    __attribute__((noinline, visibility("hidden") _CALL_TYPE))
# define DR_WEAK_DECLARATION __attribute__ ((weak))
# define DR_ANNOTATION_OR_NATIVE(annotation, native_version, ...) \
({ \
    __label__ native_run, native_end_marker; \
    extern const char *annotation##_label; \
    __asm__ volatile goto (".byte 0xeb; .byte "LABEL_REFERENCE_LENGTH"; \
                            mov _GLOBAL_OFFSET_TABLE_,%"LABEL_REFERENCE_REGISTER"; \
                            bsf "#annotation"_label@GOT,%"LABEL_REFERENCE_REGISTER"; \
                            jmp %l0;" \
                            ::: LABEL_REFERENCE_REGISTER \
                            : native_run); \
    annotation(__VA_ARGS__); \
    goto native_end_marker; \
    native_run: native_version; \
    native_end_marker: ; \
})

# define DR_DECLARE_ANNOTATION(return_type, annotation, parameters) \
     DR_ANNOTATION_ATTRIBUTES return_type annotation parameters DR_WEAK_DECLARATION
# define DR_DEFINE_ANNOTATION(return_type, annotation, parameters, body) \
    const char *annotation##_label = "dynamorio-annotation:"#annotation; \
    DR_ANNOTATION_ATTRIBUTES return_type annotation parameters \
    { \
        __label__ native_run, native_end_marker; \
        extern const char *annotation##_label; \
        __asm__ volatile goto (".byte 0xeb; .byte "LABEL_REFERENCE_LENGTH"; \
                               mov _GLOBAL_OFFSET_TABLE_,%"LABEL_REFERENCE_REGISTER"; \
                               bsr "#annotation"_label@GOT,%"LABEL_REFERENCE_REGISTER"; \
                               mov %0, %"LABEL_REFERENCE_REGISTER"; \
                               jmp %l1;" \
                               :: "g"(__builtin_frame_address(0)) \
                               : ANNOTATION_FUNCTION_CLOBBER_LIST \
                               : native_run); \
        goto native_end_marker; \
        native_run: body; \
        native_end_marker: ; \
    }
#endif

#define DR_ANNOTATION(annotation, ...) \
    DR_ANNOTATION_OR_NATIVE(annotation, , __VA_ARGS__)

#endif

