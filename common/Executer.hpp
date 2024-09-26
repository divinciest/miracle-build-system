#pragma once
#include "VariablesExpander.hpp"
#include "kaguya/kaguya.hpp"
#include "Path/Path.h"

inline void error_handler(int status, const char *message)
{
    KAGUYA_UNUSED(status);
    std::cout<<"Error : "<<message<<std::endl;
}
class Executer
{
   protected:
       kaguya::State state;
       VariablesExpander variables;
   public :
    virtual ~Executer(){}
    Executer(){}
    virtual void InitLuaState()
    {
         static auto  VariablesExpanderClass =

            kaguya::UserdataMetatable<VariablesExpander>()
            .setConstructors<VariablesExpander()>()
            .addFunction("SetVariable",VariablesExpander::SetVariable)
            .addFunction("GetVariableValueStr",VariablesExpander::GetVariableValueStr)
            .addFunction("Eval",VariablesExpander::EvalToCString);
            std::function < Path (Path *  ,const char * ) > PathPlus  = []  (Path * thiz ,const char * that) -> Path {
                return (*thiz) + (that);
                };
          static auto PathClass =  kaguya::UserdataMetatable<Path>()
            .setConstructors<Path(const char*)>()
            .addStaticFunction("MakeDir",&Path::MakeDir)
            .addStaticFunction("Concat",&Path::Concat)
            .addStaticFunction("ConcatStrPath2Str",&Path::ConcatStrPath2Str)
            .addStaticFunction("ConcatStrPath",&Path::ConcatStrPath)
            .addStaticFunction("StrPathCompare",&Path::StrPathCompare)
            .addStaticFunction("CurrentDir",&Path::CurrentDir)
            .addStaticFunction("ChDir",&Path::ChDir)
            .addStaticFunction("CreateSymLink",&Path::CreateSymLink)
            .addStaticFunction("PushPath",&Path::PushPath)
            .addStaticFunction("PopPath",&Path::PopPath)
            .addStaticFunction("ResolvePath",&Path::ResolvePath)
            .addStaticFunction("DeepCopy",&Path::DeepCopy)
            .addFunction("Exists",&Path::Exists)
            .addFunction("AddStrPath",&Path::AddStrPath)
            .addFunction("Browse",&Path::Browse)
            .addFunction("ToStr",&Path::ToStr)
            .addFunction("ToStrAntislash",&Path::ToStrAntislash)
            .addFunction("AppendComponent",&Path::AppendComponent)
            .addFunction("GetParent",&Path::GetParent)
            .addStaticFunction("Plus",PathPlus)
            .addStaticFunction("MakeIfDoesntExit",Path::MakeIfDoesntExit)
            .addFunction("Delete",&Path::Delete);
        state.setErrorHandler( error_handler );
        state["Path"].setClass(PathClass);
        state["VariablesExpander"].setClass(VariablesExpanderClass);
        state["variables"] = &variables;
        const char * home = getenv("UMBRELLA_HOME");
        if (home == nullptr)
        {
           print("Running script requires umbrella to be setup , no 'UMBRELLA_HOME' environment variable found , run 'umb setup ...' .");
           exit(1);
        }
        state["UMBRELLA_HOME"] = home;
    }
    virtual bool LoadFile(const char*) = 0;
    virtual std::string GetErrorString() = 0;
    virtual  void SetVariable (std::string var,std::string val)
    {
       variables.SetVariable(var,val);
    }
     virtual void PrepareExecution()
    {
        variables.SetVariable("this_folder",Path::CurrentDir().ToString());
    }
};
