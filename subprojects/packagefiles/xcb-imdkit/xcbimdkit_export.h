/*
 * Minimal replacement for the xcbimdkit_export.h that upstream generates with
 * CMake's generate_export_header().  We build one shared library and do not
 * hide symbols, so the export macros are empty or plain visibility defaults.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef XCBIMDKIT_EXPORT_H
#define XCBIMDKIT_EXPORT_H

#ifdef __cplusplus
#define XCBIMDKIT_EXPORT extern "C"
#else
#define XCBIMDKIT_EXPORT
#endif

#define XCBIMDKIT_NO_EXPORT
#define XCBIMDKIT_DEPRECATED
#define XCBIMDKIT_DEPRECATED_EXPORT XCBIMDKIT_EXPORT
#define XCBIMDKIT_DEPRECATED_NO_EXPORT XCBIMDKIT_NO_EXPORT

#endif /* XCBIMDKIT_EXPORT_H */
