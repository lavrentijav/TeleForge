// TeleForge plugin API bridge.
//
// Exposes the embedded native module `_teleforge` to plugins. The friendly,
// documented Python wrapper lives in the shipped `teleforge` package
// (modules/teleforge), which imports this native module.
#pragma once

#ifdef TELEFORGE_WITH_PYTHON

namespace TeleForge::Plugins {

// Must be called BEFORE Py_Initialize() so the interpreter can import the
// built-in `_teleforge` module.
void RegisterApiModule();

} // namespace TeleForge::Plugins

#endif // TELEFORGE_WITH_PYTHON
