#include <iostream>
#include <kaguya/kaguya.hpp>
#include <Executer.hpp>
#include <fstream>
#include <process.hpp>
const char * lua_return_str(const char * str)
{
   static std::string buf;
   buf = str;
   return buf.c_str();
}
struct ProcessConsoleOutput
{
    std::shared_ptr<std::string> output=std::make_shared<std::string>();
    std::shared_ptr<std::string> error=std::make_shared<std::string>();
    void Clear()
    {
        output->clear();
        error->clear();
    }
    const char * GetStdOut()
    {
        return lua_return_str(output->c_str());
    }
    const char * GetStdError()
    {
        return lua_return_str(error->c_str());
    }
};
struct Executable
{
    enum InvocationResult : int{ CANT_START_EXECUTABLE = -200 };
    Path path;
    ProcessConsoleOutput ConsoleOutput;
    void Setpath(Path & p)
      {
        if (p.IsFullPath())
        {
            path = p;
            return;
        }
        if (p.Exists())
        {
            path = Path::CurrentDir() + p;
            return;
        }
        auto sys_paths = Path::GetSystemPaths();
        for (auto & p1 : sys_paths)
        {
            Path full_path = p1 + p;
            if (full_path.Exists())
            {
                path = full_path;
                return;
            }
        }
        print("Unable to find executable '", p.ToString(),"' in file system .");
        exit(-1);
      }

  int InvokeCommand(const char* cmd)
  {
    std::string full_cmd = path.ToString() + " " + cmd;
    TinyProcessLib::Process * process = nullptr;
    process = new TinyProcessLib::Process (full_cmd.c_str(), "", [this](const char *bytes, size_t n) {
      *ConsoleOutput.output+=std::string(bytes, n);
    }, [this](const char *bytes, size_t n) {
      *ConsoleOutput.error+=std::string(bytes, n);
    },false);
    if (!process->StartedOk())
         return Executable::InvocationResult::CANT_START_EXECUTABLE;
    int status;
    while(!process->try_get_exit_status(status));    delete process;
    return status;
  }
  int InvokeCommandHidden(const char* cmd)
  {
    std::string full_cmd = path.ToString() + " " + cmd;
    TinyProcessLib::Process * process = nullptr;
    process = new TinyProcessLib::Process (full_cmd.c_str(), "", [this](const char *bytes, size_t n) {
      *ConsoleOutput.output+=std::string(bytes, n);
    }, [this](const char *bytes, size_t n) {
      *ConsoleOutput.error+=std::string(bytes, n);
    },true);
    if (!process->StartedOk())
         return Executable::InvocationResult::CANT_START_EXECUTABLE;
    int status;
    while(!process->try_get_exit_status(status));    delete process;
    return status;
  }
  GETTER(ConsoleOutput);
};

class Compiler : public Executable
{
  protected :
  public :
  GETTER(path);

  std::function<const char * (Path&,Path&,const std::vector <Path>&,std::vector <const char*>  )> GetFileCompileCommandForStaticLibrary;
  std::function<const char * (Path&,Path&,const std::vector <Path>&,std::vector <const char*>  )> GetFileCompileCommand;
  std::function<const char * (Path&,Path&,const std::vector <Path>&,std::vector <const char*>  )> GetFileCompileCommandForSharedLibrary;
};
struct Linker  : public Executable
{

  std::function < const char *( std::vector <Path> obj_files , Path & output , std::vector <Path> )> GetObjFilesLinkCommandToStaticLibrary;
  std::function < const char *( std::vector <Path> obj_files , Path & output , std::vector <Path> ,std::vector <Path>,std::vector <const char*> )> GetObjFilesLinkCommandToSharedLibrary;
  std::function < const char *( std::vector <Path> obj_files , Path & output , std::vector <Path> )> GetObjFilesLinkCommandToExecutable;
};
struct ToolChain
{
   std::string StaticLibraryExtention;
   std::string SharedLibraryExtention;
   std::string ObjectFileExtention;
   void SetStaticLibraryExtension(const char  * ex)
   {
     StaticLibraryExtention = ex;
   }
   void SetSahredLibraryExtension(const char  * ex)
   {
     SharedLibraryExtention = ex;
   }
   void SetObjectFileExtension(const char  * ex)
   {
      ObjectFileExtention = ex;
   }
   void SetLinkerPath(Path p)
   {
       if (StaticLinker == nullptr){ print("StaticLinker not initialized ."); exit(1);}
       if (DynamicLinker == nullptr){ print("DynamicLinker not initialized ."); exit(1);}
       StaticLinker->Setpath(p);
       DynamicLinker->Setpath(p);
   }
   Compiler * CCompiler = nullptr;
   Compiler * CPPCompiler = nullptr;
   Linker * StaticLinker = nullptr;
   Linker * DynamicLinker = nullptr;
   static ToolChain * gcc;
};
ToolChain* ToolChain::gcc = [] () -> ToolChain*
{
    ToolChain * gcc_toolchain = new ToolChain();
    gcc_toolchain->StaticLibraryExtention = ".a";
    gcc_toolchain->ObjectFileExtention = ".o";
    gcc_toolchain->SharedLibraryExtention = ".dll";
    Compiler * gpp = new Compiler();
    gpp->GetFileCompileCommandForStaticLibrary =
   [] (Path& file_path,Path & output_file_path,const std::vector <Path>& include_paths,std::vector <const char*> ExtraCommands)
    {
        // -c required to tell the compiler not to call the linker
      std::string rv = " -c \"" + file_path.ToString()+ "\" -o\"" +output_file_path.ToString() + "\"";
      for (auto& i : include_paths)
      {
          rv += " -I\"" + i.ToString() + "\" ";
      }
      for (auto & cmd : ExtraCommands)
      {
          rv += cmd;
          rv += " ";
      }
      print(rv);
      return lua_return_str(rv.c_str());
    };
    gpp->GetFileCompileCommand  =
   [] (Path& file_path,Path & output_file_path,const std::vector <Path>& include_paths,std::vector <const char*> ExtraCommands)
    {
        // -c required to tell the compiler not to call the linker
      std::string rv = " -c \"" + file_path.ToString()+ "\" -o\"" +output_file_path.ToString() + "\"";
      for (auto& i : include_paths)
      {
          rv += " -I\"" + i.ToString() + "\" ";
      }
      for (auto & cmd : ExtraCommands)
      {
          rv += cmd;
          rv += " ";
      }
      print(rv);
      return lua_return_str(rv.c_str());
    };
    gpp->GetFileCompileCommandForSharedLibrary  =
   [] (Path& file_path,Path & output_file_path,const std::vector <Path>& include_paths,std::vector <const char*> ExtraCommands)
    {
        // -c required to tell the compiler not to call the linker
      std::string rv = " -c \"" + file_path.ToString()+ "\" -o\"" +output_file_path.ToString() + "\"";
      for (auto& i : include_paths)
      {
          rv += " -I\"" + i.ToString() + "\" ";
      }
      for (auto & cmd : ExtraCommands)
      {
          rv += cmd;
          rv += " ";
      }
      print(rv);
      return lua_return_str(rv.c_str());
    };
    gcc_toolchain->CPPCompiler = gpp;
    gcc_toolchain->CCompiler = gpp;

    Linker* gcc_linker = new Linker();

    gcc_linker->GetObjFilesLinkCommandToStaticLibrary =[] ( std::vector <Path> obj_files , Path & output ,std::vector <Path> link_libraries) -> const char *
  {
        auto rv = "-r -s " + output.ToString() +  " ";
  for (auto & value : obj_files)
    rv +=  value.ToString() + " ";
  for (auto & value : link_libraries)
    rv +=  value.ToString() + " ";

      print(rv);
   return lua_return_str(rv.c_str());
  };
    gcc_linker->GetObjFilesLinkCommandToSharedLibrary =[] ( std::vector <Path> obj_files , Path & output ,std::vector <Path> link_libraries,std::vector <Path> link_search_path, std::vector <const char*> commands) -> const char *
  {
        std::string rv = "-shared   -Wl,--dll ";

  for (auto & value : link_search_path)
    rv +=  "-L\""  + value.ToString() + "\" ";

  for (auto & value : obj_files)
    rv +=  value.ToString() + " ";

       rv +=  "-o\"" + output.ToString() +"\" ";


  for (auto & value : link_libraries)
    rv += "-l"  + value.ToString() + " ";

  for (auto & value : commands)
    rv = rv+ value + " ";


      print(rv);
   return lua_return_str(rv.c_str());
  };
  gcc_linker->GetObjFilesLinkCommandToExecutable =  [] ( std::vector <Path> obj_files , Path & output ,std::vector <Path> link_libraries) -> const char *
  {
        auto rv = "-o " + output.ToString() +  " ";
  for (auto & value : obj_files)
    rv +=  value.ToString() + " ";
  for (auto & value : link_libraries)
    rv +=  value.ToString() + " ";

      print(rv);
   return lua_return_str(rv.c_str());
  };
    gcc_toolchain->StaticLinker = gcc_linker;
    gcc_toolchain->DynamicLinker = gcc_linker;
    return gcc_toolchain;
}();
#include <regex>
struct Target
{
    std::vector <const char * >ExtraCompilerCommands;
    std::vector <const char * >AddSourceFilters;
    static constexpr unsigned short STR_LEN = 512;
     char*  Name = nullptr;
     std::string Type; // consoleapp - sharedlibrary ....
     char* OutputFileName = nullptr;
     std::vector <Path> IncludePaths;
     const char * GetName(){return lua_return_str(Name);}
     void SetName(const char * v){strcpy(Name,v);}
      const char * GetOutputFileName(){return lua_return_str(OutputFileName);}
     void SetOutputFileName(const char * v){strcpy(OutputFileName,v);}
     std::vector <Path> link_libraries;
      std::vector <const char*> commands;
      std::vector <Path> link_search_path;
     std::string log;
     const char * GetLog()
     {
        return lua_return_str(log.c_str());
     }
     void ClearLog()
     {
         log = "";
     }
     template<typename ...Args>
    void Log(Args&&... args)
    {
        log +=  (as_string(args), ...);
    }
    Target()
    {
        Name = new char[STR_LEN];
        strcpy(Name,"null");
        OutputFileName = new char[STR_LEN];
        strcpy(OutputFileName,"null");
    }
    virtual ~Target()
    {
        delete [] Name;
        delete [] OutputFileName;
    }
    void AddCompilerCommand(const char * cmd)
    {
      ExtraCompilerCommands.push_back(cmd);
    }
    ToolChain * BuildToolChain = nullptr;
    Path WorkingPath;
    Path OutputFolder;
    void SetOutputFolder(const char* p)
    {
        OutputFolder = p;
    }
    Path IntermidiateDir;
    std::vector <Path> SourceFiles;
    void AddSourceFile (const char * path_str);
    void AddFilesMatchsRegex(const char * regex_exr);
    void AddIncludePath ( const char * path_str);
    void AddFilesMatchsRegexInFolder (Path BrowseDir ,std::regex rgx)
    {
      auto local_files = BrowseDir.BrowseRelative();
      for  (auto & p : local_files)
      {
          if (std::regex_match(p.ToStr(),rgx))
          {
              if (std::find(SourceFiles.begin(),SourceFiles.end(),p) == SourceFiles.end())
              AddSourceFile(BrowseDir + p);
          }
      }
    }
    void AddLinkLibrary(const char * );
    void SetType (const char * type)
    {
        Type = type;
    }
    const char * GetType() const
    {
        return lua_return_str(Type.c_str());
    }
    int Build();
    void PushSourceFile(Path&p)
    {
        bool does_not_match = false;
        const char * p_str = p.ToStr();
         for(auto & filter : AddSourceFilters)
            {
                std::regex rgx(filter);
                if (!std::regex_match(p_str,rgx))
                {
                        does_not_match = true;
                        break;
                }
            }
            if (!does_not_match)
            {
                SourceFiles.push_back(p);
            }
    }
    void AddSourceFilter(const char* f)
    {
        AddSourceFilters.push_back(f);
    }
    void AddLinkerCommand(const char * cmd)
    {
        commands.push_back(cmd);
    }
    void AddLinkSearchPath (Path p)
    {
        this->link_search_path.push_back(p);
    }
};

class MiracleExecuter : public Executer
{
    public :
    std::vector <Path> SearchPaths;
    Path ProjectSourcePath;
    REF_GETTER(SearchPaths);
    GETTER(ProjectSourcePath);
    void AddSearchPath ( Path & p)
    {
        if(!p.Exists()) return;
        SearchPaths.push_back(p);
    }
    void AddSearchPathRecursively ( Path & p)
    {
        if(!p.Exists()) return;
        SearchPaths.push_back(p);
        auto sub = p.Browse();
        for (auto & d : sub)
        {
            AddSearchPathRecursively(d);
        }
    }
   bool LoadFile(const char*path )
   {
       Path FilePath = path;
        std::ifstream file;
        file.open(path);
        std::string str((std::istreambuf_iterator<char>(file)),
                 std::istreambuf_iterator<char>());
        Path file_full_path = FilePath.AsFullPath();
        Path file_parent_path = file_full_path.GetParent();
        Path::PushPath(file_parent_path);
        ProjectSourcePath = file_parent_path;
        Executer::PrepareExecution();
        state.dostring(variables.Eval(str.c_str()));
           Path::PopPath();
       return true;
    }
   std::string GetErrorString(){return "";}
   void SetVariable (std::string var,std::string val)
   {
       Executer::SetVariable(var,val);
   }
   void InitLuaState()
   {
       Executer::InitLuaState();

      static auto executable_class = kaguya::UserdataMetatable<Executable>()
        .addFunction("SetPath",&Executable::Setpath)
        .addFunction("InvokeCommandHidden",&Executable::InvokeCommandHidden)
        .addFunction("GetConsoleOutput",&Executable::GetConsoleOutput)
        .addFunction("InvokeCommand",&Executable::InvokeCommand);
        state["Executable"] = executable_class;

      static auto toolchain_class = kaguya::UserdataMetatable<ToolChain>()
        .setConstructors<ToolChain()>()
        .addFunction("SetStaticLibraryExtension",&ToolChain::SetStaticLibraryExtension)
        .addFunction("SetObjectFileExtension",&ToolChain::SetObjectFileExtension)
        .addFunction("SetLinkerPath",&ToolChain::SetLinkerPath)
        .addProperty("CPPCompiler",&ToolChain::CPPCompiler)
        .addProperty("CCompiler",&ToolChain::CCompiler)
        .addProperty("StaticLinker",&ToolChain::StaticLinker)
        .addProperty("DynamicLinker",&ToolChain::DynamicLinker);
        state["ToolChain"] = toolchain_class;
       static auto compiler_class =
       kaguya::UserdataMetatable<Compiler,Executable>()
        .setConstructors<Compiler()>()
        .addFunction("GetPath",&Compiler::Getpath)
      //  .addProperty("Path",Compiler::Getpath,Compiler::Setpath)
        .addProperty("GetFileCompileCommandForStaticLibrary",&Compiler::GetFileCompileCommandForStaticLibrary)
        .addProperty("GetFileCompileCommandForSharedLibrary",&Compiler::GetFileCompileCommandForSharedLibrary)
        .addFunction("GetFileCompileCommand",&Compiler::GetFileCompileCommand);
        state["Compiler"].setClass(compiler_class);


            static auto linker_class =
       kaguya::UserdataMetatable<Linker,Executable>()
        .setConstructors<Linker()>()
        .addProperty("GetObjFilesLinkCommandToStaticLibrary",&Linker::GetObjFilesLinkCommandToStaticLibrary)
        .addProperty("GetObjFilesLinkCommandToExecutable",&Linker::GetObjFilesLinkCommandToExecutable);
        state["Linker"].setClass(linker_class);

       static auto process_console_output_class =
       kaguya::UserdataMetatable<ProcessConsoleOutput>()
        .setConstructors<ProcessConsoleOutput()>()
        .addFunction("GetStdOut",&ProcessConsoleOutput::GetStdOut)
        .addFunction("GetStdError",&ProcessConsoleOutput::GetStdError);
        state["ProcessConsoleOutput"].setClass(process_console_output_class);

       static auto target_class =
       kaguya::UserdataMetatable<Target>()
        .setConstructors<Target()>()
        .addFunction("AddSourceFile",&Target::AddSourceFile)
        .addFunction("AddLinkLibrary",&Target::AddLinkLibrary)
        .addFunction("AddLinkCommand",&Target::AddLinkerCommand)
        .addFunction("AddFilesMatchsRegex",&Target::AddFilesMatchsRegex)
        .addFunction("Build",&Target::Build)
        .addFunction("GetOutputFileName",&Target::GetOutputFileName)
        .addFunction("GetName",&Target::GetName)
        .addFunction("SetOutputFileName",&Target::SetOutputFileName)
        .addFunction("SetName",&Target::SetName)
        .addFunction("AddIncludePath",&Target::AddIncludePath)
        .addFunction("GetLog",&Target::GetLog)
        .addFunction("ClearLog",&Target::ClearLog)
        .addFunction("AddCompilerCommand",&Target::AddCompilerCommand)
        .addFunction("SetOutputFolder",&Target::SetOutputFolder)
        .addFunction("AddLinkerCommand",&Target::AddLinkerCommand)
        .addFunction("AddLinkSearchPath",&Target::AddLinkSearchPath)
        .addFunction("AddSourceFilter",&Target::AddSourceFilter)
        .addProperty("BuildToolChain",&Target::BuildToolChain)
        .addProperty("WorkingPath",&Target::WorkingPath)
        .addProperty("AddSourceFilters",&Target::AddSourceFilters)
        .addProperty("Type",Target::GetType,Target::SetType);
        state["Target"].setClass(target_class);
       static auto executer_class =
       kaguya::UserdataMetatable<MiracleExecuter>()
        .setConstructors<MiracleExecuter()>()
        .addFunction("AddSearchPath",MiracleExecuter::AddSearchPath)
        .addFunction("AddSearchPathRecursively",MiracleExecuter::AddSearchPathRecursively)
        .addFunction("GetProjectSourcePath",MiracleExecuter::GetProjectSourcePath);
        state["BuildSystem"].setClass(executer_class);

         state["bs"] = this;

        state["GCC_TOOLCHAIN"] = ToolChain::gcc;
   }
   static MiracleExecuter * GetExecuter()
   {
       static MiracleExecuter *  rv = nullptr;
       if (rv == nullptr)
       {
         rv =  new MiracleExecuter();
         rv->InitLuaState();
       }
       return rv;
   }

    std::vector <Path >GetFileIncludes (Path file ,std::vector <Path> include_paths = {})
   {
         std::vector <Path > rv;
         GetFileIncludes(file,include_paths,rv);
         return rv;
   }
   void GetFileIncludes(Path file , std::vector <Path > &include_paths,std::vector <Path >& rv)
   {
       rv.push_back(file);
       if (!file.Exists())
       {
           print("file does not exist\n");
           return;
       }
       std::ifstream input(file.ToStr());
       if (!input.is_open())
       {
           print("Cant open file\n");
          return;
       }
       char single_char = 0;
       std::string included_file = "";
       std::string command = "";
       enum INCLUDE_TYPE {UNSET,LOCAL_INCLUDE,SEARCH_INCLUDE};
       INCLUDE_TYPE include_type = UNSET;
       while (input.good())
       {
           input.get(single_char);
        if (single_char == '#')
        {

          while (input.good())
          {
                input.get(single_char);
               if (single_char == ' '){
                    while (input.good() && single_char==' ') input.get(single_char);
                    break; // '\"' or  '<' reached
                   }else if (single_char == '<' || single_char=='\"')
                   {
                       break;
                   }
                    command.push_back(single_char);
          }
          if (single_char == '<')
          {
             include_type = SEARCH_INCLUDE;
          }else if (single_char == '\"')
          {
             include_type = LOCAL_INCLUDE;
          } else
          {
               command = "";
               continue;
          }
          if (command == "include")
          {
              // current cursor position [ 'include ^' or  'include"^'  'include<^' ]
              // single_char = ' ' or '"' or '<'
             single_char = 0;
              while (input.good() && single_char != '\"' && single_char != '>')
              {
                    input.get(single_char);
                    if (single_char != '>' && single_char != '\"' )
                  included_file.push_back(single_char);
              }
              /*
                 load include file recursively


              */
                Path new_file_path;
                bool found = false;
              if (include_type == LOCAL_INCLUDE)
              {
                    // find file
                  new_file_path = file.GetParent() + included_file;
                  if (new_file_path.Exists())
                  {
                      if (std::find (rv.begin(),rv.end() ,new_file_path) == rv.end()){

                      GetFileIncludes(new_file_path,include_paths,rv);
                       found= true;
                      }
                  }
              }
              if (not found)
              {
                  for (auto& path : include_paths)
                  {
                      new_file_path =  path+included_file;
                                        if (new_file_path.Exists())
                                            {
                                                if (std::find (rv.begin(),rv.end() ,new_file_path) == rv.end()){

                                        GetFileIncludes(new_file_path,include_paths,rv);
                                        found = true; // extra
                                        break;
                                                }
                                          }
                  }
              }
              included_file = "";
          }
         // std::cout<<"'"<<command<<"'"<<std::endl;
          command = "";
        }
       }
   }

};

int Target::Build()
{
   if (!BuildToolChain)
   {
       log += "Build toolchain is not set .";
       return 1;
   }
   if (!BuildToolChain->StaticLibraryExtention.length())
   {
       log += "Build toolchain's static library files extension is not set .";
       return 1;
   }
   if (!BuildToolChain->ObjectFileExtention.length())
   {
       log += "Build toolchain's object files extension is not set .";
       return 1;
   }
   if (SourceFiles.size() == 0)
   {
       Log("Can't build with no source files .\n");
       return 1;
   }
   Path ObjDir;
   if (!WorkingPath.IsSet())
   {
       WorkingPath = MiracleExecuter::GetExecuter()->GetProjectSourcePath();
   }
   if (IntermidiateDir.IsSet())
   {
       if (IntermidiateDir.IsFullPath())
       {
            ObjDir = IntermidiateDir;
       }else
       {
           ObjDir = WorkingPath + IntermidiateDir;
       }
   }else
   {
       IntermidiateDir = WorkingPath + "obj";
       ObjDir = WorkingPath + "obj";
   }
   Path::MakeIfDoesntExit(ObjDir);
   std::vector<Path> ObjectFiles;
   enum TargetType {CONSOLE,STATIC_LIBRRY,SHARED_LIBRARY};
   TargetType target_type = (Type == "CONSOLE_APPLICATION") ? TargetType::CONSOLE : (  (Type == "SHARED_LIBRARY") ? TargetType::SHARED_LIBRARY :  TargetType::STATIC_LIBRRY);
   for (auto& f : SourceFiles)
   {
       Path obj_file = ObjDir + (f.GetFileName() + BuildToolChain->ObjectFileExtention);
       bool do_compile = false;
       auto deps = MiracleExecuter::GetExecuter()->GetFileIncludes(f,IncludePaths);
       if (obj_file.Exists())
        {
           for (auto & d : deps)
           {
                if (obj_file.GetLastModificationTime() < d.GetLastModificationTime())
                {
                    do_compile = true;
                    break;
                }

           }
       }else
       {
           do_compile = true;
       }
       if (do_compile){
       // print("compiling " ,f.ToString(),"\n");
       obj_file.Delete();
       Compiler * compiler = f.GetExtension() == ".c" ? BuildToolChain->CCompiler : BuildToolChain->CPPCompiler;
       bool status = true;
       std::string command ;
       if (target_type == TargetType::STATIC_LIBRRY)
       {

        print(compiler->path.ToStr());
           command = compiler->GetFileCompileCommandForStaticLibrary(f,obj_file,IncludePaths,ExtraCompilerCommands);
       status = compiler->InvokeCommand(command.c_str());
       } else  if (target_type == TargetType::CONSOLE)
       {
           command = compiler->GetFileCompileCommand(f,obj_file,IncludePaths,ExtraCompilerCommands);
           status = compiler->InvokeCommand(command.c_str());
       } else  if (target_type == TargetType::SHARED_LIBRARY)
       {
           command = compiler->GetFileCompileCommandForSharedLibrary(f,obj_file,IncludePaths,ExtraCompilerCommands);
           status = compiler->InvokeCommand(command.c_str());
       }
       if(status != 0)
       {
          if (status == Executable::InvocationResult::CANT_START_EXECUTABLE)
          log = as_string("Command execution failed ! \n Command :  '",compiler->Getpath().ToString() , " " ,command,"' .");
           return status;
       }

       }else
       {
      //  print(f.ToString()," is up to date .\n");
       }
       ObjectFiles.push_back(obj_file);
   }
   if (!this->OutputFolder.IsSet())
   {
     OutputFolder = MiracleExecuter::GetExecuter()->GetProjectSourcePath();
   }
   Path output_file = OutputFolder + OutputFileName;
   if (output_file.GetExtension() == "" )
   {
       if (Type == "CONSOLE_APPLICATION")
       {
         output_file.SetExtention(".exe");
       } else if (Type == "STATIC_LIBRARY")
       {
         output_file.SetExtention(BuildToolChain->StaticLibraryExtention);
       } else if (Type == "SHARED_LIBRARY")
       {
         output_file.SetExtention(".dll");
       }else
       {
        print("Unknown target type  : '" , Type , "'\n");
        return 1;
       }
   }

   output_file.Delete();
   if (Type == "CONSOLE_APPLICATION" || Type == "STATIC_LIBRARY")
    {
        if (Type=="CONSOLE_APPLICATION"){

        print(BuildToolChain->StaticLinker->path.ToStr());
        if(BuildToolChain->StaticLinker->InvokeCommand(BuildToolChain->StaticLinker->GetObjFilesLinkCommandToExecutable(ObjectFiles,output_file,link_libraries)))
            return 1;
        }else if (Type=="STATIC_LIBRARY")
        {

        print(BuildToolChain->StaticLinker->path.ToStr());
            if(BuildToolChain->StaticLinker->InvokeCommand(BuildToolChain->StaticLinker->GetObjFilesLinkCommandToStaticLibrary(ObjectFiles,output_file,link_libraries)))
                return 1;
        }
    } else if (Type == "SHARED_LIBRARY")
    {
        print(BuildToolChain->DynamicLinker->path.ToStr());
        if(BuildToolChain->DynamicLinker->InvokeCommand(BuildToolChain->DynamicLinker->GetObjFilesLinkCommandToSharedLibrary(ObjectFiles,output_file,link_libraries,link_search_path,commands)))
            return 1;
    } else
    {
        print("Unknown target type  : '" , Type , "'\n");
    }
    return 0;
//   BuildCompiler->InvokeCommand(GetFileCompileCommand());
}
void Target::AddLinkLibrary(const char * path_str)
{
    Path path = path_str;
    if (path.Exists())
    {
        link_libraries.push_back(Path::CurrentDir() + path);
        return;
    }
    auto search_paths = MiracleExecuter::GetExecuter()->GetSearchPaths();
    for (auto & p : search_paths)
    {
        Path resolved = p + path;
        if (resolved.Exists())
        {
            link_libraries.push_back(resolved);
            return;
        }
    }
    /// probably a compiler library
    link_libraries.push_back(path_str);
}
/** \brief Adds a source file to the build target , a source file path should be passed to this method
 * the path should be an absolute path , or should be relative to the current working process directory
 * or relative to the project's source folder , if the source file was never found in any of those , that
 * will cause the build system to abort the build and exit with an error msg
 * \param
 * \return
 */
void Target::AddSourceFile (const char * path_str)
{
    /*Path path = path_str;
    Path to_push;
    if (path.IsFullPath())
        to_push = path;
    else
        to_push = MiracleExecuter::GetExecuter()->GetWorkingPath() + path_str;
    SourceFiles.push_back(to_push);*/
    struct LocalException{std::string msg;};
    try{
    Path path = path_str;
    if (path.IsFullPath())
    {
       if (path.Exists())
       {
           PushSourceFile(path);
           return;
       }else
       {
           throw LocalException{ as_string(path.ToString() , " does not exist")};
       }
    } /// relative path
    Path full_path = Path::CurrentDir() + path;
    if (full_path.Exists())
    {
           PushSourceFile(full_path);
           return;
    }else
    {
        full_path = MiracleExecuter::GetExecuter()->GetProjectSourcePath() + path;
        if (full_path.Exists())
        {
           PushSourceFile(full_path);
           return;
        }else
        {
           auto & search_paths = MiracleExecuter::GetExecuter()->GetSearchPaths();
            for (auto & p :search_paths)
            {
                full_path = p + path;
                if (full_path.Exists())
                {
                   PushSourceFile(full_path);
                   return;
                }
            }
            throw LocalException{ as_string( path.ToString() , " can not be located ." ) };
        }
    }
    } catch ( LocalException e )
    {
        print("Unable to add source file , error : ",e.msg);
        exit(1);
    }
}
void Target::AddFilesMatchsRegex(const char * regex_exr)
{
  std::regex regex(regex_exr);
  AddFilesMatchsRegexInFolder(Path::CurrentDir(),regex);
  AddFilesMatchsRegexInFolder(WorkingPath,regex);
  for (auto& p : MiracleExecuter::GetExecuter()->GetSearchPaths())
    AddFilesMatchsRegexInFolder(p , regex);
}
void Target::AddIncludePath ( const char * path_str)
    {
       Path path = path_str;
       if (path.IsFullPath())
       {
           if (!path.Exists())
           {
               print(path.ToString() ,  " does not exist .");
               exit(1);
           }
           IncludePaths.push_back(path);
           return;
       }
       Path resolved = MiracleExecuter::GetExecuter()->GetProjectSourcePath() + path;
    if (!resolved.Exists())
    {
         print(path.ToString() , " Can not be located .");
         exit(1);
    }
    IncludePaths.push_back(resolved);
    }
#include <windows.h>
#include <io.h>


#define SCRIPT_FILE_EXTENSION ".ubs"

int main(int argc, const char* argv[])
{
   if (getenv("MIRACLE_HOME") == nullptr)
   {
       print("Cannot build before setting up umbrella . ");
       return 1;
   }
   MiracleExecuter * executer = MiracleExecuter::GetExecuter();

    if (argc < 2)
    {
        Path file = Path::CurrentDir() + "build"  SCRIPT_FILE_EXTENSION;
        if (file.Exists())
        {
           executer->LoadFile(file.ToStr());
        }else
        {
            print("No arguments are given and no 'build" SCRIPT_FILE_EXTENSION " found !");
            return 1;
        }
    }else
    {
        Path file = argv[1];
        for (int  i = 2;i<argc;i+=2)
        {
            executer->SetVariable(argv[i],argv[i+1]);
        }
        if (file.Exists())
        {
            if (file.GetExtension() == SCRIPT_FILE_EXTENSION)
            {
                executer->LoadFile(file.ToStr());
            }else{
                print("Only '" SCRIPT_FILE_EXTENSION "' file extension are supported .");
            }
        }
    }
    delete executer;
    return 0;
}
