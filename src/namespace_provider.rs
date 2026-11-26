// Closed scalar trusted-host provider adapter; not a general namespace linker.
// Caller authenticates the complete inventory and explicitly selects a symbol.
fn ident(s:&str)->bool{
    !s.is_empty() && s.len()<=128 && s.bytes().enumerate().all(|(i,c)|
        c.is_ascii_alphabetic()||c==b'_'||(i>0&&c.is_ascii_digit()))
}
fn deny(reason:&str)->String{format!("E_NAMESPACE_PROFILE: {reason}")}
#[cfg(test)]
fn compile(input:&str,namespace:&str,symbol:&str)->Result<String,String>{
    compile_arity(input,namespace,symbol,0)
}
pub(crate) fn compile_arity(input:&str,namespace:&str,symbol:&str,arity:usize)->Result<String,String>{
    let typed=admit_arity(input,namespace,symbol,arity)?;
    crate::emit_admitted_ir(&typed,Some((symbol,arity)))
}
pub(crate) fn admit_arity(input:&str,namespace:&str,symbol:&str,arity:usize)->Result<atto_compiler::check::TProgram,String>{
    if input.len()>4096||input.contains('\0')||!ident(namespace)||!ident(symbol)||symbol=="main"{
        return Err(deny("source or identifier budget"));
    }
    let header=format!("namespace {namespace};\npub fnc {symbol}(");
    let rest=input.strip_prefix(&header).ok_or_else(||deny("explicit public provider required"))?;
    let (params,tail)=rest.split_once(')').ok_or_else(||deny("signature"))?;
    let count=if params.is_empty(){0}else{params.split(", ").count()};
    if count!=arity||count>4{return Err("E_NAMESPACE_ARITY: incompatible selected call".into());}
    let mut names=std::collections::HashSet::new();
    if count>0 {
        for p in params.split(", "){
            let name=p.strip_prefix("int ").ok_or_else(||deny("exact integer parameters required"))?;
            if !ident(name)||!names.insert(name){return Err(deny("invalid parameter"));}
        }
    }
    let expr=tail.strip_prefix(": int { return ").and_then(|s|s.strip_suffix("; }\n"))
        .ok_or_else(||deny("closed expression body required"))?;
    if expr.is_empty()||expr.len()>1024||!expr.bytes().all(|b|b.is_ascii_alphanumeric()||b"_+*() -".contains(&b)){
        return Err(deny("closed integer expression required"));
    }
    // Only parameter identifiers, integer digits and arithmetic punctuation.
    // Reject calls before the actual frontend checks the original expression.
    let bytes=expr.as_bytes();let mut k=0;
    while k<bytes.len(){
        if bytes[k].is_ascii_alphabetic()||bytes[k]==b'_'{
            let start=k;k+=1;
            while k<bytes.len()&&(bytes[k].is_ascii_alphanumeric()||bytes[k]==b'_'){k+=1;}
            if !names.contains(&expr[start..k])||expr[k..].trim_start().starts_with('('){
                return Err(deny("closed integer expression required"));
            }
        }else{k+=1;}
    }
    let declaration=input.split_once('\n').ok_or_else(||deny("header"))?.1;
    // Keep a valid ordinary zero-argument main to preserve the normal
    // checker contract. Export only the checked provider's full-width ABI.
    let source=format!("{declaration}pub fnc main(): int {{ return 0; }}\n");
    crate::admit_ir_source(&source,crate::LibraryProfile::FreestandingCore,Some((symbol,arity)))
}
#[cfg(test)]
mod tests{
    use super::*;
    #[test]
    fn exact_admission_and_native_ir(){
        let src="namespace billing;\npub fnc function0(): int { return 42; }\n";
        let bytes=compile(src,"billing","function0").unwrap();
        assert!(bytes.contains("define i64 @atto_namespace_provider()"));
        assert!(!bytes.contains("define i32 @main()"));
        assert!(bytes.contains("ret i64 42"));
        assert_eq!(bytes,compile(src,"billing","function0").unwrap());
        assert!(compile(src,"identity","function0").unwrap_err().starts_with("E_NAMESPACE_PROFILE:"));
        assert!(compile(&src.replace("pub fnc","fnc"),"billing","function0").is_err());
        assert!(compile(&src.replace("()","(int x)").replace("42","x"),"billing","function0").unwrap_err().starts_with("E_NAMESPACE_ARITY:"));
        assert!(compile(&src.replace("42","9999999999999999999"),"billing","function0").is_err());
        assert!(compile(&src.replace("42","42; } fnc main(): int { return 0"),"billing","function0").is_err());
    }
}
