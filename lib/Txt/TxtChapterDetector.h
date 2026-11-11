#pragma once

#include <string>
#include <string_view>

namespace txtchapter {

// Chinese / plain-text chapter heading detector (e.g. "第一章 ...", "第12节 ...", "序章", "楔子").
bool isChapterHeadingLine(std::string_view utf8Line, std::string* cleanTitleOut = nullptr);

}  // namespace txtchapter
