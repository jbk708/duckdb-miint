#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <vector>
#include <cstdint>
#include <QualScore.hpp>

TEST_CASE("QualScore vector constructor", "[qualscore]") {
	SECTION("accepts max Phred score 93 with default offset 33") {
		// Phred 93 + offset 33 = ASCII 126 ('~'), the max printable ASCII character
		std::vector<uint8_t> scores = {0, 40, 93};
		miint::QualScore qs(scores);
		auto roundtrip = qs.as_vec(33);
		REQUIRE(roundtrip.size() == 3);
		CHECK(roundtrip[0] == 0);
		CHECK(roundtrip[1] == 40);
		CHECK(roundtrip[2] == 93);
	}

	SECTION("accepts quality scores in range 60-93 that were previously rejected") {
		// These would fail with the old offset_q > 93 check
		std::vector<uint8_t> scores = {61, 70, 80, 93};
		miint::QualScore qs(scores);
		auto roundtrip = qs.as_vec(33);
		REQUIRE(roundtrip.size() == 4);
		CHECK(roundtrip[0] == 61);
		CHECK(roundtrip[1] == 70);
		CHECK(roundtrip[2] == 80);
		CHECK(roundtrip[3] == 93);
	}

	SECTION("rejects score that exceeds max ASCII 126") {
		// Phred 94 + offset 33 = 127, which exceeds printable ASCII range
		std::vector<uint8_t> scores = {94};
		CHECK_THROWS_AS(miint::QualScore(scores), std::invalid_argument);
	}

	SECTION("basic roundtrip with typical quality scores") {
		std::vector<uint8_t> scores = {0, 10, 20, 30, 40};
		miint::QualScore qs(scores);
		auto roundtrip = qs.as_vec(33);
		REQUIRE(roundtrip.size() == 5);
		for (size_t i = 0; i < scores.size(); i++) {
			CHECK(roundtrip[i] == scores[i]);
		}
	}
}

TEST_CASE("QualScore write_decoded", "[qualscore]") {
	// write_decoded is how read_fastx turns every quality line into scores. A byte outside
	// [offset, offset + 93] is not a score in that encoding -- typically Phred+33 data read
	// with qual_offset=64 -- and decoding it anyway would wrap to a plausible-looking number,
	// so the read has to stop, naming the byte so the user can tell which encoding it is.
	uint8_t out[8] = {};

	SECTION("decodes the full valid range exactly") {
		REQUIRE(miint::QualScore(std::string("!I~")).write_decoded(out, 33) == 3);
		CHECK(out[0] == 0);
		CHECK(out[1] == 40);
		CHECK(out[2] == 93);
		REQUIRE(miint::QualScore(std::string("@h")).write_decoded(out, 64) == 2);
		CHECK(out[0] == 0);
		CHECK(out[1] == 40);
	}

	SECTION("rejects a byte below the offset") {
		CHECK_THROWS_WITH(miint::QualScore(std::string("hh#h")).write_decoded(out, 64),
		                  "Stored quality character out of range: #");
	}

	SECTION("rejects the byte just below the offset") {
		CHECK_THROWS_WITH(miint::QualScore(std::string("hh?h")).write_decoded(out, 64),
		                  "Stored quality character out of range: ?");
	}

	SECTION("rejects a byte more than 93 above the offset") {
		CHECK_THROWS_WITH(miint::QualScore(std::string("II\x7fI")).write_decoded(out, 33),
		                  "Stored quality character out of range: \x7f");
	}

	SECTION("rejects a byte with the high bit set") {
		CHECK_THROWS_WITH(miint::QualScore(std::string("I\xc8")).write_decoded(out, 33),
		                  "Stored quality character out of range: \xc8");
	}

	SECTION("names the first bad byte when there are several") {
		CHECK_THROWS_WITH(miint::QualScore(std::string("I\x7f\xc8I")).write_decoded(out, 33),
		                  "Stored quality character out of range: \x7f");
	}
}

TEST_CASE("miint::find_low_quality_window", "[filter]") {
	SECTION("no low quality window returns none") {
		// 'M' => ASCII 77 → 77-33 = 44
		std::vector<uint8_t> qs(10, 44);
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/20,
		                                          /*window_length=*/2);
		CHECK((obs == qs.size() + 1));
	}

	SECTION("bases exactly at min_quality are not low") {
		// all 44 ('M')
		std::vector<uint8_t> qs(10, 44);
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/44,
		                                          /*window_length=*/2);
		CHECK((obs == qs.size() + 1));
	}

	SECTION("simple window of length 2 in MMM++MM is at index 3") {
		// 'M'→44, '+'→43-33=10
		std::vector<uint8_t> qs = {
		    44, 44, 44, // M, M, M
		    10, 10,     // +, +
		    44, 44      // M, M
		};
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/15,
		                                          /*window_length=*/2);
		CHECK((obs == 3));
	}

	SECTION("window of length 3 in M++MM+++MM is at index 5") {
		// M, +, +, M, M, +, +, +, M, M
		// 44,10,10,44,44,10,10,10,44,44
		std::vector<uint8_t> qs = {44, 10, 10, 44, 44, 10, 10, 10, 44, 44};
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/15,
		                                          /*window_length=*/3);
		CHECK((obs == 5));
	}

	SECTION("first two +'s in ++MMMM gives window at 0") {
		// +, +, M, M, M, M → 10,10,44,44,44,44
		std::vector<uint8_t> qs = {10, 10, 44, 44, 44, 44};
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/11,
		                                          /*window_length=*/2);
		CHECK((obs == 0));
	}

	SECTION("first length-3 window in M++MMMM+++ is at index 7") {
		// M,+,+,M,M,M,M,+,+,+ → 44,10,10,44,44,44,44,10,10,10
		std::vector<uint8_t> qs = {44, 10, 10, 44, 44, 44, 44, 10, 10, 10};
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/11,
		                                          /*window_length=*/3);
		CHECK((obs == 7));
	}

	SECTION("longer run still flags at its start in MMMMMMM+++") {
		// M×7 then +×3: 44×7,10×3
		std::vector<uint8_t> qs = {44, 44, 44, 44, 44, 44, 44, 10, 10, 10};
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/11,
		                                          /*window_length=*/2);
		CHECK((obs == 7));
	}

	SECTION("when multiple windows exist, the first is returned") {
		// M,L,+,+,M,M,M,+,+,+
		// 'L'→76-33=43
		std::vector<uint8_t> qs1 = {44, 43, 10, 10, 44, 44, 44, 10, 10, 10};
		auto o1 = miint::find_low_quality_window(qs1,
		                                         /*min_quality=*/20,
		                                         /*window_length=*/2);
		CHECK((o1 == 2));

		// +,+,M,L,+,+,+,M,+,+,+,M,M,+,+
		// 10,10,44,43,10,10,10,44,10,10,10,44,44,10,10
		std::vector<uint8_t> qs2 = {10, 10, 44, 43, 10, 10, 10, 44, 10, 10, 10, 44, 44, 10, 10};
		auto o2 = miint::find_low_quality_window(qs2,
		                                         /*min_quality=*/20,
		                                         /*window_length=*/3);
		CHECK((o2 == 4));
	}

	SECTION("when all runs are too short, returns none") {
		// same as qs2 above but window_length=4
		std::vector<uint8_t> qs = {10, 10, 44, 43, 10, 10, 10, 44, 10, 10, 10, 44, 44, 10, 10};
		auto obs = miint::find_low_quality_window(qs,
		                                          /*min_quality=*/20,
		                                          /*window_length=*/4);
		CHECK((obs == qs.size() + 1));
	}
}
