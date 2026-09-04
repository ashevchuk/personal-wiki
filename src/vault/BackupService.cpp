#include "vault/BackupService.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <filesystem>

namespace wikicore::vault {

namespace {

constexpr size_t kReadChunkSize = 1 << 16;  // 64 KiB

}  // namespace

BackupResult createVaultBackup(const std::string& vaultPath) {
  BackupResult result;

  std::error_code ec;
  const std::filesystem::path absVault = std::filesystem::absolute(vaultPath, ec);
  if (ec || !std::filesystem::exists(absVault, ec)) {
    result.errorMessage = "vault path does not exist";
    return result;
  }
  const std::filesystem::path parentDir = absVault.parent_path();
  const std::string dirName = absVault.filename().string();

  int pipeFds[2];
  if (pipe(pipeFds) != 0) {
    result.errorMessage = "pipe() failed";
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipeFds[0]);
    close(pipeFds[1]);
    result.errorMessage = "fork() failed";
    return result;
  }

  if (pid == 0) {
    // Child: redirect stdout to the pipe's write end, then exec `tar`
    // with an explicit argv — never a shell, see this function's header
    // comment for why that matters here.
    close(pipeFds[0]);
    dup2(pipeFds[1], STDOUT_FILENO);
    close(pipeFds[1]);

    // Route tar's own stderr chatter (e.g. a permission-denied on one odd
    // file) to /dev/null so it doesn't interleave with the server's own
    // log stream. This is NOT the "swallow a helper's stderr and silently
    // read a crash as a negative result" trap — success/failure here is
    // decided below from the actual exit status, never inferred from
    // stdout alone, so a genuinely broken `tar` invocation still surfaces
    // as an error response rather than a truncated archive nobody
    // noticed.
    const int devNull = open("/dev/null", O_WRONLY);
    if (devNull >= 0) {
      dup2(devNull, STDERR_FILENO);
      close(devNull);
    }

    // --exclude=.uploads-tmp: Drogon's own multipart-upload staging buffer
    // (see main.cpp's setUploadPath comment) — 256 pre-created sharded
    // subdirectories, holds nothing but transient fragments of an upload
    // mid-flight, never a document. Pure noise in a backup, and GNU tar's
    // unqualified (no-slash) --exclude pattern matches it by basename
    // wherever it appears, not just at the top level.
    execlp("tar", "tar", "-czf", "-", "--exclude=.uploads-tmp", "-C", parentDir.c_str(),
           dirName.c_str(), static_cast<char*>(nullptr));
    _exit(127);  // only reached if execlp itself failed
  }

  // Parent: drain the child's stdout (the compressed archive) fully
  // before waiting on it — the pipe buffer is finite, so reading only
  // after waitpid() would deadlock on any archive bigger than that
  // buffer.
  close(pipeFds[1]);
  std::vector<char> buffer;
  std::array<char, kReadChunkSize> chunk{};
  ssize_t n = 0;
  while ((n = read(pipeFds[0], chunk.data(), chunk.size())) > 0) {
    buffer.insert(buffer.end(), chunk.data(), chunk.data() + n);
  }
  close(pipeFds[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (n < 0) {
    result.errorMessage = "failed reading tar output";
    return result;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    result.errorMessage = "tar exited with a non-zero status";
    return result;
  }

  result.success = true;
  result.archive = std::move(buffer);
  return result;
}

}  // namespace wikicore::vault
