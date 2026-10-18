#pragma once

// The application defines this literal in one of its precompiled headers. The
// standalone test force-includes this file first instead of dragging the whole
// prelude in.
#include <QtCore/QString>

#include <cstddef>

inline QString operator""_q(const char16_t *text, std::size_t size) {
	return QString::fromUtf16(reinterpret_cast<const char16_t*>(text), int(size));
}
