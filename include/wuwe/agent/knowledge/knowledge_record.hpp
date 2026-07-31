#ifndef WUWE_AGENT_KNOWLEDGE_RECORD_HPP
#define WUWE_AGENT_KNOWLEDGE_RECORD_HPP

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/core/content.hpp>

namespace wuwe::agent::knowledge {

struct knowledge_document {
  std::string id;
  std::string title;
  std::string content;
  std::string source_uri;
  std::map<std::string, std::string> metadata;
};

struct knowledge_chunk {
  std::string id;
  std::string document_id;
  std::string title;
  std::string content;
  std::size_t start_offset {};
  std::size_t end_offset {};
  std::size_t start_line {};
  std::size_t end_line {};
  std::string source_uri;
  std::map<std::string, std::string> metadata;
};

enum class knowledge_acl_mode {
  permissive,
  public_only,
  tenant_required,
  user_required,
  deny_if_unlabeled,
};

[[nodiscard]] inline std::string to_string(knowledge_acl_mode mode) {
  switch (mode) {
    case knowledge_acl_mode::permissive:
      return "permissive";
    case knowledge_acl_mode::public_only:
      return "public_only";
    case knowledge_acl_mode::tenant_required:
      return "tenant_required";
    case knowledge_acl_mode::user_required:
      return "user_required";
    case knowledge_acl_mode::deny_if_unlabeled:
      return "deny_if_unlabeled";
  }
  return "deny_if_unlabeled";
}

struct knowledge_access_scope {
  std::string tenant_id;
  std::string user_id;
  std::vector<std::string> roles;
  bool bypass_acl {};
  knowledge_acl_mode mode { knowledge_acl_mode::permissive };
};

struct knowledge_query {
  std::string text;
  std::size_t limit { 6 };
  std::size_t candidate_limit {};
  std::map<std::string, std::string> filters;
  knowledge_access_scope access;
  double minimum_score { 0.0 };
  double vector_weight { 1.0 };
  double lexical_weight { 0.25 };
};

struct knowledge_result {
  knowledge_chunk chunk;
  double score {};
  double vector_score {};
  double lexical_score {};
};

inline bool metadata_matches(const std::map<std::string, std::string>& metadata,
  const std::map<std::string, std::string>& filters) {
  for (const auto& [key, value] : filters) {
    const auto it = metadata.find(key);
    if (it == metadata.end() || it->second != value) {
      return false;
    }
  }
  return true;
}

inline bool csv_contains(std::string_view csv, const std::string& value) {
  if (value.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start <= csv.size()) {
    const auto end = csv.find(',', start);
    auto token =
      csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start);
    while (!token.empty() && token.front() == ' ') {
      token.remove_prefix(1);
    }
    while (!token.empty() && token.back() == ' ') {
      token.remove_suffix(1);
    }
    if (!token.empty() && token == value) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

inline bool metadata_access_matches(
  const std::map<std::string, std::string>& metadata, const knowledge_access_scope& access) {
  if (access.bypass_acl) {
    return true;
  }

  const auto explicit_public = [&] {
    if (const auto visibility = metadata.find("visibility");
        visibility != metadata.end() && visibility->second == "public") {
      return true;
    }
    const auto public_flag = metadata.find("public");
    return public_flag != metadata.end() && public_flag->second == "true";
  }();
  const auto has_nonempty = [&](const char* key) {
    const auto found = metadata.find(key);
    return found != metadata.end() && !found->second.empty();
  };
  const auto has_tenant = has_nonempty("tenant_id");
  const auto has_user =
    has_nonempty("user_id") || has_nonempty("allowed_users") || has_nonempty("allowed_roles");
  const auto labeled = explicit_public || has_tenant || has_user;

  switch (access.mode) {
    case knowledge_acl_mode::permissive:
      break;
    case knowledge_acl_mode::public_only:
      if (!explicit_public)
        return false;
      break;
    case knowledge_acl_mode::tenant_required:
      if (!explicit_public && !has_tenant)
        return false;
      break;
    case knowledge_acl_mode::user_required:
      if (!has_user)
        return false;
      break;
    case knowledge_acl_mode::deny_if_unlabeled:
      if (!labeled)
        return false;
      break;
  }

  if (const auto tenant = metadata.find("tenant_id");
      tenant != metadata.end() && tenant->second != access.tenant_id) {
    return false;
  }
  if (const auto user = metadata.find("user_id");
      user != metadata.end() && !user->second.empty() && user->second != access.user_id) {
    return false;
  }
  if (const auto users = metadata.find("allowed_users");
      users != metadata.end() && !users->second.empty() &&
      !csv_contains(users->second, access.user_id)) {
    return false;
  }
  if (const auto roles = metadata.find("allowed_roles");
      roles != metadata.end() && !roles->second.empty()) {
    for (const auto& role : access.roles) {
      if (csv_contains(roles->second, role)) {
        return true;
      }
    }
    return false;
  }
  return true;
}

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_RECORD_HPP
