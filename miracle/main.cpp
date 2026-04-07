// =============================================================================
//  Miracle Build System  —  single translation unit
//  Requires: kaguya (Lua binding), Executer.hpp, process.hpp, Path (host fw)
//  C++ standard: C++17
//
//  Architecture overview
//  ─────────────────────
//  BuildGraph      — central registry; dependency graph, topo sort, scheduling
//  Target          — one build artefact (exe / static lib / shared lib)
//  ToolChain       — compiler + linker pair; GCC default provided
//  ThreadPool      — fixed-size pool shared across all targets (no over-sub)
//  MiracleExecuter — Lua host; exposes all C++ types to script
//
//  Features
//  ────────────────────────────────────────────────────────────────────────────
//  [N1]  Dependency graph + topological sort (Kahn) + cycle detection
//  [N2]  Parallel target scheduling via shared ThreadPool
//  [N3]  Build fingerprinting (FNV-1a) — full recompile on flag change
//  [N4]  Depfile (.d) ingestion — compiler deps when available, hand-scanner fallback
//  [N5]  Out-of-source builds — SetBuildDir() per-target or globally
//  [N6]  Build configurations (debug/release/…) + --config + --target
//  [N7]  Install rules — AddInstallRule() / graph:Install(prefix) / --install
//  [N8]  pkg-config integration — AddPkgConfigDep()
//  [N9]  Lua error handling — traceback printed on script error
//  [N10] --dry-run mode
//  [N11] ANSI colour, --no-color, TTY auto-detect
//  [N12] Per-target build summary + global diagnostic totals + global timer
//  [N13] --jobs, --clean, --help, --output-file, --build-dir, --target
//
//  Fixes (previous rounds)
//  ────────────────────────────────────────────────────────────────────────────
//  [F1]  BuildOrDie() — throws std::runtime_error (kaguya converts to Lua error)
//  [F2]  Per-target + global error/warning counters; exclusive classification
//  [F4]  Depfile corrupt-fallback + Clean() removes .d and fingerprint
//  [F5]  Stale .o detection with canonical path comparison
//  [F6]  --target NAME / graph:BuildOne(name)
//  [F7]  pkg-config: owned_flags deque<string> — no raw new[], no realloc UB
//  [F8]  AddConfig: unique_ptr<BuildConfig> — no leak
//  [F9]  Destruction order: delete executer before pool
//  [F10] Config flag double-injection removed from BuildAll
//
//  Fixes (this round)
//  ────────────────────────────────────────────────────────────────────────────
//  [A1]  emit_line: error/warning classification is now exclusive (no double-count)
//  [A2]  Fingerprint and Clean() use Path operator+ (no mixed separators on Windows)
//  [A6]  Stale .o detection uses weakly_canonical for path comparison
//  [A7]  BuildOrDie uses C++ throw instead of lua_error (no longjmp past dtors)
//  [D1]  mbs_print/mbs_warn/mbs_error exposed as Lua globals for script output
//  [D5]  graph:ListTargets() returns table of registered target names
//  [D6]  Global build timer printed in final summary
//  [D7]  BuildAll prints which targets were skipped when a failure aborts the build
//
//  Kept from host framework
//  ────────────────────────
//  Custom Path — Exists, Browse, BrowseRelative, GetLastModificationTime,
//  MakeIfDoesntExit, GetExtension, GetParent, IsFullPath, AsFullPath, CurrentDir,
//  GetSystemPaths, PushPath/PopPath, Delete, SetExtention, ToStr/ToString,
//  GetFileName. std::filesystem used ONLY for weakly_canonical() in the
//  include scanner and stale-obj detection (canonicalising path comparison keys).
// =============================================================================

#include <iostream>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <queue>
#include <deque>
#include <vector>
#include <string>
#include <functional>
#include <regex>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <filesystem>   // C++17 — used only for canonical path keys

#include <kaguya/kaguya.hpp>
#include <Executer.hpp>
#include <process.hpp>

// ─────────────────────────────────────────────────────────────────────────────
//  Platform
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
#  define MBS_WINDOWS 1
#  include <windows.h>
#  include <io.h>
#  define MBS_IS_TTY(fd)  (_isatty(fd))
#  define MBS_EXE_EXT     ".exe"
#  define MBS_SOLIB_EXT   ".dll"
#else
#  define MBS_WINDOWS 0
#  include <unistd.h>
#  define MBS_IS_TTY(fd)  (isatty(fd))
#  define MBS_EXE_EXT     ""
#  define MBS_SOLIB_EXT   ".so"
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  lua_return_str — thread-local staging buffer for Lua ↔ C++ const char*
//  RULE: caller must consume before next call on the same thread.
// ─────────────────────────────────────────────────────────────────────────────
static const char* lua_return_str(const char* s)
{
    thread_local std::string buf;
    buf = s ? s : "";
    return buf.c_str();
}
static const char* lua_return_str(const std::string& s) { return lua_return_str(s.c_str()); }

// ─────────────────────────────────────────────────────────────────────────────
//  FNV-1a 64-bit hash — used for build fingerprinting
// ─────────────────────────────────────────────────────────────────────────────
static uint64_t fnv1a(const std::string& s, uint64_t h = 14695981039346656037ULL)
{
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
    return h;
}
static uint64_t fnv1a_many(const std::vector<std::string>& v)
{
    uint64_t h = 14695981039346656037ULL;
    for (auto& s : v) h = fnv1a(s, h);
    return h;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ANSI colour
// ─────────────────────────────────────────────────────────────────────────────
namespace color
{
    static bool enabled = false;
    static const char* RST = "\033[0m";
    static const char* BLD = "\033[1m";
    static const char* DIM = "\033[2m";
    static const char* RED = "\033[31m";
    static const char* YLW = "\033[33m";
    static const char* GRN = "\033[32m";
    static const char* CYN = "\033[36m";
    static const char* MAG = "\033[35m";
    static const char* BLU = "\033[34m";

    inline std::string wrap(const char* c, const std::string& s)
    { return enabled ? std::string(c) + s + RST : s; }

    inline std::string ok   (const std::string& s) { return wrap(GRN, s); }
    inline std::string warn (const std::string& s) { return wrap(YLW, s); }
    inline std::string err  (const std::string& s) { return wrap(RED, s); }
    inline std::string info (const std::string& s) { return wrap(CYN, s); }
    inline std::string dim  (const std::string& s) { return wrap(DIM, s); }
    inline std::string bold (const std::string& s) { return wrap(BLD, s); }
    inline std::string step (const std::string& s) { return wrap(MAG, s); }
    inline std::string head (const std::string& s) { return wrap(BLU, s); }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Global output state
// ─────────────────────────────────────────────────────────────────────────────
static bool          g_silent  = false;
static bool          g_dry_run = false;
static std::mutex    g_out_mtx;
static std::ofstream g_log_file;
static unsigned      g_jobs    = 0;      // 0 → hardware_concurrency()
static bool          g_clean   = false;
static std::string   g_config  = "release";   // active build configuration

// Global diagnostic counters — atomics, thread-safe without mutex.  [F2]
static std::atomic<unsigned> g_error_count{0};
static std::atomic<unsigned> g_warning_count{0};

static bool line_is_error  (const std::string& s)
{ return s.find(" error:") != std::string::npos || s.find(" fatal:") != std::string::npos; }
static bool line_is_warning(const std::string& s)
{ return s.find(" warning:") != std::string::npos || s.find(" note:") != std::string::npos; }

static void emit_line(const std::string& line, bool to_stderr = false)
{
    bool is_err  = line_is_error(line);
    // Exclusive: a line is a warning only if it isn't already an error.  [A1]
    bool is_warn = !is_err && line_is_warning(line);
    if (is_err)  ++g_error_count;
    if (is_warn) ++g_warning_count;

    std::string col = line;
    if (color::enabled)
    {
        if      (is_err)  col = color::err (line);
        else if (is_warn) col = color::warn(line);
    }
    std::lock_guard<std::mutex> lk(g_out_mtx);
    if (!g_silent)
    {
        auto& st = to_stderr ? std::cerr : std::cout;
        st << col << "\n"; st.flush();
    }
    if (g_log_file.is_open()) { g_log_file << line << "\n"; g_log_file.flush(); }
}

template<typename... Args>
static void mprint(Args&&... args) { emit_line(as_string(std::forward<Args>(args)...)); }

static void emit_captured(const std::string& text, bool to_stderr)
{
    if (text.empty()) return;
    std::istringstream ss(text); std::string line;
    while (std::getline(ss, line)) emit_line(line, to_stderr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ThreadPool — shared by target scheduling and per-target compilation
//  Prevents over-subscription: total live threads == g_jobs.
// ─────────────────────────────────────────────────────────────────────────────
class ThreadPool
{
public:
    explicit ThreadPool(unsigned n)
    {
        for (unsigned i = 0; i < n; ++i)
            workers_.emplace_back([this]{ worker_loop(); });
    }
    ~ThreadPool()
    {
        { std::unique_lock<std::mutex> lk(mtx_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    template<typename F>
    auto submit(F&& f) -> std::future<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (stop_) throw std::runtime_error("ThreadPool is stopped");
            tasks_.emplace([task](){ (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    unsigned size() const { return (unsigned)workers_.size(); }

private:
    void worker_loop()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]{ return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front()); tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    bool                              stop_ = false;
};

static ThreadPool* g_pool = nullptr;   // initialised in main()

// ─────────────────────────────────────────────────────────────────────────────
//  Process helpers
// ─────────────────────────────────────────────────────────────────────────────
struct RunResult { int rc = 0; std::string out; std::string err; };

static void wait_process(TinyProcessLib::Process* p, int& status)
{
    while (!p->try_get_exit_status(status))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

static RunResult RunProcessCapture(const std::string& cmd)
{
    if (g_dry_run)
    {
        emit_line(color::dim("[dry-run] ") + cmd);
        return {0, "", ""};
    }
    RunResult res;
    auto ob = std::make_shared<std::string>();
    auto eb = std::make_shared<std::string>();
    auto mx = std::make_shared<std::mutex>();

    TinyProcessLib::Process* proc = new TinyProcessLib::Process(
        cmd.c_str(), "",
        [ob,mx](const char* b,size_t n){ std::lock_guard<std::mutex> lk(*mx); ob->append(b,n); },
        [eb,mx](const char* b,size_t n){ std::lock_guard<std::mutex> lk(*mx); eb->append(b,n); },
        false);

    if (!proc->StartedOk()) { delete proc; res.rc = -200; return res; }
    wait_process(proc, res.rc);
    delete proc;
    res.out = *ob; res.err = *eb;
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessConsoleOutput
// ─────────────────────────────────────────────────────────────────────────────
struct ProcessConsoleOutput
{
    std::string output, error;
    void        Clear()       { output.clear(); error.clear(); }
    const char* GetStdOut()   { return lua_return_str(output); }
    const char* GetStdError() { return lua_return_str(error);  }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Executable
// ─────────────────────────────────────────────────────────────────────────────
struct Executable
{
    enum InvocationResult : int { CANT_START = -200 };
    Path                 path;
    ProcessConsoleOutput ConsoleOutput;

    void Setpath(Path& p)
    {
        if (p.IsFullPath())  { path = p; return; }
        if (p.Exists())      { path = Path::CurrentDir() + p; return; }
        for (auto& sp : Path::GetSystemPaths())
        { Path f = sp + p; if (f.Exists()) { path = f; return; } }
        mprint(color::err("Cannot find executable: ") + p.ToString());
        std::exit(-1);
    }

    int InvokeCommand(const char* cmd)
    {
        const std::string full = path.ToString() + " " + (cmd ? cmd : "");
        ConsoleOutput.Clear();
        if (g_dry_run) { emit_line(color::dim("[dry-run] ") + full); return 0; }

        auto ob = std::make_shared<std::string>();
        auto eb = std::make_shared<std::string>();
        auto mx = std::make_shared<std::mutex>();

        TinyProcessLib::Process* proc = new TinyProcessLib::Process(
            full.c_str(), "",
            [ob,mx](const char* b,size_t n){ std::lock_guard<std::mutex> lk(*mx); ob->append(b,n); },
            [eb,mx](const char* b,size_t n){ std::lock_guard<std::mutex> lk(*mx); eb->append(b,n); },
            false);

        if (!proc->StartedOk()) { delete proc; return CANT_START; }
        int s = 0; wait_process(proc, s); delete proc;
        ConsoleOutput.output = *ob; ConsoleOutput.error = *eb;
        emit_captured(ConsoleOutput.output, false);
        emit_captured(ConsoleOutput.error,  true);
        return s;
    }

    int InvokeCommandHidden(const char* cmd)
    {
        const std::string full = path.ToString() + " " + (cmd ? cmd : "");
        ConsoleOutput.Clear();
        auto res = RunProcessCapture(full);
        ConsoleOutput.output = res.out; ConsoleOutput.error = res.err;
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
    std::function<const char*(Path&,Path&,const std::vector<Path>&,std::vector<const char*>)>
        GetFileCompileCommandForStaticLibrary,
        GetFileCompileCommand,
        GetFileCompileCommandForSharedLibrary;
};

struct Linker : public Executable
{
    std::function<const char*(std::vector<Path>,Path&,std::vector<Path>)>
        GetObjFilesLinkCommandToStaticLibrary,
        GetObjFilesLinkCommandToExecutable;
    std::function<const char*(std::vector<Path>,Path&,std::vector<Path>,
                              std::vector<Path>,std::vector<const char*>)>
        GetObjFilesLinkCommandToSharedLibrary;
};

// ─────────────────────────────────────────────────────────────────────────────
//  BuildConfig — named set of compiler/linker flags
// ─────────────────────────────────────────────────────────────────────────────
struct BuildConfig
{
    std::string              name;
    std::vector<std::string> cflags;
    std::vector<std::string> ldflags;

    void AddCFlag (const char* f) { if (f) cflags.push_back(f); }
    void AddLDFlag(const char* f) { if (f) ldflags.push_back(f); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ToolChain
// ─────────────────────────────────────────────────────────────────────────────
struct ToolChain
{
    std::string StaticLibraryExtension = ".a";
    std::string SharedLibraryExtension = MBS_SOLIB_EXT;
    std::string ObjectFileExtension    = ".o";

    void SetStaticLibraryExtension(const char* e){ if (e) StaticLibraryExtension = e; }
    void SetSharedLibraryExtension(const char* e){ if (e) SharedLibraryExtension = e; }
    void SetObjectFileExtension   (const char* e){ if (e) ObjectFileExtension    = e; }
    // legacy misspelled aliases
    void SetSahredLibraryExtension(const char* e){ if (e) SharedLibraryExtension = e; }
    void SetStaticLibraryExtention(const char* e){ if (e) StaticLibraryExtension = e; }
    void SetObjectFileExtention   (const char* e){ if (e) ObjectFileExtension    = e; }

    void SetLinkerPath(Path p)
    {
        if (!StaticLinker)  { mprint(color::err("StaticLinker not initialised."));  std::exit(1); }
        if (!DynamicLinker) { mprint(color::err("DynamicLinker not initialised.")); std::exit(1); }
        StaticLinker->Setpath(p); DynamicLinker->Setpath(p);
    }

    Compiler* CCompiler   = nullptr;
    Compiler* CPPCompiler = nullptr;
    Linker*   StaticLinker  = nullptr;
    Linker*   DynamicLinker = nullptr;

    static ToolChain* gcc;
};

// ─────────────────────────────────────────────────────────────────────────────
//  GCC default toolchain (static-duration objects)
// ─────────────────────────────────────────────────────────────────────────────
ToolChain* ToolChain::gcc = []() -> ToolChain*
{
    static ToolChain tc;
    static Compiler  gpp;
    static Linker    ld;

    auto lib_flag = [](const Path& lib) -> std::string
    {
        std::string ls = lib.ToString();
        bool is_path = ls.find('/') != std::string::npos
                    || ls.find('\\') != std::string::npos
                    || (!ls.empty() && ls[0]=='-');
        return is_path ? " " + ls : " -l" + ls;
    };

    // compile_flags: adds -MMD -MF <depfile> automatically for depfile support [N4]
    auto compile_flags = [](Path& src, Path& obj,
                            const std::vector<Path>& inc,
                            std::vector<const char*> extra,
                            bool fpic = false) -> std::string
    {
        // derive depfile path alongside obj file
        std::string dep = obj.ToString();
        { auto d = dep.rfind('.'); if (d!=std::string::npos) dep=dep.substr(0,d); }
        dep += ".d";

        std::string rv = " -c \"" + src.ToString() + "\" -o\"" + obj.ToString() + "\""
                       + " -MMD -MF\"" + dep + "\"";
        for (auto& i : inc)   rv += " -I\"" + i.ToString() + "\"";
        if (fpic)              rv += " -fPIC";
        for (auto& c : extra) { if (c) { rv += " "; rv += c; } }
        return rv;
    };

    gpp.GetFileCompileCommandForStaticLibrary =
        [compile_flags](Path& s,Path& o,const std::vector<Path>& i,std::vector<const char*> e)->const char*
        { auto rv=compile_flags(s,o,i,e,false); mprint(color::dim(rv)); return lua_return_str(rv); };

    gpp.GetFileCompileCommand =
        [compile_flags](Path& s,Path& o,const std::vector<Path>& i,std::vector<const char*> e)->const char*
        { auto rv=compile_flags(s,o,i,e,false); mprint(color::dim(rv)); return lua_return_str(rv); };

    gpp.GetFileCompileCommandForSharedLibrary =
        [compile_flags](Path& s,Path& o,const std::vector<Path>& i,std::vector<const char*> e)->const char*
        { auto rv=compile_flags(s,o,i,e,true); mprint(color::dim(rv)); return lua_return_str(rv); };

    tc.CPPCompiler = &gpp;
    tc.CCompiler   = &gpp;

    ld.GetObjFilesLinkCommandToStaticLibrary =
        [](std::vector<Path> objs,Path& out,std::vector<Path>) -> const char*
        {
            std::string rv = "rcs \"" + out.ToString() + "\"";
            for (auto& o:objs) rv += " \""+o.ToString()+"\"";
            mprint(color::dim(rv)); return lua_return_str(rv);
        };

    ld.GetObjFilesLinkCommandToExecutable =
        [lib_flag](std::vector<Path> objs,Path& out,std::vector<Path> libs) -> const char*
        {
            std::string rv = "-o\"" + out.ToString() + "\"";
            for (auto& o:objs) rv += " \""+o.ToString()+"\"";
            for (auto& l:libs) rv += lib_flag(l);
            mprint(color::dim(rv)); return lua_return_str(rv);
        };

    ld.GetObjFilesLinkCommandToSharedLibrary =
        [lib_flag](std::vector<Path> objs,Path& out,
                   std::vector<Path> libs,std::vector<Path> sp,
                   std::vector<const char*> extra) -> const char*
        {
            std::string rv = "-shared";
            for (auto& s:sp)    rv += " -L\""+s.ToString()+"\"";
            for (auto& o:objs)  rv += " \""+o.ToString()+"\"";
            rv += " -o\""+out.ToString()+"\"";
            for (auto& l:libs)  rv += lib_flag(l);
            for (auto& c:extra) { if (c) { rv+=" "; rv+=c; } }
            mprint(color::dim(rv)); return lua_return_str(rv);
        };

    tc.StaticLinker = tc.DynamicLinker = &ld;
    return &tc;
}();

// ─────────────────────────────────────────────────────────────────────────────
//  InstallRule
// ─────────────────────────────────────────────────────────────────────────────
struct InstallRule
{
    std::string src_glob;   // regex pattern relative to output folder
    std::string dest_dir;   // destination directory (may contain ${prefix})
};

// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
class MiracleExecuter;
struct BuildGraph;

// ─────────────────────────────────────────────────────────────────────────────
//  Target
// ─────────────────────────────────────────────────────────────────────────────
struct Target
{
    // ── identity & type ──────────────────────────────────────────────────────
    std::string Name           = "null";
    std::string OutputFileName = "null";
    std::string Type;   // CONSOLE_APPLICATION | STATIC_LIBRARY | SHARED_LIBRARY

    // ── build configuration ──────────────────────────────────────────────────
    std::vector<Path>        IncludePaths;
    std::vector<Path>        SourceFiles;
    std::vector<Path>        link_libraries;
    std::vector<Path>        link_search_path;
    std::vector<const char*> ExtraCompilerCommands;
    std::vector<const char*> linker_commands;
    std::vector<const char*> AddSourceFilters;   // OR semantics

    ToolChain* BuildToolChain = nullptr;
    Path       WorkingPath;
    Path       OutputFolder;
    Path       IntermediateDir;
    Path       BuildDir;       // [N5] out-of-source build root for this target

    // ── dependency graph [N1] ─────────────────────────────────────────────────
    std::vector<std::string> DependencyNames;   // names of targets this one depends on
    void AddDependency(const char* name) { if (name) DependencyNames.push_back(name); }

    // ── install rules [N7] ────────────────────────────────────────────────────
    std::vector<InstallRule> InstallRules;
    void AddInstallRule(const char* src_glob, const char* dest_dir)
    { if (src_glob && dest_dir) InstallRules.push_back({src_glob, dest_dir}); }

    // ── Lua accessors ─────────────────────────────────────────────────────────
    const char* GetName()           const { return lua_return_str(Name); }
    void        SetName(const char* v)    { Name = v ? v : ""; }
    const char* GetOutputFileName() const { return lua_return_str(OutputFileName); }
    void        SetOutputFileName(const char* v) { OutputFileName = v ? v : ""; }
    const char* GetType()           const { return lua_return_str(Type); }
    void        SetType(const char* v)    { Type = v ? v : ""; }
    void        SetOutputFolder(const char* p) { if (p) OutputFolder = p; }
    void        SetOutputFolder(Path p)        { OutputFolder = p; }
    void        SetBuildDir(const char* p)     { if (p) BuildDir = p; }   // [N5]
    void        SetBuildDir(Path p)            { BuildDir = p; }

    // ── log ───────────────────────────────────────────────────────────────────
    std::string log;
    const char* GetLog()  const { return lua_return_str(log); }
    void        ClearLog()      { log.clear(); }
    template<typename... Args>
    void Log(Args&&... args) { log += as_string(std::forward<Args>(args)...); }

    // ── mutation helpers ──────────────────────────────────────────────────────
    void AddCompilerCommand(const char* c) { if (c) ExtraCompilerCommands.push_back(c); }
    void AddLinkerCommand  (const char* c) { if (c) linker_commands.push_back(c); }
    void AddLinkSearchPath (const char* p) { if (p) link_search_path.push_back(Path(p)); }
    void AddLinkSearchPath (Path p)        { link_search_path.push_back(std::move(p)); }
    void AddSourceFilter   (const char* f) { if (f) AddSourceFilters.push_back(f); }

    void PushSourceFile(Path& p)
    {
        if (AddSourceFilters.empty()) { SourceFiles.push_back(p); return; }
        const char* ps = p.ToStr();
        for (auto& f : AddSourceFilters)
            if (std::regex_match(ps, std::regex(f))) { SourceFiles.push_back(p); return; }
    }

    // ── public API ────────────────────────────────────────────────────────────
    void Clean();
    int  Build();

    // [F1][A7] BuildOrDie: throws std::runtime_error on failure, which kaguya
    // converts to a Lua error at the call boundary — safe for C++ stack unwinding.
    void BuildOrDie();

    void Install(const char* prefix);

    void AddSourceFile(const char* path_str);
    void AddFilesMatchsRegex(const char* regex_expr);
    void AddFilesMatchsRegexInFolder(Path dir, const std::regex& rgx);
    void AddIncludePath(const char* path_str);
    void AddLinkLibrary(const char* path_str);

    // ── pkg-config [N8] ───────────────────────────────────────────────────────
    void AddPkgConfigDep(const char* pkg_name);

    // Stable string storage for dynamically-allocated flags (pkg-config, etc.)
    // deque<string> guarantees that push_back NEVER invalidates pointers/references
    // to existing elements — unlike vector which may reallocate. This means the
    // c_str() pointers we store into ExtraCompilerCommands/linker_commands remain
    // valid for the lifetime of the Target.  [F7]
    std::deque<std::string> owned_flags;

private:
    // Canonical obj filename — full source path mangled to avoid collisions.
    std::string ObjFilenameFor(const Path& src) const
    {
        std::string s = src.ToString();
        std::replace(s.begin(),s.end(),'\\','/');
        if (s.size()>1 && s[1]==':') s[1]='_';
        std::replace(s.begin(),s.end(),'/','_');
        auto d = s.rfind('.');
        if (d!=std::string::npos) s=s.substr(0,d);
        return s + (BuildToolChain ? BuildToolChain->ObjectFileExtension : ".o");
    }

    // Resolve obj directory, accounting for out-of-source and config.  [N5][N6]
    Path ResolveObjDir();

    // Resolve final output file path.
    Path ResolveOutputFile();

    // Compute build fingerprint for flag-change detection.  [N3]
    std::string ComputeFingerprint(const std::vector<const char*>& extra_config_flags);

    // Check and update the stored fingerprint; return true if all files must recompile.
    bool FingerprintChanged(const Path& obj_dir,
                            const std::vector<const char*>& extra_config_flags);
};

// ─────────────────────────────────────────────────────────────────────────────
//  BuildGraph — central registry, dependency resolution, parallel scheduling
// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations for graph wrapper functions
static void graph_Register(Target* t);
static int graph_BuildAll();
static int graph_BuildOne(const char* name);
static void graph_CleanAll();
static void graph_SetGlobalBuildDir(const char* p);
static BuildConfig* graph_AddConfig(const char* name);
static int graph_Install(const char* prefix);

struct BuildGraph
{
    std::map<std::string, Target*>                      targets;   // ordered for determinism
    std::map<std::string, std::unique_ptr<BuildConfig>> configs;   // unique_ptr fixes leak [F8]
    Path                                                global_build_dir;  // [N5]
    std::string                                         only_target_override;  // [F6] set by --target

    static BuildGraph& Get()
    {
        static BuildGraph inst;
        return inst;
    }

    // ── target registry ───────────────────────────────────────────────────────
    void Register(Target* t)
    {
        if (!t || t->Name.empty() || t->Name == "null")
        {
            mprint(color::warn("RegisterTarget: target has no name, skipping."));
            return;
        }
        if (targets.count(t->Name))
            mprint(color::warn("RegisterTarget: '" + t->Name + "' already registered, replacing."));
        targets[t->Name] = t;
        // Propagate global build dir if target hasn't set its own
        if (!t->BuildDir.IsSet() && global_build_dir.IsSet())
            t->BuildDir = global_build_dir;
    }

    void SetGlobalBuildDir(const char* p)
    {
        if (!p) return;
        global_build_dir = p;
        for (auto& kv : targets)
            if (!kv.second->BuildDir.IsSet()) kv.second->BuildDir = global_build_dir;
    }

    // ── config management [N6] ─────────────────────────────────────────────────
    BuildConfig* AddConfig(const char* name)
    {
        if (!name) return nullptr;
        auto uptr = std::make_unique<BuildConfig>();
        uptr->name = name;
        auto* raw = uptr.get();
        configs[name] = std::move(uptr);   // unique_ptr owns it; no leak [F8]
        return raw;
    }
    BuildConfig* GetConfig(const char* name)
    {
        if (!name) return nullptr;
        auto it = configs.find(name);
        return it != configs.end() ? it->second.get() : nullptr;
    }

    // ── topological sort (Kahn's algorithm) [N1] ──────────────────────────────
    // Returns targets in build order, or empty on cycle detection.
    std::vector<Target*> TopologicalOrder()
    {
        std::unordered_map<std::string,int> in_degree;
        std::unordered_map<std::string,std::vector<std::string>> adj; // name → dependents

        for (auto& kv : targets) in_degree[kv.first] = 0;
        for (auto& kv : targets)
        {
            for (auto& dep : kv.second->DependencyNames)
            {
                if (!targets.count(dep))
                {
                    mprint(color::err("Target '" + kv.first + "' depends on unknown target '" + dep + "'"));
                    return {};
                }
                adj[dep].push_back(kv.first);
                in_degree[kv.first]++;
            }
        }

        std::queue<std::string> q;
        for (auto& kv : in_degree) if (kv.second == 0) q.push(kv.first);

        std::vector<Target*> order;
        while (!q.empty())
        {
            std::string name = q.front(); q.pop();
            order.push_back(targets[name]);
            for (auto& succ : adj[name])
                if (--in_degree[succ] == 0) q.push(succ);
        }

        if (order.size() != targets.size())
        {
            mprint(color::err("Dependency cycle detected! Involved targets:"));
            for (auto& kv : in_degree)
                if (kv.second > 0) mprint("  " + color::err(kv.first));
            return {};
        }
        return order;
    }

    // ── build all targets respecting dependency order [N2] ────────────────────
    // Build a specific subset of targets (by name) plus their transitive deps,
    // or all targets if names is empty.
    int BuildAll()
    {
        std::string only_target = "";
        // --target CLI flag takes precedence over any argument passed in Lua.  [F6]
        const std::string& filter = only_target_override.empty() ? only_target : only_target_override;

        auto order = TopologicalOrder();
        if (order.empty() && !targets.empty()) return 1;

        // Prune order to just the requested target + its transitive deps.
        if (!filter.empty())
        {
            if (!targets.count(filter))
            {
                mprint(color::err("--target: unknown target '") + filter + "'");
                return 1;
            }
            std::unordered_set<std::string> needed;
            std::queue<std::string> bfs;
            bfs.push(filter);
            while (!bfs.empty())
            {
                std::string n = bfs.front(); bfs.pop();
                if (!needed.insert(n).second) continue;
                for (auto& dep : targets[n]->DependencyNames) bfs.push(dep);
            }
            std::vector<Target*> pruned;
            for (auto* t : order)
                if (needed.count(t->Name)) pruned.push_back(t);
            order = std::move(pruned);
        }

        // Recompute in-degrees for the (possibly pruned) order
        std::unordered_map<std::string,int> in_deg;
        std::unordered_map<std::string,std::vector<std::string>> adj;
        std::unordered_set<std::string> in_order_set;
        for (auto* t : order) { in_deg[t->Name] = 0; in_order_set.insert(t->Name); }
        for (auto* t : order)
            for (auto& dep : t->DependencyNames)
                if (in_order_set.count(dep))
                { adj[dep].push_back(t->Name); in_deg[t->Name]++; }

        std::queue<std::string> ready;
        for (auto* t : order) if (in_deg[t->Name] == 0) ready.push(t->Name);

        std::atomic<int> global_rc{0};
        std::mutex       sched_mtx;
        std::unordered_set<std::string> started_targets;   // for D7 skip report

        while (!ready.empty() && global_rc.load()==0)
        {
            std::vector<std::pair<std::string,std::shared_future<int>>> batch;
            while (!ready.empty())
            {
                std::string name = ready.front(); ready.pop();
                started_targets.insert(name);
                Target* t = targets[name];
                auto fut = g_pool->submit([t, &global_rc]() -> int {
                    if (global_rc.load() != 0) return 0;
                    int rc = t->Build();
                    if (rc != 0) global_rc.store(rc);
                    return rc;
                });
                batch.push_back({name, fut.share()});
            }
            for (auto& [name, fut] : batch)
            {
                fut.get();
                std::lock_guard<std::mutex> lk(sched_mtx);
                for (auto& succ : adj[name])
                    if (--in_deg[succ] == 0) ready.push(succ);
            }
        }

        // D7: report every target in the build set that never started.
        // This covers both targets blocked by unmet deps (in_deg > 0) and
        // targets that became ready but were skipped because global_rc was set.
        if (global_rc.load() != 0)
        {
            std::vector<std::string> skipped;
            for (auto* t : order)
                if (!started_targets.count(t->Name)) skipped.push_back(t->Name);
            if (!skipped.empty())
            {
                mprint(color::warn("Skipped due to failure:"));
                for (auto& s : skipped) mprint(color::warn("  • ") + s);
            }
        }

        return global_rc.load();
    }

    // Convenience: build a single named target + its deps
    int BuildOne(const char* name)
    {
        std::string old_override = only_target_override;
        if (name) only_target_override = name;
        int rc = BuildAll();
        only_target_override = old_override;
        return rc;
    }

    // ── install [N7] ──────────────────────────────────────────────────────────
    int Install(const char* prefix_ptr)
    {
        std::string prefix = prefix_ptr ? prefix_ptr : "";
        mprint(color::bold("Installing to: ") + prefix);
        auto order = TopologicalOrder();
        if (order.empty() && !targets.empty()) return 1;
        for (auto* t : order) t->Install(prefix.c_str());
        return 0;
    }

    // D5: list all registered target names
    std::vector<std::string> ListTargets() const
    {
        std::vector<std::string> names;
        names.reserve(targets.size());
        for (auto& kv : targets) names.push_back(kv.first);
        return names;
    }

    // ── clean all ─────────────────────────────────────────────────────────────
    void CleanAll()
    {
        for (auto& kv : targets) kv.second->Clean();
    }
};

// Wrapper functions for Lua binding (avoids kaguya lambda issues)
static void graph_Register(Target* t) { BuildGraph::Get().Register(t); }
static int graph_BuildAll() { mprint("graph_BuildAll called"); return BuildGraph::Get().BuildAll(); }
static int graph_BuildOne(const char* name) { return BuildGraph::Get().BuildOne(name); }
static void graph_CleanAll() { BuildGraph::Get().CleanAll(); }
static void graph_SetGlobalBuildDir(const char* p) { BuildGraph::Get().SetGlobalBuildDir(p); }
static BuildConfig* graph_AddConfig(const char* name) { return BuildGraph::Get().AddConfig(name); }
static int graph_Install(const char* prefix) { return BuildGraph::Get().Install(prefix); }

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

    void AddSearchPath(Path& p)
    { if (p.Exists()) SearchPaths.push_back(p); }

    void AddSearchPathRecursively(Path& p)
    {
        if (!p.Exists()) return;
        SearchPaths.push_back(p);
        for (auto& sub : p.Browse()) AddSearchPathRecursively(sub);
    }

    bool LoadFile(const char* path)
    {
        std::ifstream file(path);
        if (!file.is_open()) { mprint(color::err("Cannot open script: ") + path); return false; }
        std::string src((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        file.close();

        Path fp=path, full=fp.AsFullPath(), parent=full.GetParent();
        Path::PushPath(parent);
        ProjectSourcePath = parent;
        Executer::PrepareExecution();

        // [N9] Lua error handling — capture traceback
        bool ok = state.dostring(variables.Eval(src.c_str()));
        if (!ok)
        {
            // kaguya stores the last error in the state; retrieve it
            std::string lua_err = lua_tostring(state.state(), -1);
            mprint(color::err("Lua error in script '") + path + "':");
            mprint(color::err(lua_err));
            lua_pop(state.state(), 1);
            Path::PopPath();
            return false;
        }
        Path::PopPath();
        return true;
    }

    std::string GetErrorString() { return ""; }
    void SetVariable(std::string var, std::string val) { Executer::SetVariable(var,val); }

    // ── dependency scanner [N4] ───────────────────────────────────────────────
    // Returns all transitively included files.
    // Prefers depfile (.d) if it exists next to the obj; falls back to hand-scanner.
    std::vector<Path> GetFileIncludes(const Path& file,
                                      const std::vector<Path>& inc = {},
                                      const std::string& depfile_path = "")
    {
        // If a compiler-generated depfile exists, prefer it — it's authoritative.  [N4]
        // Guard: if the depfile is empty or yields ≤1 entry (source only, no headers),
        // it may be a partial write from a crashed compile — fall through to hand-scanner.  [F4]
        if (!depfile_path.empty())
        {
            std::ifstream df(depfile_path);
            if (df.is_open())
            {
                std::string content((std::istreambuf_iterator<char>(df)),
                                     std::istreambuf_iterator<char>());
                df.close();
                std::vector<Path> deps;
                deps.push_back(file);

                // On Windows, paths start with a drive letter followed by ':'
                // (e.g. "D:/..."). Skip such drive-letter colons to find the
                // actual make-rule separator ':' (which is followed by space/newline).
                size_t colon = content.find(':');
                while (colon != std::string::npos) {
                    char nc = (colon + 1 < content.size()) ? content[colon + 1] : '\0';
                    if (nc != '/' && nc != '\\') break;
                    colon = content.find(':', colon + 1);
                }
                if (colon != std::string::npos)
                {
                    std::string rest = content.substr(colon + 1);
                    // Fold backslash-newline continuations into spaces
                    for (size_t i = 0; i + 1 < rest.size(); ++i)
                        if (rest[i] == '\\' && rest[i+1] == '\n')
                            { rest[i] = ' '; rest[i+1] = ' '; }
                    std::istringstream ss(rest);
                    std::string tok;
                    while (ss >> tok)
                    {
                        if (tok == "\\") continue;
                        Path dp = tok.c_str();
                        if (dp.Exists() && dp != file)
                            deps.push_back(dp);
                    }
                }

                // If we parsed at least the source file plus one header, trust it.
                // A depfile with only the source itself is suspicious (crashed compile). [F4]
                if (deps.size() > 1)
                    return deps;
                // Fall through to hand-scanner for suspicious/empty depfiles
            }
        }
        // Fallback: hand-scanner (also used when no depfile exists yet — first build)
        std::unordered_set<std::string> visited;
        std::vector<Path> result;
        ScanIncludes(file, inc, visited, result);
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

        static auto cfg_cls =
            kaguya::UserdataMetatable<BuildConfig>()
                .setConstructors<BuildConfig()>()
                .addFunction("AddCFlag",  &BuildConfig::AddCFlag)
                .addFunction("AddLDFlag", &BuildConfig::AddLDFlag);
        state["BuildConfig"].setClass(cfg_cls);

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
            kaguya::UserdataMetatable<Compiler,Executable>()
                .setConstructors<Compiler()>()
                .addFunction("GetPath", &Compiler::Getpath)
                .addProperty("GetFileCompileCommandForStaticLibrary",
                             &Compiler::GetFileCompileCommandForStaticLibrary)
                .addProperty("GetFileCompileCommandForSharedLibrary",
                             &Compiler::GetFileCompileCommandForSharedLibrary)
                .addFunction("GetFileCompileCommand",&Compiler::GetFileCompileCommand);
        state["Compiler"].setClass(cmp_cls);

        static auto lnk_cls =
            kaguya::UserdataMetatable<Linker,Executable>()
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
                .addFunction("AddLinkCommand",      &Target::AddLinkerCommand)
                .addFunction("AddLinkerCommand",    &Target::AddLinkerCommand)
                .addFunction("AddFilesMatchsRegex", &Target::AddFilesMatchsRegex)
                .addFunction("Build",               &Target::Build)
                .addFunction("BuildOrDie",          &Target::BuildOrDie)
                .addFunction("Clean",               &Target::Clean)
                .addFunction("AddDependency",        &Target::AddDependency)
                .addFunction("AddInstallRule",       &Target::AddInstallRule)
                .addFunction("AddPkgConfigDep",     &Target::AddPkgConfigDep)
                .addFunction("GetOutputFileName",   &Target::GetOutputFileName)
                .addFunction("SetOutputFileName",   &Target::SetOutputFileName)
                .addFunction("GetName",             &Target::GetName)
                .addFunction("SetName",             &Target::SetName)
                .addFunction("AddIncludePath",      &Target::AddIncludePath)
                .addFunction("GetLog",              &Target::GetLog)
                .addFunction("ClearLog",            &Target::ClearLog)
                .addFunction("AddCompilerCommand",  &Target::AddCompilerCommand)
                .addOverloadedFunctions("SetOutputFolder",
                    (void(Target::*)(const char*))&Target::SetOutputFolder,
                    (void(Target::*)(Path))&Target::SetOutputFolder)
                .addOverloadedFunctions("SetBuildDir",
                    (void(Target::*)(const char*))&Target::SetBuildDir,
                    (void(Target::*)(Path))&Target::SetBuildDir)
                .addOverloadedFunctions("AddLinkSearchPath",
                    (void(Target::*)(const char*))&Target::AddLinkSearchPath,
                    (void(Target::*)(Path))&Target::AddLinkSearchPath)
                .addFunction("AddSourceFilter",     &Target::AddSourceFilter)
                .addProperty("BuildToolChain",      &Target::BuildToolChain)
                .addProperty("WorkingPath",         &Target::WorkingPath)
                .addProperty("AddSourceFilters",    &Target::AddSourceFilters)
                .addProperty("Type",                Target::GetType, Target::SetType);
        state["Target"].setClass(tgt_cls);

        // Create a simple table for graph with function wrappers
        state["graph"] = kaguya::NewTable();
        state["graph"]["Register"] = &graph_Register;
        state["graph"]["BuildAll"] = &graph_BuildAll;
        state["graph"]["BuildOne"] = &graph_BuildOne;
        state["graph"]["CleanAll"] = &graph_CleanAll;
        state["graph"]["SetGlobalBuildDir"] = &graph_SetGlobalBuildDir;
        state["graph"]["AddConfig"] = &graph_AddConfig;
        state["graph"]["Install"] = &graph_Install;

        static auto bs_cls =
            kaguya::UserdataMetatable<MiracleExecuter>()
                .setConstructors<MiracleExecuter()>()
                .addFunction("AddSearchPath",            &MiracleExecuter::AddSearchPath)
                .addFunction("AddSearchPathRecursively", &MiracleExecuter::AddSearchPathRecursively)
                .addFunction("GetProjectSourcePath",     &MiracleExecuter::GetProjectSourcePath);
        state["BuildSystem"].setClass(bs_cls);

        state["bs"]            = this;
        state["GCC_TOOLCHAIN"] = ToolChain::gcc;

        // Expose active config + dry-run flag to Lua
        state["MBS_CONFIG"]  = g_config.c_str();  // Use .c_str() for safety [F6]
        state["MBS_DRY_RUN"] = g_dry_run;

        // D1: Lua helper functions for coloured output from build scripts
        state["mbs_print"] = kaguya::function([](const char* msg)
            { mprint(msg ? msg : ""); });
        state["mbs_warn"]  = kaguya::function([](const char* msg)
            { mprint(color::warn(msg ? msg : "")); });
        state["mbs_error"] = kaguya::function([](const char* msg)
            { mprint(color::err(msg ? msg : "")); });
    }

    static MiracleExecuter*& GetExecuterRef()
    {
        static MiracleExecuter* inst = nullptr;
        return inst;
    }

    static MiracleExecuter* GetExecuter()
    {
        static std::once_flag once;
        std::call_once(once, []{
            GetExecuterRef() = new MiracleExecuter();
            GetExecuterRef()->InitLuaState();
        });
        return GetExecuterRef();
    }

    // Delete and reset the singleton pointer to avoid dangling pointer issues.
    // Must be called at program exit before deleting the thread pool.
    static void DeleteExecuter()
    {
        delete GetExecuterRef();
        GetExecuterRef() = nullptr;
    }

private:
    void ScanIncludes(const Path& file, const std::vector<Path>& inc,
                      std::unordered_set<std::string>& visited, std::vector<Path>& result)
    {
        // Use std::filesystem for canonical key to handle ".." and symlinks
        std::string key;
        try { key = std::filesystem::weakly_canonical(file.ToStr()).string(); }
        catch (...) { key = file.ToString(); }

        if (!visited.insert(key).second) return;
        result.push_back(file);
        if (!file.Exists()) return;

        std::ifstream in(file.ToStr());
        if (!in.is_open()) return;
        std::string line;
        while (std::getline(in, line))
        {
            size_t i=0;
            while (i<line.size()&&(line[i]==' '||line[i]=='\t')) ++i;
            if (i>=line.size()||line[i]!='#') continue;
            ++i;
            while (i<line.size()&&(line[i]==' '||line[i]=='\t')) ++i;
            if (line.compare(i,7,"include")!=0) continue;
            i+=7;
            while (i<line.size()&&(line[i]==' '||line[i]=='\t')) ++i;
            if (i>=line.size()) continue;
            char op=line[i++]; if (op!='"'&&op!='<') continue;
            char cl=(op=='"')?'"':'>';
            bool local=(op=='"');
            size_t end=line.find(cl,i);
            if (end==std::string::npos) continue;
            std::string name=line.substr(i,end-i);
            if (name.empty()) continue;

            bool found=false;
            if (local) { Path c=file.GetParent()+name; if(c.Exists()){ScanIncludes(c,inc,visited,result);found=true;} }
            if (!found)
                for (auto& ip:inc) { Path c=ip+name; if(c.Exists()){ScanIncludes(c,inc,visited,result);found=true;break;} }
        }
    }
};

// =============================================================================
//  Target implementations
// =============================================================================

// [F1] BuildOrDie — throws std::runtime_error on failure, which kaguya converts
// to a Lua error at the call boundary. This avoids calling lua_error() directly
// from C++ code (which does a longjmp past C++ destructors — undefined behaviour).
void Target::BuildOrDie()
{
    int rc = Build();
    if (rc != 0)
        throw std::runtime_error(
            "Build failed for target '" + Name + "' (rc=" + std::to_string(rc) + ")");
}

Path Target::ResolveObjDir()
{
    IntermediateDir = Path();
    Path base;
    if (BuildDir.IsSet())        base = BuildDir;
    else if (WorkingPath.IsSet())base = WorkingPath;
    else base = MiracleExecuter::GetExecuter()->GetProjectSourcePath();

    const std::string& sub = (Name!="null"&&!Name.empty()) ? Name : OutputFileName;
    // [N6] Config name injected into obj path → parallel debug/release builds
    Path dir = (base + "obj") + g_config;
    dir = dir + sub;
    IntermediateDir = dir;
    return dir;
}

Path Target::ResolveOutputFile()
{
    Path base;
    if (BuildDir.IsSet())   base = BuildDir;
    else if (OutputFolder.IsSet()) base = OutputFolder;
    else base = MiracleExecuter::GetExecuter()->GetProjectSourcePath();

    Path out = base + OutputFileName;
    if (out.GetExtension().empty())
    {
        if      (Type=="CONSOLE_APPLICATION") out.SetExtention(MBS_EXE_EXT);
        else if (Type=="STATIC_LIBRARY")      out.SetExtention(BuildToolChain->StaticLibraryExtension);
        else if (Type=="SHARED_LIBRARY")      out.SetExtention(BuildToolChain->SharedLibraryExtension);
        else { mprint(color::err("Unknown target type: '") + Type + "'"); return Path(); }
    }
    return out;
}

std::string Target::ComputeFingerprint(const std::vector<const char*>& extra_config_flags)
{
    // Hash: toolchain path + sorted include paths + all compiler flags
    std::vector<std::string> parts;
    if (BuildToolChain && BuildToolChain->CPPCompiler)
        parts.push_back(BuildToolChain->CPPCompiler->path.ToString());

    std::vector<std::string> incs;
    for (auto& p : IncludePaths) incs.push_back(p.ToString());
    std::sort(incs.begin(), incs.end());
    for (auto& s : incs) parts.push_back(s);

    for (auto* f : ExtraCompilerCommands) parts.push_back(f);
    for (auto* f : extra_config_flags)    parts.push_back(f);
    parts.push_back(g_config);

    uint64_t h = fnv1a_many(parts);
    std::ostringstream ss; ss << std::hex << h;
    return ss.str();
}

bool Target::FingerprintChanged(const Path& obj_dir,
                                const std::vector<const char*>& extra_config_flags)
{
    std::string computed = ComputeFingerprint(extra_config_flags);
    // Use Path operator+ to avoid mixed '/' and '\' separators on Windows.  [A2]
    Path fp_path_obj = obj_dir + ".mbs_fingerprint";
    std::string fp_path = fp_path_obj.ToString();

    std::ifstream f(fp_path);
    if (f.is_open())
    {
        std::string stored; f >> stored; f.close();
        if (stored == computed) return false;
        mprint(color::warn("  [" + Name + "] Build flags changed — forcing full recompile."));
    }
    // Write new fingerprint
    std::ofstream out(fp_path, std::ios::trunc);
    if (out.is_open()) out << computed;
    return true;
}

void Target::Clean()
{
    Path obj_dir = ResolveObjDir();
    if (!obj_dir.Exists()) { mprint(color::dim("  ["+Name+"] Nothing to clean.")); return; }
    mprint(color::warn("  ["+Name+"] Cleaning: ") + obj_dir.ToString());
    // Delete all files including .o and .d depfiles.  [F4]
    for (auto& f : obj_dir.Browse()) f.Delete();
    // Use Path operator+ to avoid mixed separators on Windows.  [A2]
    Path fp = obj_dir + ".mbs_fingerprint";
    if (fp.Exists()) fp.Delete();
}

void Target::Install(const char* prefix_ptr)
{
    std::string prefix = prefix_ptr ? prefix_ptr : "";
    if (InstallRules.empty()) return;
    Path out = ResolveOutputFile();
    for (auto& rule : InstallRules)
    {
        // Replace ${prefix} in dest
        std::string dest = rule.dest_dir;
        size_t pos;
        while ((pos = dest.find("${prefix}")) != std::string::npos)
            dest.replace(pos, 9, prefix);

        Path dest_path = dest.c_str();
        Path::MakeIfDoesntExit(dest_path);

        // Find files matching rule.src_glob in output folder
        std::regex rgx(rule.src_glob);
        Path out_folder = out.GetParent();
        for (auto& rel : out_folder.BrowseRelative())
        {
            if (!std::regex_match(rel.ToStr(), rgx)) continue;
            Path src = (const Path&)out_folder + (const Path&)rel;
            std::string cp_cmd;
#if MBS_WINDOWS
            cp_cmd = "copy /Y \"" + src.ToString() + "\" \"" + ((const Path&)dest_path + (const Path&)rel).ToString() + "\"";
#else
            cp_cmd = "cp -f \"" + src.ToString() + "\" \"" + ((const Path&)dest_path + (const Path&)rel).ToString() + "\"";
#endif
            mprint(color::info("  install: ") + src.ToString() + " → " + ((const Path&)dest_path + (const Path&)rel).ToString());
            auto res = RunProcessCapture(cp_cmd);
            if (res.rc != 0) mprint(color::err("  install failed: ") + res.err);
        }
    }
}

// ── Build ─────────────────────────────────────────────────────────────────────

int Target::Build()
{
    auto t0 = std::chrono::steady_clock::now();

    if (!BuildToolChain)
        { log="Toolchain not set."; mprint(color::err(log)); return 1; }
    if (BuildToolChain->ObjectFileExtension.empty())
        { log="Toolchain obj extension not set."; mprint(color::err(log)); return 1; }
    if (SourceFiles.empty())
        { Log("No source files in '",Name,"'."); mprint(color::err(log)); return 1; }

    if (g_clean) Clean();

    // Reset per-target diagnostic counters so the summary is per-target.  [F2]
    unsigned errs_before  = g_error_count.load();
    unsigned warns_before = g_warning_count.load();

    Path ObjDir      = ResolveObjDir();
    Path output_file = ResolveOutputFile();
    if (!output_file.IsSet()) return 1;
    Path::MakeIfDoesntExit(ObjDir);

    enum class TT { CONSOLE, STATIC_LIB, SHARED_LIB };
    TT tt = (Type=="CONSOLE_APPLICATION") ? TT::CONSOLE :
            (Type=="SHARED_LIBRARY")      ? TT::SHARED_LIB : TT::STATIC_LIB;

    // Collect active config flags (they were injected by BuildGraph::BuildAll or direct Build())
    std::vector<const char*> active_config_flags;
    if (auto* cfg = BuildGraph::Get().GetConfig(g_config.c_str()))
        for (auto& f : cfg->cflags) active_config_flags.push_back(f.c_str());

    // [N3] Fingerprint check — force full recompile if flags changed
    bool force_recompile = FingerprintChanged(ObjDir, active_config_flags);

    // Merge config flags into compiler commands for this build
    std::vector<const char*> all_cflags = ExtraCompilerCommands;
    for (auto* f : active_config_flags) all_cflags.push_back(f);

    // ── Phase 1: decide what to compile ──────────────────────────────────────
    struct PendingCompile { Path src; Path obj; std::string depfile; std::string cmd; };
    std::vector<PendingCompile> pending;
    std::vector<Path>           all_objs;
    all_objs.reserve(SourceFiles.size());
    int skipped = 0;

    for (auto& f : SourceFiles)
    {
        std::string obj_name = ObjFilenameFor(f);
        Path obj = ObjDir + obj_name;
        all_objs.push_back(obj);

        // Derive depfile path (same as obj, different extension)
        std::string dep_path = obj.ToString();
        { auto d=dep_path.rfind('.'); if(d!=std::string::npos) dep_path=dep_path.substr(0,d); }
        dep_path += ".d";

        bool need = force_recompile || !obj.Exists();
        if (!need)
        {
            // [N4] Use depfile if available, else hand-scanner
            auto deps = MiracleExecuter::GetExecuter()
                            ->GetFileIncludes(f, IncludePaths, dep_path);
            for (auto& dep : deps)
                if (obj.GetLastModificationTime() < dep.GetLastModificationTime())
                    { need = true; break; }
        }
        if (!need) { ++skipped; continue; }

        Compiler* cmp = (f.GetExtension()==".c")
                        ? BuildToolChain->CCompiler : BuildToolChain->CPPCompiler;

        std::string args;
        if      (tt==TT::STATIC_LIB)
            args = cmp->GetFileCompileCommandForStaticLibrary(f,obj,IncludePaths,all_cflags);
        else if (tt==TT::CONSOLE)
            args = cmp->GetFileCompileCommand(f,obj,IncludePaths,all_cflags);
        else
            args = cmp->GetFileCompileCommandForSharedLibrary(f,obj,IncludePaths,all_cflags);

        pending.push_back({ f, obj, dep_path, cmp->path.ToString()+" "+args });
    }

    // [F5][A6] Detect stale .o files. Use canonical paths for comparison
    // so Browse() output and ObjDir+name always match regardless of form.
    {
#ifdef _WIN32
        // Windows filesystem is case-insensitive: normalise to lower-case so
        // files that differ only in case are not falsely treated as stale.
        auto path_key = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            return s;
        };
#else
        auto path_key = [](const std::string& s) { return s; };
#endif
        std::unordered_set<std::string> expected_objs;
        for (auto& o : all_objs)
        {
            try { expected_objs.insert(path_key(std::filesystem::weakly_canonical(o.ToStr()).string())); }
            catch (...) { expected_objs.insert(path_key(o.ToString())); }
        }
        if (ObjDir.Exists())
        {
            for (auto& f : ObjDir.Browse())
            {
                if (f.GetExtension() != BuildToolChain->ObjectFileExtension) continue;
                std::string fc;
                try { fc = path_key(std::filesystem::weakly_canonical(f.ToStr()).string()); }
                catch (...) { fc = path_key(f.ToString()); }
                if (!expected_objs.count(fc))
                {
                    mprint(color::warn("  [stale] Removing orphaned object: ") + f.ToString());
                    f.Delete();
                    std::string dp = f.ToString();
                    auto d = dp.rfind('.'); if (d != std::string::npos) dp = dp.substr(0,d);
                    Path dpath = (dp + ".d").c_str();
                    if (dpath.Exists()) dpath.Delete();
                }
            }
        }
    }

    const size_t n_pend = pending.size();
    mprint(color::head("  ┌─ ") + color::bold(Name) + " [" + g_config + "]  " +
           std::to_string(n_pend) + " to compile, " + std::to_string(skipped) + " up to date");

    // ── Phase 2: parallel compilation via shared thread pool ─────────────────
    std::atomic<bool> failed{false};
    int               fail_rc = 0;
    std::string       fail_src;
    std::mutex        fail_mtx;

    if (!pending.empty())
    {
        std::vector<std::future<RunResult>> futs;
        futs.reserve(n_pend);

        for (size_t i = 0; i < n_pend && !failed; ++i)
        {
            pending[i].obj.Delete();
            std::string cmd = pending[i].cmd;
            size_t      num = i + 1;
            std::string src_s = pending[i].src.ToString();

            mprint(color::step("  │  [") + std::to_string(num) + "/" +
                   std::to_string(n_pend) + "] " + src_s);

            // Submit to shared pool — no over-subscription regardless of #targets
            futs.push_back(g_pool->submit([cmd]{ return RunProcessCapture(cmd); }));
        }

        for (size_t i = 0; i < futs.size(); ++i)
        {
            RunResult res = futs[i].get();
            emit_captured(res.out, false);
            emit_captured(res.err, true);
            if (res.rc != 0 && !failed.exchange(true))
            {
                std::lock_guard<std::mutex> lk(fail_mtx);
                fail_rc  = res.rc;
                fail_src = pending[i].src.ToString();
            }
        }
    }

    if (failed)
    {
        log = as_string("Compile failed (rc=",fail_rc,"): ",fail_src);
        mprint(color::err("  └─ FAILED: ") + log);
        return fail_rc;
    }

    // ── incremental link guard ────────────────────────────────────────────────
    if (n_pend == 0 && output_file.Exists())
    {
        auto dur = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        mprint(color::ok("  └─ Up to date.") + color::dim("  (" +
               std::to_string(dur).substr(0,4) + "s)"));
        return 0;
    }

    mprint(color::bold("  │  [link] ") + output_file.ToString());
    output_file.Delete();

    // Collect active linker flags from config
    std::vector<const char*> all_ldflags = linker_commands;
    if (auto* cfg = BuildGraph::Get().GetConfig(g_config.c_str()))
        for (auto& f : cfg->ldflags) all_ldflags.push_back(f.c_str());

    // ── link ──────────────────────────────────────────────────────────────────
    int lrc = 0;
    if (tt == TT::CONSOLE)
    {
        lrc = BuildToolChain->StaticLinker->InvokeCommand(
            BuildToolChain->StaticLinker->GetObjFilesLinkCommandToExecutable(
                all_objs, output_file, link_libraries));
    }
    else if (tt == TT::STATIC_LIB)
    {
        lrc = BuildToolChain->StaticLinker->InvokeCommand(
            BuildToolChain->StaticLinker->GetObjFilesLinkCommandToStaticLibrary(
                all_objs, output_file, link_libraries));
    }
    else  // SHARED_LIBRARY
    {
        lrc = BuildToolChain->CPPCompiler->InvokeCommand(
            BuildToolChain->DynamicLinker->GetObjFilesLinkCommandToSharedLibrary(
                all_objs, output_file, link_libraries, link_search_path, all_ldflags));
    }

    if (lrc != 0)
    {
        log = as_string("Link failed (rc=",lrc,") for '",Name,"'");
        mprint(color::err("  └─ FAILED: ") + log);
        return lrc;
    }

    auto dur = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
    unsigned t_errs  = g_error_count.load()   - errs_before;
    unsigned t_warns = g_warning_count.load() - warns_before;
    std::string diag;
    if (t_errs  > 0) diag += color::err(" " + std::to_string(t_errs)  + " error(s)");
    if (t_warns > 0) diag += color::warn(" " + std::to_string(t_warns) + " warning(s)");
    mprint(color::ok("  └─ OK  ") + output_file.ToString() + color::dim(
           "  (" + std::to_string(n_pend) + " compiled, " +
           std::to_string(skipped) + " skipped, " +
           std::to_string(dur).substr(0,4) + "s)") + diag);
    return 0;
}

// ── AddLinkLibrary ─────────────────────────────────────────────────────────────

void Target::AddLinkLibrary(const char* path_str)
{
    if (!path_str) return;
    Path p = path_str;
    if (p.Exists()) { link_libraries.push_back(Path::CurrentDir()+p); return; }
    for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
    { Path r=sp+p; if(r.Exists()){link_libraries.push_back(r);return;} }
    link_libraries.push_back(path_str);
}

// ── AddSourceFile ─────────────────────────────────────────────────────────────

void Target::AddSourceFile(const char* path_str)
{
    if (!path_str) return;
    struct E { std::string msg; };
    try
    {
        Path p = path_str;
        if (p.IsFullPath()) { if(p.Exists()){PushSourceFile(p);return;} throw E{p.ToString()+" does not exist"}; }
        for (Path base : {Path::CurrentDir(), MiracleExecuter::GetExecuter()->GetProjectSourcePath()})
        { Path f=base+p; if(f.Exists()){PushSourceFile(f);return;} }
        for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
        { Path f=sp+p; if(f.Exists()){PushSourceFile(f);return;} }
        throw E{std::string(path_str)+" cannot be located."};
    }
    catch (E& e) { mprint(color::err("AddSourceFile: ")+e.msg); std::exit(1); }
}

// ── AddFilesMatchsRegex ────────────────────────────────────────────────────────

void Target::AddFilesMatchsRegexInFolder(Path dir, const std::regex& rgx)
{
    if (!dir.Exists()) return;
    for (auto& rel : dir.BrowseRelative())
    {
        if (!std::regex_match(rel.ToStr(), rgx)) continue;
        Path full = dir + rel;
        if (std::find(SourceFiles.begin(),SourceFiles.end(),full)==SourceFiles.end())
            AddSourceFile(full.ToStr());
    }
}

void Target::AddFilesMatchsRegex(const char* regex_expr)
{
    if (!regex_expr) return;
    std::regex rgx(regex_expr);
    Path root = WorkingPath.IsSet() ? WorkingPath
              : MiracleExecuter::GetExecuter()->GetProjectSourcePath();
    AddFilesMatchsRegexInFolder(root, rgx);
    for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
        AddFilesMatchsRegexInFolder(sp, rgx);
}

// ── AddIncludePath ─────────────────────────────────────────────────────────────

void Target::AddIncludePath(const char* path_str)
{
    if (!path_str) return;
    Path p = path_str;
    if (p.IsFullPath())
    {
        if (!p.Exists()) { mprint(color::err(p.ToString()+" does not exist.")); std::exit(1); }
        IncludePaths.push_back(p); return;
    }
    Path r = (const Path&)MiracleExecuter::GetExecuter()->GetProjectSourcePath() + (const Path&)p;
    if (!r.Exists()) { mprint(color::err(std::string(path_str ? path_str : "")+" cannot be located.")); std::exit(1); }
    IncludePaths.push_back(r);
}

// ── AddPkgConfigDep [N8] — no raw new[], uses owned_flags for stable storage [F7] ──

void Target::AddPkgConfigDep(const char* pkg_name)
{
    if (!pkg_name) return;
    auto run = [](const std::string& cmd) -> std::string
    {
        auto res = RunProcessCapture(cmd);
        if (res.rc != 0) return "";
        std::string s = res.out;
        while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
        return s;
    };

    std::string cflags = run(std::string("pkg-config --cflags ") + pkg_name);
    std::string libs   = run(std::string("pkg-config --libs ")   + pkg_name);

    if (cflags.empty() && libs.empty())
    {
        mprint(color::warn("pkg-config: '") + pkg_name + "' not found. Skipping.");
        return;
    }
    mprint(color::info("pkg-config: ") + pkg_name + "  cflags=" + cflags + "  libs=" + libs);

    // Helper: store each token in owned_flags (stable deque storage), then push
    // its c_str() into dst. All pointers remain valid for the Target's lifetime.
    auto push_flags = [&](const std::string& s, std::vector<const char*>& dst)
    {
        std::istringstream ss(s); std::string tok;
        while (ss >> tok)
        {
            owned_flags.push_back(std::move(tok));
            dst.push_back(owned_flags.back().c_str());
        }
    };

    // Split cflags into compiler commands
    push_flags(cflags, ExtraCompilerCommands);

    // Split libs into appropriate buckets.
    // All tokens go through owned_flags so no pointer is ever dangling.
    std::istringstream ss(libs); std::string tok;
    while (ss >> tok)
    {
        if (tok.size() >= 2 && tok[0]=='-' && tok[1]=='l')
        {
            owned_flags.push_back(std::move(tok));
            linker_commands.push_back(owned_flags.back().c_str());
        }
        else if (tok.size() >= 2 && tok[0]=='-' && tok[1]=='L')
        {
            // Store the path portion (after -L) in owned_flags, then wrap in Path.
            owned_flags.push_back(tok.substr(2));
            link_search_path.push_back(owned_flags.back().c_str());
        }
        else
        {
            // Full token (e.g. an absolute path) stored safely before use.
            owned_flags.push_back(std::move(tok));
            link_libraries.push_back(owned_flags.back().c_str());
        }
    }
}

// =============================================================================
//  main
// =============================================================================
#define SCRIPT_FILE_EXTENSION ".ubs"

static void print_usage(const char* argv0)
{
    std::cout << color::bold("Miracle Build System\n\n")
              << color::bold("Usage:\n")
              << "  " << argv0 << " [OPTIONS] [script" SCRIPT_FILE_EXTENSION "] [KEY VALUE ...]\n\n"
              << color::bold("Options:\n")
              << "  --silent             Suppress all console output\n"
              << "  --no-color           Disable ANSI colour\n"
              << "  --output-file FILE   Tee output to FILE\n"
              << "  --jobs N             Parallel jobs (default: CPU count)\n"
              << "  --clean              Delete object files before building\n"
              << "  --dry-run            Print commands, execute nothing\n"
              << "  --config NAME        Select build configuration (default: release)\n"
              << "  --target NAME        Build only this target + its dependencies\n"
              << "  --install PREFIX     Run install rules after build\n"
              << "  --build-dir DIR      Override global out-of-source build directory\n"
              << "  --help               Show this help\n\n"
              << "KEY VALUE pairs after the script path become Lua variables.\n"
              << "In your script: graph:BuildAll() builds everything;\n"
              << "  graph:BuildOne(\"name\") builds one target + deps;\n"
              << "  t:BuildOrDie() builds a target and aborts the script on failure.\n";
}

int main(int argc, char* argv[])
{
    color::enabled = (MBS_IS_TTY(1) != 0);

    std::string install_prefix;
    std::string override_build_dir;
    std::string only_target;   // [F6] --target NAME

    // ── pre-scan all flags ────────────────────────────────────────────────────
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a=="--silent")   { g_silent=true; }
        else if (a=="--no-color") { color::enabled=false; }
        else if (a=="--clean")    { g_clean=true; }
        else if (a=="--dry-run")  { g_dry_run=true; }
        else if (a=="--help")     { print_usage(argv[0]); return 0; }
        else if (a=="--jobs" && i+1<argc)
        { int n=std::atoi(argv[++i]); g_jobs=(n>0)?(unsigned)n:0; }
        else if (a=="--config" && i+1<argc)
        { g_config = argv[++i]; }
        else if (a=="--target" && i+1<argc)
        { only_target = argv[++i]; }           // [F6]
        else if (a=="--install" && i+1<argc)
        { install_prefix = argv[++i]; }
        else if (a=="--build-dir" && i+1<argc)
        { override_build_dir = argv[++i]; }
        else if (a=="--output-file" && i+1<argc)
        {
            const char* lp=argv[++i];
            g_log_file.open(lp, std::ios::out|std::ios::trunc);
            if (!g_log_file.is_open()) std::cerr<<"Warning: cannot open log '"<<lp<<"'\n";
        }
    }
    // this is important to keep and never be removed even if it causes startup errors
    if (!getenv("MIRACLE_HOME"))
    { mprint(color::err("MIRACLE_HOME is not set.")); return 1; }

    // ── initialise thread pool (after env check — avoids leak on early exit) ──
    unsigned n_jobs = g_jobs > 0 ? g_jobs : std::max(1u, std::thread::hardware_concurrency());
    g_pool = new ThreadPool(n_jobs);

    mprint(color::bold("Miracle Build System") + color::dim(
           "  config=" + g_config +
           (only_target.empty() ? "" : "  target=" + only_target) +
           "  jobs=" + std::to_string(n_jobs) +
           (g_dry_run ? "  [DRY-RUN]" : "")));

    auto t_global_start = std::chrono::steady_clock::now();

    MiracleExecuter* executer = MiracleExecuter::GetExecuter();

    // Expose --target to Lua so scripts can branch on it
    executer->SetVariable("MBS_TARGET", only_target);

    // Apply override build dir to BuildGraph
    if (!override_build_dir.empty())
        BuildGraph::Get().SetGlobalBuildDir(override_build_dir.c_str());

    // ── locate script ─────────────────────────────────────────────────────────
    int script_idx = -1;
    for (int i=1; i<argc; ++i)
    {
        std::string a=argv[i];
        if (a=="--silent"||a=="--no-color"||a=="--clean"||a=="--dry-run"||a=="--help") continue;
        if (a=="--jobs"||a=="--config"||a=="--target"||a=="--install"||
            a=="--build-dir"||a=="--output-file") { ++i; continue; }
        script_idx=i; break;
    }

    int script_rc = 0;
    if (script_idx==-1)
    {
        Path file = Path::CurrentDir() + ("build" SCRIPT_FILE_EXTENSION);
        if (!file.Exists())
        { mprint(color::err("No script given and no 'build" SCRIPT_FILE_EXTENSION "' found."));
          print_usage(argv[0]); script_rc = 1; }
        else
        {
            mprint(color::info("Script: ")+file.ToString());

            // If --target was given, tell BuildGraph before running the script
            // so graph:BuildAll() inside the script respects it automatically.
            if (!only_target.empty())
                BuildGraph::Get().only_target_override = only_target;

            if (!executer->LoadFile(file.ToStr())) script_rc = 1;
        }
    }
    else
    {
        Path file = argv[script_idx];
        if (!file.Exists())
        { mprint(color::err("Script not found: ")+file.ToString()); script_rc = 1; }
        else if (file.GetExtension()!=SCRIPT_FILE_EXTENSION)
        { mprint(color::err("Only '" SCRIPT_FILE_EXTENSION "' scripts are supported.")); script_rc = 1; }
        else
        {
            for (int i=script_idx+1; i<argc; )
            {
                std::string a=argv[i];
                if (a=="--silent"||a=="--no-color"||a=="--clean"||a=="--dry-run"||a=="--help")
                    { ++i; continue; }
                if (a=="--jobs"||a=="--config"||a=="--target"||a=="--install"||
                    a=="--build-dir"||a=="--output-file")
                    { i+=2; continue; }
                if (i+1<argc)
                { mprint(color::dim("  var: "+a+" = "+argv[i+1]));
                  executer->SetVariable(a,argv[i+1]); i+=2; }
                else { mprint(color::warn("Dangling arg: ")+a); ++i; }
            }

            mprint(color::info("Script: ")+file.ToString());

            // If --target was given, tell BuildGraph before running the script
            // so graph:BuildAll() inside the script respects it automatically.
            if (!only_target.empty())
                BuildGraph::Get().only_target_override = only_target;  // [F6]

            if (!executer->LoadFile(file.ToStr())) script_rc = 1;
        }
    }

    // Run install if requested and build succeeded
    if (script_rc == 0 && !install_prefix.empty())
        BuildGraph::Get().Install(install_prefix.c_str());

    // Global build summary with total elapsed time  [F2][D6]
    double total_sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_global_start).count();
    unsigned total_errs  = g_error_count.load();
    unsigned total_warns = g_warning_count.load();
    {
        std::string summary = color::bold("Build complete") +
            color::dim("  total=" + std::to_string(total_sec).substr(0,5) + "s");
        if (total_errs  > 0) summary += color::err(" " + std::to_string(total_errs)  + " error(s)");
        if (total_warns > 0) summary += color::warn(" " + std::to_string(total_warns) + " warning(s)");
        if (script_rc != 0)  summary += color::err("  FAILED");
        mprint(summary);
    }

    if (g_log_file.is_open()) g_log_file.close();
    // [F9] Delete executer before pool: ensures all Lua-driven Build() calls
    // have completed and no futures reference the pool before it is destroyed.
    MiracleExecuter::DeleteExecuter();
    delete g_pool;
    return script_rc;
}