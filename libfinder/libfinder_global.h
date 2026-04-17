#ifndef LIBFINDER_GLOBAL_H
#define LIBFINDER_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(LIBFINDER_LIBRARY)
#  define LIBFINDER_EXPORT Q_DECL_EXPORT
#else
#  define LIBFINDER_EXPORT Q_DECL_IMPORT
#endif

#endif // LIBFINDER_GLOBAL_H
