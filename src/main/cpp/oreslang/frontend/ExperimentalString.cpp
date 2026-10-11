#include "oreslang/frontend/ExperimentalString.hpp"
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
namespace oreslang {
namespace {
struct Token { std::string value; bool quoted=false; std::size_t offset=0; };
struct Init { enum Kind { Literal, Copy, Take } kind; std::string value; };
struct Binding { std::string name; Init init; };
class Source {
public:
 explicit Source(std::string_view s): text(s) {
  if(s.size()>1048576) throw std::runtime_error("experimental String source exceeds 1 MiB");
  advance();
 }
 std::vector<Binding> locals;
 std::vector<std::string> lengths;
 void parse() {
  need("pub"); need("fnc"); need("main"); need("("); need(")"); need(":"); need("int"); need("{");
  std::unordered_set<std::string> live, used;
  while(accept("val")) {
   std::string name=identifier();
   if(!used.insert(name).second) fail("duplicate String binding");
   need(":"); need("String"); need("=");
   Init init;
   if(token.quoted){init={Init::Literal,token.value};advance();}
   else {
    need("rt");
    if(accept("copy")) init.kind=Init::Copy;
    else if(accept("take")) init.kind=Init::Take;
    else fail("String ownership requires rt copy or rt take");
    init.value=identifier();
    if(!live.count(init.value)) fail("use of moved or unknown String");
    if(init.kind==Init::Take) live.erase(init.value);
   }
   need(";");locals.push_back({name,std::move(init)});live.insert(name);
   if(locals.size()>512) fail("too many String bindings");
  }
  need("return");
  do {
   need("len");need("(");auto name=identifier();need(")");
   if(!live.count(name)) fail("length reads moved or unknown String");
   lengths.push_back(std::move(name));
  } while(accept("+"));
  need(";");need("}");
  if(!token.value.empty()) fail("unexpected String source suffix");
 }
private:
 [[noreturn]] void fail(const std::string& msg)const{
  throw std::runtime_error("experimental String at byte "+std::to_string(token.offset)+": "+msg);
 }
 bool accept(std::string_view value){if(token.quoted||token.value!=value)return false;advance();return true;}
 void need(std::string_view value){if(!accept(value))fail("expected '"+std::string(value)+"'");}
 std::string identifier(){
  if(token.quoted||token.value.empty()||!(std::isalpha(static_cast<unsigned char>(token.value[0]))||token.value[0]=='_'))fail("expected String identifier");
  static const std::unordered_set<std::string> keywords={"pub","fnc","main","int","String","rt","copy","take","val","return","len","await","select","async","suspends","spawn","actor","null","true","false","over","structural"};
  if(keywords.count(token.value))fail("reserved String identifier");
  auto result=token.value;advance();return result;
 }
 void advance(){
  while(pos<text.size()){
   unsigned char c=static_cast<unsigned char>(text[pos]);
   if(std::isspace(c)){++pos;continue;}
   if(text.substr(pos,2)=="//") {auto e=text.find('\n',pos+2);pos=e==std::string_view::npos?text.size():e;continue;}
   break;
  }
  token={"",false,pos};if(pos==text.size())return;
  const std::size_t begin=pos;const unsigned char c=static_cast<unsigned char>(text[pos]);
  if(std::isalpha(c)||c=='_'){
   ++pos;while(pos<text.size()){unsigned char d=static_cast<unsigned char>(text[pos]);if(!std::isalnum(d)&&d!='_')break;++pos;}
   token.value=std::string(text.substr(begin,pos-begin));
  }else if(c=='"'){
   ++pos;token.quoted=true;
   while(pos<text.size()){
    const char ch=text[pos++];if(ch=='"')return;
    if(ch=='\\'){
     if(pos==text.size())fail("unfinished String escape");
     const char e=text[pos++];
     switch(e){case 'n':token.value.push_back('\n');break;case 'r':token.value.push_back('\r');break;case 't':token.value.push_back('\t');break;case '0':token.value.push_back('\0');break;case '"':token.value.push_back('"');break;case '\\':token.value.push_back('\\');break;default:fail("unsupported String escape");}
    } else {if(static_cast<unsigned char>(ch)<32)fail("unescaped control");token.value.push_back(ch);}
    if(token.value.size()>16777216)fail("String literal too large");
   }
   fail("unterminated String literal");
  } else if(std::string_view("():;{}+=").find(static_cast<char>(c))!=std::string_view::npos){token.value.assign(1,static_cast<char>(c));++pos;}
  else fail("unsupported String token");
 }
 std::string_view text;std::size_t pos=0;Token token;
};
}
std::string emit_experimental_string_ir(std::string_view input){
 Source source(input);source.parse();
 llvm::LLVMContext context;llvm::Module module("experimental-oreslang-string",context);
 auto* i8=llvm::Type::getInt8Ty(context);auto* i32=llvm::Type::getInt32Ty(context);auto* i64=llvm::Type::getInt64Ty(context);auto* ptr=i8->getPointerTo();
 auto* shape=llvm::StructType::create(context,{ptr,i64},"oreslang.string");
 auto* zero=llvm::Constant::getNullValue(shape);auto* ok=llvm::ConstantInt::get(i32,0);
 auto new_string=module.getOrInsertFunction("oreslang_string_new_utf8",llvm::FunctionType::get(i32,{ptr,i64,ptr},false));
 auto clone=module.getOrInsertFunction("oreslang_string_clone",llvm::FunctionType::get(i32,{ptr,ptr},false));
 auto drop=module.getOrInsertFunction("oreslang_string_drop",llvm::FunctionType::get(llvm::Type::getVoidTy(context),{ptr},false));
 auto* main=llvm::Function::Create(llvm::FunctionType::get(i64,{},false),llvm::Function::ExternalLinkage,"main",module);
 auto* entry=llvm::BasicBlock::Create(context,"entry",main);
 auto* failure=llvm::BasicBlock::Create(context,"failure",main);
 llvm::IRBuilder<> builder(entry);
 std::unordered_map<std::string,llvm::Value*> slots;
 std::vector<llvm::Value*> owned;
 for(const auto& binding:source.locals){
  auto* local=builder.CreateAlloca(shape,nullptr,binding.name+".owned");
  builder.CreateStore(zero,local);owned.push_back(local);slots.emplace(binding.name,local);
 }
 for(const auto& binding:source.locals){
  llvm::Value* dest=slots.at(binding.name);
  const auto& init=binding.init;
  if(init.kind==Init::Take){
   auto* from=slots.at(init.value);
   builder.CreateStore(builder.CreateLoad(shape,from),dest);
   builder.CreateStore(zero,from);
   continue;
  }
  llvm::Value* status=nullptr;
  if(init.kind==Init::Copy)status=builder.CreateCall(clone,{slots.at(init.value),dest});
  else if(init.value.empty())continue;
  else{
   auto* content=llvm::ConstantDataArray::getString(context,init.value,false);
   auto* global=new llvm::GlobalVariable(module,content->getType(),true,llvm::GlobalValue::PrivateLinkage,content,"string.bytes");
   auto* data=builder.CreateInBoundsGEP(global->getValueType(),global,{llvm::ConstantInt::get(i64,0),llvm::ConstantInt::get(i64,0)});
   status=builder.CreateCall(new_string,{data,llvm::ConstantInt::get(i64,init.value.size()),dest});
  }
  auto* next=llvm::BasicBlock::Create(context,"string.ok",main);
  builder.CreateCondBr(builder.CreateICmpEQ(status,ok),next,failure);
  builder.SetInsertPoint(next);
 }
 llvm::Value* length=llvm::ConstantInt::get(i64,0);
 for(const auto& name:source.lengths){auto* field=builder.CreateStructGEP(shape,slots.at(name),1);length=builder.CreateAdd(length,builder.CreateLoad(i64,field));}
 for(auto it=owned.rbegin();it!=owned.rend();++it)builder.CreateCall(drop,{*it});
 builder.CreateRet(length);
 builder.SetInsertPoint(failure);
 for(auto it=owned.rbegin();it!=owned.rend();++it)builder.CreateCall(drop,{*it});
 builder.CreateRet(llvm::ConstantInt::get(i64,93));
 std::string diagnostics;llvm::raw_string_ostream errors(diagnostics);
 if(llvm::verifyModule(module,&errors)){errors.flush();throw std::runtime_error("invalid experimental String IR: "+diagnostics);}
 std::string ir;llvm::raw_string_ostream out(ir);module.print(out,nullptr);out.flush();return ir;
}
}
