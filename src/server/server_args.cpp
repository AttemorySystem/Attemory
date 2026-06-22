#include "server/server_args.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "attemory/version.h"
#include "nlohmann/json.hpp"

namespace attemory::server_args {
namespace {

using atmcore::BackendKind;
using atmcore::KvType;
using atmcore::ModelTierSpec;
using json = nlohmann::ordered_json;

struct ModelTierEntry {
    std::string tier;
    std::string hf_repo;
    std::string file;
};

std::string env_value(const char * key) {
    const char * value = std::getenv(key);
    return value == nullptr ? std::string() : std::string(value);
}

std::filesystem::path home_dir() {
#ifdef _WIN32
    const std::string user_profile = env_value("USERPROFILE");
    if (!user_profile.empty()) {
        return user_profile;
    }
    const std::string home_drive = env_value("HOMEDRIVE");
    const std::string home_path = env_value("HOMEPATH");
    if (!home_drive.empty() || !home_path.empty()) {
        return home_drive + home_path;
    }
#else
    const std::string home = env_value("HOME");
    if (!home.empty()) {
        return home;
    }
#endif
    return ".";
}

std::string expand_user_path(const std::string & path) {
    if (path == "~") {
        return home_dir().string();
    }
    if (path.size() >= 2 && path[0] == '~' && (path[1] == '/' || path[1] == '\\')) {
        return (home_dir() / path.substr(2)).string();
    }
    return path;
}

std::filesystem::path default_cache_home() {
#ifdef _WIN32
    const std::string local_app_data = env_value("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        return local_app_data;
    }
    return home_dir() / "AppData" / "Local";
#else
    const std::string xdg_cache_home = env_value("XDG_CACHE_HOME");
    if (!xdg_cache_home.empty()) {
        return xdg_cache_home;
    }
    return home_dir() / ".cache";
#endif
}

std::filesystem::path default_data_home() {
#ifdef _WIN32
    const std::string local_app_data = env_value("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        return local_app_data;
    }
    return home_dir() / "AppData" / "Local";
#else
    const std::string xdg_data_home = env_value("XDG_DATA_HOME");
    if (!xdg_data_home.empty()) {
        return xdg_data_home;
    }
    return home_dir() / ".local" / "share";
#endif
}

void apply_platform_defaults(Options & opts) {
    const std::string data_override = env_value("ATTEMORY_DATA_DIR");
    const std::string cache_override = env_value("ATTEMORY_CACHE_DIR");

    if (!data_override.empty()) {
        opts.data_dir = expand_user_path(data_override);
    } else {
#ifdef _WIN32
        opts.data_dir = (default_data_home() / "attemory" / "data" / "sessions").string();
#else
        opts.data_dir = (default_data_home() / "attemory" / "sessions").string();
#endif
    }

    if (!cache_override.empty()) {
        opts.cache_dir = expand_user_path(cache_override);
    } else {
#ifdef _WIN32
        opts.cache_dir = (default_cache_home() / "attemory" / "cache").string();
#else
        opts.cache_dir = (default_cache_home() / "attemory").string();
#endif
    }
}

bool parse_port(const char * text, int & value) {
    char * end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == nullptr || *end != '\0' || parsed <= 0 || parsed > 65535) {
        return false;
    }
    value = (int) parsed;
    return true;
}

bool parse_non_negative_int(const char * text, int & value) {
    char * end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    value = (int) parsed;
    return true;
}

bool parse_positive_int(const char * text, int & value) {
    if (!parse_non_negative_int(text, value)) {
        return false;
    }

    return value > 0;
}

bool parse_non_negative_i32(const char * text, int32_t & value) {
    int parsed = 0;
    if (!parse_non_negative_int(text, parsed) || parsed > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    value = (int32_t) parsed;
    return true;
}

bool parse_positive_i32(const char * text, int32_t & value) {
    if (!parse_non_negative_i32(text, value)) {
        return false;
    }
    return value > 0;
}

bool parse_size_bytes(const char * text, uint64_t & value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }

    char * end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text) {
        return false;
    }

    std::string suffix(end);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char ch) {
        return (char) std::tolower(ch);
    });

    uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "b") {
        multiplier = 1;
    } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
        multiplier = 1024ULL;
    } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (suffix == "t" || suffix == "tb" || suffix == "tib") {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    } else {
        return false;
    }

    if (parsed > std::numeric_limits<uint64_t>::max() / multiplier) {
        return false;
    }
    value = (uint64_t) parsed * multiplier;
    return true;
}

bool parse_v(const char * text, std::vector<int32_t> & v) {
    v.clear();
    if (text == nullptr || *text == '\0') {
        return false;
    }

    const char * cursor = text;
    while (*cursor != '\0') {
        char * end = nullptr;
        const long parsed = std::strtol(cursor, &end, 10);
        if (end == cursor || parsed < 0 || parsed > std::numeric_limits<int32_t>::max()) {
            return false;
        }

        const int32_t id = (int32_t) parsed;
        if (std::find(v.begin(), v.end(), id) == v.end()) {
            v.push_back(id);
        }

        if (*end == '\0') {
            break;
        }
        if (*end != ',') {
            return false;
        }

        cursor = end + 1;
        if (*cursor == '\0') {
            return false;
        }
    }

    return !v.empty();
}

void apply_legacy_data_dir(const std::string & root, Options & opts, bool update_cache_dir) {
    const std::filesystem::path base =
        root.empty() ? std::filesystem::path(".") : std::filesystem::path(expand_user_path(root));
    opts.data_dir = (base / "sessions").string();
    if (update_cache_dir) {
        opts.cache_dir = (base / "cache").string();
    }
}

bool parse_backend_kind_arg(const std::string & value, BackendKind & backend) {
#ifdef __APPLE__
    if (value == "metal") {
        backend = BackendKind::METAL;
        return true;
    }
#else
    if (value == "gpu") {
        backend = BackendKind::GPU;
        return true;
    }
#endif
    if (value == "cpu") {
        backend = BackendKind::CPU;
        return true;
    }

    return false;
}

bool parse_kv_type_arg(const std::string & value, KvType & type) {
    if (value == "f16" || value == "fp16") {
        type = KvType::F16;
        return true;
    }
    if (value == "f32" || value == "fp32") {
        type = KvType::F32;
        return true;
    }
    if (value == "bf16") {
        type = KvType::BF16;
        return true;
    }
    if (value == "q8_0") {
        type = KvType::Q8_0;
        return true;
    }
    if (value == "q4_0") {
        type = KvType::Q4_0;
        return true;
    }
    if (value == "q4_1") {
        type = KvType::Q4_1;
        return true;
    }
    if (value == "q5_0") {
        type = KvType::Q5_0;
        return true;
    }
    if (value == "q5_1") {
        type = KvType::Q5_1;
        return true;
    }

    return false;
}

std::string read_file_content(const std::string & file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return "";
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool has_only_keys(
    const json & object,
    std::initializer_list<const char *> keys,
    std::string & error,
    const std::string & path) {
    if (!object.is_object()) {
        error = path + " must be an object";
        return false;
    }

    for (auto it = object.begin(); it != object.end(); ++it) {
        bool found = false;
        for (const char * key : keys) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) {
            error = "unknown config field: " + path + "." + it.key();
            return false;
        }
    }
    return true;
}

bool read_string_field(
    const json & object,
    const char * key,
    std::string & value,
    std::string & error,
    const std::string & path) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_string()) {
        error = path + "." + key + " must be a string";
        return false;
    }
    value = expand_user_path(it->get<std::string>());
    return true;
}

bool read_bool_field(
    const json & object,
    const char * key,
    bool & value,
    std::string & error,
    const std::string & path) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        error = path + "." + key + " must be a boolean";
        return false;
    }
    value = it->get<bool>();
    return true;
}

bool read_int_field(
    const json & object,
    const char * key,
    int & value,
    std::string & error,
    const std::string & path,
    bool allow_zero) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_number_integer()) {
        error = path + "." + key + " must be an integer";
        return false;
    }
    const int64_t parsed = it->get<int64_t>();
    if (parsed < 0 ||
        parsed > std::numeric_limits<int>::max() ||
        (!allow_zero && parsed == 0)) {
        error = path + "." + key + " is out of range";
        return false;
    }
    value = (int) parsed;
    return true;
}

bool read_i32_field(
    const json & object,
    const char * key,
    int32_t & value,
    std::string & error,
    const std::string & path,
    bool allow_zero) {
    if (object.find(key) == object.end()) {
        return true;
    }
    int parsed = 0;
    if (!read_int_field(object, key, parsed, error, path, allow_zero)) {
        return false;
    }
    value = (int32_t) parsed;
    return true;
}

bool read_v_field(
    const json & object,
    const char * key,
    std::vector<int32_t> & value,
    std::string & error,
    const std::string & path) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        if (!parse_v(text.c_str(), value)) {
            error = path + "." + key + " must be a non-empty comma-separated integer string";
            return false;
        }
        return true;
    }

    if (!it->is_array() || it->empty()) {
        error = path + "." + key + " must be a non-empty integer array or comma-separated integer string";
        return false;
    }

    std::vector<int32_t> parsed;
    for (const json & item : *it) {
        if (!item.is_number_integer()) {
            error = path + "." + key + " must contain integers";
            return false;
        }
        const int64_t id = item.get<int64_t>();
        if (id < 0 || id > std::numeric_limits<int32_t>::max()) {
            error = path + "." + key + " contains an out-of-range head id";
            return false;
        }
        const int32_t head = (int32_t) id;
        if (std::find(parsed.begin(), parsed.end(), head) == parsed.end()) {
            parsed.push_back(head);
        }
    }
    if (parsed.empty()) {
        error = path + "." + key + " must not be empty";
        return false;
    }
    value = std::move(parsed);
    return true;
}

std::string known_model_tiers() {
    std::string result;
    for (const ModelTierSpec & spec : atmcore::list_model_tiers()) {
        if (!result.empty()) {
            result += ", ";
        }
        result += spec.tier;
    }
    return result.empty() ? std::string("<none>") : result;
}

bool load_model_tier_entry(
    const std::string & tier,
    ModelTierEntry & entry,
    std::string & error) {
    ModelTierSpec spec;
    if (!atmcore::find_model_tier(tier, spec)) {
        error = "unknown model: " + tier + " (known: " + known_model_tiers() + ")";
        return false;
    }

    entry = {};
    entry.tier = tier;
    entry.hf_repo = spec.artifact.hf_repo;
    entry.file = spec.artifact.filename;
    return true;
}

bool apply_model_tier_config(const std::string & tier, Options & opts, std::string & error) {
    ModelTierEntry entry;
    if (!load_model_tier_entry(tier, entry, error)) {
        return false;
    }

    opts.model_tier = tier;
    return true;
}

bool apply_cli_model_tier_config(const std::string & tier, Options & opts, std::string & error) {
    opts.model_path.clear();
    return apply_model_tier_config(tier, opts, error);
}

const char * tier_from_flag(const char * arg) {
    if (std::strcmp(arg, "--tiny") == 0) {
        return "tiny";
    }
    if (std::strcmp(arg, "--small") == 0) {
        return "small";
    }
    if (std::strcmp(arg, "--medium") == 0) {
        return "medium";
    }
    if (std::strcmp(arg, "--large") == 0) {
        return "large";
    }
    return nullptr;
}

bool read_kv_type_field(
    const json & object,
    const char * key,
    KvType & value,
    std::string & error,
    const std::string & path) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_string()) {
        error = path + "." + key + " must be a string";
        return false;
    }
    if (!parse_kv_type_arg(it->get<std::string>(), value)) {
        error = "invalid " + path + "." + key;
        return false;
    }
    return true;
}

bool read_size_field(
    const json & object,
    const char * key,
    uint64_t & value,
    std::string & error,
    const std::string & path) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (it->is_number_unsigned()) {
        value = it->get<uint64_t>();
        return true;
    }
    if (it->is_number_integer()) {
        const int64_t parsed = it->get<int64_t>();
        if (parsed < 0) {
            error = path + "." + key + " must be non-negative";
            return false;
        }
        value = (uint64_t) parsed;
        return true;
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        if (!parse_size_bytes(text.c_str(), value)) {
            error = "invalid " + path + "." + key;
            return false;
        }
        return true;
    }
    error = path + "." + key + " must be a size string or integer";
    return false;
}

bool apply_server_config(const json & body, Options & opts, std::string & error) {
    if (!has_only_keys(body, {"host", "port", "http_log", "run_log"}, error, "server")) {
        return false;
    }
    return read_string_field(body, "host", opts.host, error, "server") &&
           read_int_field(body, "port", opts.port, error, "server", /*allow_zero =*/ false) &&
           read_bool_field(body, "http_log", opts.http_log, error, "server") &&
           read_bool_field(body, "run_log", opts.run_log, error, "server");
}

bool apply_model_config(const json & body, Options & opts, std::string & error) {
    if (body.is_string()) {
        return apply_model_tier_config(body.get<std::string>(), opts, error);
    }

    if (!has_only_keys(body, {"tier", "path", "v", "truncate"}, error, "model")) {
        return false;
    }

    const auto tier_it = body.find("tier");
    if (tier_it != body.end()) {
        if (!tier_it->is_string()) {
            error = "model.tier must be a string";
            return false;
        }
        if (!apply_model_tier_config(tier_it->get<std::string>(), opts, error)) {
            return false;
        }
    } else {
        opts.model_tier.clear();
    }

    return read_string_field(body, "path", opts.model_path, error, "model") &&
           read_v_field(body, "v", opts.v, error, "model") &&
           read_bool_field(body, "truncate", opts.truncate, error, "model");
}

bool apply_attention_config(const json & body, Options & opts, std::string & error) {
    if (!has_only_keys(body, {"truncate"}, error, "attention")) {
        return false;
    }
    return read_bool_field(body, "truncate", opts.truncate, error, "attention");
}

bool apply_storage_config(const json & body, Options & opts, std::string & error) {
    if (!has_only_keys(body, {"data_dir", "cache_dir"}, error, "storage")) {
        return false;
    }
    return read_string_field(body, "data_dir", opts.data_dir, error, "storage") &&
           read_string_field(body, "cache_dir", opts.cache_dir, error, "storage");
}

bool apply_runtime_config(const json & body, Options & opts, std::string & error) {
    if (!has_only_keys(
            body,
            {"backend", "n_ctx", "n_batch", "n_ubatch", "threads", "threads_batch",
             "KV_type", "KV_type_k", "KV_type_v"},
            error,
            "runtime")) {
        return false;
    }

    const auto backend_it = body.find("backend");
    if (backend_it != body.end()) {
        if (!backend_it->is_string() ||
            !parse_backend_kind_arg(backend_it->get<std::string>(), opts.runtime.backend)) {
            error = "invalid runtime.backend";
            return false;
        }
    }

    if (!read_int_field(body, "n_ctx", opts.runtime.n_ctx, error, "runtime", /*allow_zero =*/ true) ||
        !read_int_field(body, "n_batch", opts.runtime.n_batch, error, "runtime", /*allow_zero =*/ true) ||
        !read_int_field(body, "n_ubatch", opts.runtime.n_ubatch, error, "runtime", /*allow_zero =*/ true) ||
        !read_int_field(body, "threads", opts.runtime.n_threads, error, "runtime", /*allow_zero =*/ true) ||
        !read_int_field(body, "threads_batch", opts.runtime.n_threads_batch, error, "runtime", /*allow_zero =*/ true)) {
        return false;
    }

    KvType kv_type = KvType::Q4_0;
    const bool has_kv_type = body.find("KV_type") != body.end();
    if (has_kv_type) {
        if (!read_kv_type_field(body, "KV_type", kv_type, error, "runtime")) {
            return false;
        }
        opts.runtime.kv_type_k = kv_type;
        opts.runtime.kv_type_v = kv_type;
    }
    return read_kv_type_field(body, "KV_type_k", opts.runtime.kv_type_k, error, "runtime") &&
           read_kv_type_field(body, "KV_type_v", opts.runtime.kv_type_v, error, "runtime");
}

bool apply_search_config(const json & body, Options & opts, std::string & error) {
    if (!has_only_keys(
            body,
            {"candidate_top_k", "top_k"},
            error,
            "search")) {
        return false;
    }

    return read_i32_field(body, "candidate_top_k", opts.search.candidate_top_k, error, "search", /*allow_zero =*/ false) &&
           read_i32_field(body, "top_k", opts.search.top_k, error, "search", /*allow_zero =*/ true);
}

bool apply_kv_config(const json & body, Options & opts, std::string & error) {
    if (!has_only_keys(body, {"resident_budget"}, error, "KV")) {
        return false;
    }
    return read_size_field(body, "resident_budget", opts.resident_kv_budget_bytes, error, "KV");
}

bool apply_config_section(
    const json & config,
    const char * key,
    Options & opts,
    std::string & error) {
    const auto it = config.find(key);
    if (it == config.end()) {
        return true;
    }

    if (std::strcmp(key, "server") == 0) {
        return apply_server_config(*it, opts, error);
    }
    if (std::strcmp(key, "model") == 0) {
        return apply_model_config(*it, opts, error);
    }
    if (std::strcmp(key, "attention") == 0) {
        return apply_attention_config(*it, opts, error);
    }
    if (std::strcmp(key, "storage") == 0) {
        return apply_storage_config(*it, opts, error);
    }
    if (std::strcmp(key, "runtime") == 0) {
        return apply_runtime_config(*it, opts, error);
    }
    if (std::strcmp(key, "search") == 0) {
        return apply_search_config(*it, opts, error);
    }
    if (std::strcmp(key, "KV") == 0) {
        return apply_kv_config(*it, opts, error);
    }

    error = "internal config parser error";
    return false;
}

bool load_config_file(const std::string & path, Options & opts, std::string & error) {
    const std::string text = read_file_content(path);
    if (text.empty()) {
        error = "failed to read config file: " + path;
        return false;
    }

    const json config = json::parse(text, nullptr, false);
    if (config.is_discarded() || !config.is_object()) {
        error = "config file must be a valid JSON object: " + path;
        return false;
    }
    if (!has_only_keys(config, {"server", "model", "storage", "runtime", "search", "KV", "attention"}, error, "config")) {
        return false;
    }

    return apply_config_section(config, "server", opts, error) &&
           apply_config_section(config, "model", opts, error) &&
           apply_config_section(config, "attention", opts, error) &&
           apply_config_section(config, "storage", opts, error) &&
           apply_config_section(config, "runtime", opts, error) &&
           apply_config_section(config, "search", opts, error) &&
           apply_config_section(config, "KV", opts, error);
}

bool find_config_arg(int argc, char ** argv, std::string & config_path) {
    config_path.clear();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            config_path = expand_user_path(argv[++i]);
        }
    }
    return true;
}

bool has_arg(int argc, char ** argv, const char * expected) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], expected) == 0) {
            return true;
        }
    }
    return false;
}

void print_version() {
    std::printf("attemory %s\n", ATTEMORY_VERSION_STRING);
    std::printf(
        "attemory-core %s API %d\n",
        ATTEMORY_ATMCORE_VERSION_STRING,
        ATTEMORY_ATMCORE_API_VERSION);
}

std::filesystem::path default_hf_hub_cache_dir() {
    const std::string hf_hub_cache = expand_user_path(env_value("HF_HUB_CACHE"));
    if (!hf_hub_cache.empty()) {
        return hf_hub_cache;
    }
    const std::string hf_home = expand_user_path(env_value("HF_HOME"));
    if (!hf_home.empty()) {
        return std::filesystem::path(hf_home) / "hub";
    }
    return default_cache_home() / "huggingface" / "hub";
}

std::string hf_repo_cache_name(const std::string & repo) {
    std::string name = "models--";
    for (char ch : repo) {
        name += (ch == '/') ? '-' : ch;
        if (ch == '/') {
            name += '-';
        }
    }
    return name;
}

bool find_file_under(
    const std::filesystem::path & root,
    const std::string & file_name,
    std::filesystem::path & found) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return false;
    }

    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        if (!ec && it->path().filename() == file_name) {
            found = it->path();
            return true;
        }
        it.increment(ec);
        if (ec) {
            ec.clear();
        }
    }
    return false;
}

bool resolve_model_tier_path(
    const ModelTierEntry & entry,
    std::string & model_path,
    std::string & error) {
    if (entry.file.empty()) {
        error = "model " + entry.tier + " does not define a model file in attemory-core";
        return false;
    }

    if (!entry.hf_repo.empty()) {
        const std::filesystem::path repo_cache =
            default_hf_hub_cache_dir() / hf_repo_cache_name(entry.hf_repo);
        std::filesystem::path found;
        if (find_file_under(repo_cache, entry.file, found)) {
            model_path = found.string();
            return true;
        }
    }

    std::ostringstream message;
    message << "model " << entry.tier << " file was not found";
    if (!entry.hf_repo.empty()) {
        message << " in Hugging Face cache "
                << (default_hf_hub_cache_dir() / hf_repo_cache_name(entry.hf_repo)).string();
    }
    message << "; use attemory-server to download tier models or pass --model to a local GGUF file";
    error = message.str();
    return false;
}

bool resolve_configured_model(Options & opts, std::string & error) {
    if (!opts.model_path.empty() || opts.model_tier.empty()) {
        return true;
    }

    ModelTierEntry entry;
    if (!load_model_tier_entry(opts.model_tier, entry, error)) {
        return false;
    }
    return resolve_model_tier_path(entry, opts.model_path, error);
}

json model_tier_json(const ModelTierSpec & spec) {
    json body = json::object();
    body["tier"] = spec.tier;
    body["family"] = spec.family;
    body["hf_repo"] = spec.artifact.hf_repo;
    body["filename"] = spec.artifact.filename;
    body["revision"] = spec.artifact.revision;
    body["sha256"] = spec.artifact.sha256;
    return body;
}

bool handle_model_metadata_command(int argc, char ** argv, bool & handled) {
    handled = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--model-info") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--model-info requires a model tier\n");
                return false;
            }
            const std::string tier = argv[++i];
            ModelTierSpec spec;
            if (!atmcore::find_model_tier(tier, spec)) {
                std::fprintf(stderr, "unknown model: %s (known: %s)\n", tier.c_str(), known_model_tiers().c_str());
                return false;
            }
            std::printf("%s\n", model_tier_json(spec).dump(2).c_str());
            handled = true;
            return true;
        }
    }
    return true;
}

} // namespace

void print_usage(const char * program) {
#ifdef __APPLE__
    const char * backend_usage = "metal|cpu";
#else
    const char * backend_usage = "gpu|cpu";
#endif
    std::string program_name = std::filesystem::path(program).filename().string();
    if (program_name.empty()) {
        program_name = program;
    }
    std::fprintf(
        stderr,
        "Usage: %s --tiny|--small|--medium|--large [options]\n"
        "\n"
        "Options:\n"
        "  --cache-dir DIR                  KV cache directory\n"
        "  --host HOST                      Bind address, default 127.0.0.1\n"
        "  --port PORT, -p PORT             HTTP port, default 9006\n"
        "  --backend BACKEND                Runtime backend, one of %s\n"
        "  --kv-type TYPE                   KV tensor type, default q4_0\n"
        "  --resident-kv-budget BYTES       RAM budget for resident KV snapshots, not a VRAM cap\n"
        "  --search-candidate-top-k N       Search candidate count\n"
        "  --search-top-k N                 Per-segment search result count\n"
        "  --http-log, --no-http-log        Enable or disable HTTP request logs\n"
        "  --run-log, --no-run-log          Enable or disable runtime logs\n"
        "  --version                        Show version information\n"
        "  --help, -h                       Show this help\n",
        program_name.c_str(),
        backend_usage
    );
}

std::string join_v(const std::vector<int32_t> & v) {
    std::string result;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) {
            result += ',';
        }
        result += std::to_string(v[i]);
    }
    return result;
}

bool parse_args(int argc, char ** argv, Options & opts) {
    apply_platform_defaults(opts);

    if (has_arg(argc, argv, "--version")) {
        print_version();
        std::exit(0);
    }

    bool metadata_command_handled = false;
    if (!handle_model_metadata_command(argc, argv, metadata_command_handled)) {
        return false;
    }
    if (metadata_command_handled) {
        std::exit(0);
    }

#if defined(ATTEMORY_SERVER_DEFAULT_NATIVE_ENGINE)
    opts.runtime.use_native_engine = true;
#endif

    std::string config_path;
    if (!find_config_arg(argc, argv, config_path)) {
        return false;
    }
    if (!config_path.empty()) {
        std::string error;
        if (!load_config_file(config_path, opts, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }
    const bool cli_has_cache_dir = has_arg(argc, argv, "--cache-dir");

    for (int i = 1; i < argc; ++i) {
        const char * arg = argv[i];

        if (std::strcmp(arg, "--config") == 0 && i + 1 < argc) {
            ++i;
        } else if (const char * tier = tier_from_flag(arg)) {
            std::string error;
            if (!apply_cli_model_tier_config(tier, opts, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return false;
            }
        } else if ((std::strcmp(arg, "--model") == 0 || std::strcmp(arg, "-m") == 0) && i + 1 < argc) {
            opts.model_path = expand_user_path(argv[++i]);
        } else if (std::strcmp(arg, "--data-dir") == 0 && i + 1 < argc) {
            apply_legacy_data_dir(argv[++i], opts, !cli_has_cache_dir);
        } else if (std::strcmp(arg, "--cache-dir") == 0 && i + 1 < argc) {
            opts.cache_dir = expand_user_path(argv[++i]);
        } else if (std::strcmp(arg, "--host") == 0 && i + 1 < argc) {
            opts.host = argv[++i];
        } else if ((std::strcmp(arg, "--port") == 0 || std::strcmp(arg, "-p") == 0) && i + 1 < argc) {
            const char * value = argv[++i];
            if (!parse_port(value, opts.port)) {
                std::fprintf(stderr, "invalid port: %s; expected an integer in 1..65535\n", value);
                return false;
            }
        } else if ((std::strcmp(arg, "--backend") == 0 || std::strcmp(arg, "-b") == 0) && i + 1 < argc) {
            if (!parse_backend_kind_arg(argv[++i], opts.runtime.backend)) {
                return false;
            }
        } else if ((std::strcmp(arg, "--n-ctx") == 0 || std::strcmp(arg, "-c") == 0) && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], opts.runtime.n_ctx)) {
                return false;
            }
        } else if (std::strcmp(arg, "--n-batch") == 0 && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], opts.runtime.n_batch)) {
                return false;
            }
        } else if (std::strcmp(arg, "--n-ubatch") == 0 && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], opts.runtime.n_ubatch)) {
                return false;
            }
        } else if (std::strcmp(arg, "--threads") == 0 && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], opts.runtime.n_threads)) {
                return false;
            }
        } else if (std::strcmp(arg, "--threads-batch") == 0 && i + 1 < argc) {
            if (!parse_positive_int(argv[++i], opts.runtime.n_threads_batch)) {
                return false;
            }
        } else if (std::strcmp(arg, "--v") == 0 && i + 1 < argc) {
            if (!parse_v(argv[++i], opts.v)) {
                return false;
            }
        } else if (std::strcmp(arg, "--truncate") == 0) {
            opts.truncate = true;
        } else if (std::strcmp(arg, "--no-truncate") == 0) {
            opts.truncate = false;
        } else if (std::strcmp(arg, "--search-candidate-top-k") == 0 && i + 1 < argc) {
            if (!parse_positive_i32(argv[++i], opts.search.candidate_top_k)) {
                return false;
            }
        } else if (std::strcmp(arg, "--search-top-k") == 0 && i + 1 < argc) {
            if (!parse_non_negative_i32(argv[++i], opts.search.top_k)) {
                return false;
            }
        } else if (std::strcmp(arg, "--native-engine") == 0) {
            opts.runtime.use_native_engine = true;
        } else if (std::strcmp(arg, "--resident-kv-budget") == 0 && i + 1 < argc) {
            if (!parse_size_bytes(argv[++i], opts.resident_kv_budget_bytes)) {
                return false;
            }
        } else if (std::strcmp(arg, "--http-log") == 0) {
            opts.http_log = true;
        } else if (std::strcmp(arg, "--no-http-log") == 0) {
            opts.http_log = false;
        } else if (std::strcmp(arg, "--run-log") == 0) {
            opts.run_log = true;
        } else if (std::strcmp(arg, "--no-run-log") == 0) {
            opts.run_log = false;
        } else if (std::strcmp(arg, "--kv-type") == 0 && i + 1 < argc) {
            KvType kv_type = KvType::Q4_0;
            if (!parse_kv_type_arg(argv[++i], kv_type)) {
                return false;
            }
            opts.runtime.kv_type_k = kv_type;
            opts.runtime.kv_type_v = kv_type;
        } else if ((std::strcmp(arg, "--kv-type-k") == 0 || std::strcmp(arg, "--cache-type-k") == 0) && i + 1 < argc) {
            if (!parse_kv_type_arg(argv[++i], opts.runtime.kv_type_k)) {
                return false;
            }
        } else if ((std::strcmp(arg, "--kv-type-v") == 0 || std::strcmp(arg, "--cache-type-v") == 0) && i + 1 < argc) {
            if (!parse_kv_type_arg(argv[++i], opts.runtime.kv_type_v)) {
                return false;
            }
        } else if (std::strcmp(arg, "--chat-template") == 0 && i + 1 < argc) {
            opts.runtime.chat_template = argv[++i];
            opts.runtime.chat_template_use_jinja = false;
            opts.chat_template_source = opts.runtime.chat_template;
        } else if (std::strcmp(arg, "--chat-template-file") == 0 && i + 1 < argc) {
            const std::string path = expand_user_path(argv[++i]);
            opts.runtime.chat_template = read_file_content(path);
            if (opts.runtime.chat_template.empty()) {
                std::fprintf(stderr, "failed to read chat template file: %s\n", path.c_str());
                return false;
            }
            opts.runtime.chat_template_use_jinja = true;
            opts.chat_template_source = path;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            return false;
        }
    }

    std::string error;
    if (!resolve_configured_model(opts, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (opts.truncate && opts.model_tier.empty() && opts.v.empty()) {
        return false;
    }

    if (opts.model_path.empty()) {
        std::fprintf(stderr, "missing model; set config model or pass --model PATH\n");
        return false;
    }

    return true;
}

} // namespace attemory::server_args
