#pragma once

#include "tfc/data/entities.h"

namespace TfContentHash {

[[nodiscard]] std::string ComputeEdited(const EditedMessage &message);
[[nodiscard]] std::string ComputeDeleted(const DeletedMessage &message);

} // namespace TfContentHash
