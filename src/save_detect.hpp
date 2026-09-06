#pragma once
#include <string>
#include <vector>
#include "backup.hpp"
#include "types.hpp"

struct SaveDetectResult {
    BackupDevice::Type type = BackupDevice::Type::None;
    std::string matchedTag;
};

SaveDetectResult detectSaveType(const std::vector<u8>& romData);
