#pragma once

#include <expected>

// Either a value or a POSIX errno code such as ENOENT
template <typename T>
using FSResult = std::expected<T, int>;
