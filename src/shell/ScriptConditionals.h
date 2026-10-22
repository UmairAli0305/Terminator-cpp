#ifndef SCRIPT_CONDITIONALS_H
#define SCRIPT_CONDITIONALS_H
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace ScriptConditionals {
struct Frame { bool parentActive; bool matched; bool active; bool elseSeen; };
struct State { std::vector<Frame> frames; };
inline std::string trim(const std::string& s) {
    auto a=s.find_first_not_of(" \t\r");
    if(a==std::string::npos) return "";
    return s.substr(a,s.find_last_not_of(" \t\r")-a+1);
}
inline std::string keyword(const std::string& line) {
    std::istringstream in(trim(line)); std::string k; in>>k;
    if(k=="if" || k=="elif" || k=="else" || k=="fi" || k=="then") return k;
    return "";
}
inline bool conditionText(const std::string& line, const std::string& k, std::string& expression) {
    std::string rest=trim(trim(line).substr(k.size()));
    if(rest.size()<6 || rest.substr(rest.size()-6)!="; then") return false;
    expression=trim(rest.substr(0,rest.size()-6));
    return expression.size()>=2 && expression.front()=='[' && expression.back()==']';
}
inline bool validate(const std::vector<std::string>& lines, std::string& error) {
    struct ParseFrame { bool hasElse=false; };
    std::vector<ParseFrame> stack;
    for(size_t i=0;i<lines.size();++i) {
        const auto k=keyword(lines[i]); std::string expr;
        if(k=="if") {
            if(!conditionText(lines[i],k,expr)) {error="invalid if condition at line "+std::to_string(i+1);return false;}
            stack.push_back({});
        } else if(k=="elif") {
            if(stack.empty() || stack.back().hasElse || !conditionText(lines[i],k,expr)) {
                error="invalid elif at line "+std::to_string(i+1);return false;
            }
        } else if(k=="else") {
            if(stack.empty() || stack.back().hasElse || trim(lines[i])!="else") {
                error="invalid else at line "+std::to_string(i+1);return false;
            }
            stack.back().hasElse=true;
        } else if(k=="fi") {
            if(stack.empty() || trim(lines[i])!="fi") {error="unexpected fi at line "+std::to_string(i+1);return false;}
            stack.pop_back();
        } else if(k=="then") {error="unexpected then at line "+std::to_string(i+1);return false;}
        else {
            std::istringstream in(lines[i]);std::string first;in>>first;
            if(first=="for" || first=="while" || first=="function") {
                error="unsupported control structure at line "+std::to_string(i+1);return false;
            }
        }
    }
    if(!stack.empty()) {error="missing fi";return false;}
    return true;
}
inline std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out; std::string word;char quote=0;
    for(char c:text) {
        if((c=='\'' || c=='"') && (!quote || quote==c)) {quote=quote?0:c;continue;}
        if(std::isspace(static_cast<unsigned char>(c)) && !quote) {
            if(!word.empty()) {out.push_back(word);word.clear();}
        } else word+=c;
    }
    if(!word.empty()) out.push_back(word);
    return out;
}
inline bool evaluate(const std::string& expression, const std::filesystem::path& cwd) {
    const std::string inside=trim(expression.substr(1,expression.size()-2));
    auto v=words(inside);
    if(v.size()==2 && (v[0]=="-f" || v[0]=="-d")) {
        std::filesystem::path p(v[1]);if(p.is_relative())p=cwd/p;
        std::error_code ec;
        return v[0]=="-f" ? std::filesystem::is_regular_file(p,ec) : std::filesystem::is_directory(p,ec);
    }
    if(v.size()!=3) return false;
    if(v[1]=="=" || v[1]=="==") return v[0]==v[2];
    if(v[1]=="!=") return v[0]!=v[2];
    try {
        size_t a=0,b=0;long long x=std::stoll(v[0],&a),y=std::stoll(v[2],&b);
        if(a!=v[0].size() || b!=v[2].size())return false;
        if(v[1]=="-eq")return x==y;
        if(v[1]=="-ne")return x!=y;
        if(v[1]=="-gt")return x>y;
        if(v[1]=="-lt")return x<y;
        if(v[1]=="-ge")return x>=y;
        if(v[1]=="-le")return x<=y;
    } catch(...) {}
    return false;
}
inline bool active(const State& s) {return s.frames.empty() || s.frames.back().active;}
inline void advance(State& s,const std::string& line,const std::filesystem::path& cwd) {
    std::string k=keyword(line),expr;
    if(k=="if") {
        conditionText(line,k,expr);bool parent=active(s);
        bool yes=parent && evaluate(expr,cwd);
        s.frames.push_back({parent,yes,yes,false});
    } else if(k=="elif") {
        auto& f=s.frames.back();conditionText(line,k,expr);
        f.active=f.parentActive && !f.matched && evaluate(expr,cwd);
        f.matched=f.matched || f.active;
    } else if(k=="else") {
        auto& f=s.frames.back();f.active=f.parentActive && !f.matched;
        f.matched=true;f.elseSeen=true;
    } else if(k=="fi") s.frames.pop_back();
}
}
#endif
