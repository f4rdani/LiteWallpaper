#pragma once
#include <string>

namespace miniwp {
namespace ui {

void OpenOptimizeModal(
    const std::string& path,
    const std::string& action,
    int srcW,
    int srcH,
    int targetW,
    int targetH
);
void RenderOptimizeModal();
void RenderOptimizationProgress();

void OpenCaptureModal(const std::string& video_path, int targetFilter = 0);
void RenderCaptureFrameModal();
void ShutdownCaptureModalWorker();

bool IsAnyModalActive() noexcept;

} // namespace ui
} // namespace miniwp
