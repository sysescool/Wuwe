#include <wuwe/agent/skills/skill_resolver.hpp>

#include <algorithm>
#include <set>
#include <utility>

namespace wuwe::agent::skills {
namespace {

struct pending_requirement {
  skill_dependency dependency;
  std::string parent;
  std::vector<std::string> path;
  bool root { false };
};

struct resolution_state {
  std::map<std::string, skill_package_ptr> selected;
  std::map<std::string, std::set<std::string>> edges;
  std::set<std::string> roots;
  std::vector<skill_diagnostic> diagnostics;
};

skill_diagnostic diagnostic(
  std::string code, std::string message, const pending_requirement& requirement) {
  auto path = requirement.path;
  path.push_back(requirement.dependency.id);
  return {
    .severity = skill_diagnostic_severity::error,
    .code = std::move(code),
    .message = std::move(message),
    .skill_id = requirement.dependency.id,
    .dependency_path = std::move(path),
  };
}

void optional_warning(
  resolution_state& state, const pending_requirement& requirement, std::string message) {
  auto value = diagnostic("optional_dependency_skipped", std::move(message), requirement);
  value.severity = skill_diagnostic_severity::warning;
  state.diagnostics.push_back(std::move(value));
}

bool pending_less(const pending_requirement& lhs, const pending_requirement& rhs) {
  if (lhs.dependency.id != rhs.dependency.id) {
    return lhs.dependency.id < rhs.dependency.id;
  }
  if (lhs.dependency.optional != rhs.dependency.optional) {
    return !lhs.dependency.optional;
  }
  if (lhs.parent != rhs.parent) {
    return lhs.parent < rhs.parent;
  }
  return lhs.dependency.version.expression() < rhs.dependency.version.expression();
}

bool reaches(const resolution_state& state, const std::string& from, const std::string& target,
  std::set<std::string>& visited) {
  if (from == target) {
    return true;
  }
  if (!visited.insert(from).second) {
    return false;
  }
  const auto found = state.edges.find(from);
  if (found == state.edges.end()) {
    return false;
  }
  return std::any_of(found->second.begin(), found->second.end(), [&](const auto& dependency) {
    return reaches(state, dependency, target, visited);
  });
}

bool would_create_cycle(const resolution_state& state, const pending_requirement& requirement) {
  if (requirement.parent.empty()) {
    return false;
  }
  std::set<std::string> visited;
  return reaches(state, requirement.dependency.id, requirement.parent, visited);
}

class solver final {
public:
  solver(const skill_registry_snapshot& registry, const skill_resolution_request& request)
      : registry_(registry), request_(request) {
  }

  bool run(std::vector<pending_requirement> pending, resolution_state& state) {
    if (pending.empty()) {
      return true;
    }
    std::sort(pending.begin(), pending.end(), pending_less);
    auto current = std::move(pending.front());
    pending.erase(pending.begin());

    if (current.path.size() >= request_.limits.max_depth) {
      return fail_or_skip(state,
        current,
        "resolution_limit_exceeded",
        "skill dependency depth exceeds the configured limit",
        std::move(pending));
    }
    if (std::find(current.path.begin(), current.path.end(), current.dependency.id) !=
        current.path.end()) {
      return fail_or_skip(state,
        current,
        "dependency_cycle",
        "skill dependency graph contains a cycle",
        std::move(pending));
    }
    if (would_create_cycle(state, current)) {
      return fail_or_skip(state,
        current,
        "dependency_cycle",
        "skill dependency graph contains a cycle",
        std::move(pending));
    }

    const auto selected = state.selected.find(current.dependency.id);
    if (selected != state.selected.end()) {
      if (!current.dependency.version.matches(selected->second->descriptor().version)) {
        return fail_or_skip(state,
          current,
          "version_conflict",
          "selected version " + selected->second->descriptor().version.string() +
            " does not satisfy " + current.dependency.version.expression(),
          std::move(pending));
      }
      connect(state, current);
      if (current.root) {
        state.roots.insert(current.dependency.id);
      }
      return run(std::move(pending), state);
    }

    auto candidates = registry_.versions(current.dependency.id);
    candidates.erase(
      std::remove_if(candidates.begin(),
        candidates.end(),
        [&](const auto& item) {
          const auto& version = item->descriptor().version;
          return (!version.stable() && !current.dependency.version.allows_prerelease()) ||
                 !current.dependency.version.matches(version);
        }),
      candidates.end());

    skill_diagnostic best_failure = diagnostic("dependency_not_found",
      "no registered version satisfies " + current.dependency.version.expression(),
      current);
    for (const auto& candidate : candidates) {
      if (++candidate_attempts_ > request_.limits.max_candidate_attempts) {
        best_failure = diagnostic("resolution_limit_exceeded",
          "skill candidate attempts exceed the configured limit",
          current);
        break;
      }
      if (state.selected.size() >= request_.limits.max_skills) {
        best_failure = diagnostic("resolution_limit_exceeded",
          "resolved skill count exceeds the configured limit",
          current);
        break;
      }
      const auto& dependencies = candidate->manifest().dependencies;
      if (dependencies.size() > request_.limits.max_dependencies_per_skill) {
        best_failure = diagnostic("resolution_limit_exceeded",
          "skill declares more dependencies than the configured limit",
          current);
        continue;
      }

      auto branch = state;
      branch.selected.emplace(current.dependency.id, candidate);
      connect(branch, current);
      if (current.root) {
        branch.roots.insert(current.dependency.id);
      }

      auto branch_pending = pending;
      auto sorted_dependencies = dependencies;
      std::sort(sorted_dependencies.begin(),
        sorted_dependencies.end(),
        [](const auto& lhs, const auto& rhs) {
          if (lhs.id != rhs.id) {
            return lhs.id < rhs.id;
          }
          if (lhs.optional != rhs.optional) {
            return !lhs.optional;
          }
          return lhs.version.expression() < rhs.version.expression();
        });
      auto dependency_path = current.path;
      dependency_path.push_back(current.dependency.id);
      for (const auto& dependency : sorted_dependencies) {
        branch_pending.push_back({ dependency, current.dependency.id, dependency_path, false });
      }
      if (run(std::move(branch_pending), branch)) {
        state = std::move(branch);
        return true;
      }
      if (!branch.diagnostics.empty()) {
        best_failure = branch.diagnostics.back();
      }
    }

    if (current.dependency.optional) {
      optional_warning(state, current, best_failure.message);
      return run(std::move(pending), state);
    }
    state.diagnostics.push_back(std::move(best_failure));
    return false;
  }

private:
  static void connect(resolution_state& state, const pending_requirement& requirement) {
    if (!requirement.parent.empty()) {
      state.edges[requirement.parent].insert(requirement.dependency.id);
    }
  }

  bool fail_or_skip(resolution_state& state, const pending_requirement& current, std::string code,
    std::string message, std::vector<pending_requirement> pending) {
    if (current.dependency.optional) {
      optional_warning(state, current, std::move(message));
      return run(std::move(pending), state);
    }
    state.diagnostics.push_back(diagnostic(std::move(code), std::move(message), current));
    return false;
  }

  const skill_registry_snapshot& registry_;
  const skill_resolution_request& request_;
  std::size_t candidate_attempts_ { 0 };
};

void append_topological(const std::string& id, const resolution_state& state,
  std::set<std::string>& visited, std::vector<resolved_skill>& output) {
  if (!visited.insert(id).second) {
    return;
  }
  const auto dependencies = state.edges.find(id);
  if (dependencies != state.edges.end()) {
    for (const auto& dependency : dependencies->second) {
      append_topological(dependency, state, visited, output);
    }
  }
  const auto package = state.selected.find(id);
  if (package != state.selected.end()) {
    output.push_back({ package->second, state.roots.contains(id) });
  }
}

} // namespace

skill_resolution_result skill_resolver::resolve(
  const skill_registry_snapshot& registry, const skill_resolution_request& request) const {
  skill_resolution_result output;
  if (request.roots.empty()) {
    output.diagnostics.push_back({ .severity = skill_diagnostic_severity::error,
      .code = "roots_required",
      .message = "at least one root skill requirement is required" });
    return output;
  }
  if (request.roots.size() > request.limits.max_roots || request.limits.max_skills == 0 ||
      request.limits.max_depth == 0 || request.limits.max_candidate_attempts == 0) {
    output.diagnostics.push_back({ .severity = skill_diagnostic_severity::error,
      .code = "resolution_limit_exceeded",
      .message = "skill resolution request exceeds or disables a configured safety limit" });
    return output;
  }

  std::vector<pending_requirement> pending;
  pending.reserve(request.roots.size());
  for (const auto& root : request.roots) {
    auto required_root = root;
    required_root.optional = false;
    pending.push_back({ std::move(required_root), {}, {}, true });
  }
  resolution_state state;
  solver implementation(registry, request);
  if (!implementation.run(std::move(pending), state)) {
    output.diagnostics = std::move(state.diagnostics);
    return output;
  }

  std::set<std::string> visited;
  for (const auto& root : state.roots) {
    append_topological(root, state, visited, output.skills);
  }
  for (const auto& [id, package] : state.selected) {
    (void)package;
    append_topological(id, state, visited, output.skills);
  }
  output.success = true;
  output.diagnostics = std::move(state.diagnostics);
  return output;
}

} // namespace wuwe::agent::skills
