/*
 * max_rss_stubs.c — sample the resident set size of a running process.
 *
 * IMPORTANT: on Linux this deliberately does NOT report VmHWM.  olly traces
 * through a runtime_events ring, which is a file-backed mmap ("<pid>.events")
 * that the kernel counts in the traced process's RSS.  The ring is sized by
 * OCAMLRUNPARAM `e` (log2 words per domain) times `d` (max domains), so it is
 * routinely hundreds of MB — with e=25,d=2 it is 512 MB.  Reporting VmHWM
 * therefore attributed olly's own instrumentation to the program under
 * measurement: a benchmark with a 500 MB footprint measured ~1 GB, a 100%
 * overstatement, and one that grew with the ring size rather than with anything
 * the program did.
 *
 * Instead we sum the per-mapping Rss from /proc/<pid>/smaps, skipping the
 * ".events" ring.  That is the program's own resident footprint.  The trade-off
 * is deliberate: VmHWM is an exact kernel-maintained high-water mark, whereas
 * smaps gives the *current* breakdown, so the caller (Rss_poller) turns it into
 * a peak by sampling.  A peak that is sampled but measures the right thing beats
 * an exact peak that measures the wrong one; raise the sampling rate
 * (--rss-freq) if a short-lived spike matters.
 *
 * macOS and FreeBSD keep their existing whole-process readings — neither
 * exposes a cheap per-mapping RSS breakdown, so the ring cannot be excluded
 * there.  Those platforms are unaffected by this change (and olly's tracing is
 * Linux-centric in practice).
 *
 * On Linux these are all in the same /proc/<pid>/status text file, so
 * collecting them requires no extra syscalls — just scanning more lines
 * in the same read pass (pure OCaml file I/O would also work).
 *
 * On macOS, struct proc_taskinfo already has pti_virtual_size alongside
 * pti_resident_size, but no stack/data breakdown — that would need
 * task_info() with TASK_VM_INFO.
 *
 * On FreeBSD, struct kinfo_proc has ki_rssize (RSS) and ki_size (total
 * VM) but not a heap/stack split.
 *
 * Currently we only extract VmHWM (peak resident set size) from
 * /proc/<pid>/status on Linux.  The same file exposes additional fields
 * that would be valuable for GC sweep / compiler-comparison benchmarks:
 *
 *   Field     What it measures                   Useful for
 *   -------   --------------------------------   ----------------------------------
 *   VmRSS     Current RSS at sample time         Memory trajectory over time
 *   VmData    Heap + anonymous mappings           Directly reflects GC heap sizing;
 *                                                 changes with minor-heap size (s)
 *                                                 and space overhead (o) parameters
 *   VmStk     Stack size                          Stack-heavy benchmarks: deep
 *                                                 recursion, effects/continuations
 *                                                 (multicore-effects suite), and
 *                                                 comparing stack segment handling
 *                                                 across compiler versions
 *   VmPeak    Peak virtual address space           Total address space pressure
 *                                                 including mmap'd regions and the
 *                                                 runtime events ring buffer
 *   VmSize    Current virtual address space       Same as VmPeak but instantaneous
 *   Threads   Thread count                        Sanity check for multicore
 *                                                 benchmarks (confirms domain count)
 *
 */

#include <caml/mlvalues.h>

#if defined(__linux__)

#include <stdio.h>
#include <string.h>

/* Sum Rss across all mappings except the runtime_events ring.
 *
 * smaps alternates a mapping header with that mapping's fields:
 *
 *   7f3c1a000000-7f3c1a400000 rw-s 00000000 00:19 12345  /tmp/…/4242.events
 *   Rss:                4096 kB
 *   …
 *
 * A header's first whitespace-delimited token is an address range, which does
 * not end in ':'; a field line's does ("Rss:").  That distinction is what tells
 * the two apart, so we track whether the mapping we are inside is the ring and
 * skip its Rss.  Reading smaps is more expensive than status (the kernel walks
 * the page tables), which is why this is sampled rather than read per event.
 */
CAMLprim value olly_get_rss_kb(value v_pid) {
  int pid = Int_val(v_pid);
  char path[64];
  char line[4096];
  long total = 0;
  int in_events_ring = 0;
  FILE *f;

  snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
  f = fopen(path, "r");
  if (!f)
    return Val_long(0);

  while (fgets(line, sizeof(line), f)) {
    char *sp = strchr(line, ' ');
    if (sp && sp > line && *(sp - 1) != ':') {
      /* mapping header: decide whether to count this mapping at all */
      in_events_ring = (strstr(line, ".events") != NULL);
    } else if (!in_events_ring && strncmp(line, "Rss:", 4) == 0) {
      long rss = 0;
      sscanf(line + 4, " %ld", &rss);
      total += rss;
    }
  }
  fclose(f);
  return Val_long(total);
}

#elif defined(__APPLE__)

#include <libproc.h>
#include <sys/proc_info.h>

CAMLprim value olly_get_rss_kb(value v_pid) {
  int pid = Int_val(v_pid);
  struct proc_taskinfo ti;
  int ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti));
  if (ret <= 0)
    return Val_long(0);
  return Val_long(ti.pti_resident_size / 1024);
}

#elif defined(__FreeBSD__)

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

CAMLprim value olly_get_rss_kb(value v_pid) {
  int pid = Int_val(v_pid);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  struct kinfo_proc kp;
  size_t len = sizeof(kp);
  if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0)
    return Val_long(0);
  return Val_long((long)kp.ki_rssize * getpagesize() / 1024);
}

#else

CAMLprim value olly_get_rss_kb(value v_pid) {
  (void)v_pid;
  return Val_long(0);
}

#endif
