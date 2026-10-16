#include "install_skb_arrow.hpp"

#include "install_report.hpp"
#include "skb_arrow/install.hpp"

#include <mutex>
#include <string>

namespace duckdb {

namespace {

namespace sk = ::duckdb::miint::skb_arrow;

void InstallSkbArrowExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	ClientContext &context = state.GetContext();
	sk::InstallOptions options;
	options.existing = sk::FindSkbArrow();
	options.uv = sk::FindUv();
	options.cache_dir = sk::MiintCacheDir();
	options.force = InstallForceArg(args);
	const sk::InstallReport report = sk::Install(options, [&context] { context.BlockingInterruptCheck(); });
	SetInstallReport(result, report.installed, report.path, report.version, report.message);
}

void SkbArrowAvailableExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	// Cached once true, never while false: false is what a user sees before
	// running install_skb_arrow(), and it must not outlive the install.
	static std::mutex mutex;
	static bool available = false;
	bool answer;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!available) {
			const std::string binary = sk::FindSkbArrow();
			ClientContext &context = state.GetContext();
			try {
				if (!binary.empty()) {
					sk::Probe(binary, [&context] { context.BlockingInterruptCheck(); });
					available = true;
				}
			} catch (const sk::Error &) {
				// Missing, broken, or incompatible: not available.
			}
		}
		answer = available;
	}
	result.Reference(Value::BOOLEAN(answer), count_t(args.size()));
}

} // namespace

void RegisterSkbArrowInstallFunctions(ExtensionLoader &loader) {
	ScalarFunctionSet install("install_skb_arrow");
	for (const auto &arguments : {vector<LogicalType> {}, vector<LogicalType> {LogicalType::BOOLEAN}}) {
		ScalarFunction fn(arguments, InstallReportType(), InstallSkbArrowExecute);
		fn.SetVolatile();
		install.AddFunction(fn);
	}
	loader.RegisterFunction(install);
	loader.RegisterFunction(ScalarFunction("skb_arrow_available", {}, LogicalType::BOOLEAN, SkbArrowAvailableExecute));
}

} // namespace duckdb
