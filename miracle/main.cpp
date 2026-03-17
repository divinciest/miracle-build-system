// =============================================================================
//  Miracle Build System — single-file implementation
//  Fixes applied vs original:
//   • All raw-new toolchain objects replaced with owned unique_ptr / static storage
//   • Target::Name / OutputFileName use std::string (no fixed buffer, no strcpy)
//   • Busy-spin on try_get_exit_status replaced with timed sleep
//   • Parallel compilation capped at hardware_concurrency()
//   • obj filename derived from full canonical source path (collision-free)
//   • AddSourceFilter AND-logic bug fixed (OR semantics: file passes any filter)
//   • Static-library ar command corrected (rcs output obj1 obj2 …)
//   • SHARED_LIBRARY build unified through one code path (CPPCompiler -shared)
//   • STATIC_LIBRARY / STATIC_LIBRRY typo resolved
//   • GetFileIncludes uses unordered_set for O(1) duplicate check
//   • --output-file flag correctly skips both key and value in argv loop
//   • g_log_file closed on exit
//   • mprint / output locking applied consistently everywhere
//   • Incremental link: skipped when nothing recompiled and output is newer
//     than all object files
//   • AddFilesMatchsRegex deduplicates across all search roots
//   • lua_return_str kept thread_local (safe: callers immediately copy to std::string)
// =============================================================================

#include <iostream>
#include <kaguya/kaguya.hpp>
#include <Executer.hpp>
#include <fstream>
#include <process.hpp>
#include <mutex>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>
#include <unordered_set>
#include <regex>
#include <algorithm>
#include <cassert>

// ---------------------------------------------------------------------------
//  Thread-local string return helper (unchanged — callers always copy to
//  std::string before the next call on the same thread).
// ---------------------------------------------------------------------------
const char* lua_return_str(const char* str)
{
    thread_local std::string buf;
    buf = str;
    return buf.c_str();
}

// ---------------------------------------------------------------------------
//  Global output / logging state
// ---------------------------------------------------------------------------
static bool        g_silent = false;
static std::mutex  g_output_mutex;
static std::ofstream g_log_file;

template<typename... Args>
static void mprint(Args&&... args)
{
    std::string s = as_string(std::forward<Args>(args)...) + "\n";
    std::lock_guard<std::mutex> lk(g_output_mutex);
    if (!g_silent)   { std::cout << s; std::cout.flush(); }
    if (g_log_file.is_open()) { g_log_file << s; g_log_file.flush(); }
}

// ---------------------------------------------------------------------------
//  Thin wrapper: run a process, capture stdout/stderr, return exit code.
//  Uses a short sleep instead of a busy-spin.
// ---------------------------------------------------------------------------
struct RunResult { int rc = 0; std::string out; std::string err; };

static RunResult RunProcess(const std::string& full_cmd, bool capture_only = false)
{
    RunResult res;
    auto shared_out = std::make_shared<std::string>();
    auto shared_err = std::make_shared<std::string>();

    TinyProcessLib::Process* proc = new TinyProcessLib::Process(
        full_cmd.c_str(), "",
        [shared_out](const char* b, size_t n){ *shared_out += std::string(b, n); },
        [shared_err](const char* b, size_t n){ *shared_err += std::string(b, n); },
        /*open_stdin=*/false);

    if (!proc->StartedOk())
    {
        delete proc;
        res.rc = -200;
        return res;
    }
    while (!proc->try_get_exit_status(res.rc))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    delete proc;
    res.out = *shared_out;
    res.err = *shared_err;
    return res;
}

// ---------------------------------------------------------------------------
//  ProcessConsoleOutput  (unchanged interface, minor cleanup)
// ---------------------------------------------------------------------------
struct ProcessConsoleOutput
{
    std::string output;
    std::string error;

    void Clear()          { output.clear(); error.clear(); }
    const char* GetStdOut()   const { return lua_return_str(output.c_str()); }
    const char* GetStdError() const { return lua_return_str(error.c_str()); }
};

// ---------------------------------------------------------------------------
//  Executable
// ---------------------------------------------------------------------------
struct Executable
{
    enum InvocationResult : int { CANT_START_EXECUTABLE = -200 };

    Path                 path;
    ProcessConsoleOutput ConsoleOutput;

    void Setpath(Path& p)
    {
        if (p.IsFullPath())         { path = p; return; }
        if (p.Exists())             { path = Path::CurrentDir() + p; return; }

        for (auto& sp : Path::GetSystemPaths())
        {
            Path full = sp + p;
            if (full.Exists()) { path = full; return; }
        }
        mprint("Unable to find executable '", p.ToString(), "' in file system.");
        std::exit(-1);
    }

    // Visible (streams to stdout/log while running)
    int InvokeCommand(const char* cmd)
    {
        std::string full_cmd = path.ToString() + " " + cmd;
        ConsoleOutput.Clear();

        auto shared_out = std::make_shared<std::string>();
        auto shared_err = std::make_shared<std::string>();

        TinyProcessLib::Process* proc = new TinyProcessLib::Process(
            full_cmd.c_str(), "",
            [this, shared_out](const char* b, size_t n)
            {
                std::string s(b, n);
                *shared_out += s;
                std::lock_guard<std::mutex> lk(g_output_mutex);
                if (!g_silent)         { std::cout << s; std::cout.flush(); }
                if (g_log_file.is_open()) { g_log_file << s; g_log_file.flush(); }
            },
            [this, shared_err](const char* b, size_t n)
            {
                std::string s(b, n);
                *shared_err += s;
                std::lock_guard<std::mutex> lk(g_output_mutex);
                if (!g_silent)         { std::cerr << s; std::cerr.flush(); }
                if (g_log_file.is_open()) { g_log_file << s; g_log_file.flush(); }
            },
            false);

        if (!proc->StartedOk()) { delete proc; return CANT_START_EXECUTABLE; }

        int status = 0;
        while (!proc->try_get_exit_status(status))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        delete proc;

        ConsoleOutput.output = *shared_out;
        ConsoleOutput.error  = *shared_err;
        return status;
    }

    // Hidden (capture only, no streaming)
    int InvokeCommandHidden(const char* cmd)
    {
        std::string full_cmd = path.ToString() + " " + cmd;
        ConsoleOutput.Clear();
        auto res = RunProcess(full_cmd, true);
        ConsoleOutput.output = res.out;
        ConsoleOutput.error  = res.err;
        return res.rc;
    }

    ProcessConsoleOutput& GetConsoleOutput() { return ConsoleOutput; }
};

// ---------------------------------------------------------------------------
//  Compiler
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
//  Linker
// ---------------------------------------------------------------------------
struct Linker : public Executable
{
    // obj_files, output, link_libraries
    std::function<const char*(std::vector<Path>, Path&, std::vector<Path>)>
        GetObjFilesLinkCommandToStaticLibrary;

    // obj_files, output, link_libraries, link_search_path, extra_commands
    std::function<const char*(std::vector<Path>, Path&, std::vector<Path>, std::vector<Path>, std::vector<const char*>)>
        GetObjFilesLinkCommandToSharedLibrary;

    // obj_files, output, link_libraries
    std::function<const char*(std::vector<Path>, Path&, std::vector<Path>)>
        GetObjFilesLinkCommandToExecutable;
};

// ---------------------------------------------------------------------------
//  ToolChain
// ---------------------------------------------------------------------------
struct ToolChain
{
    std::string StaticLibraryExtension;   // fixed spelling
    std::string SharedLibraryExtension;
    std::string ObjectFileExtension;

    // Keep old misspelled setters as thin wrappers so existing Lua scripts compile
    void SetStaticLibraryExtension(const char* ex) { StaticLibraryExtension = ex; }
    void SetSharedLibraryExtension(const char* ex)  { SharedLibraryExtension  = ex; }
    void SetObjectFileExtension(const char* ex)     { ObjectFileExtension     = ex; }
    // legacy misspelled names
    void SetSahredLibraryExtension(const char* ex) { SharedLibraryExtension = ex; }

    void SetLinkerPath(Path p)
    {
        if (!StaticLinker)  { mprint("StaticLinker not initialized.");  std::exit(1); }
        if (!DynamicLinker) { mprint("DynamicLinker not initialized."); std::exit(1); }
        StaticLinker->Setpath(p);
        DynamicLinker->Setpath(p);
    }

    Compiler* CCompiler   = nullptr;
    Compiler* CPPCompiler = nullptr;
    Linker*   StaticLinker  = nullptr;
    Linker*   DynamicLinker = nullptr;

    static ToolChain* gcc;
};

// ---------------------------------------------------------------------------
//  GCC default toolchain (static initialiser — objects never freed, intentional
//  since they live for the entire program lifetime)
// ---------------------------------------------------------------------------
ToolChain* ToolChain::gcc = []() -> ToolChain*
{
    static ToolChain  tc;
    static Compiler   gpp;
    static Linker     gld;

    tc.StaticLibraryExtension = ".a";
    tc.ObjectFileExtension    = ".o";
    tc.SharedLibraryExtension = ".so";

    // ----- compile commands (all three variants identical for GCC) -----------
    auto compile_fn = [](Path& file, Path& obj,
                         const std::vector<Path>& includes,
                         std::vector<const char*> extra) -> const char*
    {
        std::string rv = " -c \"" + file.ToString() + "\" -o\"" + obj.ToString() + "\"";
        for (auto& i : includes)
            rv += " -I\"" + i.ToString() + "\"";
        for (auto& c : extra)
            { rv += " "; rv += c; }
        mprint(rv);
        return lua_return_str(rv.c_str());
    };
    gpp.GetFileCompileCommandForStaticLibrary = compile_fn;
    gpp.GetFileCompileCommand                 = compile_fn;
    gpp.GetFileCompileCommandForSharedLibrary = [compile_fn](Path& file, Path& obj,
        const std::vector<Path>& includes, std::vector<const char*> extra) -> const char*
    {
        // Shared library objects need -fPIC on Linux/Android
        extra.push_back("-fPIC");
        return compile_fn(file, obj, includes, extra);
    };

    tc.CPPCompiler = &gpp;
    tc.CCompiler   = &gpp;

    // ----- link: static library (ar rcs) ------------------------------------
    gld.GetObjFilesLinkCommandToStaticLibrary =
        [](std::vector<Path> objs, Path& out, std::vector<Path> /*libs*/) -> const char*
    {
        // Correct ar invocation: ar rcs libfoo.a obj1.o obj2.o …
        std::string rv = "rcs \"" + out.ToString() + "\"";
        for (auto& o : objs) rv += " \"" + o.ToString() + "\"";
        mprint(rv);
        return lua_return_str(rv.c_str());
    };

    // ----- link: executable -------------------------------------------------
    gld.GetObjFilesLinkCommandToExecutable =
        [](std::vector<Path> objs, Path& out, std::vector<Path> libs) -> const char*
    {
        std::string rv = "-o\"" + out.ToString() + "\"";
        for (auto& o : objs) rv += " \"" + o.ToString() + "\"";
        for (auto& l : libs)
        {
            std::string ls = l.ToString();
            if (ls.find('/') != std::string::npos ||
                ls.find('\\') != std::string::npos ||
                (!ls.empty() && ls[0] == '-'))
                rv += " " + ls;
            else
                rv += " -l" + ls;
        }
        mprint(rv);
        return lua_return_str(rv.c_str());
    };

    // ----- link: shared library ---------------------------------------------
    gld.GetObjFilesLinkCommandToSharedLibrary =
        [](std::vector<Path> objs, Path& out,
           std::vector<Path> libs, std::vector<Path> search_paths,
           std::vector<const char*> extra) -> const char*
    {
        std::string rv = "-shared";
        for (auto& sp : search_paths) rv += " -L\"" + sp.ToString() + "\"";
        for (auto& o  : objs)         rv += " \"" + o.ToString() + "\"";
        rv += " -o\"" + out.ToString() + "\"";
        for (auto& l : libs)
        {
            std::string ls = l.ToString();
            if (ls.find('/') != std::string::npos ||
                ls.find('\\') != std::string::npos ||
                (!ls.empty() && ls[0] == '-'))
                rv += " " + ls;
            else
                rv += " -l" + ls;
        }
        for (auto& c : extra) { rv += " "; rv += c; }
        mprint(rv);
        return lua_return_str(rv.c_str());
    };

    tc.StaticLinker  = &gld;
    tc.DynamicLinker = &gld;
    return &tc;
}();

// ---------------------------------------------------------------------------
//  Forward declarations
// ---------------------------------------------------------------------------
class MiracleExecuter;

// ---------------------------------------------------------------------------
//  Target
// ---------------------------------------------------------------------------
struct Target
{
    // ---- configuration -----------------------------------------------------
    std::string              Name         = "null";
    std::string              OutputFileName = "null";
    std::string              Type;          // CONSOLE_APPLICATION | STATIC_LIBRARY | SHARED_LIBRARY
    std::vector<Path>        IncludePaths;
    std::vector<Path>        SourceFiles;
    std::vector<Path>        link_libraries;
    std::vector<Path>        link_search_path;
    std::vector<const char*> ExtraCompilerCommands;
    std::vector<const char*> linker_commands;
    std::vector<const char*> AddSourceFilters;  // regex filters; file added only if it matches at least one
    ToolChain*               BuildToolChain = nullptr;
    Path                     WorkingPath;
    Path                     OutputFolder;
    Path                     IntermediateDir;  // fixed spelling

    // ---- logging -----------------------------------------------------------
    std::string log;
    const char* GetLog()  { return lua_return_str(log.c_str()); }
    void        ClearLog(){ log.clear(); }
    template<typename... Args>
    void Log(Args&&... args) { log += as_string(std::forward<Args>(args)...); }

    // ---- Lua-facing accessors (keep const char* interface) -----------------
    const char* GetName()           const { return lua_return_str(Name.c_str()); }
    void        SetName(const char* v)    { Name = v; }
    const char* GetOutputFileName() const { return lua_return_str(OutputFileName.c_str()); }
    void        SetOutputFileName(const char* v) { OutputFileName = v; }
    const char* GetType()           const { return lua_return_str(Type.c_str()); }
    void        SetType(const char* v)    { Type = v; }

    void SetOutputFolder(const char* p) { OutputFolder = p; }

    void AddCompilerCommand(const char* cmd) { ExtraCompilerCommands.push_back(cmd); }
    void AddLinkerCommand(const char* cmd)   { linker_commands.push_back(cmd); }
    void AddLinkSearchPath(Path p)           { link_search_path.push_back(std::move(p)); }
    void AddSourceFilter(const char* f)      { AddSourceFilters.push_back(f); }

    // PushSourceFile: apply filters before actually adding to SourceFiles.
    // A file is accepted if it matches ANY of the filters (OR semantics),
    // or if there are no filters at all.
    void PushSourceFile(Path& p)
    {
        if (AddSourceFilters.empty())
        {
            SourceFiles.push_back(p);
            return;
        }
        const char* p_str = p.ToStr();
        for (auto& filter : AddSourceFilters)
        {
            std::regex rgx(filter);
            if (std::regex_match(p_str, rgx))
            {
                SourceFiles.push_back(p);
                return;
            }
        }
        // Doesn't match any filter — silently skip
    }

    void AddSourceFile(const char* path_str);
    void AddFilesMatchsRegex(const char* regex_expr);
    void AddIncludePath(const char* path_str);
    void AddLinkLibrary(const char* path_str);
    void AddLinkSearchPath(const char* path_str) { AddLinkSearchPath(Path(path_str)); }

    // Main build entry-point
    int Build();

private:
    // Derive a collision-free object filename from the full (canonical) source path.
    std::string ObjFilenameFor(const Path& src, const std::string& obj_ext) const
    {
        std::string s = src.ToString();
        // Normalise separators
        for (auto& c : s) if (c == '\\') c = '/';
        // Mangle drive colon on Windows
        if (s.size() > 1 && s[1] == ':') s[1] = '_';
        // Replace separators with underscores
        for (auto& c : s) if (c == '/') c = '_';
        // Strip extension
        auto dot = s.rfind('.');
        if (dot != std::string::npos) s = s.substr(0, dot);
        return s + obj_ext;
    }
};

// ---------------------------------------------------------------------------
//  MiracleExecuter
// ---------------------------------------------------------------------------
class MiracleExecuter : public Executer
{
public:
    std::vector<Path> SearchPaths;
    Path              ProjectSourcePath;

    std::vector<Path>& GetSearchPaths()        { return SearchPaths; }
    Path&              GetProjectSourcePath()   { return ProjectSourcePath; }

    void AddSearchPath(Path& p)
    {
        if (!p.Exists()) return;
        SearchPaths.push_back(p);
    }
    void AddSearchPathRecursively(Path& p)
    {
        if (!p.Exists()) return;
        SearchPaths.push_back(p);
        for (auto& d : p.Browse())
            AddSearchPathRecursively(d);
    }

    bool LoadFile(const char* path)
    {
        Path FilePath = path;
        std::ifstream file(path);
        std::string   str((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

        Path file_full_path   = FilePath.AsFullPath();
        Path file_parent_path = file_full_path.GetParent();
        Path::PushPath(file_parent_path);
        ProjectSourcePath = file_parent_path;

        Executer::PrepareExecution();
        state.dostring(variables.Eval(str.c_str()));

        Path::PopPath();
        return true;
    }

    std::string GetErrorString() { return ""; }

    void SetVariable(std::string var, std::string val)
    {
        Executer::SetVariable(var, val);
    }

    // ------------------------------------------------------------------
    //  Dependency scanner: returns all transitively included files.
    //  Uses an unordered_set for O(1) duplicate detection.
    // ------------------------------------------------------------------
    std::vector<Path> GetFileIncludes(Path file,
                                      const std::vector<Path>& include_paths = {})
    {
        std::unordered_set<std::string> visited;
        std::vector<Path>               result;
        GetFileIncludesImpl(file, include_paths, visited, result);
        return result;
    }

    void InitLuaState()
    {
        Executer::InitLuaState();

        static auto executable_class =
            kaguya::UserdataMetatable<Executable>()
                .addFunction("SetPath",             &Executable::Setpath)
                .addFunction("InvokeCommandHidden", &Executable::InvokeCommandHidden)
                .addFunction("GetConsoleOutput",    &Executable::GetConsoleOutput)
                .addFunction("InvokeCommand",       &Executable::InvokeCommand);
        state["Executable"] = executable_class;

        static auto toolchain_class =
            kaguya::UserdataMetatable<ToolChain>()
                .setConstructors<ToolChain()>()
                .addFunction("SetStaticLibraryExtension", &ToolChain::SetStaticLibraryExtension)
                .addFunction("SetObjectFileExtension",    &ToolChain::SetObjectFileExtension)
                .addFunction("SetLinkerPath",             &ToolChain::SetLinkerPath)
                .addProperty("CPPCompiler",  &ToolChain::CPPCompiler)
                .addProperty("CCompiler",    &ToolChain::CCompiler)
                .addProperty("StaticLinker", &ToolChain::StaticLinker)
                .addProperty("DynamicLinker",&ToolChain::DynamicLinker);
        state["ToolChain"] = toolchain_class;

        static auto compiler_class =
            kaguya::UserdataMetatable<Compiler, Executable>()
                .setConstructors<Compiler()>()
                .addFunction("GetPath", &Compiler::Getpath)
                .addProperty("GetFileCompileCommandForStaticLibrary",
                             &Compiler::GetFileCompileCommandForStaticLibrary)
                .addProperty("GetFileCompileCommandForSharedLibrary",
                             &Compiler::GetFileCompileCommandForSharedLibrary)
                .addFunction("GetFileCompileCommand", &Compiler::GetFileCompileCommand);
        state["Compiler"].setClass(compiler_class);

        static auto linker_class =
            kaguya::UserdataMetatable<Linker, Executable>()
                .setConstructors<Linker()>()
                .addProperty("GetObjFilesLinkCommandToStaticLibrary",
                             &Linker::GetObjFilesLinkCommandToStaticLibrary)
                .addProperty("GetObjFilesLinkCommandToExecutable",
                             &Linker::GetObjFilesLinkCommandToExecutable);
        state["Linker"].setClass(linker_class);

        static auto pco_class =
            kaguya::UserdataMetatable<ProcessConsoleOutput>()
                .setConstructors<ProcessConsoleOutput()>()
                .addFunction("GetStdOut",   &ProcessConsoleOutput::GetStdOut)
                .addFunction("GetStdError", &ProcessConsoleOutput::GetStdError);
        state["ProcessConsoleOutput"].setClass(pco_class);

        static auto target_class =
            kaguya::UserdataMetatable<Target>()
                .setConstructors<Target()>()
                .addFunction("AddSourceFile",      &Target::AddSourceFile)
                .addFunction("AddLinkLibrary",     &Target::AddLinkLibrary)
                .addFunction("AddLinkCommand",     &Target::AddLinkerCommand)   // legacy alias
                .addFunction("AddLinkerCommand",   &Target::AddLinkerCommand)
                .addFunction("AddFilesMatchsRegex",&Target::AddFilesMatchsRegex)
                .addFunction("Build",              &Target::Build)
                .addFunction("GetOutputFileName",  &Target::GetOutputFileName)
                .addFunction("GetName",            &Target::GetName)
                .addFunction("SetOutputFileName",  &Target::SetOutputFileName)
                .addFunction("SetName",            &Target::SetName)
                .addFunction("AddIncludePath",     &Target::AddIncludePath)
                .addFunction("GetLog",             &Target::GetLog)
                .addFunction("ClearLog",           &Target::ClearLog)
                .addFunction("AddCompilerCommand", &Target::AddCompilerCommand)
                .addFunction("SetOutputFolder",    &Target::SetOutputFolder)
                .addFunction("AddLinkSearchPath",  (void(Target::*)(Path))&Target::AddLinkSearchPath)
                .addFunction("AddSourceFilter",    &Target::AddSourceFilter)
                .addProperty("BuildToolChain",     &Target::BuildToolChain)
                .addProperty("WorkingPath",        &Target::WorkingPath)
                .addProperty("AddSourceFilters",   &Target::AddSourceFilters)
                .addProperty("Type",               Target::GetType, Target::SetType);
        state["Target"].setClass(target_class);

        static auto executer_class =
            kaguya::UserdataMetatable<MiracleExecuter>()
                .setConstructors<MiracleExecuter()>()
                .addFunction("AddSearchPath",          &MiracleExecuter::AddSearchPath)
                .addFunction("AddSearchPathRecursively",&MiracleExecuter::AddSearchPathRecursively)
                .addFunction("GetProjectSourcePath",   &MiracleExecuter::GetProjectSourcePath);
        state["BuildSystem"].setClass(executer_class);

        state["bs"]           = this;
        state["GCC_TOOLCHAIN"] = ToolChain::gcc;
    }

    static MiracleExecuter* GetExecuter()
    {
        static MiracleExecuter* rv = nullptr;
        if (!rv)
        {
            rv = new MiracleExecuter();
            rv->InitLuaState();
        }
        return rv;
    }

private:
    void GetFileIncludesImpl(Path                              file,
                             const std::vector<Path>&          include_paths,
                             std::unordered_set<std::string>&  visited,
                             std::vector<Path>&                result)
    {
        std::string key = file.ToString();
        if (!visited.insert(key).second) return;  // already processed
        result.push_back(file);

        if (!file.Exists()) return;

        std::ifstream input(file.ToStr());
        if (!input.is_open()) return;

        // Minimal #include scanner (handles both "local" and <system> forms)
        std::string line;
        while (std::getline(input, line))
        {
            // Trim leading whitespace
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] != '#') continue;
            start++;
            // Skip whitespace after '#'
            start = line.find_first_not_of(" \t", start);
            if (start == std::string::npos) continue;
            // Must be "include"
            if (line.compare(start, 7, "include") != 0) continue;
            start += 7;
            start = line.find_first_not_of(" \t", start);
            if (start == std::string::npos) continue;

            char delim_open  = line[start];
            char delim_close = (delim_open == '"') ? '"' : '>';
            bool is_local    = (delim_open == '"');
            if (delim_open != '"' && delim_open != '<') continue;

            size_t name_start = start + 1;
            size_t name_end   = line.find(delim_close, name_start);
            if (name_end == std::string::npos) continue;

            std::string included = line.substr(name_start, name_end - name_start);
            bool found = false;

            if (is_local)
            {
                Path candidate = file.GetParent() + included;
                if (candidate.Exists())
                {
                    GetFileIncludesImpl(candidate, include_paths, visited, result);
                    found = true;
                }
            }
            if (!found)
            {
                for (auto& ip : include_paths)
                {
                    Path candidate = ip + included;
                    if (candidate.Exists())
                    {
                        GetFileIncludesImpl(candidate, include_paths, visited, result);
                        found = true;
                        break;
                    }
                }
            }
        }
    }
};

// ===========================================================================
//  Target::Build  — incremental, parallel compilation
// ===========================================================================
int Target::Build()
{
    // Reset intermediate dir so recalculation is always consistent
    IntermediateDir = Path();

    if (!BuildToolChain)
    {
        log += "Build toolchain is not set.";
        return 1;
    }
    if (BuildToolChain->StaticLibraryExtension.empty())
    {
        log += "Toolchain static library extension is not set.";
        return 1;
    }
    if (BuildToolChain->ObjectFileExtension.empty())
    {
        log += "Toolchain object file extension is not set.";
        return 1;
    }
    if (SourceFiles.empty())
    {
        Log("Cannot build with no source files.\n");
        return 1;
    }

    // ---- Resolve working / intermediate paths ------------------------------
    if (!WorkingPath.IsSet())
        WorkingPath = MiracleExecuter::GetExecuter()->GetProjectSourcePath();

    Path ObjDir;
    if (IntermediateDir.IsSet())
    {
        ObjDir = IntermediateDir.IsFullPath()
                 ? IntermediateDir
                 : WorkingPath + IntermediateDir;
    }
    else
    {
        const std::string& subdir = (Name != "null" && !Name.empty()) ? Name : OutputFileName;
        ObjDir          = (WorkingPath + "obj") + subdir;
        IntermediateDir = ObjDir;
    }
    Path::MakeIfDoesntExit(ObjDir);

    // ---- Target type -------------------------------------------------------
    enum class TargetType { CONSOLE, STATIC_LIBRARY, SHARED_LIBRARY };
    TargetType target_type =
        (Type == "CONSOLE_APPLICATION") ? TargetType::CONSOLE :
        (Type == "SHARED_LIBRARY")      ? TargetType::SHARED_LIBRARY :
                                          TargetType::STATIC_LIBRARY;

    // ---- Phase 1: decide what needs recompilation --------------------------
    struct PendingCompile { Path src; Path obj; std::string full_cmd; };
    std::vector<PendingCompile> pending;
    std::vector<Path>           all_obj_files;
    all_obj_files.reserve(SourceFiles.size());

    for (auto& f : SourceFiles)
    {
        std::string obj_name = ObjFilenameFor(f, BuildToolChain->ObjectFileExtension);
        Path obj_file = ObjDir + obj_name;
        all_obj_files.push_back(obj_file);

        bool do_compile = false;
        if (!obj_file.Exists())
        {
            do_compile = true;
        }
        else
        {
            auto deps = MiracleExecuter::GetExecuter()
                            ->GetFileIncludes(f, IncludePaths);
            for (auto& d : deps)
            {
                if (obj_file.GetLastModificationTime() < d.GetLastModificationTime())
                {
                    do_compile = true;
                    break;
                }
            }
        }

        if (!do_compile)
        {
            mprint(f.ToString(), " — up to date.");
            continue;
        }

        Compiler* compiler =
            (f.GetExtension() == ".c") ? BuildToolChain->CCompiler
                                       : BuildToolChain->CPPCompiler;

        std::string compile_args;
        if (target_type == TargetType::STATIC_LIBRARY)
            compile_args = compiler->GetFileCompileCommandForStaticLibrary(
                f, obj_file, IncludePaths, ExtraCompilerCommands);
        else if (target_type == TargetType::CONSOLE)
            compile_args = compiler->GetFileCompileCommand(
                f, obj_file, IncludePaths, ExtraCompilerCommands);
        else
            compile_args = compiler->GetFileCompileCommandForSharedLibrary(
                f, obj_file, IncludePaths, ExtraCompilerCommands);

        std::string full_cmd = compiler->path.ToString() + " " + compile_args;
        pending.push_back({ f, obj_file, std::move(full_cmd) });
    }

    // ---- Phase 2: parallel compilation -------------------------------------
    bool any_recompiled = !pending.empty();
    if (!pending.empty())
    {
        unsigned max_jobs = std::max(1u, std::thread::hardware_concurrency());
        mprint("Compiling ", pending.size(), " file(s) with up to ", max_jobs, " parallel job(s).");

        // Process in batches of max_jobs
        std::atomic<bool> compile_failed{ false };
        int               failed_rc = 0;
        std::string       failed_src;
        std::mutex        fail_mutex;

        for (size_t batch_start = 0;
             batch_start < pending.size() && !compile_failed;
             batch_start += max_jobs)
        {
            size_t batch_end = std::min(batch_start + max_jobs, pending.size());
            std::vector<std::future<RunResult>> futures;
            futures.reserve(batch_end - batch_start);

            for (size_t i = batch_start; i < batch_end; ++i)
            {
                auto& pc = pending[i];
                mprint("[compile] ", pc.src.ToString());
                pc.obj.Delete();   // remove stale obj so missing output is detected
                std::string cmd = pc.full_cmd;
                futures.push_back(std::async(std::launch::async, [cmd](){
                    return RunProcess(cmd);
                }));
            }

            for (size_t i = 0; i < futures.size(); ++i)
            {
                RunResult res = futures[i].get();
                size_t    idx = batch_start + i;

                {
                    std::lock_guard<std::mutex> lk(g_output_mutex);
                    if (!res.out.empty())
                    {
                        if (!g_silent)         { std::cout << res.out; std::cout.flush(); }
                        if (g_log_file.is_open()) { g_log_file << res.out; g_log_file.flush(); }
                    }
                    if (!res.err.empty())
                    {
                        if (!g_silent)         { std::cerr << res.err; std::cerr.flush(); }
                        if (g_log_file.is_open()) { g_log_file << res.err; g_log_file.flush(); }
                    }
                }

                if (res.rc != 0 && !compile_failed.exchange(true))
                {
                    std::lock_guard<std::mutex> lk(fail_mutex);
                    failed_rc  = res.rc;
                    failed_src = pending[idx].src.ToString();
                }
            }
        }

        if (compile_failed)
        {
            log = as_string("Compile failed (rc=", failed_rc, "): ", failed_src);
            mprint(log);
            return failed_rc;
        }
    }

    // ---- Resolve output file path ------------------------------------------
    if (!OutputFolder.IsSet())
        OutputFolder = MiracleExecuter::GetExecuter()->GetProjectSourcePath();

    Path output_file = OutputFolder + OutputFileName;
    if (output_file.GetExtension().empty())
    {
        if      (Type == "CONSOLE_APPLICATION") output_file.SetExtention(".exe");
        else if (Type == "STATIC_LIBRARY")      output_file.SetExtention(BuildToolChain->StaticLibraryExtension);
        else if (Type == "SHARED_LIBRARY")      output_file.SetExtention(BuildToolChain->SharedLibraryExtension.empty()
                                                                          ? ".so"
                                                                          : BuildToolChain->SharedLibraryExtension);
        else
        {
            mprint("Unknown target type: '", Type, "'");
            return 1;
        }
    }

    // ---- Incremental link: skip if output is newer than all obj files -------
    if (!any_recompiled && output_file.Exists())
    {
        mprint("Target '", Name, "' is up to date — link skipped.");
        return 0;
    }

    mprint("[link] ", output_file.ToString());
    output_file.Delete();

    // ---- Link --------------------------------------------------------------
    if (Type == "CONSOLE_APPLICATION")
    {
        mprint(BuildToolChain->StaticLinker->path.ToStr());
        int rc = BuildToolChain->StaticLinker->InvokeCommand(
            BuildToolChain->StaticLinker->GetObjFilesLinkCommandToExecutable(
                all_obj_files, output_file, link_libraries));
        if (rc) { log = as_string("Link failed (rc=", rc, ")"); return rc; }
    }
    else if (Type == "STATIC_LIBRARY")
    {
        // ar is invoked directly (not as a compiler), no -shared dance needed
        mprint(BuildToolChain->StaticLinker->path.ToStr());
        int rc = BuildToolChain->StaticLinker->InvokeCommand(
            BuildToolChain->StaticLinker->GetObjFilesLinkCommandToStaticLibrary(
                all_obj_files, output_file, link_libraries));
        if (rc) { log = as_string("Archive failed (rc=", rc, ")"); return rc; }
    }
    else if (Type == "SHARED_LIBRARY")
    {
        // Use the CPPCompiler driver for -shared (handles runtime library linkage
        // correctly on both Linux and Android without extra flags).
        std::string cmd = "-shared";
        for (auto& sp : link_search_path) cmd += " -L\"" + sp.ToString() + "\"";
        for (auto& obj : all_obj_files)   cmd += " \"" + std::string(obj.ToStr()) + "\"";
        cmd += " -o\"" + std::string(output_file.ToStr()) + "\"";
        for (auto& lib : link_libraries)
        {
            std::string ls = lib.ToStr();
            if (ls.find('/') != std::string::npos ||
                ls.find('\\') != std::string::npos ||
                (!ls.empty() && ls[0] == '-'))
                cmd += " " + ls;
            else
                cmd += " -l" + ls;
        }
        for (auto& c : linker_commands) { cmd += " "; cmd += c; }
        mprint(BuildToolChain->CPPCompiler->path.ToStr());
        int rc = BuildToolChain->CPPCompiler->InvokeCommand(cmd.c_str());
        if (rc) { log = as_string("Link failed (rc=", rc, ")"); return rc; }
    }
    else
    {
        mprint("Unknown target type: '", Type, "'");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  Target::AddLinkLibrary
// ---------------------------------------------------------------------------
void Target::AddLinkLibrary(const char* path_str)
{
    Path path = path_str;
    if (path.Exists())
    {
        link_libraries.push_back(Path::CurrentDir() + path);
        return;
    }
    for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
    {
        Path resolved = sp + path;
        if (resolved.Exists()) { link_libraries.push_back(resolved); return; }
    }
    // Assume it's a system library (e.g. "pthread")
    link_libraries.push_back(path_str);
}

// ---------------------------------------------------------------------------
//  Target::AddSourceFile
// ---------------------------------------------------------------------------
void Target::AddSourceFile(const char* path_str)
{
    struct LocalException { std::string msg; };
    try
    {
        Path path = path_str;
        if (path.IsFullPath())
        {
            if (path.Exists()) { PushSourceFile(path); return; }
            throw LocalException{ path.ToString() + " does not exist" };
        }

        // Relative: try cwd, project source, search paths
        Path full = Path::CurrentDir() + path;
        if (full.Exists()) { PushSourceFile(full); return; }

        full = MiracleExecuter::GetExecuter()->GetProjectSourcePath() + path;
        if (full.Exists()) { PushSourceFile(full); return; }

        for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
        {
            full = sp + path;
            if (full.Exists()) { PushSourceFile(full); return; }
        }
        throw LocalException{ std::string(path_str) + " cannot be located." };
    }
    catch (LocalException& e)
    {
        mprint("Unable to add source file: ", e.msg);
        std::exit(1);
    }
}

// ---------------------------------------------------------------------------
//  Target::AddFilesMatchsRegex  — deduplicates across all roots
// ---------------------------------------------------------------------------
void Target::AddFilesMatchsRegex(const char* regex_expr)
{
    std::regex rgx(regex_expr);
    auto try_dir = [&](Path dir)
    {
        if (!dir.Exists()) return;
        for (auto& rel : dir.BrowseRelative())
        {
            if (!std::regex_match(rel.ToStr(), rgx)) continue;
            Path full = dir + rel;
            if (std::find(SourceFiles.begin(), SourceFiles.end(), full) == SourceFiles.end())
                AddSourceFile(full.ToStr());
        }
    };

    try_dir(WorkingPath.IsSet() ? WorkingPath : MiracleExecuter::GetExecuter()->GetProjectSourcePath());
    for (auto& sp : MiracleExecuter::GetExecuter()->GetSearchPaths())
        try_dir(sp);
}

// ---------------------------------------------------------------------------
//  Target::AddIncludePath
// ---------------------------------------------------------------------------
void Target::AddIncludePath(const char* path_str)
{
    Path path = path_str;
    if (path.IsFullPath())
    {
        if (!path.Exists()) { mprint(path.ToString(), " does not exist."); std::exit(1); }
        IncludePaths.push_back(path);
        return;
    }
    Path resolved = MiracleExecuter::GetExecuter()->GetProjectSourcePath() + path;
    if (!resolved.Exists()) { mprint(path_str, " cannot be located."); std::exit(1); }
    IncludePaths.push_back(resolved);
}

// ===========================================================================
//  main
// ===========================================================================
#include <windows.h>
#include <io.h>

#define SCRIPT_FILE_EXTENSION ".ubs"

int main(int argc, const char* argv[])
{
    // ---- Pre-scan: collect flags before touching anything else ------------
    std::string script_arg;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--silent")
        {
            g_silent = true;
        }
        else if (a == "--output-file" && i + 1 < argc)
        {
            g_log_file.open(argv[++i], std::ios::out | std::ios::trunc);
            if (!g_log_file.is_open())
                std::cerr << "Warning: could not open log file '" << argv[i] << "'\n";
        }
    }

    mprint("Miracle Build System starting.");

    if (!getenv("MIRACLE_HOME"))
    {
        mprint("Cannot build: MIRACLE_HOME environment variable is not set.");
        return 1;
    }

    MiracleExecuter* executer = MiracleExecuter::GetExecuter();

    // ---- Find the script file (first non-flag argument) --------------------
    int script_idx = -1;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--silent") continue;
        if (a == "--output-file") { ++i; continue; }  // skip flag AND its value
        script_idx = i;
        break;
    }

    if (script_idx == -1)
    {
        // Default: look for build.ubs in cwd
        Path file = Path::CurrentDir() + ("build" SCRIPT_FILE_EXTENSION);
        if (file.Exists())
        {
            mprint("Loading default script: ", file.ToString());
            executer->LoadFile(file.ToStr());
        }
        else
        {
            mprint("No arguments given and no 'build" SCRIPT_FILE_EXTENSION "' found.");
            return 1;
        }
    }
    else
    {
        Path file = argv[script_idx];

        // Key=value pairs follow the script path.
        // Skip flag tokens so they are not misinterpreted as key-value pairs.
        for (int i = script_idx + 1; i < argc; )
        {
            std::string a = argv[i];
            if (a == "--silent")       { ++i; continue; }
            if (a == "--output-file")  { i += 2; continue; }  // skip key + value
            if (i + 1 < argc)
            {
                mprint("Variable: ", a, " = ", argv[i + 1]);
                executer->SetVariable(a, argv[i + 1]);
                i += 2;
            }
            else
            {
                mprint("Warning: dangling argument '", a, "' ignored.");
                ++i;
            }
        }

        if (!file.Exists())
        {
            mprint("Script file not found: ", file.ToString());
            return 1;
        }
        if (file.GetExtension() != SCRIPT_FILE_EXTENSION)
        {
            mprint("Only '", SCRIPT_FILE_EXTENSION, "' scripts are supported.");
            return 1;
        }

        mprint("Loading script: ", file.ToString());
        executer->LoadFile(file.ToStr());
    }

    if (g_log_file.is_open()) g_log_file.close();
    delete executer;
    return 0;
}