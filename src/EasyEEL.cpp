#ifndef DOCTEST_CONFIG_DISABLE
 #define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#endif
#include <doctest/doctest.h>

#include "EasyEEL.h"
#include <optional>
#include "WDL/lineparse.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>

#define opaque ((void *)this)
#define EEL_STRING_GET_CONTEXT_POINTER(opaque) (((EELVM *)opaque)->_eel_str_ctx)
#ifndef EEL_STRING_STDOUT_WRITE
  #ifndef EELSCRIPT_NO_STDIO
    #define EEL_STRING_STDOUT_WRITE(x,len) { fwrite(x,len,1,stdout); fflush(stdout); }
  #endif
#endif
#undef opaque
#include "WDL/eel2/eel_strings.h"
#include "WDL/eel2/eel_misc.h"


static 
auto commonInit(EELVM *this_, NSEEL_VMCTX &VM)
  -> void
{
    this_->_eel_str_ctx = new eel_string_context_state;
    eel_string_initvm(VM);
    this_->registerFunction("printf", 0, _eel_printf);
}

EELVM::EELVM(std::vector<const char*> sections) 
    : _section_names(sections)
{
    VM = NSEEL_VM_alloc(); // create virtual machine
    commonInit(this, VM);
}

EELVM::EELVM(void* this_ptr, std::vector<const char*> sections, const char* filename_) 
    : _section_names(sections)
{
    VM = NSEEL_VM_alloc();
    NSEEL_VM_SetCustomFuncThis(VM, this_ptr);
    _filename.Set(filename_);
    commonInit(this, VM);
}

EELVM::EELVM(std::vector<const char*> sections, const char* filename_) 
    : _section_names(sections)
{
    VM = NSEEL_VM_alloc(); 
    _filename.Set(filename_);
    commonInit(this, VM);
}

auto EELVM::registerFunction(const char* name, int min_arguments, 
    EEL_F(NSEEL_CGEN_CALL *fptr)(void *, INT_PTR, EEL_F **))
  -> void
{
    // number of arguments is exact, except with 1 which is used for 1 or 0 arguments
    min_arguments = min_arguments < 1 ? 1 : min_arguments;
    NSEEL_addfunc_varparm(name, min_arguments, NSEEL_PProc_THIS, fptr);
}

auto EELVM::registerVar(const char* name)
  -> double*
{
    return NSEEL_VM_regvar(VM, name);
}

auto EELVM::setThis(void* t)
  -> void
{
    NSEEL_VM_SetCustomFuncThis(VM, t);
}

auto EELVM::getCodeHandles()
  -> WDL_PtrKeyedArray<NSEEL_CODEHANDLE>*
{
    return &_codehandles;
}

EELVM::~EELVM()
{
    // destroy all codehandles
    for (int i = 0; i < _codehandles.GetSize(); ++i) {
        NSEEL_code_free(_codehandles.Enumerate(i));
    }
    _codehandles.DeleteAll();

    NSEEL_VM_free(VM);
}

auto EELVM::executeHandle(int h)
  -> bool
{
    if (_codehandles.Exists(h)) {
        NSEEL_code_execute(_codehandles.Get(h));
        return true;
    } 
    else {
        return false;
    }
}

auto EELVM::executeHandle(std::string section_name)
  -> bool
{
    auto iter = _handle_map.find(section_name);
    if (iter != _handle_map.end())
    {
        NSEEL_code_execute(iter->second);
        return true;
    } 
    else {
        return false;
    }
}

static
auto compileBlock(NSEEL_VMCTX &vm, const WDL_FastString& curblock, int lineoffs, WDL_FastString& results)
  -> std::optional<NSEEL_CODEHANDLE>
{
    NSEEL_CODEHANDLE code_handle = NSEEL_code_compile_ex(vm, curblock.Get(), lineoffs, NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS);
    if (!code_handle) {
        char* err = NSEEL_code_getcodeerror(vm);
        if (err) {
            results.AppendFormatted(1024, "\nError: %s\r\n", err);
        }
        return {};
    }
    return code_handle;
}

auto EELVM::SetCodeSection(std::string tok, int parsestate,  
    const WDL_FastString &curblock, WDL_FastString &results, int lineoffs)
  -> bool
{   
    if (parsestate < 0) return false;

    auto code_handle = compileBlock(VM, curblock, lineoffs, results);
    if (!code_handle) return false;

    // must call this after compilation for string support
    _eel_str_ctx->update_named_vars(VM);
    
    _codehandles.Insert(parsestate, *code_handle);
    _handle_map.insert( {tok, *code_handle});
    return true;
}

// parses lines in stream for our @abcde sections, gathers the lines
// together and calls SetCodeSection
auto EELVM::compileStream(std::istream &stream, WDL_FastString &results)
  -> bool
{
    enum ParseState {
        kMidComment = -3
    };
    bool comment_state = false;
    int parsestate = -1, cursec_lineoffs = 0, lineoffs = 0;
    WDL_FastString curblock;
    curblock.SetLen(512);
    std::string cur_tok, last_tok;

    for (std::string line; std::getline(stream, line);) {
        // getline strips system line breaks, null \r too to catch strays
        const auto length = line.length();
        if (length && line[length - 1] == '\r') 
            line[length - 1] = '\0';

        lineoffs++;
        char* linebuf = &line[0];
        LineParser lp(comment_state);
        if (linebuf[0]
            && lp.parse(linebuf) == 0 // parse errors are non-zero
            && lp.getnumtokens() > 0 
            && lp.gettoken_str(0)[0] == '@')
        {
            auto tmp = lp.gettoken_str(0);
            if (tmp != cur_tok) 
                last_tok = cur_tok;
            cur_tok = tmp;
            
            auto it = std::find(_section_names.begin(), _section_names.end(), cur_tok);
            
            if (it != _section_names.end()) {
                if (parsestate >= 0)
                    SetCodeSection(last_tok, parsestate, curblock, results, cursec_lineoffs);

                parsestate = it - _section_names.begin();
                cursec_lineoffs = lineoffs;
                curblock.Set("");
            } 
            else {
                results.AppendFormatted(1024, "\tWarning: Undeclared section: %s\r\n", cur_tok.c_str());
            }
        } 
        else {
            const char *p = linebuf;
            if (parsestate == ParseState::kMidComment) {
                while (*p) {
                    if (p[0] == '*' && p[1] == '/') {
                        parsestate = -1; // end of comment!
                        p += 2;
                        break;
                    }
                    p++;
                }
            }
            if (parsestate == -1 && p[0]) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p || (p[0] == '/' && p[1] == '/')) {
                    // do nothing
                } 
                else {
                    if (*p == '/' && p[1] == '*') {
                        parsestate = ParseState::kMidComment;
                    } 
                    else {
                        results.AppendFormatted(1024 
                            , "\tWarning: line '%.100s' (and possibly more)' are not in valid"
                              " section and may be ignored\r\n"
                            , linebuf);
                        parsestate = -2;
                    }
                }
            }

            if (parsestate >= 0) {
                curblock.Append(linebuf);
                curblock.Append("\n");
            }
        }
    };

    SetCodeSection(cur_tok, parsestate, curblock, results, cursec_lineoffs);
    return results.GetLength() == 0;
}

auto EELVM::compileFile(const std::string &filename, WDL_FastString &results)
  -> bool
{
    auto file = std::ifstream(filename);
    if (!file) {
        results.AppendFormatted(1024, "\tError: fopen() - Failed opening file: %s\r\n", filename.c_str());
        return false;
    }
    auto res = compileStream(file, results);
    file.close();
    return res;
}

auto EELVM::compileFile(WDL_FastString &results)
  -> bool
{
    return compileFile(std::string(_filename.Get()), results);
}



#ifndef DOCTEST_CONFIG_DISABLE 
// need these implemented for EEL2 to compile
void NSEEL_HOSTSTUB_EnterMutex() { }
void NSEEL_HOSTSTUB_LeaveMutex() { }
#endif

static
auto compile(EELVM& vm, WDL_FastString& results)
-> bool
{
    vm.compileFile(results);
    INFO(results.Get());
    auto ok = results.GetLength() == 0;
    return ok;
}

TEST_CASE("EasyEEL2: compile and execute stringstream") {
    EELVM vm({ "@code", "@numpty" });
    WDL_FastString results;
    std::istringstream code(R"__(
        @code
        // iejrotijeirjtoe
        a = 12; 
        printf("This is executing.");
        /* ioijiosdjf
            isodijfiosdjf
        */
        
        @numpty
        // comment a += 19999;
        /* a += 200000;
        multiline   
        */ a += 1;
        b = 2;

    )__");

    auto* a = vm.registerVar("a");
    SUBCASE("compilation") {
        vm.compileStream(code, results);
        INFO(results.Get());
        CHECK(results.GetLength() == 0);
        CHECK(vm.getCodeHandlesSize() == 2);
    }
    SUBCASE("execution") {
        vm.compileStream(code, results);
        INFO(results.Get());
        CHECK(results.GetLength() == 0);
        CHECK(vm.getCodeHandlesSize() == 2);
        CHECK(vm.executeHandle(0) == true);
        CHECK(vm.executeHandle("@numpty") == true);
    }
    *a = 100.;

    SUBCASE("change registered var script") {
        vm.compileStream(code, results);
        INFO(results.Get());
        CHECK(vm.executeHandle(1) == true);
        CHECK(*a == 101);
    }
}

TEST_CASE("EEL2 parsing") {
    LineParser lp(true);
    auto is = std::istringstream("@code;\n a = 1; ");
    std::vector<char> buffer(4096, 0);
    is.read(buffer.data(), 4096);
    char* linebuf = buffer.data();
    CHECK(!lp.parse(linebuf));
    CHECK(lp.getnumtokens() > 0);
    CHECK(lp.gettoken_str(0)[0] == '@');
}

TEST_CASE("EasyEEL2: compile file with 3 sections") {
    EELVM vm({ "@code", "@bling", "@alwayslast" }, "test-script.eel");
    WDL_FastString results;
    CHECK(compile(vm, results));
    INFO("#codehandles = " << vm.getCodeHandlesSize());
    SUBCASE("number of code handles") {
        CHECK(vm.getCodeHandlesSize() == 3);
    }
    SUBCASE("execute first section") {
        INFO(results.Get());
        CHECK(vm.executeHandle("@code") == true);
    }
}

TEST_CASE("EasyEEL2: string tests") {
    EELVM vm({ "@code" });
    WDL_FastString results;
    std::istringstream code(R"__(
        @code
        a = "U";
    )__");
    SUBCASE("compilation") {
        CHECK(vm.compileStream(code, results));
        INFO(results.Get());
        CHECK(results.GetLength() == 0);
    }
    SUBCASE("execution") {
        CHECK(vm.compileStream(code, results));
        INFO(results.Get());
        CHECK(vm.executeHandle(0) == true);
    }
}



