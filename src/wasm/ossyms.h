// Upstream's runtime symbols behind stdlib/os.goose (runtime_os.h), which a
// program may also bind its own extern fns to, as upstream's C runtime is
// always linked: each with the os function lib/os.goose implements it as,
// and a call of that function, which the driver puts in a thread_fn of its
// own so that the checker specializes it (src/driver.cpp, OsKeepAlive).
#pragma once

namespace goosewasm {

struct OsSymbol {
    const char *sym, *fn, *call;
};

inline const OsSymbol os_symbols[] = {
    { "gs_os_read_file", "read_file", "read_file(\"\", b);" },
    { "gs_os_write_file", "write_file", "write_file(\"\", \"\");" },
    { "gs_os_write_file_atomic", "write_file_atomic", "write_file_atomic(\"\", \"\");" },
    { "gs_os_append_file", "append_file", "append_file(\"\", \"\");" },
    { "gs_os_file_exists", "file_exists", "file_exists(\"\");" },
    { "gs_os_remove_file", "delete_file", "delete_file(\"\");" },
    { "gs_os_rename_file", "rename_file", "rename_file(\"\", \"\");" },
    { "gs_os_make_dir", "make_dir", "make_dir(\"\");" },
    { "gs_os_is_dir", "is_dir", "is_dir(\"\");" },
    { "gs_os_remove_dir", "delete_dir", "delete_dir(\"\");" },
    { "gs_os_list_dir", "list_dir", "list_dir(\"\", b);" },
    { "gs_os_read_line", "read_line", "read_line(b);" },
    { "gs_os_read_stdin", "read_stdin", "read_stdin(b);" },
    { "gs_os_write_stdout", "write_stdout", "write_stdout(\"\");" },
    { "gs_os_write_stderr", "write_stderr", "write_stderr(\"\");" },
    { "gs_os_flush_stdout", "flush_stdout", "flush_stdout();" },
    { "gs_os_arg_count", "arg_count", "arg_count();" },
    { "gs_os_arg", "arg", "arg(0, b);" },
    { "gs_os_getenv", "env", "env(\"\", b);" },
    { "gs_os_resource_dir", "resource_dir", "resource_dir(b);" },
    { "gs_os_time_ns", "time_ns", "time_ns();" },
    { "gs_os_clock_ns", "clock_ns", "clock_ns();" },
    { "gs_os_sleep_ms", "sleep_ms", "sleep_ms(0);" },
    { "gs_os_random_u64", "random_seed", "random_seed();" },
};

}  // namespace goosewasm
