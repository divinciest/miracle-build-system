#include "path.h"
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <fstream>
#define  IS_SEPERATOR(X) (X=='\\' or X=='/')
#define DEFAULT_SEPERATOR '/'
#include <stdexcept>
#include <Helpers/Helpers.hpp>
#include "path.h"
Path::Path(){}
Path::Path(std::string s)
{
    if(!DecomposeStringPath(Helpers::String::RemoveSpacesFromSides(s),components))
      throw std::invalid_argument("Given string is not a valide path");
}
Path::Path(const char * s):Path(std::string(s))
{
}
Path Path::GetParent()
{
    if (components.size() == 0) return *this;
    Path parent = *this;
    parent.components.pop_back();
    return parent;
}
Path::Path(const Path& p):components(p.components) {}
std::string Path::GetExtension()
{
    if (components.size() == 0) return "";
    std::string last = components.back();
    int pos = 0;
    int len = last.length();
    pos = last.find_last_of(".");
    if (pos==-1)
        return "";
    return last.substr(pos,len-pos);
}
void Path::Reset()
{
    components.clear();
    buffer[0] = 0;
}
void Path::DeepCopy(Path f1,Path  f2)
{
    if (f1 == f2) return;
    if (f1.IsFile())
    {
        assert(Path::CopyFile(f1,f2));
    }else
    if (f1.IsFolder())
    {
        assert(MakeDir(f2));
        DIR * f1d = opendir(f1);
        assert(f1d);
        dirent* entry = nullptr;
        readdir(f1d);readdir(f1d);
        while ((entry = readdir(f1d)) !=nullptr )
        {
          DeepCopy(f1 + entry->d_name,f2 + entry->d_name);
        }
        closedir(f1d);
    }
}
Path* Path::Concat(Path* p1,Path* p2)
{
    return new Path( (*p1) + (*p2));
}
bool  Path::ChDir(const char * p1)
{
    return (chdir(p1) == 0);
}
Path Path::CurrentDir()
{
    char  buf[256];
    const char * rv = getcwd(&buf[0],256);
    if (rv)
    return Path(rv);
    return Path();
}
/**
 @Experimental
**/
bool Path::MakeRelative(Path& path)
{
    if (path.components.size() == 0) return false;
    if (components.size() == 0) return false;
    int pos_in_this_path = 0;
    bool start_removing = false;
    for (auto & c : components) //// for each component of this path
    {
        if (c == *path.components.begin()) //// if one of them match the beginning f the other path
        {
            if (components.size()-pos_in_this_path  > path.components.size() ) /// this path is more deep than the other , it can't be relative to it
            {
                /// we should go back some steps
                unsigned int depth_difference = (components.size() - pos_in_this_path)-path.components.size();
                path.components.clear();
                for (unsigned  i = 0;i<depth_difference;i++)
                 {
                     path.components.push_back(".."); /// go back honey !
                 }
                 return true;
            }
            start_removing = true; /// we start removing from that point all common components
            path.components.erase(path.components.begin());
        }else{ /// components does not match
          if (start_removing) /// did we al ready find a common component ?
            break;/// no more common components
        }
        pos_in_this_path++;
    }
    return start_removing;
}
Path Path::Create(const char* s)
{
    return Path(s);
}
bool Path::MakeDir(Path dir)
{
     unsigned int count = dir.GetComponentCount();
    if (count == 0) return false;
    std::string current_path;
    DIR* current_dir = nullptr;
    std::string top_created_folder = "";
    for(unsigned int c = 0; c<count; c++)
    {
        current_path += dir.GetComponent(c);
        current_path.push_back('/');
        if ( (current_dir = opendir(current_path.c_str())) == nullptr )
        {
            if (!mkdir(current_path.c_str()))
            {
                if (top_created_folder!="")
                {
                    remove(current_path.c_str());
                    return false;
                }
            }
            else
            {
                if (top_created_folder == "")  top_created_folder = current_path;
            }
        }
        closedir(current_dir);
    }
    return true;
}
bool Path::CopyFile(Path f1,Path f2)
{
    std::ifstream  src(f1, std::ios::binary);
    if (!src.good()) return false;
    std::ofstream  dst(f2,   std::ios::binary);
    if (!dst.good()) return false;
    dst << src.rdbuf();
    return true;
}
bool Path::IsFolder() const
{
#ifdef WIN32
    if (components.size() == 1 && components[0].length()>1 && components[0][1] == ':') return true;
#endif // WIN32
    struct stat path_stat;
    std::string path_cpp_str = ToString();
    stat(path_cpp_str.c_str(), &path_stat);
    return S_ISDIR(path_stat.st_mode);
}
bool Path::IsFile()
{
    struct stat path_stat;
    std::string path_cpp_str = ToString();
    stat(path_cpp_str.c_str(), &path_stat);
    return S_ISREG(path_stat.st_mode);
}
Path Path::operator+ (std::string s)
{
    Path p(s);
    return (*this)+p;
}
Path Path::operator+ (const char* s)
{
    Path p(s);
    return (*this)+p;
}
Path Path::operator=(Path p)
{
    components.clear();
    for(auto c:p.components) components.push_back(c);
    return p;
}

const char* Path::ToStr()
{
    UpdateBuffer();
    return & buffer[0];
}
const char* Path::ToStrAntislash()
{
    UpdateBuffer();
    std::string rv = buffer;
    Helpers::String::ReplaceOccurences(rv,"/","\\");
    return strdup(rv.c_str());
}
Path Path::operator + (const Path& p1) const
{
    if(p1.IsFullPath())
    {
        return p1;
    }
    Path rv(*this);
    for (unsigned int c=0; c<p1.components.size(); c++)
    {
        rv.components.push_back(p1.components[c]);
    }
    return rv;
}
std::string Path::ToString() const
{
    std::string rv;
    if (!components.size()>1)
        rv.push_back(DEFAULT_SEPERATOR);
    for(unsigned int c=0; c<components.size(); c++)
    {
        rv+=components[c];
        if (c == components.size()-1) break;
        rv.push_back(DEFAULT_SEPERATOR);
    }
    if (components.size() == 1&& IsFullPath())
        rv.push_back(DEFAULT_SEPERATOR);
    return rv;
}
const char *  Path::StringCopy()
{
    UpdateBuffer();
    char * rv = new char[strlen(buffer)+1];
    strcpy(rv,buffer);
    return rv;
}
void Path::DropOne()
{
    components.pop_back();
}
const char *  Path::ConcatStrPath2Str(const char * str_p1,const char* str_p2)
{
   return (Path(str_p1)+str_p2).StringCopy();
}

Path  Path::ConcatStrPath(const char * str_p1,const char* str_p2)
{
   return (Path(str_p1)+str_p2);
}
bool Path::operator == (const Path& other)
{
    if (other.components.size() != components.size()) return false;
        for (unsigned int i = 0;i<components.size();i++)
    {
        if (components[i] != other.components[i]) return false;
    }
    return true;
}
bool Path::StrPathCompare(const char * p1 , const char * p2)
{
   return (Path(p1)==(Path(p2)));
}
bool Path::IsFullPath() const
{
#if defined(_WIN32)
   if (components.size() == 0)
   return false;
   return components[0].find(":")<components[0].length();
#else
#error "No implimentation for this platform"
#endif
}
bool Path::DecomposeStringPath(std::string p,std::vector<std::string>& components)
{
    bool error = false;
    std::string component="";
    components.clear();
    while (p.length() && IS_SEPERATOR( p[0]))
    {
        p.erase(p.begin());
    }
    char current = '\0';
    for (unsigned int c =0; c<p.length(); c++)
    {
        current = p[c];
        if (IS_SEPERATOR(current))
        {
          /*  if (component == "")
            {
                //error = true; ignore
                continue;
            }*/
            if (component.length())
            components.push_back(component);
            component="";
        }
        else
        {
            if (current=='<' || current=='>' || current=='?' || current=='|' || current=='"' || current=='*')
            {
                error = true;
                break;
            }
            else
            {
                if (current == ':')
                {
                    if (components.size() != 0) // not first component => not drive letter
                    {
                        error = true;
                        break;
                    }
                }
                component.push_back(current);
            }
        }
    }
    if (component.length()) components.push_back(component);
    if (components.size() == 0) error = true;
    return !error;
}
unsigned int Path::GetComponentCount()
{
  return components.size();
}
std::string Path::GetComponent(unsigned int index)
{
  assert(index>=0 && index<components.size());
  return components[index];
}
void Path::ReplaceComponent(unsigned int index,std::string c)
{
    components[index] = c;
}
Path::operator const char* ()
{
    UpdateBuffer();
    return buffer;
}
void Path::UpdateBuffer()
{
    strcpy(buffer,ToString().c_str());
}
void Path::AppendComponent(const char* c)
{
    if (c == nullptr) return;
    components.push_back(c);
}

#include <chrono>
#include <experimental/filesystem>
std::time_t Path::GetLastModificationTime()
{
    std::experimental::filesystem::path p = ToStr();
    auto ftime = std::experimental::filesystem::last_write_time(p);
    std::time_t cftime = decltype(ftime)::clock::to_time_t(ftime); // assuming system_clock
    return cftime;
}

void Path::SetExtention(std::string ex)
{
    if (components.size() == 0)
    {
        components.push_back(ex);
        return;
    }
    auto pos = components.back().find_last_of(".");
    if (pos != std::string::npos)
        components.back().replace(pos, components.back().length()-pos,ex.c_str());
        else
    components.back() += ex;
}
std::string Path::GetFileName()
{
    if (components.size() == 0) return "";
    std::string last = components.back();
    return last.substr(0,last.length()-this->GetExtension().length());
}
#include <experimental/filesystem>
bool Path::CreateSymLink(Path* the_new,Path* the_old)
{
  if (!(the_new&&the_old)) return false;
    std::experimental::filesystem:: create_symlink(the_old->ToStr(),the_new->ToStr());
    return std::experimental::filesystem::is_symlink(the_new->ToStr());
}
Path Path::AsFullPath()
{
    if (this->IsFullPath()) return *this;
    return Path::CurrentDir() + *this;
}
bool Path::Exists()
{
    return  std::experimental::filesystem::exists(ToStr());
}
void Path::PushPath(const char* p)
{
    CurrentPathStack.push_back(Path(p));
    ChDir(p);
}
void Path::PopPath()
{
    if (CurrentPathStack.size() != 0)
     CurrentPathStack.pop_back();
    if (CurrentPathStack.size() != 0)
    {
        ChDir(CurrentPathStack.back().ToStr());
    }
}
void Path::Resolve() // Remove return points
{
   decltype(components) stack;
   for (unsigned int  i = 0;i<components.size();i++)
   {
       if (components[i] == "..")
       {
           stack.pop_back();
       }else
       {
           stack.push_back(components[i]);
       }
   }
    components = stack;
}
std::vector<Path> Path::Browse()
{
    DIR * dir = opendir(ToStr());
    if (dir == nullptr) return {};
    readdir(dir);
    readdir(dir);
    dirent * entry = nullptr;
    std::vector <Path> rv;
    Path copy = *this;
    while ((entry = readdir(dir)) != nullptr)
    {
      copy.AppendComponent(entry->d_name);
      rv.push_back(copy);
      copy.components.pop_back();
    }
    return rv;
}
std::vector<Path> Path::BrowseRelative()
{
    DIR * dir = opendir(ToStr());
    if (dir == nullptr) return {};
    readdir(dir);
    readdir(dir);
    dirent * entry = nullptr;
    std::vector <Path> rv;
    while ((entry = readdir(dir)) != nullptr)
    {
      rv.push_back(Path(entry->d_name));
    }
    return rv;
}
bool Path::MakeIfDoesntExit(Path& p)
{
    if (p.Exists()) return true;
    if (p.GetExtension() == "")
    {
       return Path::MakeDir(p);
    }
    return false;
}
void Path::Delete()
{
    UpdateBuffer();
    remove(buffer);
}
void  Path::AddStrPath(const char*p)
{
  *this = *this + Path(p);
}
const char * Path::ReturnCString(const char * str)
{
   CStringReturnBuffer = str;
   return CStringReturnBuffer.c_str();
}
const char * Path::ResolvePath(const char* p)
{
    Path p1(p);
    p1.Resolve();
   return ReturnCString(p1.ToStr());
}

#include <iterator>

template<typename Out>
void split(const std::string &s, char delim, Out result) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        *(result++) = item;
    }
}

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, std::back_inserter(elems));
    return elems;
}
#include <functional>
std::vector < Path> Path::GetSystemPaths()
{
  const char *  path_var = getenv("path");
  if (path_var == nullptr) return {};
  std::vector <std::string> paths = split(std::string(path_var),';');
  std::function<std::string(std::string)> expand = [&expand] (std::string  s) -> std::string
  {
      std::string rv;
      std::string elm;
      enum MODE{IN_ENV,OUTOF_ENV};
      MODE mode = MODE::OUTOF_ENV;
      for (auto&c : s)
      {
          if (c == '%')
          {
              if (mode == MODE::OUTOF_ENV)
              {
                    mode =MODE::IN_ENV;
              } else if (mode == MODE::IN_ENV)
              {
                    const char *  val = getenv(elm.c_str());
                    if (val != nullptr)
                    {
                        elm = expand(std::string(val));
                    } else
                    {
                        elm = "%" + elm + "%"; // sorry buddy XD
                    }
                 mode = MODE::OUTOF_ENV;
              }
              rv += elm;
              elm = "";
          }else
          {
              elm.push_back(c);
          }
      }
      if (elm != "" && elm!="." && elm.find_first_not_of(" ") != std::string::npos) rv += elm;
       return rv;
  };
  std::vector <Path> rv;
  for (auto& p : paths)
  {
      p = expand(p);
      if (p.length() && p.find_first_not_of(" ") != std::string::npos)
      rv.push_back(p);
  }
return rv;
}
std::vector<Path> Path::CurrentPathStack = {Path::CurrentDir()};
std::string Path::CStringReturnBuffer;
