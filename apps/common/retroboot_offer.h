//
// The installer's RetroBoot question, apart from the window so the tests can run it: is there a RetroBoot tree on
// the stick to offer to remove, how big it is, and the words of the question. Nothing here deletes anything - the
// removal itself is InstallOptions::removeRetroBoot in the installer job.
//
#pragma once

#include <cstdint>
#include <string>

namespace retrobootoffer {

// true only when RetroArch/bin/retroboot is a folder on the stick, or - for a stick with the old layout
// (legacyLayout, not migrated yet) - retroarch/retroboot at its root. A link in its place is not offered (the job
// keeps a linked tree), RetroArch/bin/apps alone is not a reason to ask. Names are matched without regard to case.
bool offerRemoveRetroBoot(const std::string &root, bool legacyLayout);

// the bytes in the files of the offered retroboot tree (apps/ is removed only where empty, so it adds nothing);
// links are counted as nothing and never followed. 0 when nothing is offered.
uint64_t retroBootBytes(const std::string &root, bool legacyLayout);

// "about 612 MB" (rounded to the nearest MB, at least 1)
std::string sizeText(uint64_t bytes);

// the checkbox label and the helper line under it
std::string checkboxLabel(uint64_t bytes);
std::string helperLine();

// the confirmation after Install; `drive` is "F:" (a backslash or a slash after it is dropped)
std::string confirmationText(const std::string &drive, uint64_t bytes);

} // namespace retrobootoffer
