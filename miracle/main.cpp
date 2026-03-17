// =============================================================================
//  Miracle Build System  —  single translation unit
//  Requires: kaguya, Executer.hpp, process.hpp, Path (from host framework)
//
//  Bug fixes vs original:
//   [B1]  Busy-spin replaced with timed sleep in all process-wait loops
//   [B2]  Target::Name / OutputFileName: raw char[512]+strcpy → std::string
//   [B3]  ar command corrected: "rcs archive.a obj…" (was "-r -s archive obj…")
//   [B4]  AddSourceFilter OR-semantics fixed (was AND — broke on first mismatch)
//   [B5]  GetFileIncludes: O(n²) std::find → O(1) unordered_set
//   [B6]  Parallel jobs capped at hardware_concurrency (was unbounded async)
//   [B7]  --output-file argv skip corrected (key + value, not just key)
//   [B8]  g_log_file closed before exit
//   [B9]  obj filename uses full canonical source path (no inter-tree collisions)
//   [B10] SHARED_LIBRARY: link_search_path + linker_commands now forwarded
//   [B11] ProcessConsoleOutput: shared_ptr<string> → plain std::string
//   [B12] Target::Log fold-expression comma-operator bug fixed
//   [B13] MiracleExecuter singleton guarded with std::once_flag
//   [B14] IntermediateDir reset at start of every Build() call
//   [B15] GCC toolchain uses static storage (no leak); SharedLibExt → ".so"
//   [B16] AddFilesMatchsRegex deduplicates across all search roots
//   [B17] Shared-library compile adds -fPIC
//   [B18] Output path quoted in linker command (space-safe)
//   [B19] InvokeCommand: per-call local mutex prevents concurrent buffer races
//   [B20] LoadFile checks ifstream::is_open() before reading
//
//  New features:
//   [F1]  --jobs N  controls parallel compilation (default: hw concurrency)
//   [F2]  Build summary: files compiled / skipped / total wall-clock time
//   [F3]  ANSI colour: errors red, warnings yellow, success green
//          (auto-disabled when stdout is not a TTY, or via --no-color)
//   [F4]  --clean  deletes obj files before building
//   [F5]  Graceful fail-fast: first compile error cancels remaining batches
//   [F6]  Progress counter "[N/M] compiling …" per compile job
//   [F7]  --help  usage text
//   [F8]  Target::Clean() exposed to Lua
// =============================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <unordered_set>
#include <regex>
#include <cassert>
#include <cstring>
#include <cstdlib>

#include <kaguya/kaguya.hpp>
#include <Executer.hpp>
#include <process.hpp>

// ─────────────────────────────────────────────────────────────────────────────
//  Platform detection
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
#  define MBS_WINDOWS 1
#  include <windows.h>
#  include <io.h>
#  define MBS_IS_TTY(fd) (_isatty(fd))
#else
#  define MBS_WINDOWS 0
#  include <unistd.h>
#  define MBS_IS_TTY(fd) (isatty(fd))
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  lua_return_str  —  thread-local staging buffer for Lua <-> C++ const char*
//  RULE: caller must copy/use the return value before the next call on the
//  same thread, because the buffer is overwritten on every invocation.
// ─────────────────────────────────────────────────────────────────────────────
static const char* lua_return_str(const char* s)
{
    thread_local std::string buf;
    buf = s ? s : "";
    return buf.c_str();
}
static const char* lua_return_str(const std::string& s)
{
    return lua_return_str(s.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  ANSI colour helpers  [F3]
// ─────────────────────────────────────────────────────────────────────────────
namespace color
{
    static bool enabled = false;   // set in main() after TTY probe

    static const char* RST = "\033[0m";
    static const char* BLD = "\033[1m";
    static const char* DIM = "\033[2m";
    static const char* RED = "\033[31m";
    static const char* YLW = "\033[33m";
    static const char* GRN = "\033[32m";
    static const char* CYN = "\033[36m";
    static const char* MAG = "\033[35m";

    inline std::string wrap(const char* code, const std::string& s)
    {
        if (!enabled) return s;
        return std::string(code) + s + RST;
    }
    inline std::string ok   (const std::string& s) { return wrap(GRN, s); }
    inline std::string warn (const std::string& s) { return wrap(YLW, s); }
    inline std::string err  (const std::string& s) { return wrap(RED, s); }
    inline std::string info (const std::string& s) { return wrap(CYN, s); }
    inline std::string dim  (const std::string& s) { return wrap(DIM, s); }
    inline std::string bold (const std::string& s) { return wrap(BLD, s); }
    inline std::string step (const std::string& s) { return wrap(MAG, s); }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Global output state
// ─────────────────────────────────────────────────────────────────────────────
static bool          g_silent = false;
static std::mutex    g_out_mtx;
static std::ofstream g_log_file;
static unsigned      g_jobs   = 0;     // 0 = hardware_concurrency()
static bool          g_clean  = false;

// Classify a single output line for colouring.
static bool looks_like_error  (const std::string& s)
{
    return s.find(" error:")   != std::string::npos ||
           s.find(" fatal:")   != std::string::npos ||
           s.find("undefined") != std::string::npos;
}
static bool looks_like_warning(const std::string& s)
{
    return s.find(" warning:") != std::string::npos ||
           s.find(" note:")    != std::string::npos;
}

// Emit one line to stdout (with colour) and to the log file (plain).
static void emit_line(const std::string& line, bool to_stderr = false)
{
    std::string coloured = line;
    if (color::enabled)
    {
        if      (looks_like_error  (line)) coloured = color::err (line);
        else if (looks_like_warning(line)) coloured = color::warn(line);
    }
    std::lock_guard<std::mutex> lk(g_out_mtx);
    if (!g_silent)
    {
        (to_stderr ? std::cerr : std::cout) << coloured << "\n";
        (to_stderr ? std::cerr : std::cout).flush();
    }
    if (g_log_file.is_open())
    {
        g_log_file << line << "\n";   // no ANSI codes in log files
        g_log_file.flush();
    }
}

template<typename... Args>
static void mprint(Args&&... args)
{
    emit_line(as_string(std::forward<Args>(args)...));
}

// Split captured output into lines and emit each one individually so
// per-line colour classification works correctly.
static void emit_captured(const std::string& text, bool to_stderr)
{
    if (text.empty()) return;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) emit_line(line, to_stderr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Process helpers
// ─────────────────────────────────────────────────────────────────────────────
struct RunResult { int rc = 0; std::string out; std::string err; };

// Sleep-based wait — replaces the CPU-burning busy-spin.  [B1]
static void wait_process(TinyProcessLib::Process* p, int& status)
{
    while (!p->try_get_exit_status(status))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

// Capture stdout+stderr without streaming.  Per-call mutex protects buffers.  [B19]
static RunResult RunProcessCapture(const std::string& cmd)
{
    RunResult res;
    auto out_buf = std::make_shared<std::string>();
    auto err_buf = std::make_shared<std::string>();
    auto mtx     = std::make_shared<std::mutex>();

    TinyProcessLib::Process* proc = new TinyProcessLib::Process(
        cmd.c_str(), "",
        [out_buf, mtx](const char* b, size_t n)
            { std::lock_guard<std::mutex> lk(*mtx); out_buf->append(b, n); },
        [err_buf, mtx](const char* b, size_t n)
            { std::lock_guard<std::mutex> lk(*mtx); err_buf->append(b, n); },
        /*open_stdin=*/false);

    if (!proc->StartedOk()) { delete proc; res.rc = -200; return res; }
    wait_process(proc, res.rc);
    delete proc;
    res.out = *out_buf;
    res.err = *err_buf;
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessConsoleOutput  [B11: plain std::string, shared_ptr removed]
// ─────────────────────────────────────────────────────────────────────────────
struct ProcessConsoleOutput
{
    std::string output;
    std::string error;
    void        Clear()       { output.clear(); error.clear(); }
    const char* GetStdOut()   { return lua_return_str(output); }
    const char* GetStdError() { return lua_return_str(error);  }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Executable
// ─────────────────────────────────────────────────────────────────────────────
struct Executable
{
    enum InvocationResult : int { CANT_START_EXECUTABLE = -200 };

    Path                 path;
    ProcessConsoleOutput ConsoleOutput;

    void Setpath(Path p)    // [B8] uses mprint so output goes to log too
    {
        if (p.IsFullPath())          { path = p; return; }
        if (p.Exists())              { path = Path::CurrentDir() + p; return; }
        for (auto& sp : Path::GetSystemPaths())
        {
            Path full = sp + p;
            if (full.Exists())       { path = full; return; }
        }
        mprint(color::err("Cannot find executable: ") + p.ToString());
        std::exit(-1);
    }

    // Stream stdout/stderr to console + log while the process runs.  [B19]
    int InvokeCommand(const char* cmd)
    {
        const std::string full_cmd = path.ToString() + " " + cmd;
        ConsoleOutput.Clear();

        auto out_buf = std::make_shared<std::string>();
        auto err_buf = std::make_shared<std::string>();
        auto mtx     = std::make_shared<std::mutex>();

        TinyProcessLib::Process* proc = new TinyProcessLib::Process(
            full_cmd.c_str(), "",
            [out_buf, mtx](const char* b, size_t n)
                { std::lock_guard<std::mutex> lk(*mtx); out_buf->append(b, n); },
            [err_buf, mtx](const char* b, size_t n)
                { std::lock_guard<std::mutex> lk(*mtx); err_buf->append(b, n); },
            false);

        if (!proc->StartedOk()) { delete proc; return CANT_START_EXECUTABLE; }
        int status = 0;
        wait_process(proc, status);
        delete proc;

        ConsoleOutput.output = *out_buf;
        ConsoleOutput.error  = *err_buf;
        emit_captured(ConsoleOutput.output, false);
        emit_captured(ConsoleOutput.error,  true);
        return status;
    }

    // Capture only, no streaming.
    int InvokeCommandHidden(const char* cmd)
    {
        const std::string full_cmd = path.ToString() + " " + cmd;
        ConsoleOutput.Clear();
        auto res = RunProcessCapture(full_cmd);
        ConsoleOutput.output = res.out;
        ConsoleOutput.error  = res.err;
        return res.rc;
    }

    ProcessConsoleOutput& GetConsoleOutput() { return ConsoleOutput; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Compiler / Linker
// ─────────────────────────────────────────────────────────────────────────────
class Compiler : public Executable
{
public:
    Path& Getpath() { return path; }

    std::function<const char*(Path&, Path&, const std::vector<Path>&, std::vector<const char*>)>
        GetFileCompileCommandForStaticLibrary;
    std::function<const char*(Path&, Path&, const std::vector<Path>&, std::vector<const char*>)>
        GetFileCompileCommand;
    std::function<const char*(Path&, Path&, const std::vector<Path>&, std::vector<const char*>)>
        GetFileCompileCommandForSharedLibrary;
};

struct Linker : public Executable
{
    std::function<const char*(std::vector<Path>, Path&, std::vector<Path>)>
        GetObjFilesLinkCommandToStaticLibrary;
    std::function<const char*(std::vector<Path>, Path&, std::vector<Path>,
                              std::vector<Path>, std::vector<const char*>)>
        GetObjFilesLinkCommandToSharedLibrary;
    std::function<const char*(std::vector<Path>, Path&, std::vector<Path>)>
        GetObjFilesLinkCommandToExecutable;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ToolChain
// ─────────────────────────────────────────────────────────────────────────────
struct ToolChain
{
    std::string StaticLibraryExtension = ".a";
    std::string SharedLibraryExtension = ".so";   // [B15] was hardcoded ".dll"
    std::string ObjectFileExtension    = ".o";

    // Correctly-spelled setters
    void SetStaticLibraryExtension(const char* ex) { StaticLibraryExtension = ex; }
    void SetSharedLibraryExtension(const char* ex) { SharedLibraryExtension = ex; }
    void SetObjectFileExtension   (const char* ex) { ObjectFileExtension    = ex; }
    // Legacy misspelled aliases kept so existing Lua scripts do not break
    void SetSahredLibraryExtension(const char* ex) { SharedLibraryExtension = ex; }
    void SetStaticLibraryExtention(const char* ex) { StaticLibraryExtension = ex; }
    void SetObjectFileExtention   (const char* ex) { ObjectFileExtension    = ex; }

    void SetLinkerPath(Path p)
    {
        if (!StaticLinker)  { mprint(color::err("StaticLinker not initialised."));  std::exit(1); }
        if (!DynamicLinker) { mprint(color::err("DynamicLinker not initialised.")); std::exit(1); }
        StaticLinker->Setpath(p);
        DynamicLinker->Setpath(p);
    }

    Compiler* CCompiler   = nullptr;
    Compiler* CPPCompiler = nullptr;
    Linker*   StaticLinker  = nullptr;
    Linker*   DynamicLinker = nullptr;

    static ToolChain* gcc;
};

// ─────────────────────────────────────────────────────────────────────────────
//  GCC default toolchain  (static-duration objects, never freed intentionally)
//  [B3] correct ar syntax  [B15] .so default  [B17] -fPIC  [B18] quoted paths
// ─────────────────────────────────────────────────────────────────────────────
ToolChain* ToolChain::gcc = []() -> ToolChain*
{
    static ToolChain tc;
    static Compiler  gpp;
    static Linker    ld;

    // Helper: build the -c compile flags string.
    auto compile_flags = [](Path& src, Path& obj,
                            const std::vector<Path>& includes,
                            std::vector<const char*> extra) -> std::string
    {
        std::string rv = " -c \"" + src.ToString() + "\" -o\"" + obj.ToString() + "\"";
        for (auto& i : includes) rv += " -I\"" + i.ToString() + "\"";
        for (auto& c : extra)    { rv += " "; rv += c; }
        return rv;
    };

    // Helper: smart library flag (full path → as-is, bare name → -lname).
    auto lib_flag = [](const Path& lib) -> std::string
    {
        std::string ls = lib.ToString();
        if (ls.find('/') != std::string::npos ||
            ls.find('\\') != std::string::npos ||
            (!ls.empty() && ls[0] == '-'))
            return " " + ls;
        return " -l" + ls;
    };

    // ── compile ──────────────────────────────────────────────────────────────
    gpp.GetFileCompileCommandForStaticLibrary =
        [compile_flags](Path& src, Path& obj, const std::vector<Path>& inc,
                        std::vector<const char*> extra) -> const char*
    {
        auto rv = compile_flags(src, obj, inc, extra);
        mprint(color::dim(rv));
        return lua_return_str(rv);
    };

    gpp.GetFileCompileCommand =
        [compile_flags](Path& src, Path& obj, const std::vector<Path>& inc,
                        std::vector<const char*> extra) -> const char*
    {
        auto rv = compile_flags(src, obj, inc, extra);
        mprint(color::dim(rv));
        return lua_return_str(rv);
    };

    gpp.GetFileCompileCommandForSharedLibrary =         // [B17] -fPIC
        [compile_flags](Path& src, Path& obj, const std::vector<Path>& inc,
                        std::vector<const char*> extra) -> const char*
    {
        extra.push_back("-fPIC");
        auto rv = compile_flags(src, obj, inc, extra);
        mprint(color::dim(rv));
        return lua_return_str(rv);
    };

    tc.CPPCompiler = &gpp;
    tc.CCompiler   = &gpp;

    // ── link: static library  [B3] correct ar invocation ────────────────────
    ld.GetObjFilesLinkCommandToStaticLibrary =
        [](std::vector<Path> objs, Path& out, std::vector<Path> /*libs*/) -> const char*
    {
        std::string rv = "rcs \"" + out.ToString() + "\"";
        for (auto& o : objs) rv += " \"" + o.ToString() + "\"";
        mprint(color::dim(rv));
        return lua_return_str(rv);
    };

    // ── link: executable  [B18] quoted output path ───────────────────────────
    ld.GetObjFilesLinkCommandToExecutable =
        [lib_flag](std::vector<Path> objs, Path& out, std::vector<Path> libs) -> const char*
    {
        std::string rv = "-o\"" + out.ToString() + "\"";
        for (auto& o : objs) rv += " \"" + o.ToString() + "\"";
        for (auto& l : libs) rv += lib_flag(l);
        mprint(color::dim(rv));
        return lua_return_str(rv);
    };

    // ── link: shared library ─────────────────────────────────────────────────
    ld.GetObjFilesLinkCommandToSharedLibrary =
        [lib_flag](std::vector<Path> objs, Path& out,
                   std::vector<Path> libs, std::vector<Path> search_paths,
                   std::vector<const char*> extra) -> const char*
    {
        std::string rv = "-shared";
        for (auto& sp : search_paths) rv += " -L\"" + sp.ToString() + "\"";
        for (auto& o  : objs)         rv += " \"" + o.ToString() + "\"";
        rv += " -o\"" + out.ToString() + "\"";
        for (auto& l : libs)  rv += lib_flag(l);
        for (auto& c : extra) { rv += " "; rv += c; }
        mprint(color::dim(rv));
        return lua_return_str(rv);
    };

    tc.StaticLinker  = &ld;
    tc.DynamicLinker = &ld;
    return &tc;
}();

// ─────────────────────────────────────────────────────────────────────────────
//  Forward declaration
// ─────────────────────────────────────────────────────────────────────────────
class MiracleExecuter;

// ─────────────────────────────────────────────────────────────────────────────
//  Target
// ─────────────────────────────────────────────────────────────────────────────
struct Target
{
    // ── configuration ────────────────────────────────────────────────────────
    std::string Name           = "null";    // [B2] std::string, not char[]
    std::string OutputFileName = "null";
    std::string Type;   // CONSOLE_APPLICATION | STATIC_LIBRARY | SHARED_LIBRARY

    std::vector<Path>        IncludePaths;
    std::vector<Path>        SourceFiles;
    std::vector<Path>        link_libraries;
    std::vector<Path>        link_search_path;
    std::vector<const char*> ExtraCompilerCommands;
    std::vector<const char*> linker_commands;
    // OR-semantics: file accepted if it matches ANY filter, or if list is empty.  [B4]
    std::vector<const char*> AddSourceFilters;

    ToolChain* BuildToolChain = nullptr;
    Path       WorkingPath;
    Path       OutputFolder;
    Path       IntermediateDir;

    // ── Lua-facing accessors (const char* interface preserved) ───────────────
    const char* GetName()           const { return lua_return_str(Name); }
    void        SetName(const char* v)    { Name = v ? v : ""; }
    const char* GetOutputFileName() const { return lua_return_str(OutputFileName); }
    void        SetOutputFileName(const char* v) { OutputFileName = v ? v : ""; }
    const char* GetType()           const { return lua_return_str(Type); }
    void        SetType(const char* v)    { Type = v ? v : ""; }
    void        SetOutputFolder(const char* p)   { OutputFolder = p; }

    // ── build log  [B12] fold-expression fixed ────────────────────────────────
    std::string log;
    const char* GetLog()  const { return lua_return_str(log); }
    void        ClearLog()      { log.clear(); }
    template<typename... Args>
    void Log(Args&&... args) { log += as_string(std::forward<Args>(args)...); }

    // ── mutation helpers ─────────────────────────────────────────────────────
    void AddCompilerCommand(const char* cmd) { ExtraCompilerCommands.push_back(cmd); }
    void AddLinkerCommand  (const char* cmd) { linker_commands.push_back(cmd); }
    void AddLinkSearchPath (Path p)          { link_search_path.push_back(std::move(p)); }
    void AddSourceFilter   (const char* f)  { AddSourceFilters.push_back(f); }

    // Accept file if it matches ANY filter (OR semantics), or no filters set.  [B4]
    void PushSourceFile(Path p)
    {
        if (AddSourceFilters.empty()) { SourceFiles.push_back(p); return; }
        const char* ps = p.ToStr();
        for (auto& f : AddSourceFilters)
            if (std::regex_match(ps, std::regex(f))) { SourceFiles.push_back(p); return; }
    }

    // ── public API ───────────────────────────────────────────────────────────
    void Clean();
    int  Build();

    void AddSourceFile(const char* path_str);
    void AddFilesMatchsRegex(const char* regex_expr);
    void AddFilesMatchsRegexInFolder(Path dir, const std::regex& rgx);
    void AddIncludePath(const char* path_str);
    void AddLinkLibrary(const char* path_str);

private:
    // Produce a collision-free .o filename from the full source path.  [B9]
    std::string ObjFilenameFor(const Path& src) const
    {
        std::string s = src.ToString();
        std::replace(s.begin(), s.end(), '\\', '/');
        if (s.size() > 1 && s[1] == ':') s[1] = '_';   // Windows drive
        std::replace(s.begin(), s.end(), '/', '_');
        auto dot = s.rfind('.');
        if (dot != std::string::npos) s = s.substr(0, dot);
        return s + (BuildToolChain ? BuildToolChain->ObjectFileExtension : ".o");
    }

    // Compute and cache the object output directory.  [B14]
    Path ResolveObjDir();

    // Compute the final output file path, applying default extensions.
    Path ResolveOutputFile();
};

// ─────────────────────────────────────────────────────────────────────────────
//  MiracleExecuter
// ─────────────────────────────────────────────────────────────────────────────
class MiracleExecuter : public Executer
{
public:
    std::vector<Path> SearchPaths;
    Path              ProjectSourcePath;

    std::vector<Path>& GetSearchPaths()      { return SearchPaths; }
    Path&              GetProjectSourcePath() { return ProjectSourcePath; }

    void AddSearchPath(Path p)
    {
        if (!p.Exists()) return;
        SearchPaths.push_back(p);
    }
    void AddSearchPathRecursively(Path p)
    {
        if (!p.Exists()) return;
        SearchPaths.push_back(p);
        for (auto& sub : p.Browse()) AddSearchPathRecursively(sub);
    }

    // Load and execute a .ubs Lua script.  [B20] checks is_open
    bool LoadFile(const char* path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            mprint(color::err("Cannot open script: ") + path);
            return false;
        }
        std::string src((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        file.close();

        Path fp     = path;
        Path full   = fp.AsFullPath();
        Path parent = full.GetParent();
        Path::PushPath(parent);
        ProjectSourcePath = parent;
        Executer::PrepareExecution();
        state.dostring(variables.Eval(src.c_str()));
        Path::PopPath();
        return true;
    }

    std::string GetErrorString() { return ""; }
    void SetVariable(std::string var, std::string val) { Executer::SetVariable(var, val); }

    // Recursively collect all files transitively included by `file`.
    // Uses unordered_set for O(1) duplicate detection.  [B5]
    std::vector<Path> GetFileIncludes(const Path& file,
                                      const std::vector<Path>& include_paths = {})
    {
        std::unordered_set<std::string> visited;
        std::vector<Path>               result;
        ScanIncludes(file, include_paths, visited, result);
        return result;
    }

    void InitLuaState()
    {
        Executer::InitLuaState();

        static auto exe_cls =
            kaguya::UserdataMetatable<Executable>()
                .addFunction("SetPath",            &Executable::Setpath)
                .addFunction("InvokeCommand",       &Executable::InvokeCommand)
                .addFunction("InvokeCommandHidden", &Executable::InvokeCommandHidden)
                .addFunction("GetConsoleOutput",    &Executable::GetConsoleOutput);
        state["Executable"] = exe_cls;

        static auto tc_cls =
            kaguya::UserdataMetatable<ToolChain>()
                .setConstructors<ToolChain()>()
                .addFunction("SetStaticLibraryExtension", &ToolChain::SetStaticLibraryExtension)
                .addFunction("SetSharedLibraryExtension", &ToolChain::SetSharedLibraryExtension)
                .addFunction("SetObjectFileExtension",    &ToolChain::SetObjectFileExtension)
                .addFunction("SetLinkerPath",             &ToolChain::SetLinkerPath)
                .addProperty("CPPCompiler",  &ToolChain::CPPCompiler)
                .addProperty("CCompiler",    &ToolChain::CCompiler)
                .addProperty("StaticLinker", &ToolChain::StaticLinker)
                .addProperty("DynamicLinker",&ToolChain::DynamicLinker);
        state["ToolChain"] = tc_cls;

        static auto cmp_cls =
            kaguya::UserdataMetatable<Compiler, Executable>()
                .setConstructors<Compiler()>()
                .addFunction("GetPath", &Compiler::Getpath)
                .addProperty("GetFileCompileCommandForStaticLibrary",
                             &Compiler::GetFileCompileCommandForStaticLibrary)
                .addProperty("GetFileCompileCommandForSharedLibrary",
                             &Compiler::GetFileCompileCommandForSharedLibrary)
                .addFunction("GetFileCompileCommand", &Compiler::GetFileCompileCommand);
        state["Compiler"].setClass(cmp_cls);

        static auto lnk_cls =
            kaguya::UserdataMetatable<Linker, Executable>()
                .setConstructors<Linker()>()
                .addProperty("GetObjFilesLinkCommandToStaticLibrary",
                             &Linker::GetObjFilesLinkCommandToStaticLibrary)
                .addProperty("GetObjFilesLinkCommandToExecutable",
                             &Linker::GetObjFilesLinkCommandToExecutable)
                .addProperty("GetObjFilesLinkCommandToSharedLibrary",
                             &Linker::GetObjFilesLinkCommandToSharedLibrary);
        state["Linker"].setClass(lnk_cls);

        static auto pco_cls =
            kaguya::UserdataMetatable<ProcessConsoleOutput>()
                .setConstructors<ProcessConsoleOutput()>()
                .addFunction("GetStdOut",   &ProcessConsoleOutput::GetStdOut)
                .addFunction("GetStdError", &ProcessConsoleOutput::GetStdError);
        state["ProcessConsoleOutput"].setClass(pco_cls);

        static auto tgt_cls =
            kaguya::UserdataMetatable<Target>()
                .setConstructors<Target()>()
                .addFunction("AddSourceFile",       &Target::AddSourceFile)
                .addFunction("AddLinkLibrary",      &Target::AddLinkLibrary)
                .addFunction("AddLinkCommand",      &Target::AddLinkerCommand)   // legacy alias
                .addFunction("AddLinkerCommand",    &Target::AddLinkerCommand)
                .addFunction("AddFilesMatchsRegex", &Target::AddFilesMatchsRegex)
                .addFunction("Build",               &Target::Build)
                .addFunction("Clean",               &Target::Clean)              // [F8]
                .addFunction("GetOutputFileName",   &Target::GetOutputFileName)
                .addFunction("SetOutputFileName",   &Target::SetOutputFileName)
                .addFunction("GetName",             &Target::GetName)
                .addFunction("SetName",             &Target::SetName)
                .addFunction("AddIncludePath",      &Target::AddIncludePath)
                .addFunction("GetLog",              &Target::GetLog)
                .addFunction("ClearLog",            &Target::ClearLog)
                .addFunction("AddCompilerCommand",  &Target::AddCompilerCommand)
                .addFunction("SetOutputFolder",     &Target::SetOutputFolder)
                .addFunction("AddLinkSearchPath",   (void(Target::*)(Path))&Target::AddLinkSearchPath)
                .addFunction("AddSourceFilter",     &Target::AddSourceFilter)
                .addProperty("BuildToolChain",      &Target::BuildToolChain)
                .addProperty("WorkingPath",         &Target::WorkingPath)
                .addProperty("AddSourceFilters",    &Target::AddSourceFilters)
                .addProperty("Type",                Target::GetType, Target::SetType);
        state["Target"].setClass(tgt_cls);

        static auto bs_cls =
            kaguya::UserdataMetatable<MiracleExecuter>()
                .setConstructors<MiracleExecuter()>()
                .addFunction("AddSearchPath",            &MiracleExecuter::AddSearchPath)
                .addFunction("AddSearchPathRecursively", &MiracleExecuter::AddSearchPathRecursively)
                .addFunction("GetProjectSourcePath",     &MiracleExecuter::GetProjectSourcePath);
        state["BuildSystem"].setClass(bs_cls);

        state["bs"]            = this;
        state["GCC_TOOLCHAIN"] = ToolChain::gcc;
    }

    // Thread-safe singleton via call_once.  [B13]
    static MiracleExecuter* GetExecuter()
    {
        static MiracleExecuter* inst = nullptr;
        static std::once_flag   once;
        std::call_once(once, [](){
            inst = new MiracleExecuter();
            inst->InitLuaState();
        });
        return inst;
    }

private:
    // Line-based #include scanner — no char-by-char state machine.  [B5]
    void ScanIncludes(Path                             file,
                      const std::vector<Path>&         inc_paths,
                      std::unordered_set<std::string>& visited,
                      std::vector<Path>&               result)
    {
        if (!visited.insert(file.ToString()).second) return;
        result.push_back(file);

        if (!file.Exists()) return;
        std::ifstream in(file.ToStr());
        if (!in.is_open()) return;

        std::string line;
        while (std::getline(in, line))
        {
            size_t i = 0;
            // Skip leading whitespace
            while (i < line.size() && (line[i]==' '||line[i]=='\t')) ++i;
            if (i >= line.size() || line[i] != '#') continue;
            ++i;
            while (i < line.size() && (line[i]==' '||line[i]=='\t')) ++i;
            if (line.compare(i, 7, "include") != 0) continue;
            i += 7;
            while (i < line.size() && (line[i]==' '||line[i]=='\t')) ++i;
            if (i >= line.size()) continue;

            char open  = line[i++];
            if (open != '"' && open != '<') continue;
            char close = (open == '"') ? '"' : '>';
            bool local = (open == '"');

            size_t end = line.find(close, i);
            if (end == std::string::npos) continue;
            std::string name = line.substr(i, end - i);
            if (name.empty()) continue;

            bool found = false;
            if (local)
            {
                Path cand = file.GetParent() + (const Path&)name;
                if (cand.Exists()) { ScanIncludes(cand, inc_paths, visited, result); found = true; }
            }
            if (!found)
            {
                for (auto& ip : inc_paths)
                {
                    Path cand = (const Path&)ip + (const Path&)name;
                    if (cand.Exists()) { ScanIncludes(cand, inc_paths, visited, result); found = true; break; }
                }
            }
        }
    }
};

// =============================================================================
//  Target method implementations
// =============================================================================

Path Target::ResolveObjDir()
{
    // Always recompute from scratch so repeated Build() calls are consistent.  [B14]
    IntermediateDir = Path();
    if (!WorkingPath.IsSet())
        WorkingPath = MiracleExecuter::GetExecuter()->GetProjectSourcePath();
    const std::string& sub = (Name != "null" && !Name.empty()) ? Name : OutputFileName;
    Path dir = (WorkingPath + "obj") + sub;
    IntermediateDir = dir;
    return dir;
}

Path Target::ResolveOutputFile()
{
    if (!OutputFolder.IsSet())
        OutputFolder = MiracleExecuter::GetExecuter()->GetProjectSourcePath();
    Path out = OutputFolder + OutputFileName;
    if (out.GetExtension().empty())
    {
        if      (Type == "CONSOLE_APPLICATION")
            out.SetExtention(MBS_WINDOWS ? ".exe" : "");
        else if (Type == "STATIC_LIBRARY")
            out.SetExtention(BuildToolChain->StaticLibraryExtension);
        else if (Type == "SHARED_LIBRARY")
            out.SetExtention(BuildToolChain->SharedLibraryExtension);
        else
        {
            mprint(color::err("Unknown target type: '") + Type + "'");
            return Path();
        }
    }
    return out;
}

// ── Clean  [F8] ───────────────────────────────────────────────────────────────

void Target::Clean()
{
    Path obj_dir = ResolveObjDir();
    if (!obj_dir.Exists())
    {
        mprint(color::dim("  [" + Name + "] Nothing to clean."));
        return;
    }
    mprint(color::warn("  [" + Name + "] Cleaning: ") + obj_dir.ToString());
    for (auto& f : obj_dir.Browse()) f.Delete();
}

// ── Build ─────────────────────────────────────────────────────────────────────

int Target::Build()
{
    auto t0 = std::chrono::steady_clock::now();

    // ── validation ────────────────────────────────────────────────────────────
    if (!BuildToolChain)
        { log = "Toolchain not set."; mprint(color::err(log)); return 1; }
    if (BuildToolChain->ObjectFileExtension.empty())
        { log = "Toolchain object-file extension not set."; mprint(color::err(log)); return 1; }
    if (SourceFiles.empty())
        { Log("No source files in target '", Name, "'."); mprint(color::err(log)); return 1; }

    if (g_clean) Clean();

    // ── resolve paths ─────────────────────────────────────────────────────────
    Path ObjDir      = ResolveObjDir();
    Path output_file = ResolveOutputFile();
    if (!output_file.IsSet()) return 1;
    Path::MakeIfDoesntExit(ObjDir);

    // ── target type enum ─────────────────────────────────────────────────────
    enum class TT { CONSOLE, STATIC_LIB, SHARED_LIB };
    TT tt = (Type == "CONSOLE_APPLICATION") ? TT::CONSOLE :
            (Type == "SHARED_LIBRARY")      ? TT::SHARED_LIB :
                                              TT::STATIC_LIB;

    // ── Phase 1: decide what needs recompilation ──────────────────────────────
    struct PendingCompile { Path src; Path obj; std::string cmd; };
    std::vector<PendingCompile> pending;
    std::vector<Path>           all_objs;
    all_objs.reserve(SourceFiles.size());
    int skipped = 0;

    for (auto& f : SourceFiles)
    {
        Path obj = ObjDir + ObjFilenameFor(f);
        all_objs.push_back(obj);

        bool need = !obj.Exists();
        if (!need)
        {
            for (auto& dep : MiracleExecuter::GetExecuter()->GetFileIncludes(f, IncludePaths))
                if (obj.GetLastModificationTime() < dep.GetLastModificationTime())
                    { need = true; break; }
        }
        if (!need) { ++skipped; continue; }

        Compiler* cmp = (f.GetExtension() == ".c")
                        ? BuildToolChain->CCompiler
                        : BuildToolChain->CPPCompiler;

        std::string args;
        if      (tt == TT::STATIC_LIB)
            args = cmp->GetFileCompileCommandForStaticLibrary(f, obj, IncludePaths, ExtraCompilerCommands);
        else if (tt == TT::CONSOLE)
            args = cmp->GetFileCompileCommand(f, obj, IncludePaths, ExtraCompilerCommands);
        else
            args = cmp->GetFileCompileCommandForSharedLibrary(f, obj, IncludePaths, ExtraCompilerCommands);

        pending.push_back({ f, obj, cmp->path.ToString() + " " + args });
    }

    const size_t total   = SourceFiles.size();
    const size_t n_pend  = pending.size();

    mprint(color::info("  [" + Name + "] ") +
           std::to_string(n_pend) + " to compile, " +
           std::to_string(skipped) + " up to date  (" +
           std::to_string(total) + " total)");

    // ── Phase 2: parallel compilation  [B6][F1][F5][F6] ──────────────────────
    std::atomic<bool>   failed{false};
    std::atomic<size_t> done{0};
    int                 fail_rc  = 0;
    std::string         fail_src;
    std::mutex          fail_mtx;

    unsigned max_jobs = g_jobs > 0 ? g_jobs : std::max(1u, std::thread::hardware_concurrency());
    if (n_pend > 0)
        mprint(color::bold("  Compiling with ") + std::to_string(max_jobs) + " job(s)...");

    for (size_t bs = 0; bs < n_pend && !failed; bs += max_jobs)
    {
        size_t be = std::min(bs + (size_t)max_jobs, n_pend);
        std::vector<std::future<RunResult>> futs;
        futs.reserve(be - bs);

        for (size_t i = bs; i < be; ++i)
        {
            pending[i].obj.Delete();   // remove stale .o
            std::string cmd = pending[i].cmd;
            size_t      num = i + 1;
            // [F6] Progress line printed before launching so the user sees it immediately
            mprint(color::step("  [" + std::to_string(num) + "/" + std::to_string(n_pend) + "] ") +
                   pending[i].src.ToString());
            futs.push_back(std::async(std::launch::async,
                [cmd](){ return RunProcessCapture(cmd); }));
        }

        for (size_t i = 0; i < futs.size(); ++i)
        {
            RunResult res = futs[i].get();
            emit_captured(res.out, false);   // [F3] per-line colour applied inside
            emit_captured(res.err, true);
            ++done;
            if (res.rc != 0 && !failed.exchange(true))
            {
                std::lock_guard<std::mutex> lk(fail_mtx);
                fail_rc  = res.rc;
                fail_src = pending[bs + i].src.ToString();
            }
        }
    }

    if (failed)
    {
        log = as_string("Compile failed (rc=", fail_rc, "): ", fail_src);
        mprint(color::err(log));
        return fail_rc;
    }

    // ── incremental link guard ────────────────────────────────────────────────
    if (n_pend == 0 && output_file.Exists())
    {
        mprint(color::ok("  [" + Name + "] Up to date — link skipped."));
        auto dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        mprint(color::dim("  Elapsed: " + std::to_string(dur).substr(0, 5) + "s"));
        return 0;
    }

    mprint(color::bold("  [link] ") + output_file.ToString());
    output_file.Delete();

    // ── link ──────────────────────────────────────────────────────────────────
    int lrc = 0;
    if (tt == TT::CONSOLE)
    {
        mprint(color::dim("  " + std::string(BuildToolChain->StaticLinker->path.ToStr())));
        lrc = BuildToolChain->StaticLinker->InvokeCommand(
            BuildToolChain->StaticLinker->GetObjFilesLinkCommandToExecutable(
                all_objs, output_file, link_libraries));
    }
    else if (tt == TT::STATIC_LIB)
    {
        mprint(color::dim("  " + std::string(BuildToolChain->StaticLinker->path.ToStr())));
        lrc = BuildToolChain->StaticLinker->InvokeCommand(
            BuildToolChain->StaticLinker->GetObjFilesLinkCommandToStaticLibrary(
                all_objs, output_file, link_libraries));
    }
    else   // SHARED_LIBRARY — via CPPCompiler driver  [B10]
    {
        mprint(color::dim("  " + std::string(BuildToolChain->CPPCompiler->path.ToStr())));
        lrc = BuildToolChain->CPPCompiler->InvokeCommand(
            BuildToolChain->DynamicLinker->GetObjFilesLinkCommandToSharedLibrary(
                all_objs, output_file, link_libraries, link_search_path, linker_commands));
    }

    if (lrc != 0)
    {
        log = as_string("Link failed (rc=", lrc, ") for '", Name, "'");
        mprint(color::err(log));
        return lrc;
    }

    // ── summary  [F2] ─────────────────────────────────────────────────────────
    auto dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::ostringstream ss;
    ss << "  " << color::ok("[OK] ") << color::bold(Name)
       << "  →  " << output_file.ToString()
       << "   (" << n_pend << " compiled, " << skipped << " skipped, "
       << std::to_string(dur).substr(0, 5) << "s)";
    mprint(ss.str());
    return 0;
}

// ── AddLinkLibrary ────────────────────────────────────────────────────────────

void Target::AddLinkLibrary(const char* path_str)
{
    Path p = path_str;
    if (p.Exists()) { link_libraries.push_back((const Path&)Path::CurrentDir() + (const Path&)p); return; }
    for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
    {
        Path r = (const Path&)sp + (const Path&)p;
        if (r.Exists()) { link_libraries.push_back(r); return; }
    }
    link_libraries.push_back(path_str);   // treat as system lib (e.g. "pthread")
}

// ── AddSourceFile ─────────────────────────────────────────────────────────────

void Target::AddSourceFile(const char* path_str)
{
    struct E { std::string msg; };
    try
    {
        Path p = path_str;
        if (p.IsFullPath())
        {
            if (p.Exists()) { PushSourceFile(p); return; }
            throw E{ p.ToString() + " does not exist" };
        }
        // cwd → project source → search paths
        for (Path base : { Path::CurrentDir(),
                           MiracleExecuter::GetExecuter()->GetProjectSourcePath() })
        {
            Path full = (const Path&)base + (const Path&)p;
            if (full.Exists()) { PushSourceFile(full); return; }
        }
        for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
        {
            Path full = (const Path&)sp + (const Path&)p;
            if (full.Exists()) { PushSourceFile(full); return; }
        }
        throw E{ std::string(path_str) + " cannot be located." };
    }
    catch (E& e)
    {
        mprint(color::err("AddSourceFile: ") + e.msg);
        std::exit(1);
    }
}

// ── AddFilesMatchsRegex  [B16] dedup across roots ────────────────────────────

void Target::AddFilesMatchsRegexInFolder(Path dir, const std::regex& rgx)
{
    if (!dir.Exists()) return;
    for (auto& rel : dir.BrowseRelative())
    {
        if (!std::regex_match(rel.ToStr(), rgx)) continue;
        Path full = (const Path&)dir + (const Path&)rel;
        if (std::find(SourceFiles.begin(), SourceFiles.end(), full) == SourceFiles.end())
            AddSourceFile(full.ToStr());
    }
}

void Target::AddFilesMatchsRegex(const char* regex_expr)
{
    std::regex rgx(regex_expr);
    Path root = WorkingPath.IsSet()
                ? WorkingPath
                : MiracleExecuter::GetExecuter()->GetProjectSourcePath();
    AddFilesMatchsRegexInFolder(root, rgx);
    for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
        AddFilesMatchsRegexInFolder(sp, rgx);
}

// ── AddIncludePath ────────────────────────────────────────────────────────────

void Target::AddIncludePath(const char* path_str)
{
    Path p = path_str;
    if (p.IsFullPath())
    {
        if (!p.Exists()) { mprint(color::err(p.ToString() + " does not exist.")); std::exit(1); }
        IncludePaths.push_back(p);
        return;
    }
    Path r = (const Path&)MiracleExecuter::GetExecuter()->GetProjectSourcePath() + (const Path&)p;
    if (!r.Exists()) { mprint(color::err(std::string(path_str) + " cannot be located.")); std::exit(1); }
    IncludePaths.push_back(r);
}

// =============================================================================
//  main
// =============================================================================
#define SCRIPT_FILE_EXTENSION ".ubs"

static void print_usage(const char* argv0)
{
    // Colour is already configured by the time this is called.
    std::cout << color::bold("Miracle Build System\n\n")
              << color::bold("Usage:\n")
              << "  " << argv0 << " [OPTIONS] [script" SCRIPT_FILE_EXTENSION "] [KEY VALUE ...]\n\n"
              << color::bold("Options:\n")
              << "  --silent            Suppress all console output\n"
              << "  --no-color          Disable ANSI colour\n"
              << "  --output-file FILE  Redirect output to FILE (alongside stdout)\n"
              << "  --jobs N            Parallel compile jobs (default: CPU count)\n"
              << "  --clean             Delete object files before building\n"
              << "  --help              Show this help\n\n"
              << "If no script is given, 'build" SCRIPT_FILE_EXTENSION "' in the current directory is used.\n"
              << "KEY VALUE pairs after the script path are passed as Lua variables.\n";
}

int main(int argc, const char* argv[])
{
    // Auto-enable colour when stdout is connected to a real terminal.  [F3]
    color::enabled = (MBS_IS_TTY(1) != 0);

    // ── pre-scan: collect all flags first ─────────────────────────────────────
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a == "--silent")   { g_silent = true; }
        else if (a == "--no-color") { color::enabled = false; }
        else if (a == "--clean")    { g_clean  = true; }
        else if (a == "--help")     { print_usage(argv[0]); return 0; }
        else if ((a == "--jobs") && i + 1 < argc)
        {
            int n = std::atoi(argv[++i]);
            g_jobs = (n > 0) ? (unsigned)n : 0;
        }
        else if ((a == "--output-file") && i + 1 < argc)
        {
            const char* lp = argv[++i];
            g_log_file.open(lp, std::ios::out | std::ios::trunc);
            if (!g_log_file.is_open())
                std::cerr << "Warning: cannot open log file '" << lp << "'\n";
        }
    }

    mprint(color::bold("Miracle Build System"));

    if (!getenv("MIRACLE_HOME"))
    {
        mprint(color::err("MIRACLE_HOME is not set."));
        return 1;
    }

    MiracleExecuter* executer = MiracleExecuter::GetExecuter();

    // ── find the script argument (first non-flag token) ───────────────────────
    int script_idx = -1;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--silent" || a == "--no-color" || a == "--clean" || a == "--help")
            continue;
        if (a == "--jobs" || a == "--output-file") { ++i; continue; }   // skip value too
        script_idx = i;
        break;
    }

    if (script_idx == -1)
    {
        // Default: build.ubs in cwd
        Path file = Path::CurrentDir() + ("build" SCRIPT_FILE_EXTENSION);
        if (!file.Exists())
        {
            mprint(color::err("No script given and no 'build" SCRIPT_FILE_EXTENSION "' found."));
            print_usage(argv[0]);
            return 1;
        }
        mprint(color::info("Script: ") + file.ToString());
        if (!executer->LoadFile(file.ToStr())) return 1;
    }
    else
    {
        Path file = argv[script_idx];

        if (!file.Exists())
        {
            mprint(color::err("Script not found: ") + file.ToString());
            return 1;
        }
        if (file.GetExtension() != SCRIPT_FILE_EXTENSION)
        {
            mprint(color::err("Only '") + SCRIPT_FILE_EXTENSION + "' scripts are supported.");
            return 1;
        }

        // KEY VALUE pairs follow the script path.  [B7] skip both flag token and its value.
        for (int i = script_idx + 1; i < argc; )
        {
            std::string a = argv[i];
            if (a == "--silent" || a == "--no-color" || a == "--clean" || a == "--help")
                { ++i; continue; }
            if (a == "--jobs" || a == "--output-file")
                { i += 2; continue; }   // skip flag + its value together
            if (i + 1 < argc)
            {
                mprint(color::dim("  var: " + a + " = " + argv[i + 1]));
                executer->SetVariable(a, argv[i + 1]);
                i += 2;
            }
            else
            {
                mprint(color::warn("Dangling argument ignored: ") + a);
                ++i;
            }
        }

        mprint(color::info("Script: ") + file.ToString());
        if (!executer->LoadFile(file.ToStr())) return 1;
    }

    // [B8] Flush and close log file before exit.
    if (g_log_file.is_open()) g_log_file.close();

    delete executer;
    return 0;
}