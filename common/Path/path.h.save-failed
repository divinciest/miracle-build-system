#pragma once
#ifndef __PATH_HPP_INCLUDED__
#define __PATH_HPP_INCLUDED__

#include <chrono>
#include <vector>
#include <string>
#define PATH_MAX_LENGTH 256
class Path
{
private :
    std::vector<std::string> components;
    static std::vector <Path> CurrentPathStack;
    static std::string CStringReturnBuffer;
public :
    char buffer[PATH_MAX_LENGTH];
    Path();
    Path(std::string s);
    Path(const Path& p);
    Path(const char*);
    static bool DecomposeStringPath(std::string p,std::vector<std::string>& components);
    std::string ToString()const;
    Path GetParent();
    bool IsFullPath() const;
    Path operator + (const Path& p1) const;
    Path operator + (std::string s);
    Path operator + (const char*);
    Path operator = (Path);
    bool operator == (const Path&);
    operator const char* ();
    std::string GetExtension();
    void SetExtention(std::string);
    static void DeepCopy(Path,Path);
    static bool CopyFile(Path,Path);
    static bool MakeDir(Path);
    static Path Create(const char*);
    void  AddStrPath(const char*);
    void UpdateBuffer();
    bool IsFile();
    bool IsFolder() const;
    std::string GetFileName();
    const char* ToStr();
    const char* ToStrAntislash();
    static Path* Concat(Path*,Path*);
    unsigned int GetComponentCount();
    std::string GetComponent(unsigned int);
    void ReplaceComponent(unsigned int,std::string);
    bool MakeRelative(Path&);
    const char * StringCopy();
    static const char * ConcatStrPath2Str(const char * ,const char*);
    static Path ConcatStrPath(const char * ,const char*);
    static bool StrPathCompare(const char * p1 , const char * p2);
    static bool ChDir(const char * p1);
    static Path CurrentDir();
    void AppendComponent(const char*);
    void DropOne();
    void Reset();
    static bool CreateSymLink(Path* src,Path* trgt);
    bool Exists();
    static void PushPath(const char* p);
    static void PopPath();
    void Resolve();
    void Delete();
    Path AsFullPath();
    std::time_t GetLastModificationTime();
    static const char* ResolvePath(const char*);
    static const char * ReturnCString(const char *);
    std::vector<Path> Browse();
    std::vector<Path> BrowseRelative();
    static bool MakeIfDoesntExit(Path&);
    inline bool IsSet(){return GetComponentCount()!=0;}
    static std::vector <Path> GetSystemPaths();
};

#endif // __PATH_HPP_INCLUDED__
