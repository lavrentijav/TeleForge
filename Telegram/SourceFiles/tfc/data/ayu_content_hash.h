#pragma once

#include "tfc/data/entities.h"

namespace AyuContentHash {

[[nodiscard]] std::string ComputeEdited(const EditedMessage &message);
[[nodiscard]] std::string ComputeDeleted(const DeletedMessage &message);

} // namespace AyuContentHash
