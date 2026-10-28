#pragma once
// Owner of an htslib hFILE until something else takes it (hts_hopen, through SAMReader). Kept apart from
// SAMReader.hpp so an opener can return one without pulling in the reader.

#include <htslib-1.22.1/htslib/hfile.h>
#include <memory>

namespace miint {

struct HFileDeleter {
	void operator()(hFILE *hf) const {
		if (hf) {
			(void)hclose(hf);
		}
	}
};

using HFilePtr = std::unique_ptr<hFILE, HFileDeleter>;

} // namespace miint
