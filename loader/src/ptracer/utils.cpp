#include <linux/limits.h>

#include <vector>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <array>
#include <cinttypes>
#include <sys/ptrace.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/auxv.h>
#include <elf.h>
#include <link.h>
#include <vector>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>
#include <signal.h>
#include <sstream>
#include <ios>
#include <cstring>
#include <sys/stat.h>

#include <sched.h>
#include <fcntl.h>

#include "utils.hpp"
#include "logging.h"

bool switch_mnt_ns(int pid, int *fd) {
  int nsfd, old_nsfd = -1;

  /* WARNING: C++ keyword */
  char path[PATH_MAX];
  if (pid == 0) {
    if (fd != NULL) {
      nsfd = *fd;
      *fd = -1;
    } else return false;

    snprintf(path, sizeof(path), "/proc/self/fd/%d", nsfd);
  } else {
    if (fd != NULL) {
      old_nsfd = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
      if (old_nsfd == -1) {
        PLOGE("get old nsfd");

        return false;
      }

      *fd = old_nsfd;
    }

    snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid);

    nsfd = open(path, O_RDONLY | O_CLOEXEC);
    if (nsfd == -1) {
      PLOGE("open nsfd %s", path);

      close(old_nsfd);

      return false;
    }
  }

  if (setns(nsfd, CLONE_NEWNS) == -1) {
    PLOGE("set ns to %s", path);

    close(nsfd);
    close(old_nsfd);

    return false;
  }

  close(nsfd);

  return true;
}

/* WARNING: C++ keyword */
std::vector<MapInfo> MapInfo::Scan(const std::string &pid) {
  constexpr static auto kPermLength = 5;
  constexpr static auto kMapEntry = 7;

  /* WARNING: C++ keyword */
  std::vector<MapInfo> info;
  char file_name[NAME_MAX];
  snprintf(file_name, sizeof(file_name), "/proc/%s/maps", pid.c_str());

  /* WARNING: C++ keyword */
  auto maps = std::unique_ptr<FILE, decltype(&fclose)>{fopen(file_name, "r"), &fclose};
  if (maps) {
    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    /* WARNING: C++ keyword */
    while ((read = getline(&line, &len, maps.get())) > 0) {
      line[read - 1] = '\0';

      uintptr_t start = 0;
      uintptr_t end = 0;
      uintptr_t off = 0;
      ino_t inode = 0;
      unsigned int dev_major = 0;
      unsigned int dev_minor = 0;

      /* WARNING: C++ keyword */
      std::array<char, kPermLength> perm {'\0'};

      int path_off;
      if (sscanf(line, "%" PRIxPTR "-%" PRIxPTR " %4s %" PRIxPTR " %x:%x %lu %n%*s", &start,
              &end, perm.data(), &off, &dev_major, &dev_minor, &inode,
              &path_off) != kMapEntry) continue;

      while (path_off < read && isspace(line[path_off])) path_off++;

      /* WARNING: C++ keyword */
      MapInfo &ref = info.emplace_back(MapInfo{
        start,
        end,
        0,
        perm[3] == 'p',
        off,
        static_cast<dev_t>(makedev(dev_major, dev_minor)),
        inode,
        line + path_off
      });

      if (perm[0] == 'r') ref.perms |= PROT_READ;
      if (perm[1] == 'w') ref.perms |= PROT_WRITE;
      if (perm[2] == 'x') ref.perms |= PROT_EXEC;
    }

    free(line);
  }

  return info;
}

ssize_t write_proc(int pid, uintptr_t remote_addr, const void *buf, size_t len) {
  LOGV("write to remote addr %" PRIxPTR " size %zu", remote_addr, len);

  struct iovec local = {
    .iov_base = (void *)buf,
    .iov_len = len
  };

  struct iovec remote = {
    .iov_base = (void *)remote_addr,
    .iov_len = len
  };

  ssize_t l = process_vm_writev(pid, &local, 1, &remote, 1, 0);
  if (l == -1) PLOGE("process_vm_writev");
  else if ((size_t)l != len) LOGW("not fully written: %zu, excepted %zu", l, len);

  return l;
}

ssize_t read_proc(int pid, uintptr_t remote_addr, void *buf, size_t len) {
  struct iovec local = {
    .iov_base = (void *)buf,
    .iov_len = len
  };

  struct iovec remote = {
    .iov_base = (void *)remote_addr,
    .iov_len = len
  };

  ssize_t l = process_vm_readv(pid, &local, 1, &remote, 1, 0);
  if (l == -1) PLOGE("process_vm_readv");
  else if ((size_t)l != len) LOGW("not fully read: %zu, excepted %zu", l, len);

  return l;
}

bool get_regs(int pid, struct user_regs_struct &regs) {
  #if defined(__x86_64__) || defined(__i386__)
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == -1) {
      PLOGE("getregs");

      return false;
    }
  #elif defined(__aarch64__) || defined(__arm__)
    struct iovec iov = {
      .iov_base = &regs,
      .iov_len = sizeof(struct user_regs_struct),
    };

    if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) == -1) {
      PLOGE("getregs");

      return false;
    }
  #endif

  return true;
}

bool set_regs(int pid, struct user_regs_struct &regs) {
  #if defined(__x86_64__) || defined(__i386__)
    if (ptrace(PTRACE_SETREGS, pid, 0, &regs) == -1) {
      PLOGE("setregs");

      return false;
    }
  #elif defined(__aarch64__) || defined(__arm__)
    struct iovec iov = {
      .iov_base = &regs,
      .iov_len = sizeof(struct user_regs_struct),
    };

    if (ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov) == -1) {
      PLOGE("setregs");

      return false;
    }
  #endif

  return true;
}

/* WARNING: C++ keyword */
std::string get_addr_mem_region(std::vector<MapInfo> &info, uintptr_t addr) {
  /* WARNING: C++ keyword */
  for (auto &map: info) {
    if (map.start <= addr && map.end > addr) {
      /* WARNING: C++ keyword */
      auto s = std::string(map.path);

      s += ' ';
      s += map.perms & PROT_READ ? 'r' : '-';
      s += map.perms & PROT_WRITE ? 'w' : '-';
      s += map.perms & PROT_EXEC ? 'x' : '-';

      return s;
    }
  }

  return "<unknown>";
}

/* WARNING: C++ keyword */
void *find_module_return_addr(std::vector<MapInfo> &info, std::string_view suffix) {
  /* WARNING: C++ keyword */
  for (auto &map: info) {
    /* WARNING: C++ keyword */
    if ((map.perms & PROT_EXEC) == 0 && map.path.ends_with(suffix)) return (void *)map.start;
  }

  return NULL;
}

/* WARNING: C++ keyword */
void *find_module_base(std::vector<MapInfo> &info, std::string_view suffix) {
  /* WARNING: C++ keyword */
  for (auto &map: info) {
    /* WARNING: C++ keyword */
    if (map.offset == 0 && map.path.ends_with(suffix)) return (void *)map.start;
  }

  return NULL;
}

/* WARNING: C++ keyword */
void *find_func_addr(std::vector<MapInfo> &local_info, std::vector<MapInfo> &remote_info, std::string_view module, std::string_view func) {
  void *lib = dlopen(module.data(), RTLD_NOW);
  if (lib == NULL) {
    LOGE("failed to open lib %s: %s", module.data(), dlerror());

    return NULL;
  }

  uint8_t *sym = (uint8_t *)dlsym(lib, func.data());
  if (sym == NULL) {
    LOGE("failed to find sym %s in %s: %s", func.data(), module.data(), dlerror());

    dlclose(lib);

    return NULL;
  }

  LOGD("sym %s: %p", func.data(), sym);

  dlclose(lib);

  uint8_t *local_base = (uint8_t *)find_module_base(local_info, module);
  if (local_base == NULL) {
    LOGE("failed to find local base for module %s", module.data());

    return NULL;
  }

  uint8_t *remote_base = (uint8_t *)find_module_base(remote_info, module);
  if (remote_base == NULL) {
    LOGE("failed to find remote base for module %s", module.data());

    return NULL;
  }

  LOGD("found local base %p remote base %p", local_base, remote_base);

  uint8_t *addr = (sym - local_base) + remote_base;
  LOGD("addr %p", addr);

  return addr;
}

/* WARNING: C++ keyword */
void align_stack(struct user_regs_struct &regs, long preserve) {
  /* INFO: ~0xf is a negative value, and REG_SP is unsigned,
            so we must cast REG_SP to signed type before subtracting
            then cast back to unsigned type.
  */
  regs.REG_SP = (uintptr_t)((intptr_t)(regs.REG_SP - preserve) & ~0xf);
}

/* WARNING: C++ keyword */
uintptr_t push_string(int pid, struct user_regs_struct &regs, const char *str) {
  size_t len = strlen(str) + 1;

  regs.REG_SP -= len;

  align_stack(regs);

  uintptr_t addr = (uintptr_t)regs.REG_SP;
  if (!write_proc(pid, addr, str, len)) LOGE("failed to write string %s", str);

  LOGD("pushed string %" PRIxPTR, addr);

  return addr;
}

/* WARNING: C++ keyword */
uintptr_t remote_call(int pid, struct user_regs_struct &regs, uintptr_t func_addr, uintptr_t return_addr, std::vector<long> &args) {
  align_stack(regs);

  /* WARNING: C++ keyword */
  LOGV("calling remote function %" PRIxPTR " args %zu", func_addr, args.size());

  /* WARNING: C++ keyword */
  for (auto &a: args) {
    LOGV("arg %p", (void *) a);
  }

  #if defined(__x86_64__)
    if (args.size() >= 1) regs.rdi = args[0];
    if (args.size() >= 2) regs.rsi = args[1];
    if (args.size() >= 3) regs.rdx = args[2];
    if (args.size() >= 4) regs.rcx = args[3];
    if (args.size() >= 5) regs.r8 = args[4];
    if (args.size() >= 6) regs.r9 = args[5];
    if (args.size() > 6) {
      long remain = (args.size() - 6L) * sizeof(long);
      align_stack(regs, remain);

      if (!write_proc(pid, (uintptr_t) regs.REG_SP, args.data(), remain)) LOGE("failed to push arguments");
    }

    regs.REG_SP -= sizeof(long);

    if (!write_proc(pid, (uintptr_t) regs.REG_SP, &return_addr, sizeof(return_addr))) LOGE("failed to write return addr");

    regs.REG_IP = func_addr;
  #elif defined(__i386__)
    if (args.size() > 0) {
      long remain = (args.size()) * sizeof(long);
      align_stack(regs, remain);

      if (!write_proc(pid, (uintptr_t) regs.REG_SP, args.data(), remain)) LOGE("failed to push arguments");
    }

    regs.REG_SP -= sizeof(long);

    if (!write_proc(pid, (uintptr_t) regs.REG_SP, &return_addr, sizeof(return_addr))) LOGE("failed to write return addr");

    regs.REG_IP = func_addr;
  #elif defined(__aarch64__)
    for (size_t i = 0; i < args.size() && i < 8; i++) {
      regs.regs[i] = args[i];
    }

    if (args.size() > 8) {
      long remain = (args.size() - 8) * sizeof(long);
      align_stack(regs, remain);

      write_proc(pid, (uintptr_t)regs.REG_SP, args.data(), remain);
    }

    regs.regs[30] = return_addr;
    regs.REG_IP = func_addr;
  #elif defined(__arm__)
    for (size_t i = 0; i < args.size() && i < 4; i++) {
      regs.uregs[i] = args[i];
    }

    if (args.size() > 4) {
      long remain = (args.size() - 4) * sizeof(long);
      align_stack(regs, remain);

      write_proc(pid, (uintptr_t)regs.REG_SP, args.data(), remain);
    }

    regs.uregs[14] = return_addr;
    regs.REG_IP = func_addr;

    constexpr auto CPSR_T_MASK = 1lu << 5;

    if ((regs.REG_IP & 1) != 0) {
      regs.REG_IP = regs.REG_IP & ~1;
      regs.uregs[16] = regs.uregs[16] | CPSR_T_MASK;
    } else {
      regs.uregs[16] = regs.uregs[16] & ~CPSR_T_MASK;
    }
  #endif

  if (!set_regs(pid, regs)) {
    LOGE("failed to set regs");

    return 0;
  }

  ptrace(PTRACE_CONT, pid, 0, 0);

  int status;
  wait_for_trace(pid, &status, __WALL);
  if (!get_regs(pid, regs)) {
    LOGE("failed to get regs after call");

    return 0;
  }

  if (WSTOPSIG(status) == SIGSEGV) {
    if ((uintptr_t)regs.REG_IP != return_addr) {
      LOGE("wrong return addr %p", (void *) regs.REG_IP);

      return 0;
    }

    return regs.REG_RET;
  } else {
    char status_str[64];
    parse_status(status, status_str, sizeof(status_str));

    LOGE("stopped by other reason %s at addr %p", status_str, (void *)regs.REG_IP);
  }

  return 0;
}

int fork_dont_care() {
  pid_t pid = fork();

  if (pid < 0) PLOGE("fork 1");
  else if (pid == 0) {
    pid = fork();
    if (pid < 0) PLOGE("fork 2");
    else if (pid > 0) exit(0);
  } else {
    int status;
    waitpid(pid, &status, __WALL);
  }

  return pid;
}

void wait_for_trace(int pid, int *status, int flags) {
  while (1) {
    pid_t result = waitpid(pid, status, flags);
    if (result == -1) {
      if (errno == EINTR) continue;

      PLOGE("wait %d failed", pid);
      exit(1);
    }

    if (!WIFSTOPPED(*status)) {
      char status_str[64];
      parse_status(*status, status_str, sizeof(status_str));

      LOGE("process %d not stopped for trace: %s, exit", pid, status_str);

      exit(1);
    }

    return;
  }
}

void parse_status(int status, char *buf, size_t len) {
  snprintf(buf, len, "0x%x ", status);

  if (WIFEXITED(status)) {
    snprintf(buf + strlen(buf), len - strlen(buf), "exited with %d", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    snprintf(buf + strlen(buf), len - strlen(buf), "signaled with %s(%d)", sigabbrev_np(WTERMSIG(status)), WTERMSIG(status));
  } else if (WIFSTOPPED(status)) {
    snprintf(buf + strlen(buf), len - strlen(buf), "stopped by ");

    int stop_sig = WSTOPSIG(status);
    snprintf(buf + strlen(buf), len - strlen(buf), "signal=%s(%d),", sigabbrev_np(stop_sig), stop_sig);
    snprintf(buf + strlen(buf), len - strlen(buf), "event=%s", parse_ptrace_event(status));
  } else {
    snprintf(buf + strlen(buf), len - strlen(buf), "unknown");
  }
}

int get_program(int pid, char *buf, size_t size) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/proc/%d/exe", pid);

  ssize_t sz = readlink(path, buf, size);

  if (sz == -1) {
    PLOGE("readlink /proc/%d/exe", pid);

    return -1;
  }

  buf[sz] = 0;

  return 0;
}
