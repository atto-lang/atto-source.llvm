// Closed scalar trusted-host provider adapter; not a general namespace linker.
// Caller authenticates the complete inventory and explicitly selects a symbol.
fn ident(s:&str)->bool{
    !s.is_empty() && s.len()<=128 && s.bytes().enumerate().all(|(i,c)|
        c.is_ascii_alphabetic()||c==b'_'||(i>0&&c.is_ascii_digit()))
}
fn deny(reason:&str)->String{format!("E_NAMESPACE_PROFILE: {reason}")}
pub(crate) fn compile(input:&str,namespace:&str,symbol:&str)->Result<String,String>{
    if input.len()>4096||input.contains('\0')||!ident(namespace)||!ident(symbol)||symbol=="main"{
        return Err(deny("source or identifier budget"));
    }
    let header=format!("namespace {namespace};\npub fnc {symbol}(");
    let rest=input.strip_prefix(&header).ok_or_else(||deny("explicit public provider required"))?;
    let (params,tail)=rest.split_once(')').ok_or_else(||deny("signature"))?;
    if !params.is_empty(){return Err("E_NAMESPACE_ARITY: zero-argument provider required".into());}
    let literal=tail.strip_prefix(": int { return ").and_then(|s|s.strip_suffix("; }\n"))
        .ok_or_else(||deny("closed literal body required"))?;
    let digits=literal.strip_prefix('-').unwrap_or(literal);
    if digits.is_empty()||digits.len()>19||!digits.bytes().all(|b|b.is_ascii_digit())||
        (digits.len()>1&&digits.starts_with('0')){
        return Err(deny("integer literal required"));
    }
    // Preserve the verified provider declaration and expression. The closed
    // namespace metadata is authenticated by the caller; the checked scalar
    // compiler sees the declaration and a synthetic entry invoking it.
    let declaration=input.split_once('\n').ok_or_else(||deny("header"))?.1;
    let source=format!("{declaration}pub fnc main(): int {{ return {symbol}(); }}\n");
    crate::compile_ir_entry(&source,crate::LibraryProfile::FreestandingCore,true)
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
