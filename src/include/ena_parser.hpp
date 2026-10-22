#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace miint {

enum class ENAAccessionType { STUDY, SAMPLE, RUN, EXPERIMENT, UNKNOWN };

struct SampleAttribute {
	std::string sample_accession;
	std::string tag;
	std::string value;
};

struct ENATSVResult {
	std::vector<std::string> column_names;
	std::vector<std::vector<std::string>> rows;
};

// Pure parsing and URL construction utilities for ENA APIs.
// No network dependencies — safe for unit tests. The search URLs start with
// ResolvePortalBaseFromEnv(), so they depend on MIINT_ENA_PORTAL_URL_BASE.
class ENAParser {
public:
	static ENAAccessionType DetectAccessionType(const std::string &accession);
	static std::string QueryParamForAccessionType(ENAAccessionType type);

	// Validate that accession contains only safe characters (alphanumeric, underscore, hyphen, dot)
	static void ValidateAccession(const std::string &accession);
	// Validate that fields contain only safe characters (alphanumeric, underscore, comma)
	static void ValidateFields(const std::string &fields);

	// URL construction
	static std::string BuildSearchURL(const std::string &accession, const std::string &result_type,
	                                  const std::string &fields);
	// Compound-query URL: query=<col>%20IN%20(%22acc1%22,%22acc2%22,...). All accessions must share the
	// same type (e.g., all RUN). Throws on empty input or UNKNOWN type.
	static std::string BuildSearchURLBatch(const std::vector<std::string> &accessions, ENAAccessionType accession_type,
	                                       const std::string &result_type, const std::string &fields);
	static std::string BuildXMLURL(const std::vector<std::string> &accessions);

	// Parsing
	static ENATSVResult ParseTSV(const std::string &tsv);
	static std::vector<SampleAttribute> ParseSampleAttributesXML(const std::string &xml);

	// Filter submitted_* fields by format (e.g., "SFF").
	// Returns download URLs, raw aspera entries, and total bytes for matching entries.
	struct SubmittedFilterResult {
		std::vector<std::string> urls;       // download URLs (via FTPtoHTTPS)
		std::vector<std::string> aspera_raw; // Raw aspera field entries (unparsed)
		uint64_t total_bytes = 0;
	};
	static SubmittedFilterResult FilterSubmittedByFormat(const std::string &submitted_ftp,
	                                                     const std::string &submitted_aspera,
	                                                     const std::string &submitted_format,
	                                                     const std::string &submitted_bytes,
	                                                     const std::string &target_format);

	// Helpers
	// The download URLs in a ';'-separated *_ftp field: an HTTPS URL for each of
	// ENA's entries, which carry no scheme or start with ftp://, and an entry
	// that names http:// or https:// (a mock Portal's) as given.
	static std::vector<std::string> FTPtoHTTPS(const std::string &ftp_field);
	static std::string DefaultFields(const std::string &result_type);

	// Base URL of the Browser API. The Portal API's is ResolvePortalBaseFromEnv().
	static constexpr const char *BROWSER_BASE = "https://www.ebi.ac.uk/ena/browser/api";
};

// Default portal API base. May be overridden via the MIINT_ENA_PORTAL_URL_BASE
// env var (test fixture only — production users should not need to set it).
constexpr const char *DEFAULT_PORTAL_BASE = "https://www.ebi.ac.uk/ena/portal/api";

// The Portal API base for every Portal request miint makes: env-var override,
// falling back to DEFAULT_PORTAL_BASE, without a trailing slash.
std::string ResolvePortalBaseFromEnv();

// Strip a single trailing slash from a base URL so callers passing
// "https://.../api" and "https://.../api/" produce identical URLs.
std::string TrimTrailingSlash(std::string s);

// Percent-encode a value spliced into an ENA URL, as a query parameter value
// or a path segment. Encodes every byte outside the RFC 3986 unreserved set
// (alnum + `-._~`).
std::string PercentEncodeQueryValue(const std::string &s);

} // namespace miint
