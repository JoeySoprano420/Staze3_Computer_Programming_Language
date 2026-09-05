#include "staze/parser.hpp"
#include "staze/diagnostic.hpp"
#include <string_view>

namespace staze {

const Token& Parser::peek(std::size_t n) const { return tokens_[std::min(pos_+n,tokens_.size()-1)]; }
bool Parser::is(TokenKind k) const { return peek().kind==k; }
bool Parser::match(TokenKind k){ if(!is(k))return false; ++pos_; return true; }
Token Parser::consume(TokenKind k,const char* m){ if(!is(k)) throw CompileError(peek().location,m); return tokens_[pos_++]; }
bool Parser::text_is(std::string_view s) const { return peek().text==s; }
Token Parser::consume_word(std::string_view s,const char* m){ if(!text_is(s)) throw CompileError(peek().location,m); return tokens_[pos_++]; }
void Parser::optional_semi(){ match(TokenKind::Semicolon); }

std::string Parser::parse_qualified_name(){
    std::string out=consume(TokenKind::Identifier,"expected name").text;
    while(match(TokenKind::Dot)){ out+='.'; out+=consume(TokenKind::Identifier,"expected name after '.'").text; }
    return out;
}
std::string Parser::parse_semantic_name(){
    auto accept_name=[&]()->Token{
        if(is(TokenKind::Identifier)||is(TokenKind::BuiltinType)||is(TokenKind::ReservedKeyword)) return tokens_[pos_++];
        throw CompileError(peek().location,"expected semantic name");
    };
    std::string out=accept_name().text;
    while(is(TokenKind::Dot)||is(TokenKind::DoubleColon)){
        bool dc=match(TokenKind::DoubleColon); if(!dc) consume(TokenKind::Dot,"expected '.'"); out += dc?"::":"."; out+=accept_name().text;
    }
    return out;
}

std::string Parser::parse_use_decl(){
    consume(TokenKind::KwUse,"expected 'use'"); std::string out=parse_qualified_name();
    if(match(TokenKind::DoubleColon)){
        out+="::{"; consume(TokenKind::LBrace,"expected '{' after '::'"); bool first=true;
        while(!is(TokenKind::RBrace)){ if(!first){consume(TokenKind::Comma,"expected ','");out+=',';} out+=consume(TokenKind::Identifier,"expected imported name").text;first=false; }
        consume(TokenKind::RBrace,"expected '}'"); out+='}';
    } else if(match(TokenKind::KwAs)){out+=" as ";out+=consume(TokenKind::Identifier,"expected import alias").text;}
    optional_semi(); return out;
}

TypeRef Parser::parse_type(){
    Token t;
    if(is(TokenKind::BuiltinType)||is(TokenKind::Identifier)) t=tokens_[pos_++];
    else throw CompileError(peek().location,"expected type");
    TypeRef out{t.text,{},t.location};
    if(match(TokenKind::Less)){
        for(;;){
            if(is(TokenKind::Integer)){ auto n=tokens_[pos_++]; out.args.push_back(TypeRef{n.text,{},n.location}); }
            else out.args.push_back(parse_type());
            if(!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::Greater,"expected '>' to close type arguments");
    }
    return out;
}

Parameter Parser::parse_parameter(){
    auto n=consume(TokenKind::Identifier,"expected parameter name"); consume(TokenKind::Colon,"expected ':' after parameter name");
    std::string auth="read";
    if(is(TokenKind::KwRead)||is(TokenKind::KwRevise)||is(TokenKind::KwMove)||is(TokenKind::KwInsert)||is(TokenKind::KwRemove)||is(TokenKind::KwLink)||is(TokenKind::KwUnlink)||is(TokenKind::KwDelegate)) auth=tokens_[pos_++].text;
    else if(text_is("borrow")||text_is("borrow_mut")||text_is("shared")||text_is("send")) auth=tokens_[pos_++].text;
    return Parameter{n.text,auth,parse_type(),n.location};
}

std::vector<std::string> Parser::parse_faults(){
    std::vector<std::string> out; consume(TokenKind::KwFaults,"expected 'faults'"); consume(TokenKind::LBracket,"expected '[' after faults");
    if(!is(TokenKind::RBracket)){ for(;;){out.push_back(parse_semantic_name()); if(!match(TokenKind::Comma))break;} }
    consume(TokenKind::RBracket,"expected ']' after faults"); return out;
}

Expr Parser::make_binary(BinaryOp op,Expr lhs,Expr rhs,SourceLocation w){
    Expr e; e.id=next_expr_id_++;e.where=w;e.node=BinaryExpr{op,std::make_shared<Expr>(std::move(lhs)),std::make_shared<Expr>(std::move(rhs))};return e;
}

Expr Parser::parse_primary(){
    auto make=[&](SourceLocation w,auto node){Expr e;e.id=next_expr_id_++;e.where=w;e.node=std::move(node);return e;};
    if(is(TokenKind::Integer)){auto t=tokens_[pos_++];return make(t.location,IntExpr{t.text});}
    if(is(TokenKind::String)){auto t=tokens_[pos_++];return make(t.location,TextExpr{t.text});}
    if(match(TokenKind::KwTrue)){auto&t=tokens_[pos_-1];return make(t.location,BoolExpr{true});}
    if(match(TokenKind::KwFalse)){auto&t=tokens_[pos_-1];return make(t.location,BoolExpr{false});}
    if(match(TokenKind::LParen)){auto e=parse_expression();consume(TokenKind::RParen,"expected ')' after expression");return e;}
    if(is(TokenKind::Identifier)||is(TokenKind::BuiltinType)||is(TokenKind::KwMove)||
       (is(TokenKind::ReservedKeyword) && (peek().text=="many" || peek().text=="none" || peek().text=="borrow_mut" || peek().text=="share" || peek().text=="retain" || peek().text=="release" || peek().text=="send"))){
        auto t=tokens_[pos_++]; return make(t.location,NameExpr{t.text});
    }
    throw CompileError(peek().location,"expected expression");
}

Expr Parser::parse_postfix(){
    Expr e=parse_primary();
    for(;;){
        if(match(TokenKind::Dot)||match(TokenKind::DoubleColon)){
            const bool dc=tokens_[pos_-1].kind==TokenKind::DoubleColon;
            Token member;
            if(is(TokenKind::Identifier)||is(TokenKind::BuiltinType)||is(TokenKind::ReservedKeyword)||
               is(TokenKind::KwRead)||is(TokenKind::KwMove)||is(TokenKind::KwInsert)||is(TokenKind::KwRemove)||
               is(TokenKind::KwLink)||is(TokenKind::KwUnlink)||is(TokenKind::KwDelegate)||is(TokenKind::KwAccess)||
               is(TokenKind::KwRevise)||is(TokenKind::KwSet)||is(TokenKind::KwIndex)) {
                member=tokens_[pos_++];
            } else {
                throw CompileError(peek().location,"expected member name");
            }
            auto* name=std::get_if<NameExpr>(&e.node); if(!name) throw CompileError(member.location,"member/qualification is currently supported on semantic names only");
            name->name += dc?"::":"."; name->name += member.text; continue;
        }
        if(match(TokenKind::LParen)){
            auto* name=std::get_if<NameExpr>(&e.node); if(!name) throw CompileError(e.where,"call target must be a named instruction in this milestone");
            CallExpr c{name->name,{}};
            if(!is(TokenKind::RParen)){for(;;){c.args.push_back(parse_expression());if(!match(TokenKind::Comma))break;}}
            consume(TokenKind::RParen,"expected ')' after arguments"); e.node=std::move(c); continue;
        }
        break;
    }
    while(is(TokenKind::KwBypass)||is(TokenKind::KwDelete)){
        FaultHandler h; h.where=peek().location;
        if(match(TokenKind::KwBypass)){
            h.kind=HandlerKind::Bypass; h.fault_name=parse_semantic_name(); consume(TokenKind::KwUsing,"expected 'using' after bypass fault");
            h.replacement=std::make_shared<Expr>(parse_expression());
        } else {
            consume(TokenKind::KwDelete,"expected delete"); h.kind=HandlerKind::Delete; h.fault_name=parse_semantic_name();
        }
        e.handlers.push_back(std::move(h));
    }
    return e;
}
Expr Parser::parse_unary(){
    if(match(TokenKind::Minus)){auto w=tokens_[pos_-1].location;Expr e;e.id=next_expr_id_++;e.where=w;e.node=UnaryExpr{UnaryOp::Negate,std::make_shared<Expr>(parse_unary())};return e;}
    if(match(TokenKind::KwNot)){auto w=tokens_[pos_-1].location;Expr e;e.id=next_expr_id_++;e.where=w;e.node=UnaryExpr{UnaryOp::Not,std::make_shared<Expr>(parse_unary())};return e;}
    return parse_postfix();
}
Expr Parser::parse_multiplicative(){auto e=parse_unary();for(;;){BinaryOp op;if(match(TokenKind::Star))op=BinaryOp::Mul;else if(match(TokenKind::Slash))op=BinaryOp::Div;else if(match(TokenKind::Percent))op=BinaryOp::Mod;else break;auto w=tokens_[pos_-1].location;e=make_binary(op,std::move(e),parse_unary(),w);}return e;}
Expr Parser::parse_additive(){auto e=parse_multiplicative();for(;;){BinaryOp op;if(match(TokenKind::Plus))op=BinaryOp::Add;else if(match(TokenKind::Minus))op=BinaryOp::Sub;else break;auto w=tokens_[pos_-1].location;e=make_binary(op,std::move(e),parse_multiplicative(),w);}return e;}
Expr Parser::parse_comparison(){auto e=parse_additive();for(;;){BinaryOp op;if(match(TokenKind::Less))op=BinaryOp::Less;else if(match(TokenKind::LessEqual))op=BinaryOp::LessEqual;else if(match(TokenKind::Greater))op=BinaryOp::Greater;else if(match(TokenKind::GreaterEqual))op=BinaryOp::GreaterEqual;else break;auto w=tokens_[pos_-1].location;e=make_binary(op,std::move(e),parse_additive(),w);break;}return e;}
Expr Parser::parse_equality(){auto e=parse_comparison();if(match(TokenKind::Equal)||match(TokenKind::NotEqual)){auto k=tokens_[pos_-1].kind;auto w=tokens_[pos_-1].location;e=make_binary(k==TokenKind::Equal?BinaryOp::Equal:BinaryOp::NotEqual,std::move(e),parse_comparison(),w);}return e;}
Expr Parser::parse_and(){auto e=parse_equality();while(match(TokenKind::KwAnd)){auto w=tokens_[pos_-1].location;e=make_binary(BinaryOp::And,std::move(e),parse_equality(),w);}return e;}
Expr Parser::parse_or(){auto e=parse_and();while(match(TokenKind::KwOr)){auto w=tokens_[pos_-1].location;e=make_binary(BinaryOp::Or,std::move(e),parse_and(),w);}return e;}
Expr Parser::parse_expression(){return parse_or();}

Block Parser::parse_block(){
    consume(TokenKind::LBrace,"expected '{'"); Block out; while(!is(TokenKind::RBrace)){if(is(TokenKind::End))throw CompileError(peek().location,"unterminated block");out.push_back(parse_statement());} consume(TokenKind::RBrace,"expected '}'");return out;
}

Statement Parser::parse_statement(){
    if(match(TokenKind::KwShadow)) throw CompileError(tokens_[pos_-1].location,"explicit shadow syntax is reserved but nested shadowing is not implemented in Compiler 0.8");
    if(match(TokenKind::KwTransaction)){auto w=tokens_[pos_-1].location;return Statement{TransactionStmt{parse_block(),w}};}
    if(match(TokenKind::KwParallel)){auto w=tokens_[pos_-1].location;return Statement{ParallelStmt{parse_block(),w}};}
    if(match(TokenKind::KwTask)){auto w=tokens_[pos_-1].location;return Statement{TaskStmt{parse_block(),w}};}
    if(is(TokenKind::Identifier) && (peek(1).kind==TokenKind::ColonEqual || peek(1).kind==TokenKind::Colon)){
        auto n=consume(TokenKind::Identifier,"expected local name"); std::optional<TypeRef> type; bool rev=false;
        if(match(TokenKind::Colon)){ if(match(TokenKind::KwRevise)) rev=true; type=parse_type(); }
        consume(TokenKind::ColonEqual,"expected ':=' for binding creation"); auto value=parse_expression(); optional_semi();
        return Statement{LocalDecl{n.text,std::move(type),rev,std::move(value),n.location}};
    }
    if(match(TokenKind::KwSet)){auto w=tokens_[pos_-1].location;auto target=parse_qualified_name();consume(TokenKind::Equal,"expected '=' after set target");auto v=parse_expression();optional_semi();return Statement{SetStmt{target,std::move(v),w}};}
    if(match(TokenKind::KwRevise)){auto w=tokens_[pos_-1].location;auto target=parse_qualified_name();consume(TokenKind::KwBy,"expected 'by' after revise target");auto d=parse_expression();optional_semi();return Statement{ReviseStmt{target,std::move(d),w}};}
    if(match(TokenKind::KwIf)){auto w=tokens_[pos_-1].location;auto c=parse_expression();auto t=parse_block();Block e;if(match(TokenKind::KwElse)){if(is(TokenKind::KwIf)){auto nested=parse_statement();e.push_back(std::move(nested));}else e=parse_block();}return Statement{IfStmt{std::move(c),std::move(t),std::move(e),w}};}
    if(match(TokenKind::KwWhile)){auto w=tokens_[pos_-1].location;auto c=parse_expression();auto b=parse_block();return Statement{WhileStmt{std::move(c),std::move(b),w}};}
    if(match(TokenKind::KwLoop)){auto w=tokens_[pos_-1].location;return Statement{LoopStmt{parse_block(),w}};}
    if(match(TokenKind::KwBreak)){auto w=tokens_[pos_-1].location;optional_semi();return Statement{BreakStmt{w}};}
    if(match(TokenKind::KwContinue)){auto w=tokens_[pos_-1].location;optional_semi();return Statement{ContinueStmt{w}};}
    if(match(TokenKind::KwReturn)){auto w=tokens_[pos_-1].location;std::optional<Expr> v;if(!is(TokenKind::RBrace)&&!is(TokenKind::Semicolon))v=parse_expression();optional_semi();return Statement{ReturnStmt{std::move(v),w}};}
    if(match(TokenKind::KwPerform)){auto w=tokens_[pos_-1].location;auto v=parse_expression();optional_semi();return Statement{PerformStmt{std::move(v),w}};}
    if(match(TokenKind::KwFault)){
        auto w=tokens_[pos_-1].location;auto f=parse_semantic_name();std::vector<Expr> payload;
        if(match(TokenKind::LParen)){
            if(!is(TokenKind::RParen)){for(;;){payload.push_back(parse_expression());if(!match(TokenKind::Comma))break;}}
            consume(TokenKind::RParen,"expected ')' after fault payload");
        }
        optional_semi();return Statement{FaultStmt{f,std::move(payload),w}};
    }
    auto w=peek().location;auto e=parse_expression();optional_semi();return Statement{ExprStmt{std::move(e),w}};
}

InstructionDecl Parser::parse_instruction(){
    InstructionDecl f;f.is_public=match(TokenKind::KwPublic);auto kw=consume(TokenKind::KwInstruction,"expected instruction");f.where=kw.location;f.name=consume(TokenKind::Identifier,"expected instruction name").text;
    consume(TokenKind::LBracket,"expected '[' after instruction name");if(!is(TokenKind::RBracket)){for(;;){f.params.push_back(parse_parameter());if(!match(TokenKind::Comma))break;}}consume(TokenKind::RBracket,"expected ']' after parameters");
    consume(TokenKind::Arrow,"expected '->'");f.result_type=parse_type();
    while(match(TokenKind::At)){
        Token d=tokens_[pos_++];
        if(d.text!="borrow_from"&&d.text!="borrow_mut_from") throw CompileError(d.location,"Compiler 0.8 instruction result directive must be @borrow_from(name) or @borrow_mut_from(name)");
        consume(TokenKind::LParen,"expected '(' after result borrow directive");auto src=consume(TokenKind::Identifier,"expected source parameter name");consume(TokenKind::RParen,"expected ')' after result borrow source");
        if(f.result_borrow_from) throw CompileError(d.location,"instruction may declare only one result borrow contract");
        f.result_borrow_mode=d.text=="borrow_mut_from"?"unique-mut":"shared-read";f.result_borrow_from=src.text;
    }
    if(is(TokenKind::KwFaults)) f.faults=parse_faults();
    f.body=parse_block();
    return f;
}

FaultDecl Parser::parse_fault_decl(){
    auto kw=consume(TokenKind::KwFault,"expected fault");FaultDecl f;f.where=kw.location;f.name=consume(TokenKind::Identifier,"expected fault name").text;
    if(match(TokenKind::LBrace)){
        while(!is(TokenKind::RBrace)){
            Token n;
            if(is(TokenKind::Identifier)||is(TokenKind::ReservedKeyword)||is(TokenKind::BuiltinType)) n=tokens_[pos_++];
            else throw CompileError(peek().location,"expected fault field");
            consume(TokenKind::Colon,"expected ':'");f.fields.push_back({n.text,parse_type()});optional_semi();
        }
        consume(TokenKind::RBrace,"expected '}'");
    }else optional_semi();return f;
}

DatasetDecl Parser::parse_dataset(){
    auto kw=consume(TokenKind::KwDataset,"expected dataset");DatasetDecl d;d.where=kw.location;d.name=consume(TokenKind::Identifier,"expected dataset name").text;consume(TokenKind::LBrace,"expected '{'");
    while(!is(TokenKind::RBrace)){
        if(match(TokenKind::KwRule)){
            auto field=consume(TokenKind::Identifier,"expected rule field");std::string pred;
            while(!is(TokenKind::RBrace)&&!is(TokenKind::KwIndex)&&!is(TokenKind::KwRule)&&!(is(TokenKind::Identifier)&&(peek(1).kind==TokenKind::Colon))) { if(!pred.empty())pred+=' ';pred+=tokens_[pos_++].text; }
            d.rules.push_back({field.text,pred,field.location});continue;
        }
        if(match(TokenKind::KwIndex)){d.indexes.push_back(consume(TokenKind::Identifier,"expected indexed field").text);optional_semi();continue;}
        bool key=match(TokenKind::KwKey);auto n=consume(TokenKind::Identifier,"expected dataset field");consume(TokenKind::Colon,"expected ':' after dataset field");bool rev=match(TokenKind::KwRevise);auto type=parse_type();std::optional<Expr> def;if(match(TokenKind::Equal))def=parse_expression();optional_semi();d.fields.push_back({key,rev,n.text,std::move(type),std::move(def),n.location});
    }
    consume(TokenKind::RBrace,"expected '}'");return d;
}

ChoiceDecl Parser::parse_choice(){
    auto kw=consume(TokenKind::KwChoice,"expected choice");ChoiceDecl c;c.where=kw.location;c.name=consume(TokenKind::Identifier,"expected choice name").text;
    consume(TokenKind::LBrace,"expected '{' after choice name");
    while(!is(TokenKind::RBrace)){
        auto n=consume(TokenKind::Identifier,"expected choice case name");ChoiceCase arm;arm.name=n.text;arm.where=n.location;
        if(match(TokenKind::LParen)){
            if(!is(TokenKind::RParen)){
                for(;;){
                    auto fn=consume(TokenKind::Identifier,"expected choice payload field name");consume(TokenKind::Colon,"expected ':' after choice payload field");
                    arm.fields.push_back({fn.text,parse_type(),fn.location});if(!match(TokenKind::Comma))break;
                }
            }
            consume(TokenKind::RParen,"expected ')' after choice payload fields");
        }
        optional_semi();c.cases.push_back(std::move(arm));
    }
    consume(TokenKind::RBrace,"expected '}' after choice");return c;
}

RelationDecl Parser::parse_relation(){
    auto kw=consume(TokenKind::KwRelation,"expected relation");RelationDecl r;r.where=kw.location;r.name=consume(TokenKind::Identifier,"expected relation name").text;
    consume(TokenKind::LBracket,"expected '['");consume_word("source","expected 'source'");consume(TokenKind::Colon,"expected ':'");r.source_type=parse_type();consume(TokenKind::Comma,"expected ','");consume_word("target","expected 'target'");consume(TokenKind::Colon,"expected ':'");r.target_type=parse_type();consume(TokenKind::RBracket,"expected ']'");consume(TokenKind::LBrace,"expected '{'");
    while(!is(TokenKind::RBrace)){
        if(text_is("source")){++pos_;consume_word("cardinality","expected cardinality");r.source_cardinality=tokens_[pos_++].text;continue;}
        if(text_is("target")){++pos_;consume_word("cardinality","expected cardinality");r.target_cardinality=tokens_[pos_++].text;continue;}
        if(text_is("unique")){++pos_;consume_word("pair","expected pair");r.unique_pair=true;continue;}
        if(text_is("reverse")){++pos_;consume_word("index","expected index");if(text_is("yes")){++pos_;r.reverse_index=true;} else if(text_is("no")){++pos_;r.reverse_index=false;} else throw CompileError(peek().location,"expected yes/no");continue;}
        if(text_is("ownership")){++pos_;r.ownership=tokens_[pos_++].text;continue;}
        throw CompileError(peek().location,"unsupported relation clause in Compiler 0.8");
    }
    consume(TokenKind::RBrace,"expected '}'");return r;
}

PoolDecl Parser::parse_pool(){
    auto kw=consume(TokenKind::KwPool,"expected pool");PoolDecl p;p.where=kw.location;p.name=consume(TokenKind::Identifier,"expected pool name").text;
    while(match(TokenKind::At)){
        auto n=tokens_[pos_++];PoolDirective d{n.text,std::nullopt,n.location};
        if(match(TokenKind::LParen)){std::string arg;while(!is(TokenKind::RParen)){if(!arg.empty())arg+=' ';arg+=tokens_[pos_++].text;}consume(TokenKind::RParen,"expected ')' after directive");d.argument=arg;}p.directives.push_back(std::move(d));
    }
    consume(TokenKind::LBrace,"expected '{'");while(!is(TokenKind::RBrace)){auto n=consume(TokenKind::Identifier,"expected pool field");consume(TokenKind::Colon,"expected ':'");bool rev=match(TokenKind::KwRevise);p.fields.push_back({n.text,rev,parse_type(),n.location});optional_semi();}consume(TokenKind::RBrace,"expected '}'");return p;
}

Program Parser::parse_program(){
    Program p;consume(TokenKind::KwStaze,"file must begin with 'staze 3'");auto v=consume(TokenKind::Integer,"expected Staze version");if(v.text!="3")throw CompileError(v.location,"this compiler implements Staze generation 3 only");p.staze_version=3;
    consume(TokenKind::KwModule,"expected module declaration");p.module_name=parse_qualified_name();optional_semi();while(is(TokenKind::KwUse))p.imports.push_back(parse_use_decl());
    while(!is(TokenKind::End)){
        if(is(TokenKind::KwFault)){p.faults.push_back(parse_fault_decl());continue;}
        if(is(TokenKind::KwDataset)){p.datasets.push_back(parse_dataset());continue;}
        if(is(TokenKind::KwChoice)){p.choices.push_back(parse_choice());continue;}
        if(is(TokenKind::KwRelation)){p.relations.push_back(parse_relation());continue;}
        if(is(TokenKind::KwPool)){p.pools.push_back(parse_pool());continue;}
        if(is(TokenKind::KwPublic)||is(TokenKind::KwInstruction)){p.instructions.push_back(parse_instruction());continue;}
        throw CompileError(peek().location,"expected a v3 top-level declaration");
    }
    consume(TokenKind::End,"unexpected trailing input");return p;
}

} // namespace staze
