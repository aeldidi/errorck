#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../sqlite3.h"
#include "subprocess.h"

namespace fs = std::filesystem;

struct CommandResult {
  int exit_code = 0;
  std::string stdout_output;
  std::string stderr_output;
};

static void ReadPipe(FILE *pipe, std::string *output) {
  char buffer[4096];
  while (true) {
    size_t bytes = std::fread(buffer, 1, sizeof(buffer), pipe);
    if (bytes == 0) {
      break;
    }
    output->append(buffer, bytes);
  }
}

static CommandResult RunCommand(const std::vector<std::string> &args) {
  CommandResult result;
  if (args.empty()) {
    result.exit_code = 127;
    result.stderr_output = "empty command\n";
    return result;
  }

  std::vector<const char *> argv;
  argv.reserve(args.size() + 1);
  for (const std::string &arg : args) {
    argv.push_back(arg.c_str());
  }
  argv.push_back(nullptr);

  subprocess_s process = {};
  int options = subprocess_option_inherit_environment |
                subprocess_option_search_user_path;
  if (subprocess_create(argv.data(), options, &process) != 0) {
    result.exit_code = 127;
    result.stderr_output = "subprocess_create failed\n";
    return result;
  }

  FILE *stdout_pipe = subprocess_stdout(&process);
  FILE *stderr_pipe = subprocess_stderr(&process);

  // Drain both pipes so the child cannot block on full buffers.
  std::thread stdout_thread;
  std::thread stderr_thread;
  if (stdout_pipe) {
    stdout_thread = std::thread(ReadPipe, stdout_pipe, &result.stdout_output);
  }
  if (stderr_pipe) {
    stderr_thread = std::thread(ReadPipe, stderr_pipe, &result.stderr_output);
  }

  int exit_code = 0;
  if (subprocess_join(&process, &exit_code) != 0) {
    result.exit_code = 127;
  } else {
    result.exit_code = exit_code;
  }

  if (stdout_thread.joinable()) {
    stdout_thread.join();
  }
  if (stderr_thread.joinable()) {
    stderr_thread.join();
  }

  subprocess_destroy(&process);
  return result;
}

static bool ReadFile(const fs::path &path, std::string &out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

static bool WriteFile(const fs::path &path, const std::string &contents) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << contents;
  return true;
}

static void EnsureTrailingNewline(std::string &text) {
  if (text.empty() || text.back() != '\n') {
    text.push_back('\n');
  }
}

static fs::path WeaklyCanonical(const fs::path &path) {
  std::error_code ec;
  fs::path resolved = fs::weakly_canonical(path, ec);
  if (ec) {
    return path;
  }
  return resolved;
}

static bool IsSubpath(const fs::path &path, const fs::path &base) {
  auto path_it = path.begin();
  auto base_it = base.begin();
  for (; base_it != base.end(); ++base_it, ++path_it) {
    if (path_it == path.end() || *path_it != *base_it) {
      return false;
    }
  }
  return true;
}

// Normalize absolute paths so golden files stay stable across machines.
static std::string NormalizePath(const std::string &path_str,
                                 const fs::path &test_dir) {
  fs::path path(path_str);
  if (!path.is_absolute()) {
    return path_str;
  }

  fs::path base = WeaklyCanonical(test_dir);
  fs::path full = WeaklyCanonical(path);
  if (IsSubpath(full, base)) {
    std::error_code ec;
    fs::path rel = fs::relative(full, base, ec);
    if (!ec) {
      return rel.generic_string();
    }
  }

  return path.filename().generic_string();
}

static std::string NormalizeOutput(const std::string &output,
                                   const fs::path &test_dir) {
  std::string normalized;
  size_t start = 0;
  while (start < output.size()) {
    size_t end = output.find('\n', start);
    std::string line =
        output.substr(start, end == std::string::npos ? end : end - start);

    const std::string needle = "\"filename\":";
    size_t key = line.find(needle);
    while (key != std::string::npos) {
      size_t value_start = key + needle.size();
      while (value_start < line.size() && line[value_start] == ' ') {
        ++value_start;
      }
      if (value_start >= line.size() || line[value_start] != '"') {
        key = line.find(needle, key + needle.size());
        continue;
      }
      ++value_start;
      size_t value_end = line.find('"', value_start);
      if (value_end == std::string::npos) {
        break;
      }
      std::string path = line.substr(value_start, value_end - value_start);
      std::string normalized_path = NormalizePath(path, test_dir);
      line.replace(value_start, value_end - value_start, normalized_path);
      key = line.find(needle, value_start + normalized_path.size());
    }

    normalized += line;
    if (end == std::string::npos) {
      break;
    }
    normalized.push_back('\n');
    start = end + 1;
  }

  return normalized;
}

static bool ReadDatabaseOutput(const fs::path &db_path, std::string &out,
                               std::string &error) {
  sqlite3 *db = nullptr;
  int rc = sqlite3_open(db_path.string().c_str(), &db);
  if (rc != SQLITE_OK) {
    error = "Failed to open database: " +
            std::string(db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    if (db) {
      sqlite3_close(db);
    }
    return false;
  }

  // Order by row id so test output stays stable across runs.
  const char *sql =
      "SELECT name, filename, line, column, handling_type, "
      "assigned_filename, assigned_line, assigned_column FROM watched_calls "
      "ORDER BY id;";
  sqlite3_stmt *stmt = nullptr;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    error = "Failed to query database: " + std::string(sqlite3_errmsg(db));
    sqlite3_close(db);
    return false;
  }

  std::string result;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    const char *filename =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    int line = sqlite3_column_int(stmt, 2);
    int column = sqlite3_column_int(stmt, 3);
    const char *handling =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
    const char *assigned_filename =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
    int assigned_line = sqlite3_column_int(stmt, 6);
    int assigned_column = sqlite3_column_int(stmt, 7);

    result += "{\"name\":\"";
    result += name ? name : "";
    result += "\",\"filename\":\"";
    result += filename ? filename : "";
    result += "\",\"line\":\"";
    result += std::to_string(line);
    result += "\",\"column\":\"";
    result += std::to_string(column);
    result += "\",\"handlingType\":\"";
    result += handling ? handling : "";
    result += "\"";
    if (assigned_filename) {
      result += ", \"assigned\": { \"filename\": \"";
      result += assigned_filename;
      result += "\", \"line\": \"";
      result += std::to_string(assigned_line);
      result += "\", \"column\": \"";
      result += std::to_string(assigned_column);
      result += "\" }";
    }
    result += "}";
    result += "\n";
  }

  if (rc != SQLITE_DONE) {
    error = "Failed to read results: " + std::string(sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  out = result;
  return true;
}

static std::vector<std::string> ReadCompileFlags(const fs::path &path) {
  std::ifstream in(path);
  std::vector<std::string> flags;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (!line.empty() && line.front() == '#') {
      continue;
    }
    flags.push_back(line);
  }
  return flags;
}

static std::string EscapeJson(const std::string &text) {
  // Minimal escaping keeps compile_commands.json valid without a JSON library.
  std::string escaped;
  escaped.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += c;
      break;
    }
  }
  return escaped;
}

static bool WriteCompileCommands(const fs::path &output_dir,
                                 const fs::path &test_dir,
                                 const std::vector<std::string> &flags) {
  // ClangTool matches compile commands against absolute source paths.
  fs::path directory = WeaklyCanonical(test_dir);
  fs::path source_path = WeaklyCanonical(test_dir / "main.c");
  std::ostringstream json;
  json << "[\n  {\n";
  json << "    \"directory\": \"" << EscapeJson(directory.string()) << "\",\n";
  json << "    \"file\": \"" << EscapeJson(source_path.string()) << "\",\n";
  json << "    \"arguments\": [";
  json << "\"clang\"";
  for (const std::string &flag : flags) {
    json << ", \"" << EscapeJson(flag) << "\"";
  }
  json << ", \"-c\", \"" << EscapeJson(source_path.string()) << "\"";
  json << "]\n  }\n]\n";

  fs::path output_path = output_dir / "compile_commands.json";
  return WriteFile(output_path, json.str());
}

static void PrintDiff(const fs::path &expected_path,
                      const fs::path &actual_path) {
  // Keep diff output readable without relying on a shell.
  CommandResult diff =
      RunCommand({"diff", "-u", expected_path.string(), actual_path.string()});
  if (!diff.stdout_output.empty()) {
    std::cerr << diff.stdout_output;
  }
  if (!diff.stderr_output.empty()) {
    std::cerr << diff.stderr_output;
  }
  if (diff.exit_code == 127 || diff.exit_code > 1) {
    std::cerr << "(diff command failed)\n";
  }
}

static void PrintUsage(const char *argv0) {
  std::cerr << "Usage: " << argv0
            << " --build-dir <path> --test-dir <path>\n";
}

int main(int argc, char **argv) {
  fs::path build_dir;
  fs::path test_dir;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--build-dir" && i + 1 < argc) {
      build_dir = argv[++i];
    } else if (arg == "--test-dir" && i + 1 < argc) {
      test_dir = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      PrintUsage(argv[0]);
      return 2;
    } else {
      std::cerr << "Unexpected argument: " << arg << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }

  if (build_dir.empty()) {
    std::cerr << "--build-dir is required.\n";
    PrintUsage(argv[0]);
    return 2;
  }

  if (test_dir.empty()) {
    std::cerr << "--test-dir is required.\n";
    PrintUsage(argv[0]);
    return 2;
  }

  std::error_code ec;
  if (!fs::exists(test_dir, ec)) {
    std::cerr << "Test directory not found: " << test_dir << "\n";
    return 2;
  }

  fs::path errorck_path = build_dir / "errorck";
  if (!fs::exists(errorck_path, ec)) {
    std::cerr << "errorck binary not found: " << errorck_path << "\n";
    return 2;
  }

  fs::path main_path = test_dir / "main.c";
  fs::path flags_path = test_dir / "compile_flags.txt";
  fs::path expected_path = test_dir / "expected.jsonl";
  fs::path notable_path = test_dir / "functions.json";

  if (!fs::exists(main_path, ec)) {
    std::cerr << "Missing main.c in " << test_dir << "\n";
    return 1;
  }
  if (!fs::exists(flags_path, ec)) {
    std::cerr << "Missing compile_flags.txt in " << test_dir << "\n";
    return 1;
  }
  if (!fs::exists(expected_path, ec)) {
    std::cerr << "Missing expected.jsonl in " << test_dir << "\n";
    return 1;
  }
  if (!fs::exists(notable_path, ec)) {
    std::cerr << "Missing functions.json in " << test_dir << "\n";
    return 1;
  }

  std::vector<std::string> flags = ReadCompileFlags(flags_path);
  fs::path test_build_dir = build_dir / "tests" / test_dir.filename();
  fs::create_directories(test_build_dir, ec);
  if (ec) {
    std::cerr << "Failed to create build dir: " << test_build_dir << "\n";
    return 1;
  }

  if (!WriteCompileCommands(test_build_dir, test_dir, flags)) {
    std::cerr << "Failed to write compile_commands.json for " << test_dir
              << "\n";
    return 1;
  }

  fs::path db_path = test_build_dir / "results.sqlite";
  std::vector<std::string> command = {
      errorck_path.string(),
      "--notable-functions",
      notable_path.string(),
      "--db",
      db_path.string(),
      "--overwrite-if-needed",
      "-p",
      test_build_dir.string(),
      main_path.string()};
  CommandResult result = RunCommand(command);
  if (result.exit_code != 0) {
    std::cerr << "errorck failed for " << test_dir << " (exit "
              << result.exit_code << ")\n";
    if (!result.stdout_output.empty()) {
      std::cerr << result.stdout_output;
    }
    if (!result.stderr_output.empty()) {
      std::cerr << result.stderr_output;
    }
    return 1;
  }

  std::string db_output;
  std::string db_error;
  if (!ReadDatabaseOutput(db_path, db_output, db_error)) {
    std::cerr << "Failed to read database output for " << test_dir << "\n";
    if (!db_error.empty()) {
      std::cerr << db_error << "\n";
    }
    return 1;
  }

  std::string normalized = NormalizeOutput(db_output, test_dir);
  EnsureTrailingNewline(normalized);

  std::string expected;
  if (!ReadFile(expected_path, expected)) {
    std::cerr << "Failed to read expected output for " << test_dir << "\n";
    return 1;
  }
  EnsureTrailingNewline(expected);

  if (normalized != expected) {
    fs::path actual_path = test_build_dir / "actual.jsonl";
    if (!WriteFile(actual_path, normalized)) {
      std::cerr << "Failed to write actual output for " << test_dir << "\n";
      return 1;
    }

    std::cerr << "FAIL " << test_dir.filename().string() << "\n";
    PrintDiff(expected_path, actual_path);
    return 1;
  }

  std::cout << "PASS " << test_dir.filename().string() << "\n";
  return 0;
}
