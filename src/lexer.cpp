#include "staze/lexer.hpp"
#include "staze/diagnostic.hpp"
#include <cctype>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace staze {

const char* token_kind_name(TokenKind k) {
    switch (k) {
    case TokenKind::End:return "end of file"; case TokenKind::Identifier:return "identifier";
    case TokenKind::Integer:return "integer"; case TokenKind::String:return "string";
    case TokenKind::BuiltinType:return "built-in type"; case TokenKind::ReservedKeyword:return "reserved keyword";
#define K(x,s) case TokenKind::x:return s;
    K(KwStaze,"staze") K(KwModule,"module") K(KwUse,"use") K(KwAs,"as") K(KwPublic,"public") K(KwPrivate,"private")
    K(KwInstruction,"instruction") K(KwFaults,"faults") K(KwFault,"fault") K(KwPerform,"perform") K(KwReturn,"return")
    K(KwSet,"set") K(KwRevise,"revise") K(KwBy,"by") K(KwIf,"if") K(KwElse,"else") K(KwWhile,"while") K(KwDo,"do")
    K(KwLoop,"loop") K(KwBreak,"break") K(KwContinue,"continue") K(KwShadow,"shadow") K(KwDataset,"dataset") K(KwKey,"key")
    K(KwRule,"rule") K(KwIndex,"index") K(KwRelation,"relation") K(KwPool,"pool") K(KwChoice,"choice") K(KwTransaction,"transaction") K(KwParallel,"parallel") K(KwTask,"task") K(KwDelegate,"delegate") K(KwAccess,"access")
    K(KwRead,"read") K(KwMove,"move") K(KwInsert,"insert") K(KwRemove,"remove") K(KwLink,"link") K(KwUnlink,"unlink")
    K(KwTrue,"true") K(KwFalse,"false") K(KwAnd,"and") K(KwOr,"or") K(KwNot,"not") K(KwBypass,"bypass") K(KwDelete,"delete") K(KwUsing,"using")
    K(ColonEqual,":=") K(Arrow,"->") K(DoubleColon,"::") K(Ellipsis,"...") K(Equal,"=") K(NotEqual,"!=")
    K(Plus,"+") K(Minus,"-") K(Star,"*") K(Slash,"/") K(Percent,"%") K(Less,"<") K(LessEqual,"<=") K(Greater,">") K(GreaterEqual,">=")
    K(Dot,".") K(Comma,",") K(Colon,":") K(At,"@") K(LBracket,"[") K(RBracket,"]") K(LBrace,"{") K(RBrace,"}") K(LParen,"(") K(RParen,")") K(Semicolon,";")
#undef K
    }
    return "token";
}

bool Lexer::at_end() const noexcept { return pos_ >= source_.size(); }
char Lexer::peek(std::size_t n) const noexcept { const auto p=pos_+n; return p<source_.size()?source_[p]:'\0'; }
char Lexer::advance(){ char c=source_[pos_++]; if(c=='\n'){++line_;column_=1;} else ++column_; return c; }
bool Lexer::match(char c){ if(at_end()||peek()!=c) return false; advance(); return true; }
Token Lexer::make(TokenKind k,std::string t,SourceLocation s) const { return {k,std::move(t),s}; }

void Lexer::skip_space_and_comments(){
    for(;;){
        while(!at_end() && std::isspace(static_cast<unsigned char>(peek()))) advance();
        if(peek()=='/'&&peek(1)=='/'){ while(!at_end()&&peek()!='\n') advance(); continue; }
        if(peek()=='/'&&peek(1)=='*'){
            SourceLocation s{line_,column_}; advance(); advance(); std::size_t depth=1;
            while(depth){ if(at_end()) throw CompileError(s,"unterminated block comment");
                if(peek()=='/'&&peek(1)=='*'){advance();advance();++depth;}
                else if(peek()=='*'&&peek(1)=='/'){advance();advance();--depth;}
                else advance(); }
            continue;
        }
        break;
    }
}

Token Lexer::lex_identifier_or_keyword(){
    SourceLocation s{line_,column_}; auto b=pos_;
    while(!at_end()){ unsigned char c=static_cast<unsigned char>(peek()); if(!std::isalnum(c)&&peek()!='_') break; advance(); }
    std::string t(source_.substr(b,pos_-b));
    static const std::unordered_map<std::string,TokenKind> kw{
        {"staze",TokenKind::KwStaze},{"module",TokenKind::KwModule},{"use",TokenKind::KwUse},{"as",TokenKind::KwAs},
        {"public",TokenKind::KwPublic},{"private",TokenKind::KwPrivate},{"instruction",TokenKind::KwInstruction},{"faults",TokenKind::KwFaults},
        {"fault",TokenKind::KwFault},{"perform",TokenKind::KwPerform},{"return",TokenKind::KwReturn},{"set",TokenKind::KwSet},
        {"revise",TokenKind::KwRevise},{"by",TokenKind::KwBy},{"if",TokenKind::KwIf},{"else",TokenKind::KwElse},{"while",TokenKind::KwWhile},
        {"do",TokenKind::KwDo},{"loop",TokenKind::KwLoop},{"break",TokenKind::KwBreak},{"continue",TokenKind::KwContinue},{"shadow",TokenKind::KwShadow},
        {"dataset",TokenKind::KwDataset},{"key",TokenKind::KwKey},{"rule",TokenKind::KwRule},{"index",TokenKind::KwIndex},{"relation",TokenKind::KwRelation},
        {"pool",TokenKind::KwPool},{"choice",TokenKind::KwChoice},{"transaction",TokenKind::KwTransaction},{"parallel",TokenKind::KwParallel},{"task",TokenKind::KwTask},{"delegate",TokenKind::KwDelegate},{"access",TokenKind::KwAccess},{"read",TokenKind::KwRead},
        {"move",TokenKind::KwMove},{"insert",TokenKind::KwInsert},{"remove",TokenKind::KwRemove},{"link",TokenKind::KwLink},{"unlink",TokenKind::KwUnlink},
        {"true",TokenKind::KwTrue},{"false",TokenKind::KwFalse},{"and",TokenKind::KwAnd},{"or",TokenKind::KwOr},{"not",TokenKind::KwNot},
        {"bypass",TokenKind::KwBypass},{"delete",TokenKind::KwDelete},{"using",TokenKind::KwUsing}
    };
    if(auto it=kw.find(t);it!=kw.end()) return make(it->second,std::move(t),s);
    static const std::unordered_map<std::string,bool> builtin{{"unit",true},{"bool",true},{"i32",true},{"i64",true},{"u32",true},{"u64",true},{"text",true},
        {"i8",true},{"i16",true},{"i128",true},{"u8",true},{"u16",true},{"u128",true},{"isize",true},{"usize",true},{"f16",true},{"f32",true},{"f64",true},{"f128",true},
        {"decimal",true},{"char",true},{"bytes",true},{"never",true},{"list",true},{"array",true},{"tuple",true},{"optional",true},{"ref",true},{"borrow",true},
        {"handle",true},{"ptr",true},{"slice",true},{"range",true},{"rangeset",true},{"dictionary",true},{"stack",true},{"atomic",true},{"dyn",true},{"callable",true},{"quantity",true}};
    if(builtin.contains(t)) return make(TokenKind::BuiltinType,std::move(t),s);
    static const std::unordered_map<std::string,bool> reserved{
        {"alias",true},{"ascending",true},{"await",true},{"branch",true},{"cascade",true},{"category",true},{"chain",true},{"compile",true},
        {"constant",true},{"context",true},{"cyclic",true},{"descending",true},{"distinct",true},{"entity",true},{"family",true},{"for",true},{"foreign",true},
        {"from",true},{"generate",true},{"group",true},{"implements",true},{"in",true},{"into",true},{"label",true},{"many",true},{"nest",true},{"none",true},
        {"one",true},{"otherwise",true},{"outside",true},{"package",true},{"property",true},{"same",true},{"select",true},{"signature",true},
        {"source",true},{"step",true},{"structural",true},{"target",true},{"test",true},{"to",true},{"type",true},{"unique",true},
        {"unsafe",true},{"until",true},{"value",true},{"web",true},{"when",true},{"where",true},{"within",true},{"zero",true},{"zero_or_one",true},
        {"cardinality",true},{"pair",true},{"reverse",true},{"yes",true},{"no",true},{"ownership",true},{"establish",true},{"transfer",true},{"reclaim",true},{"lifetime",true}
    };
    if(reserved.contains(t)) return make(TokenKind::ReservedKeyword,std::move(t),s);
    return make(TokenKind::Identifier,std::move(t),s);
}

Token Lexer::lex_integer(){
    SourceLocation s{line_,column_}; auto b=pos_;
    while(!at_end()&&std::isdigit(static_cast<unsigned char>(peek()))) advance();
    while(!at_end()&&std::isalnum(static_cast<unsigned char>(peek()))) advance();
    return make(TokenKind::Integer,std::string(source_.substr(b,pos_-b)),s);
}

Token Lexer::lex_string(){
    SourceLocation s{line_,column_}; advance(); std::string v;
    while(!at_end()&&peek()!='"'){
        char c=advance(); if(c!='\\'){v.push_back(c);continue;} if(at_end()) throw CompileError(s,"unterminated escape sequence");
        char e=advance(); switch(e){case 'n':v.push_back('\n');break;case 'r':v.push_back('\r');break;case 't':v.push_back('\t');break;case '\\':v.push_back('\\');break;case '"':v.push_back('"');break;case '0':v.push_back('\0');break;default:throw CompileError(s,"unsupported string escape");}
    }
    if(at_end()) throw CompileError(s,"unterminated string literal");
    advance();
    return make(TokenKind::String,std::move(v),s);
}

static bool valid_utf8(std::string_view s){
    std::size_t i=0; while(i<s.size()){ unsigned char c=static_cast<unsigned char>(s[i]); if(c<=0x7f){++i;continue;} std::size_t n=0; std::uint32_t cp=0;
        if((c&0xe0)==0xc0){n=2;cp=c&0x1f;if(cp==0)return false;} else if((c&0xf0)==0xe0){n=3;cp=c&0xf;} else if((c&0xf8)==0xf0){n=4;cp=c&7;} else return false;
        if(i+n>s.size()) return false;
        for(std::size_t j=1;j<n;++j){
            unsigned char d=static_cast<unsigned char>(s[i+j]);
            if((d&0xc0)!=0x80) return false;
            cp=(cp<<6)|(d&0x3f);
        }
        if((n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)) return false;
        i+=n;
    }
    return true;
}

std::vector<Token> Lexer::lex_all(){
    if(!valid_utf8(source_)) throw CompileError({1,1},"source is not valid UTF-8");
    if(source_.size()>=3 && static_cast<unsigned char>(source_[0])==0xef && static_cast<unsigned char>(source_[1])==0xbb && static_cast<unsigned char>(source_[2])==0xbf) pos_=3;
    std::vector<Token> out;
    while(!at_end()){
        skip_space_and_comments(); if(at_end()) break; SourceLocation s{line_,column_}; char c=peek();
        if(std::isalpha(static_cast<unsigned char>(c))||c=='_'){out.push_back(lex_identifier_or_keyword());continue;}
        if(std::isdigit(static_cast<unsigned char>(c))){out.push_back(lex_integer());continue;}
        if(c=='"'){out.push_back(lex_string());continue;}
        advance();
        switch(c){
        case ':': if(match('='))out.push_back(make(TokenKind::ColonEqual,":=",s)); else if(match(':'))out.push_back(make(TokenKind::DoubleColon,"::",s)); else out.push_back(make(TokenKind::Colon,":",s)); break;
        case '-': if(match('>'))out.push_back(make(TokenKind::Arrow,"->",s)); else out.push_back(make(TokenKind::Minus,"-",s)); break;
        case '.': if(peek()=='.'&&peek(1)=='.'){advance();advance();out.push_back(make(TokenKind::Ellipsis,"...",s));} else out.push_back(make(TokenKind::Dot,".",s)); break;
        case '!': if(match('='))out.push_back(make(TokenKind::NotEqual,"!=",s)); else throw CompileError(s,"expected '=' after '!'"); break;
        case '<': if(match('=')) out.push_back(make(TokenKind::LessEqual,"<=",s)); else out.push_back(make(TokenKind::Less,"<",s)); break;
        case '>': if(match('=')) out.push_back(make(TokenKind::GreaterEqual,">=",s)); else out.push_back(make(TokenKind::Greater,">",s)); break;
        case '=': out.push_back(make(TokenKind::Equal,"=",s)); break; case '+':out.push_back(make(TokenKind::Plus,"+",s));break;
        case '*':out.push_back(make(TokenKind::Star,"*",s));break; case '/':out.push_back(make(TokenKind::Slash,"/",s));break; case '%':out.push_back(make(TokenKind::Percent,"%",s));break;
        case ',':out.push_back(make(TokenKind::Comma,",",s));break; case '@':out.push_back(make(TokenKind::At,"@",s));break; case '[':out.push_back(make(TokenKind::LBracket,"[",s));break;
        case ']':out.push_back(make(TokenKind::RBracket,"]",s));break; case '{':out.push_back(make(TokenKind::LBrace,"{",s));break; case '}':out.push_back(make(TokenKind::RBrace,"}",s));break;
        case '(':out.push_back(make(TokenKind::LParen,"(",s));break; case ')':out.push_back(make(TokenKind::RParen,")",s));break; case ';':out.push_back(make(TokenKind::Semicolon,";",s));break;
        default: throw CompileError(s,std::string("unexpected character: '")+c+"'");
        }
    }
    out.push_back(make(TokenKind::End,"",{line_,column_})); return out;
}

} // namespace staze
