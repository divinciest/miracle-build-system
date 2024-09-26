#pragma once
#ifndef __HERLPERS_HPP_INCLUDED__
#define  __HERLPERS_HPP_INCLUDED__
#include <string>
#include <iostream>
#include <sstream>
namespace Helpers
{
    namespace String{

    inline    std::string RemoveSpacesFromSides(std::string str)
    {
        while (str.length() && str.back() == ' ')
            str.pop_back();
        while (str.length() && str.front() == ' ')
            str.erase(str.begin());
        return str;
    }


    /** \brief Parses a string and return sequences of string , text+variable_name + text + variable_name ...
     *   the return value is a vector of pairs of string and bool , with bool indicating whether an element is a variable name or not
     */
    inline void ReplaceOccurences (std::string & chartDataString,const std::string &s,const std::string& t )
    {
        std::string::size_type n = 0;
        while ( ( n = chartDataString.find( s, n ) ) != std::string::npos )
        {
            chartDataString.replace( n, s.size(), t );
            n += t.size();
        }
    }
    }
}

template<typename ...Args>
std::string as_string(Args&&... args)
{

    std::stringstream ss;
    (ss << ... << args);
    return ss.str();
}
template<typename ...Args>
void print(Args&&... args)
{

    (std::cout << ... << args) << '\n';
}
template<typename ...Args>
void print_note(Args&&... args)
{

    std::cout<<"Note : "<<std::endl;
    (print(args), ...);
}
#define GETTER(X) inline decltype(X) Get##X()const{return X;}
#define REF_GETTER(X) inline decltype(X)& Get##X(){return X;}
#define SETTER(X) inline void Set##X( decltype(X) X_){X = (decltype(X)) (X_);}

#define CONCAT1(X,Y) X##Y
#define CONCAT(X,Y) CONCAT1(X,Y)

#endif // __HERLPERS_HPP_INCLUDED__
