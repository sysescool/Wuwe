#include <atomic>
#include <chrono>
#include <barrier>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/filesystem/filesystem.hpp>
#include <wuwe/agent/process/process.hpp>

namespace fs_tools = wuwe::agent::filesystem;
namespace process_tools = wuwe::agent::process;

namespace {

std::filesystem::path current_executable;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct temporary_directory {
  std::filesystem::path path;

  explicit temporary_directory(std::string name) {
    static std::atomic_uint64_t next_id{1};
#ifdef _WIN32
    const auto process_id = static_cast<std::uint64_t>(_getpid());
#else
    const auto process_id = static_cast<std::uint64_t>(getpid());
#endif
    path = std::filesystem::temp_directory_path() /
      ("wuwe-" + std::move(name) + "-" + std::to_string(process_id) + "-" +
       std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    error.clear();
    if (!std::filesystem::create_directories(path, error) || error) {
      throw std::runtime_error(
        "failed to create temporary directory: " + path.string() + ": " + error.message());
    }
  }

  ~temporary_directory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

class throwing_filesystem_backend final : public fs_tools::filesystem_backend {
public:
  fs_tools::filesystem_result read_text(
    const fs_tools::read_text_request&,
    std::stop_token) override {
    throw std::runtime_error("injected backend failure");
  }

  fs_tools::filesystem_result file_info(
    const fs_tools::file_info_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result write_text(
    const fs_tools::write_text_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result replace_text(
    const fs_tools::replace_text_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result list_directory(
    const fs_tools::list_directory_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result glob(
    const fs_tools::glob_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result search_text(
    const fs_tools::search_text_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result create_directory(
    const fs_tools::create_directory_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result copy_path(
    const fs_tools::transfer_path_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result move_path(
    const fs_tools::transfer_path_request&, std::stop_token) override { return unsupported(); }
  fs_tools::filesystem_result remove_path(
    const fs_tools::remove_path_request&, std::stop_token) override { return unsupported(); }

private:
  static fs_tools::filesystem_result unsupported() {
    return {
      .status = fs_tools::filesystem_status::io_error,
      .error_message = "unsupported test operation",
    };
  }
};

fs_tools::filesystem_policy writable_policy(const std::filesystem::path& root) {
  fs_tools::filesystem_policy policy;
  policy.root = root;
  policy.allow_write = true;
  policy.allow_create_directory = true;
  policy.allow_copy = true;
  policy.allow_move = true;
  policy.allow_remove = true;
  policy.require_approval_for_write = false;
  policy.require_approval_for_move = false;
  policy.require_approval_for_remove = false;
  return policy;
}

void filesystem_round_trip_and_concurrency_guards() {
  temporary_directory root("filesystem-roundtrip");
  temporary_directory outside("filesystem-copy-outside");
  wuwe::agent::audit::in_memory_audit_sink audit;
  fs_tools::filesystem_runtime runtime(
    fs_tools::make_local_filesystem_backend(), writable_policy(root.path), &audit);

  const auto created = runtime.write_text({
    .path = "src/example.txt",
    .content = "alpha\nbeta\nalpha\n",
    .disposition = fs_tools::write_disposition::create_new,
    .create_parent_directories = true,
    .metadata = {
      { "operation_id", "spoofed" },
      { "operation", "spoofed" },
      { "partial", "true" },
    },
  });
  require(created.successful() && !created.revision.empty(),
    "atomic create returns a revision: status=" + to_string(created.status) +
      " error=" + created.error_message + " revision=" + created.revision);
  require(created.metadata.at("operation_id") != "spoofed" &&
      created.metadata.at("operation") == "write_text",
    "runtime-owned filesystem metadata cannot be spoofed by callers or backends");
  require(!created.metadata.contains("partial"),
    "callers cannot invent filesystem progress metadata");
  const auto created_info = runtime.file_info({
    .path = "src/example.txt",
    .metadata = { { "type", "symlink" }, { "size", "999999" } },
  });
  require(created_info.metadata.at("type") == "regular_file" &&
      created_info.metadata.at("size") ==
        std::to_string(sizeof("alpha\nbeta\nalpha\n") - 1),
    "callers cannot overwrite backend-owned filesystem metadata");
  require(runtime.write_text({
      .path = "src/example.txt",
      .content = "unexpected",
      .disposition = fs_tools::write_disposition::create_new,
    }).status == fs_tools::filesystem_status::already_exists,
    "create_new never overwrites");
  require(runtime.write_text({
      .path = "src/example.txt",
      .content = "unexpected",
      .expected_revision = std::string("sha256:stale"),
    }).status == fs_tools::filesystem_status::conflict,
    "stale revisions are rejected");

  const auto race_base = runtime.write_text({
    .path = "race.txt",
    .content = "base",
    .disposition = fs_tools::write_disposition::create_new,
  });
  std::barrier start_race(3);
  fs_tools::filesystem_result race_left;
  fs_tools::filesystem_result race_right;
  std::thread left([&] {
    start_race.arrive_and_wait();
    race_left = runtime.write_text({
      .path = "race.txt", .content = "left",
      .expected_revision = race_base.revision,
    });
  });
  std::thread right([&] {
    start_race.arrive_and_wait();
    race_right = runtime.write_text({
      .path = "race.txt", .content = "right",
      .expected_revision = race_base.revision,
    });
  });
  start_race.arrive_and_wait();
  left.join();
  right.join();
  const auto successful_writes =
    static_cast<int>(race_left.successful()) + static_cast<int>(race_right.successful());
  const auto conflicts =
    static_cast<int>(race_left.status == fs_tools::filesystem_status::conflict) +
    static_cast<int>(race_right.status == fs_tools::filesystem_status::conflict);
  require(successful_writes == 1 && conflicts == 1,
    "process-local mutation serialization makes revision checks atomic");

  const auto replaced = runtime.replace_text({
    .path = "src/example.txt",
    .old_text = "beta",
    .new_text = "gamma",
    .expected_revision = created.revision,
  });
  require(replaced.successful() && replaced.affected_items == 1,
    "exact replacement succeeds once");
  require(runtime.replace_text({
      .path = "src/example.txt",
      .old_text = "alpha",
      .new_text = "delta",
      .expected_replacements = 1,
    }).status == fs_tools::filesystem_status::conflict,
    "ambiguous replacement is rejected");

  const auto read = runtime.read_text({ .path = "src/example.txt" });
  require(read.successful() && read.content == "alpha\ngamma\nalpha\n",
    "text round trip preserves content");
  const auto info = runtime.file_info({
    .path = "src/example.txt", .include_revision = true,
  });
  require(info.successful() && info.revision == read.revision &&
      info.metadata.at("type") == "regular_file",
    "file_info exposes a matching opaque revision");

  const auto search = runtime.search_text({
    .path = ".",
    .query = "alpha",
    .file_pattern = "**/*.txt",
  });
  require(search.successful() && search.matches.size() == 2 &&
      search.matches.front().path == std::filesystem::path("src/example.txt"),
    "literal search is root-relative and line-aware");
  const auto output_limited_search = runtime.search_text({
    .path = ".",
    .query = "alpha",
    .file_pattern = "**/*.txt",
    .max_output_bytes = 4,
  });
  require(output_limited_search.successful() &&
      output_limited_search.matches.empty() && output_limited_search.truncated &&
      output_limited_search.metadata.at("output_limit_reached") == "true",
    "search output has an independent byte budget");

  const auto copied = runtime.copy_path({
    .source = "src/example.txt",
    .destination = "copy/example.txt",
  });
  require(copied.successful() && copied.bytes_processed == read.content.size(),
    "copy reports bounded bytes");
  const auto self_copy = runtime.copy_path({
    .source = "src", .destination = "src/nested", .recursive = true,
  });
  require(self_copy.status == fs_tools::filesystem_status::invalid_path,
    "a recursive copy cannot descend into its own destination: status=" +
      fs_tools::to_string(self_copy.status) + " error=" +
      self_copy.error_message);
  const auto moved = runtime.move_path({
      .source = "copy/example.txt",
      .destination = "moved/example.txt",
    });
  require(moved.successful() &&
      moved.path == std::filesystem::path("copy/example.txt") &&
      moved.destination == std::filesystem::path("moved/example.txt"),
    "move succeeds and preserves root-relative result paths");
  require(runtime.create_directory({ .path = "move-directory/child" }).successful(),
    "directory move fixture is created");
  require(runtime.move_path({
      .source = "move-directory", .destination = "move-directory-target",
    }).status == fs_tools::filesystem_status::type_mismatch,
    "directory moves require explicit recursive acknowledgement");
  require(runtime.move_path({
      .source = "move-directory", .destination = "move-directory-target",
      .recursive = true,
    }).successful(),
    "explicit directory move succeeds atomically");
  require(runtime.list_directory({
      .path = ".", .recursive = true, .max_depth = 8, .max_entries = 100,
    }).entries.size() >= 4,
    "recursive listing returns bounded entries");
  require(runtime.glob({
      .path = ".", .pattern = "**/*.txt", .max_depth = 8,
    }).entries.size() == 3,
    "glob spans directories");

  require(runtime.write_text({
      .path = "symlink-source/redirect/payload.data",
      .content = "must-stay-inside-root",
      .disposition = fs_tools::write_disposition::create_new,
      .create_parent_directories = true,
    }).successful(),
    "symlink destination fixture source is created");
  require(runtime.create_directory({
      .path = "symlink-destination",
    }).successful(),
    "symlink destination fixture directory is created");
  std::filesystem::create_directories(outside.path / "redirect-target");
  std::error_code destination_symlink_error;
  std::filesystem::create_directory_symlink(
    outside.path / "redirect-target",
    root.path / "symlink-destination" / "redirect",
    destination_symlink_error);
  if (!destination_symlink_error) {
    const auto linked_destination_copy = runtime.copy_path({
      .source = "symlink-source",
      .destination = "symlink-destination",
      .overwrite = true,
      .recursive = true,
    });
    require(linked_destination_copy.status ==
        fs_tools::filesystem_status::permission_denied &&
        !std::filesystem::exists(
          outside.path / "redirect-target" / "payload.data"),
      "recursive copy cannot traverse a pre-existing destination symlink");
  }

  const auto removed = runtime.remove_path({
    .path = "moved", .recursive = true,
  });
  require(removed.successful() && removed.affected_items >= 2,
    "recursive removal is explicit and bounded");
  require(!audit.events().empty(), "filesystem operations emit audit events");
}

void filesystem_policy_and_tool_boundaries() {
  temporary_directory root("filesystem-policy");
  std::ofstream binary(root.path / "binary.bin", std::ios::binary);
  binary.write("a\0b", 3);
  binary.close();

  fs_tools::filesystem_policy read_only;
  read_only.root = root.path;
  fs_tools::filesystem_runtime runtime(
    fs_tools::make_local_filesystem_backend(), read_only);
  require(runtime.read_text({ .path = "../outside.txt" }).status ==
      fs_tools::filesystem_status::outside_root,
    "parent traversal is denied");
  require(runtime.read_text({ .path = root.path / "binary.bin" }).status ==
      fs_tools::filesystem_status::permission_denied,
    "absolute paths are denied by default");
  require(runtime.read_text({ .path = "binary.bin" }).status ==
      fs_tools::filesystem_status::type_mismatch,
    "text tools reject binary content");
  require(runtime.search_text({
      .path = ".", .query = "",
    }).status == fs_tools::filesystem_status::invalid_request,
    "request validation is distinct from path validation");
  std::filesystem::create_directories(root.path / "target");
  std::ofstream(root.path / "target" / "value.txt") << "value";
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(
    root.path / "target", root.path / "linked", symlink_error);
  if (!symlink_error) {
    require(runtime.read_text({ .path = "linked/value.txt" }).status ==
        fs_tools::filesystem_status::permission_denied,
      "symbolic links are denied when follow_symlinks is false");
  }
  require(runtime.write_text({ .path = "new.txt", .content = "x" }).status ==
      fs_tools::filesystem_status::permission_denied,
    "write capability is disabled by default");

  auto approval_policy = writable_policy(root.path);
  approval_policy.require_approval_for_write = true;
  fs_tools::filesystem_runtime approval_runtime(
    fs_tools::make_local_filesystem_backend(), approval_policy);
  require(approval_runtime.write_text({
      .path = "approval.txt", .content = "x",
    }).status == fs_tools::filesystem_status::approval_denied,
    "missing approval fails closed");
  std::stop_source cancelled_write;
  cancelled_write.request_stop();
  require(approval_runtime.write_text({
      .path = "cancelled-approval.txt", .content = "x",
    }, cancelled_write.get_token()).status == fs_tools::filesystem_status::cancelled,
    "pre-cancellation does not trigger an approval request");

  fs_tools::filesystem_runtime root_runtime(
    fs_tools::make_local_filesystem_backend(), writable_policy(root.path));
  require(root_runtime.remove_path({ .path = ".", .recursive = true }).status ==
      fs_tools::filesystem_status::permission_denied,
    "configured root can never be removed");

  std::stop_source cancelled;
  cancelled.request_stop();
  require(runtime.read_text({ .path = "binary.bin" }, cancelled.get_token()).status ==
      fs_tools::filesystem_status::cancelled,
    "pre-cancelled operations do not access content");

  fs_tools::filesystem_runtime tool_runtime(
    fs_tools::make_local_filesystem_backend(), writable_policy(root.path));
  fs_tools::filesystem_tool_provider tools(tool_runtime);
  require(tools.tools().size() == 11,
    "enabled filesystem policy exposes eleven structured tools");
  require(!tools.invoke("write_file",
      R"({"path":"tool.txt","content":"tool-data","create_new":true})").error_code,
    "write_file tool succeeds");
  const auto tool_read = tools.invoke("read_file", R"({"path":"tool.txt"})");
  require(!tool_read.error_code &&
      nlohmann::json::parse(tool_read.content).at("content") == "tool-data",
    "read_file tool returns structured content");

  wuwe::agent::audit::in_memory_audit_sink backend_audit;
  fs_tools::filesystem_runtime throwing_runtime(
    std::make_unique<throwing_filesystem_backend>(), read_only, &backend_audit);
  const auto backend_failure = throwing_runtime.read_text({ .path = "binary.bin" });
  require(backend_failure.status == fs_tools::filesystem_status::io_error &&
      backend_failure.error_message.find("injected backend failure") != std::string::npos &&
      !backend_audit.events().empty(),
    "backend exceptions are contained and audited");
}

process_tools::process_policy self_process_policy(const std::filesystem::path& root) {
  process_tools::process_policy policy;
  policy.working_directory_root = root;
  policy.allowed_executables = { current_executable };
  policy.allowed_environment_overrides = { "WUWE_PROCESS_TEST" };
  policy.max_limits.timeout = std::chrono::seconds(5);
  policy.max_limits.max_stdout_bytes = 64 * 1024;
  policy.max_limits.max_stderr_bytes = 64 * 1024;
  return policy;
}

process_tools::process_request probe_request(
  std::vector<std::string> arguments,
  const std::filesystem::path& workdir = ".") {
  arguments.insert(arguments.begin(), "--process-probe");
  return {
    .executable = current_executable,
    .arguments = std::move(arguments),
    .workdir = workdir,
  };
}

void process_execution_is_structured_and_bounded() {
  temporary_directory root("process-runtime");
  auto invalid_default_policy = self_process_policy(root.path);
  invalid_default_policy.default_workdir = root.path.parent_path();
  bool invalid_default_rejected = false;
  try {
    process_tools::process_runtime invalid_runtime(
      process_tools::make_local_process_backend(), invalid_default_policy);
  }
  catch (const std::invalid_argument&) {
    invalid_default_rejected = true;
  }
  require(invalid_default_rejected,
    "invalid default working directories fail during runtime construction");

  wuwe::agent::audit::in_memory_audit_sink audit;
  process_tools::process_runtime runtime(
    process_tools::make_local_process_backend(), self_process_policy(root.path), &audit);

  const auto echo = runtime.run(probe_request({ "echo", "hello world", "quoted-value" }));
  require(echo.termination_reason == process_tools::process_termination_reason::exited &&
      echo.exit_code == 0 && echo.stdout_text == "hello world|quoted-value",
    "argv execution preserves argument boundaries: reason=" +
      process_tools::to_string(echo.termination_reason) + " exit=" +
      (echo.exit_code ? std::to_string(*echo.exit_code) : "none") +
      " stdout=" + echo.stdout_text + " stderr=" + echo.stderr_text +
      " error=" + echo.error_message);
  auto metadata_request = probe_request({ "echo", "metadata" });
  metadata_request.metadata["operation_id"] = "spoofed";
  metadata_request.metadata["shell"] = "true";
  metadata_request.metadata["signal"] = "999";
  const auto metadata_result = runtime.run(std::move(metadata_request));
  require(metadata_result.metadata.at("operation_id") != "spoofed" &&
      metadata_result.metadata.at("shell") == "false" &&
      !metadata_result.metadata.contains("signal"),
    "runtime-owned process metadata cannot be spoofed by callers or backends");

  auto environment_request = probe_request({ "env", "WUWE_PROCESS_TEST" });
  environment_request.environment["WUWE_PROCESS_TEST"] = "isolated-value";
  require(runtime.run(std::move(environment_request)).stdout_text == "isolated-value",
    "environment overrides are explicitly allowlisted");

  auto input_request = probe_request({ "stdin" });
  input_request.stdin_text = "bounded-input";
  require(runtime.run(std::move(input_request)).stdout_text == "bounded-input",
    "stdin is delivered without a shell");

  auto argument_limited = probe_request({ "echo", "never" });
  argument_limited.limits.max_argument_count = 2;
  require(runtime.run(std::move(argument_limited)).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "request-level argument limits are enforced before launch");

  auto stdin_limited = probe_request({ "stdin" });
  stdin_limited.stdin_text = "too-large";
  stdin_limited.limits.max_stdin_bytes = 4;
  require(runtime.run(std::move(stdin_limited)).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "request-level stdin limits are enforced before launch");

  auto environment_limited = probe_request({ "env", "WUWE_PROCESS_TEST" });
  environment_limited.environment["WUWE_PROCESS_TEST"] = "too-large";
  environment_limited.limits.max_environment_bytes = 8;
  require(runtime.run(std::move(environment_limited)).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "request-level environment limits are enforced before launch");

  auto truncated_request = probe_request({ "spam", "4096" });
  truncated_request.limits.max_stdout_bytes = 32;
  const auto truncated = runtime.run(std::move(truncated_request));
  require(truncated.stdout_text.size() == 32 && truncated.stdout_truncated,
    "output is drained but retained only to the configured bound");

  auto zero_limit_request = probe_request({ "spam", "70000" });
  zero_limit_request.limits.max_stdout_bytes = 0;
  const auto zero_limit = runtime.run(std::move(zero_limit_request));
  require(zero_limit.stdout_text.size() == 64 * 1024 && zero_limit.stdout_truncated,
    "zero cannot bypass the policy output limit");

  auto timeout_request = probe_request({ "sleep", "2000" });
  timeout_request.limits.timeout = std::chrono::milliseconds(100);
  require(runtime.run(std::move(timeout_request)).termination_reason ==
      process_tools::process_termination_reason::timed_out,
    "timeout terminates the process tree");

  std::stop_source cancelled;
  cancelled.request_stop();
  require(runtime.run(probe_request({ "echo", "never" }), cancelled.get_token()).termination_reason ==
      process_tools::process_termination_reason::cancelled,
    "pre-cancellation prevents launch");

  auto denied_request = probe_request({ "echo", "never" });
  denied_request.executable = "not-allowed";
  require(runtime.run(std::move(denied_request)).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "executables fail closed against the allowlist");

  std::ofstream(root.path / current_executable.filename(), std::ios::binary)
    << "not-an-executable";
  auto basename_policy = self_process_policy(root.path);
  basename_policy.allowed_executables = { current_executable.filename() };
  basename_policy.executable_search_paths = { current_executable.parent_path() };
  process_tools::process_runtime basename_runtime(
    process_tools::make_local_process_backend(), basename_policy);
  auto same_name_attack = probe_request({ "echo", "never" });
  same_name_attack.executable = root.path / current_executable.filename();
  require(basename_runtime.run(std::move(same_name_attack)).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "a basename allowlist cannot authorize an arbitrary absolute path");

  auto denied_environment = probe_request({ "echo", "never" });
  denied_environment.environment["UNDECLARED_ENVIRONMENT"] = "value";
  require(runtime.run(std::move(denied_environment)).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "environment overrides fail closed against the allowlist");

  auto outside = probe_request({ "echo", "never" });
  outside.workdir = root.path.parent_path();
  require(runtime.run(std::move(outside)).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "workdir cannot escape its configured root");
  require(!audit.events().empty(), "process execution emits audit events");
}

void process_tools_and_shell_are_explicit() {
  temporary_directory root("process-tools");
  auto policy = self_process_policy(root.path);
  wuwe::agent::audit::in_memory_audit_sink audit;
  process_tools::process_runtime runtime(
    process_tools::make_local_process_backend(), policy, &audit);
  process_tools::process_tool_provider tools(runtime);
  require(tools.tools().size() == 1, "raw shell is not exposed by default");
  const auto invocation = tools.invoke("run_process", nlohmann::json {
    { "executable", current_executable.generic_string() },
    { "arguments", { "--process-probe", "echo", "tool-process" } },
  }.dump());
  require(!invocation.error_code &&
      nlohmann::json::parse(invocation.content).at("stdout") == "tool-process",
    "run_process tool uses structured argv");
  require(runtime.run_shell({ .command = "echo never" }).termination_reason ==
      process_tools::process_termination_reason::policy_denied,
    "shell is disabled by policy");
  require(!audit.events().empty() &&
      audit.events().back().outcome ==
        wuwe::agent::audit::audit_event_outcome::denied,
    "preflight shell denials are audited consistently");

  policy.allow_shell = true;
  policy.require_approval_for_shell = true;
  process_tools::process_runtime no_approval(
    process_tools::make_local_process_backend(), policy);
  require(no_approval.run_shell({ .command = "echo never" }).termination_reason ==
      process_tools::process_termination_reason::approval_denied,
    "shell approval fails closed");

  wuwe::agent::approval::allow_all_approval_service approvals;
  process_tools::process_runtime shell_runtime(
    process_tools::make_local_process_backend(), policy, nullptr, &approvals);
  process_tools::process_tool_provider shell_tools(shell_runtime, {
    .expose_shell_tool = true,
  });
  require(shell_tools.tools().size() == 2,
    "shell appears only after policy and provider opt in");
  const auto shell = shell_tools.invoke("run_shell", R"({"command":"echo shell-ok"})");
  require(!shell.error_code &&
      nlohmann::json::parse(shell.content).at("stdout").get<std::string>().find("shell-ok") != std::string::npos,
    "approved shell adapter executes through the configured shell");
}

void run(const char* name, void (*test)()) {
  test();
  std::cout << "[passed] " << name << '\n';
}

std::string environment_value(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string result(value);
  std::free(value);
  return result;
#else
  const auto* value = std::getenv(name);
  return value ? std::string(value) : std::string {};
#endif
}

int process_probe(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string mode = argv[2];
  if (mode == "echo") {
    for (int index = 3; index < argc; ++index) {
      if (index > 3) std::cout << '|';
      std::cout << argv[index];
    }
    return 0;
  }
  if (mode == "stdin") {
    std::cout << std::string(
      std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    return 0;
  }
  if (mode == "env" && argc >= 4) {
    std::cout << environment_value(argv[3]);
    return 0;
  }
  if (mode == "sleep" && argc >= 4) {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(argv[3])));
    return 0;
  }
  if (mode == "spam" && argc >= 4) {
    std::cout << std::string(static_cast<std::size_t>(std::stoull(argv[3])), 'x');
    return 0;
  }
  if (mode == "exit" && argc >= 4) return std::stoi(argv[3]);
  return 3;
}

} // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--process-probe") {
    return process_probe(argc, argv);
  }
  try {
    current_executable = std::filesystem::canonical(argv[0]);
    run("filesystem round trip", filesystem_round_trip_and_concurrency_guards);
    run("filesystem policy and tools", filesystem_policy_and_tool_boundaries);
    run("process structured execution", process_execution_is_structured_and_bounded);
    run("process tools and shell", process_tools_and_shell_are_explicit);
    return 0;
  }
  catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
