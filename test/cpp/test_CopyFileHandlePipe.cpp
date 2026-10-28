// Tests for the local gzip writer's open mode (src/include/bgzf_write_mode.hpp). The mode keeps O_EXCL,
// which refuses an existing file, only for regular-file targets: stdout, /dev/stdout, /dev/null and
// pre-created pipes always exist, so with O_EXCL a gzip COPY to them would fail with EEXIST, as in
//   duckdb -c "COPY (...) TO '/dev/stdout' (FORMAT FASTQ, COMPRESSION gzip)" | downstream_tool
// These tests check the mode decision and that gzip data written with the chosen mode arrives through
// a real FIFO.
//
// POSIX-only (mkfifo / O_NONBLOCK): not built on Windows or WASM.

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include <catch2/catch_test_macros.hpp>

#include <bgzf_write_mode.hpp>
#include <htslib-1.22.1/htslib/bgzf.h>
#include <zlib.h>

#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Inflate the first gzip member (the lone bgzf data block for a small payload) back to plaintext.
std::string GunzipFirstMember(const std::string &in) {
	z_stream zs;
	std::memset(&zs, 0, sizeof(zs));
	REQUIRE(inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK);
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
	zs.avail_in = static_cast<uInt>(in.size());
	std::string out;
	char buf[8192];
	int ret;
	do {
		zs.next_out = reinterpret_cast<Bytef *>(buf);
		zs.avail_out = sizeof(buf);
		ret = inflate(&zs, Z_NO_FLUSH);
		REQUIRE((ret == Z_OK || ret == Z_STREAM_END));
		out.append(buf, sizeof(buf) - zs.avail_out);
	} while (ret != Z_STREAM_END);
	inflateEnd(&zs);
	return out;
}

} // namespace

TEST_CASE("BgzfWriteMode keeps O_EXCL only for regular files", "[copy][bgzf][pipe]") {
	// stdout / stderr / fd aliases stream -> no O_EXCL.
	REQUIRE(miint::BgzfWriteMode("-", 6) == "w6");
	REQUIRE(miint::BgzfWriteMode("/dev/stdout", 6) == "w6");
	REQUIRE(miint::BgzfWriteMode("/dev/stderr", 6) == "w6");
	REQUIRE(miint::BgzfWriteMode("/dev/fd/1", 6) == "w6");
	// /dev/null is an existing non-regular (char) device -> no O_EXCL.
	REQUIRE(miint::BgzfWriteMode("/dev/null", 6) == "w6");
	// A not-yet-existing regular path keeps the atomic no-clobber guard.
	REQUIRE(miint::BgzfWriteMode("/tmp/miint_nonexistent_xyz.fq.gz", 6) == "wx6");
}

TEST_CASE("BgzfWriteMode carries COMPRESSION_LEVEL into the mode, with or without O_EXCL", "[copy][bgzf]") {
	// htslib reads the level from the mode's digit; a level that never reaches it is silently 6.
	REQUIRE(miint::BgzfWriteMode("/dev/null", 1) == "w1");
	REQUIRE(miint::BgzfWriteMode("-", 0) == "w0");
	REQUIRE(miint::BgzfWriteMode("/tmp/miint_nonexistent_xyz.fq.gz", 9) == "wx9");
	REQUIRE(miint::BgzfWriteMode("/tmp/miint_nonexistent_xyz.fq.gz", 1) == "wx1");
}

// A target naming an existing FIFO gets no O_EXCL, and the gzip data written through it arrives intact.
TEST_CASE("gzip COPY streams to a named pipe without O_EXCL EEXIST", "[copy][bgzf][pipe]") {
	char dir_tmpl[] = "/tmp/miint_pipe_XXXXXX";
	const char *dir = mkdtemp(dir_tmpl);
	REQUIRE(dir != nullptr);
	const std::string fifo = std::string(dir) + "/out.fq.gz";
	REQUIRE(mkfifo(fifo.c_str(), 0600) == 0);
	const std::string payload = "@r1\nACGTACGTACGTACGT\n+\nIIIIIIIIIIIIIIII\n";

	REQUIRE(miint::BgzfWriteMode(fifo, 6) == "w6");

	// The read end is opened non-blocking first, so the writer's blocking O_WRONLY open succeeds at
	// once and a failing open fails the test instead of hanging. The payload is far below the pipe
	// buffer, so the writer never blocks on a full pipe.
	const int rfd = open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
	REQUIRE(rfd >= 0);

	BGZF *bg = bgzf_open(fifo.c_str(), miint::BgzfWriteMode(fifo, 6).c_str());
	REQUIRE(bg != nullptr);
	REQUIRE(bgzf_write(bg, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
	REQUIRE(bgzf_close(bg) == 0);

	std::string captured;
	char buf[8192];
	for (;;) {
		ssize_t n = read(rfd, buf, sizeof(buf));
		if (n > 0) {
			captured.append(buf, static_cast<size_t>(n));
			continue;
		}
		break; // EOF (writer closed), or nothing left to read
	}
	close(rfd);

	REQUIRE(!captured.empty());
	REQUIRE(GunzipFirstMember(captured) == payload);

	unlink(fifo.c_str());
	rmdir(dir);
}

#endif // !_WIN32 && !__EMSCRIPTEN__
