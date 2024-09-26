/*
Copyright (C)  2004 Artem Khodush

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. The name of the author may not be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <libexecstream/exec-stream.h>

#include <vector>
#include <list>
#include <map>
#include <string>
#include <iostream>
#include <sstream>

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <climits>
#include <cstdio>

#ifdef _WIN32

#include <windows.h>

void sleep( int seconds )
{
    Sleep( seconds*1000 );
}

#else

#include <unistd.h>

#endif

// class for collecting and printing test results

class test_results_t {
public:
    static void add_test( std::string const & name );
    static bool register_failure( std::string const & assertion, std::string const & file, int line );
    static int print( std::ostream & o );

    class error_t : public std::exception {
    public:
        error_t( std::string const & msg )
        : m_msg( msg )
        {
        }

        ~error_t() throw()
        {
        }

        virtual char const * what() const throw()
        {
            return m_msg.c_str();
        }

    private:
        std::string m_msg;
    };

private:
    struct failure_t {
        std::string assertion;
        std::string file;
        int line;
    };
    typedef std::vector< failure_t > failures_t;
    struct result_t {
        std::string test_name;
        failures_t failures;
    };
    typedef std::vector< result_t > results_t;

    static results_t m_results;
    static bool m_printed;

    struct force_print_t {
        ~force_print_t();
    };
    friend struct force_print_t;

    static force_print_t m_force_print;
};

test_results_t::results_t test_results_t::m_results;
bool test_results_t::m_printed;
test_results_t::force_print_t test_results_t::m_force_print;

void test_results_t::add_test( std::string const & name )
{
    results_t::iterator i=m_results.begin();
    while( i!=m_results.end() && i->test_name!=name ) {
        ++i;
    }
    if( i!=m_results.end() ) {
        throw error_t( "test_results_t::add_test: duplicate test name: "+name );
    }
    result_t & r=*m_results.insert( m_results.end(), result_t() );
    r.test_name=name;
}

bool test_results_t::register_failure( std::string const & assertion, std::string const & file, int line )
{
    if( m_results.empty() ) {
        throw error_t( "test_results_t::register_failure: called without add_test() first" );
    }
    failures_t & failures=m_results.back().failures;
    failure_t & failure=*failures.insert( failures.end(), failure_t() );
    failure.assertion=assertion;
    failure.file=file;
    failure.line=line;
    return true;
}

int test_results_t::print( std::ostream & o )
{
    int n_ok=0;
    int n_failed=0;
    for( results_t::iterator res_i=m_results.begin(); res_i!=m_results.end(); ++res_i ) {
        if( res_i->failures.empty() ) {
            ++n_ok;
        }else {
            o<<"FAILED TEST: "<<res_i->test_name<<"\n";
            for( failures_t::iterator fail_i=res_i->failures.begin(); fail_i!=res_i->failures.end(); ++fail_i ) {
                o<<"    "<<fail_i->assertion<<" [at file "<<fail_i->file<<" line "<<fail_i->line<<"]\n";
            }
            ++n_failed;
        }
    }
    o<<"\nOK: "<<n_ok<<" FAILED: "<<n_failed<<"\n";
    m_printed=true;
    return n_failed;
}

test_results_t::force_print_t::~force_print_t()
{
    if( !test_results_t::m_results.empty() && !test_results_t::m_printed ) {
        test_results_t::print( std::cerr );
        std::cerr<<"UNEXPECTED TESTSUITE TERMINATION\n";
        std::cerr<<"while running test '"<<m_results.back().test_name<<"'\n";
    }
}

#define TEST_NAME(n) test_results_t::add_test(n);
#define TEST(e) ((e) || test_results_t::register_failure( #e, __FILE__, __LINE__ ))

std::string read_all( std::istream & i )
{
    std::string one;
    std::string ret;
    while( std::getline( i, one ).good() ) {
        ret+=one;
        ret+="\n";
    }
    ret+=one;
    return ret;
}

std::string random_string( std::size_t size )
{
    std::string s;
    s.reserve( size );
    srand( 1 );
    while( s.size()<size ) {
        char c=rand()%CHAR_MAX;
        if( isprint( c ) && !isspace( c ) ) {
            s+=c;
        }
    }
    return s;
}

bool check_if_english_error_messages()
{
#ifdef _WIN32
    std::string s;
    LPVOID buf;
    if( FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
        0,
        1,
        MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
        (LPTSTR) &buf,
        0,
        0
        )==0 ) {
        return false;
    }else {
        s=(LPTSTR)buf;
        LocalFree( buf );
        return s.find( "Incorrect function" )!=std::string::npos;
    }
#else
    std::string s( strerror( 1 ) );
    return s.find( "Operation not permitted" )!=std::string::npos;
#endif
}

// each function below is run as a child process for some test

int hello()
{
    std::cout<<"hello";
    return 0;
}

int helloworld()
{
    std::cout<<"hello\n";
    std::cout<<"world";
    return 0;
}

int hello_o_world_e()
{
    std::cout<<"hello";
    std::cerr<<"world";
    return 0;
}

int with_space()
{
    std::cout<<"with space ok\n";
    return 0;
}

int write_after_pause()
{
    sleep( 10 );
    std::cout<<"after pause";
    return 0;
}

int read_after_pause()
{
    sleep( 10 );
    std::string s;
    std::cin>>s;
    if( s==random_string( 20000 ) ) {
        std::cerr<<"OK";
    }else {
        std::cerr<<"ERROR";
    }
    return 0;
}

int dont_read()
{
    return 0;
}

int dont_stop()
{
    std::string s=random_string( 100 )+"\n";
    while( true ) {
        std::cout<< s;
    }
    return 0;
}

int echo_size() {
    std::string s;
    while( std::getline( std::cin, s ).good() ) {
        std::cout<<s.size()<<std::endl;
    }
    return 0;
}

int echo()
{
    std::string s;
    while( std::getline( std::cin, s ).good() ) {
        std::cout<<s<<"\n";
    }
    return 0;
}

int echo_with_err()
{
    std::string s;
    std::cerr<<random_string( 500000 );
    while( std::getline( std::cin, s ).good() ) {
        std::cout<<s<<"\n";
    }
    return 0;
}

int echo_picky()
{
    std::string s;
    while( std::getline( std::cin, s ).good() ) {
        if( s=="STOP" ) {
            std::cerr<<"OK\n";
            return 0;
        }
        std::cout<<s<<"\n";
    }
    return 0;
}

int pathologic()
{
    std::string in_s=random_string( 1000 );
    int cnt=2500;
    for( int i=0; i<cnt; ++i ) {
        std::string t=in_s+"\n";
        fputs( t.c_str(), stdout );
    }
    std::string out_s;
    int n=0;

    char buf[1002];

    while( fgets( buf, sizeof( buf ), stdin )!=0 ) {
        int len=strlen( buf );

        if( len>0 && buf[len-1]=='\n' ) {
            --len;
        }
        out_s.assign( buf, len );
        if( out_s!=in_s ) {
            fputs( "ERROR", stderr );
            return 0;
        }
        ++n;
    }
    if( n==1200 ) {
        fputs( "OK", stderr );
    }else {
        fputs( "ERROR", stderr );
    }
    return 0;
}

int long_out_line()
{
    std::cout<<random_string( 2000000 );
    return 0;
}

int exit_code()
{
    std::string s;
    std::stringstream ss;
    std::cin>>s;
    ss.str( s );
    int n;
    ss>>n;
    return n;
}

// selection of child functions

typedef int(*child_func_t)();

child_func_t find_child_func( std::string const & name )
{
    typedef std::map< std::string, child_func_t > child_funcs_t;
    static child_funcs_t funcs;
    if( funcs.size()==0 ) {
        funcs["hello"]=hello;
        funcs["helloworld"]=helloworld;
        funcs["hello-o-world-e"]=hello_o_world_e;
        funcs["with space"]=with_space;
        funcs["write-after-pause"]=write_after_pause;
        funcs["read-after-pause"]=read_after_pause;
        funcs["dont-read"]=dont_read;
        funcs["dont-stop"]=dont_stop;
        funcs["echo-size"]=echo_size;
        funcs["echo"]=echo;
        funcs["echo-with-err"]=echo_with_err;
        funcs["echo-picky"]=echo_picky;
        funcs["pathologic"]=pathologic;
        funcs["long-out-line"]=long_out_line;
        funcs["exit-code"]=exit_code;
    }
    child_funcs_t::iterator i=funcs.find( name );
    return i==funcs.end() ? 0 : i->second;
}

void pathologic_one( exec_stream_t & exec_stream, bool expect_ok=true )
{
    std::string in_s=random_string( 1000 );
    int cnt=1200;
    for( int i=0; i<cnt; ++i ) {
        exec_stream.in()<<in_s<<"\n";
    }
    exec_stream.close_in();
    std::string out_s;
    int n=0;
    while( std::getline( exec_stream.out(), out_s ).good() ) {
        if( out_s!=in_s ) {
            TEST( 0=="out_s!=in_s" );
            break;
        }
        ++n;
    }
    TEST( n==2500 );
    exec_stream.err()>>out_s;
    if( expect_ok ) {
        TEST( out_s=="OK" );
    }else {
        TEST( out_s=="ERROR" );
    }
}

#include <string>
#include <fstream>
#include <assert.h>
int main( int argc, char ** argv )
{

try {
    exec_stream_t es( "mingw32-make.exe", "-version" ); // run perl without any arguments
    es.close_in();
    std::string line;
    std::ofstream fileout("file.log");
    assert(fileout.is_open());
    while(std::getline(es.out(),line))
    {
        std::cout<<line<<std::endl;
    }

  std::cout<<"From here "<<std::endl;
    Sleep(3000);
    exit(0);
}catch( std::exception const & e ) {
    std::cerr << "error: "  <<  e.what()  <<  "\n";
}
}
