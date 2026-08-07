#include <wuwe/agent/filesystem/filesystem_core.hpp>

namespace wuwe::agent::filesystem {

std::string to_string(filesystem_status status) {
  switch (status) {
    case filesystem_status::ok:
      return "ok";
    case filesystem_status::not_found:
      return "not_found";
    case filesystem_status::already_exists:
      return "already_exists";
    case filesystem_status::invalid_path:
      return "invalid_path";
    case filesystem_status::invalid_request:
      return "invalid_request";
    case filesystem_status::outside_root:
      return "outside_root";
    case filesystem_status::permission_denied:
      return "permission_denied";
    case filesystem_status::approval_denied:
      return "approval_denied";
    case filesystem_status::type_mismatch:
      return "type_mismatch";
    case filesystem_status::conflict:
      return "conflict";
    case filesystem_status::limit_exceeded:
      return "limit_exceeded";
    case filesystem_status::cancelled:
      return "cancelled";
    case filesystem_status::io_error:
      return "io_error";
  }
  return "io_error";
}

std::string to_string(filesystem_entry_type type) {
  switch (type) {
    case filesystem_entry_type::regular_file:
      return "regular_file";
    case filesystem_entry_type::directory:
      return "directory";
    case filesystem_entry_type::symlink:
      return "symlink";
    case filesystem_entry_type::other:
      return "other";
  }
  return "other";
}

std::string to_string(write_disposition disposition) {
  switch (disposition) {
    case write_disposition::create_new:
      return "create_new";
    case write_disposition::overwrite:
      return "overwrite";
  }
  return "overwrite";
}

} // namespace wuwe::agent::filesystem
