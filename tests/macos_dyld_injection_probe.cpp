#ifdef __APPLE__
#include <cstdio>
#include <cstdlib>

__attribute__((constructor)) static void attempt_unsandboxed_write() {
  const auto* marker = std::getenv("WUWE_DYLD_MARKER");
  if (!marker) return;
  if (auto* output = std::fopen(marker, "wb")) {
    std::fputs("injected", output);
    std::fclose(output);
  }
}
#endif
