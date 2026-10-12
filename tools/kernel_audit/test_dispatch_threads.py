"""Run the real kernel lookup/dispatch bodies under a forced thread interleave.

Run on Windows with CMake and MSVC installed:
    python tools/kernel_audit/test_dispatch_threads.py

The test extracts the actual functions, substitutes two trivial bridge targets,
and forces A:lookup(0), B:lookup(1), A:call, B:call. It checks both target selection
and guest stack cleanup. A control build with the previous shared slot must fail.
No game executable, files, or running processes are needed.
"""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r"""
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#define RECOMP_TLS __declspec(thread)
#define XBOX_KERNEL_THUNK_TABLE_SIZE 2
#define KERNEL_VA_BASE 0xFE000000u
#define KERNEL_VA_END (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)
typedef void (*bridge_func_t)(void);
typedef void (*recomp_func_t)(void);
static RECOMP_TLS uint32_t g_eax, g_esp;
static bridge_func_t g_slot_bridges[2];
static ULONG g_slot_ordinals[2] = { 100, 200 };
static int g_slot_arg_bytes[2] = { 4, 12 };
static int g_kernel_call_count;
static uint32_t g_kernel_watch_va;
static uint32_t guest_stack[256];
#define BRIDGE_MEM32(va) guest_stack[(va) / 4]
"""

HARNESS = r"""
static HANDLE a_ready, b_ready, a_done;
static int a_error, b_error;
static void bridge_a(void) { g_eax = 111; }
static void bridge_b(void) { g_eax = 222; }
static DWORD WINAPI run_a(void *unused) {
    recomp_func_t fn;
    (void)unused;
    g_esp = 128;
    fn = recomp_lookup_kernel(KERNEL_VA_BASE);
    SetEvent(a_ready);
    if (WaitForSingleObject(b_ready, 5000) != WAIT_OBJECT_0) {
        a_error = 2; SetEvent(a_done); return 0;
    }
    fn();
    a_error = g_eax != 111 || g_esp != 136;
    SetEvent(a_done);
    return 0;
}
static DWORD WINAPI run_b(void *unused) {
    recomp_func_t fn;
    (void)unused;
    g_esp = 128;
    if (WaitForSingleObject(a_ready, 5000) != WAIT_OBJECT_0) {
        b_error = 2; SetEvent(b_ready); return 0;
    }
    fn = recomp_lookup_kernel(KERNEL_VA_BASE + 4);
    SetEvent(b_ready);
    if (WaitForSingleObject(a_done, 5000) != WAIT_OBJECT_0) {
        b_error = 2; return 0;
    }
    fn();
    b_error = g_eax != 222 || g_esp != 144;
    return 0;
}
int main(void) {
    HANDLE threads[2];
    g_slot_bridges[0] = bridge_a;
    g_slot_bridges[1] = bridge_b;
    a_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    b_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    a_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!a_ready || !b_ready || !a_done) return 3;
    threads[0] = CreateThread(NULL, 0, run_a, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, run_b, NULL, 0, NULL);
    if (!threads[0] || !threads[1] ||
        WaitForMultipleObjects(2, threads, TRUE, 10000) != WAIT_OBJECT_0) return 3;
    CloseHandle(threads[0]); CloseHandle(threads[1]);
    CloseHandle(a_ready); CloseHandle(b_ready); CloseHandle(a_done);
    printf("dispatch result: thread A=%d, thread B=%d\n", a_error, b_error);
    return a_error || b_error;
}
"""


def extract_function(source, signature):
    match = re.search(re.escape(signature) + r"\s*\{.*?^\}", source, re.S | re.M)
    assert match, f"missing function: {signature}"
    return match.group(0)


def main():
    source = (ROOT / "src/kernel/kernel_bridge.c").read_text(encoding="utf-8")
    slot = re.search(r"^static (?:RECOMP_TLS )?int g_kernel_dispatch_slot = -1;", source, re.M)
    assert slot, "missing dispatch slot declaration"
    body = "\n".join([
        extract_function(source, "static void kernel_thunk_dispatch(void)"),
        extract_function(source, "recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)"),
    ])
    with tempfile.TemporaryDirectory(prefix="kernel-dispatch-") as directory:
        root = Path(directory)
        (root / "fixed.c").write_text(PRELUDE + slot.group(0) + body + HARNESS, encoding="utf-8")
        shared = slot.group(0).replace("RECOMP_TLS ", "")
        (root / "shared.c").write_text(PRELUDE + shared + body + HARNESS, encoding="utf-8")
        (root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(dispatch_test C)\n"
            "add_executable(fixed fixed.c)\nadd_executable(shared shared.c)\n",
            encoding="utf-8",
        )
        subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"), "-A", "x64"], check=True)
        subprocess.run(["cmake", "--build", str(root / "build"), "--config", "Release"], check=True)
        for target, expected in (("fixed", 0), ("shared", 1)):
            result = subprocess.run([str(root / "build/Release" / (target + ".exe"))], capture_output=True, text=True)
            assert result.returncode == expected, (target, result.returncode, result.stdout, result.stderr)
            print(f"ok {target}: {result.stdout.strip()}")


if __name__ == "__main__":
    main()
