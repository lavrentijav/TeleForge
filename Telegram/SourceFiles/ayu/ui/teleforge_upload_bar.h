// Copyright @Radolyn, 2026
#pragma once

namespace TeleForge::UI {

// Global upload progress bar pinned to the bottom of the active main window, so
// the "export local DB" progress stays visible from anywhere while the upload
// runs in the background. `total <= 0` means "finished / nothing to do" (100%).
void ShowUploadProgress(int done, int total);
void HideUploadProgress();

} // namespace TeleForge::UI
