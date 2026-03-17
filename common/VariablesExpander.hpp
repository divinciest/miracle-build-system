#pragma once
#include <Helpers/Helpers.hpp>
class VariablesExpander
{
private :
    typedef std::vector <std::string> VarResolvingStack;
    std::map<std::string,std::string> var_map;
public :
    std::string Eval(std::string to_eval)
    {
        if (to_eval =="") return to_eval;
        SetVariable("last_eval",to_eval);
        std::string eval = GetEvaluetedValue("last_eval");
        return eval;
    }
    const char *  EvalToCString(const char *  to_eval)
    {
        return  strdup(Eval(to_eval).c_str());
    }
    std::string GetVariableValue(std::string var)
    {
        static int counter = 0;
        if (var == "counter")
        {
            return as_string(counter++);
        }
       return var_map[var];
    }
    void SetVariable(std::string name,std::string value)
    {
        var_map[name] = value;
    }
    const char * GetVariableValueStr(const char *  val)
    {
        return strdup( GetVariableValue( std::string(val) ).c_str() );
    }
    std::string GetEvaluetedValue(std::string var,std::vector<std::string>& dependies)
    {
        auto decomposed = DecomposeString(GetVariableValue(var));

        if (decomposed.size() > 1 || ( decomposed.size() == 1 && decomposed[0].first==true ))
        {
            //int old_size = dependies.size();
            /*for (auto p : decomposed)
            {
                if (p.first)
                {
                    dependies.push_back(p.second);
                }
            }*/

            for (auto & d : dependies)
            {
                if (var == d){
                    print("WARNNING : interdependency detected for the following variables : ");
                for (auto & d : dependies)
                {
                    print(d,"\n");
                }
                    return "$(null)";
                }
            }
            std::string value;
            for (auto p : decomposed)
            {
                if (p.first)
                {
                    dependies.push_back(var);
                    value += GetEvaluetedValue(p.second,dependies);
                    dependies.pop_back();
                }
                else
                {
                    value+=p.second;
                }
            }
            return value;
        }
        else
        {
            std::string rv = GetVariableValue(var);
            return rv.length()?rv:"$(null)";
        }
    }
    std::string GetEvaluetedValue(std::string var)
    {
        std::vector<std::string> dependies_stack;
        return GetEvaluetedValue(var,dependies_stack);
    }
    std::vector <std::pair<bool,std::string>> DecomposeString(std::string str)
    {
        enum MODE {NONE, EXPECTING_VAR,READING_NAME};
        MODE mode =NONE;
        std::string name;
        std::string txt;
        std::vector <std::pair<bool,std::string>>  rv;
        for (auto it = str.begin(); it!=str.end(); it++)
        {
            char c = *it;
            if (mode == READING_NAME)
            {
                if (c== ')')
                {
                    if (name.length())
                        rv.push_back(std::make_pair(true,name));
                    name = "";
                    mode = NONE;
                }
                else
                {
                    name += c;
                }
            }
            else
            if (  mode == EXPECTING_VAR )
            {
                if (c=='(')
                {
                    mode = READING_NAME;
                    if (txt.length())
                        rv.push_back(std::make_pair(false,txt));
                    txt="";
                }else
                {
                    mode =NONE;
                    txt += "$"; // '$' was mistaken
                    txt += c;
                }
            }
            else if (c == '$'&& mode == NONE)
            {
                mode = EXPECTING_VAR;
            }
            else
            {
                txt+=c;
            }
        }
        assert(!(txt.length() && name.length()));
        if (txt.length())
        {
            rv.push_back(std::make_pair(false,txt));
        }
        else if (name.length())
        {
            rv.push_back(std::make_pair(false,name)); // false because we didnt reach ')'
        }
        return rv;
    }
    std::string ParseName(std::string to_parse)
    {
        // to_parse => '$(' + var_name + ')'
        auto p1 = to_parse.find_first_of("(")+1;
        auto p2 = to_parse.find_last_of(")");
        return to_parse.substr(p1,p2-p1);
    }
    void ResolveValue(VarResolvingStack& stack,std::string value)
    {

    }
};
