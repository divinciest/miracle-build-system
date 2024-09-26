#include "process.hpp"
#include <cassert>
#include <iostream>
using namespace std;
using namespace TinyProcessLib;
int main()
{
  auto output=make_shared<string>();
  auto error=make_shared<string>();
  {
    Process process("cmd echo dhia", "", [output](const char *bytes, size_t n) {
      *output+=string(bytes, n);
    }, [error](const char *bytes, size_t n) {
      *error+=string(bytes, n);
    });
    int status;
    while(!process.try_get_exit_status(status));
    //assert(process.get_exit_status()>0);
    //assert(output->substr(0, 4)=="Test");
    //assert(!error->empty());
    std::cout<<*output<<std::endl;
   std::cout<<"Exit status  : "<<status<<std::endl;
    output->clear();
    error->clear();
  }
}
