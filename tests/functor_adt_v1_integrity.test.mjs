import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

// A pure reference model, NEVER an LLVM backend pass. Production LLVM coverage
// requires the independent native admission gate and actual guest output.
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),"..");
const dir=path.join(root,"contracts/functor-adt-v1");
const manifest=JSON.parse(readFileSync(path.join(dir,"functor-adt-v1.json"),"utf8"));
const Some=v=>({tag:"Some",value:v});
const None=()=>({tag:"None"});
const Ok=v=>({tag:"Ok",value:v});
const Err=e=>({tag:"Err",error:e});
const optionMap=(o,f)=>o.tag==="Some"?Some(f(o.value)):o;
const resultMap=(r,f)=>r.tag==="Ok"?Ok(f(r.value)):r;
const resultMapErr=(r,f)=>r.tag==="Err"?Err(f(r.error)):r;
const optionAndThen=(o,f)=>o.tag==="Some"?f(o.value):o;
const resultAndThen=(r,f)=>r.tag==="Ok"?f(r.value):r;
const optionFlatten=o=>optionAndThen(o,x=>x);
const resultFlatten=r=>resultAndThen(r,x=>x);
const optionTranspose=o=>o.tag==="None"?Ok(None()):
  o.value.tag==="Ok"?Ok(Some(o.value.value)):Err(o.value.error);
const resultTranspose=r=>r.tag==="Err"?Some(Err(r.error)):
  r.value.tag==="None"?None():Some(Ok(r.value.value));
const plus=x=>x+3;
const times=x=>x*5;
const equal=(a,b)=>assert.deepEqual(a,b);
const gitBlob=buf=>createHash("sha1").update(Buffer.from("blob "+buf.length+"\0")).update(buf).digest("hex");
const values=[-3,-1,0,1,2,7,42,100];
const errors=["","bad","timeout","domain","#symbol"];
const options=values.map(Some).concat([None()]);
const results=values.map(Ok).concat(errors.map(Err));
const nestedOptions=options.flatMap(v=>[Some(Some(v)),Some(None()),None()]);
const nestedResults=results.flatMap(v=>[Ok(Ok(v)),Ok(Err("middle")),Err("outer")]);

test("immutable 17-source corpus has pinned Java proof, NEVER invented LLVM parity",()=>{
  assert.equal(manifest.contract_id,"functor-adt-v1");
  assert.equal(manifest.schema_version,1);
  assert.equal(manifest.source.source_commit,"faa149c6bcc7510a659c86499f614efdaffcc4e8");
  assert.equal(manifest.source.fixture_tree,"be201b1c667057582a6e7e53025bb1d1938179c6");
  assert.equal(manifest.cases.length,17);
  assert.equal(manifest.evidence.java.positive_assertions,36);
  assert.equal(manifest.evidence.java.negative_fixtures,15);
  assert.equal(manifest.evidence.java_native_aot.negative_compiler_cases,15);
  const seen=new Set();
  let positives=0,negatives=0;
  for(const c of manifest.cases){
    assert.ok(!seen.has(c.id));seen.add(c.id);
    assert.match(c.id,/^[a-z0-9-]+$/);
    assert.equal(c.backends.llvm,"PENDING");
    assert.equal(c.backends.java,"GUEST_EXECUTED_EXACT_BLOB_SUBSET");
    assert.equal(c.execution_profiles.java_graalvm_native_aot,"GUEST_EXECUTED_EXACT_BLOB_SUBSET");
    assert.equal(c.path,"fixtures/functor-adt-v1/"+c.id+".atto");
    const buf=readFileSync(path.join(dir,"fixtures",c.id+".atto"));
    assert.equal(gitBlob(buf),c.blob_sha,c.id+" Git blob changed");
    assert.ok(buf.toString("utf8").endsWith("\n"));
    if(c.kind==="compile_and_execute"){
      positives++;
      const ones=c.id==="nested-functor-positive"?16:20;
      assert.equal(c.expected_stdout,"FNV"+(ones===16?"1":"2")+":"+"1".repeat(ones)+":END");
      assert.equal(c.diagnostic_category,null);
    }else{
      negatives++;
      assert.equal(c.kind,"compile_reject");
      assert.ok(c.diagnostic_category);
      assert.equal(c.expected_stdout,null);
    }
  }
  assert.equal(positives,2);
  assert.equal(negatives,15);
});

test("Option map laws: identity, composition and lazy None callback",()=>{
  for(const value of options){
    equal(optionMap(value,x=>x),value);
    equal(optionMap(optionMap(value,plus),times),optionMap(value,x=>times(plus(x))));
    let calls=0; const observed=optionMap(value,x=>{calls++;return x+1;});
    assert.equal(calls,value.tag==="Some"?1:0);
    assert.equal(observed.tag,value.tag);
  }
});
test("Result map laws: identity, composition and dormant Err callback",()=>{
  for(const value of results){
    equal(resultMap(value,x=>x),value);
    equal(resultMap(resultMap(value,plus),times),resultMap(value,x=>times(plus(x))));
    let calls=0;resultMap(value,x=>{calls++;return x+1;});
    assert.equal(calls,value.tag==="Ok"?1:0);
  }
});
test("map_err changes error slot only, preserves successful payload and skips callback",()=>{
  for(const value of results){
    let calls=0;const transformed=resultMapErr(value,e=>{calls++;return e.length;});
    assert.equal(calls,value.tag==="Err"?1:0);
    assert.equal(transformed.tag,value.tag);
    if(value.tag==="Ok")equal(transformed,value);
    else assert.equal(transformed.error,value.error.length);
  }
});
test("Option and_then and Result and_then preserve failure/absence and eliminate one layer",()=>{
  for(const value of options){
    equal(optionAndThen(value,x=>Some(x+3)),optionMap(value,plus));
    equal(optionAndThen(value,x=>None()),None());
  }
  for(const value of results){
    equal(resultAndThen(value,x=>Ok(x+3)),resultMap(value,plus));
    if(value.tag==="Err")equal(resultAndThen(value,x=>Err("unexpected")),value);
  }
});
test("transpose is involutive in both directions for every Option/Result variant",()=>{
  const left=[None(),...values.map(x=>Some(Ok(x))),...errors.map(e=>Some(Err(e)))];
  const right=[...values.map(x=>Ok(Some(x))),Ok(None()),...errors.map(Err)];
  for(const value of left)equal(resultTranspose(optionTranspose(value)),value);
  for(const value of right)equal(optionTranspose(resultTranspose(value)),value);
});
test("transpose never evaluates error or empty payload callback",()=>{
  const source=[None(),Some(Err("outer")),Some(Ok(42))];
  equal(source.map(optionTranspose),[Ok(None()),Err("outer"),Ok(Some(42))]);
  const reverse=[Err("outer"),Ok(None()),Ok(Some(42))];
  equal(reverse.map(resultTranspose),[Some(Err("outer")),None(),Some(Ok(42))]);
});
test("Option flatten removes exactly one constructor and satisfies associativity",()=>{
  for(const v of nestedOptions){
    equal(optionFlatten(optionFlatten(v)),optionFlatten(optionMap(v,optionFlatten)));
    if(v.tag==="Some")assert.ok(["Some","None"].includes(optionFlatten(v).tag));
  }
  const three=Some(Some(Some(42)));
  equal(optionFlatten(three),Some(Some(42)));
  equal(optionFlatten(optionFlatten(three)),Some(42));
});
test("Result flatten removes exactly one constructor, preserves inner and outer Err",()=>{
  for(const v of nestedResults){
    equal(resultFlatten(resultFlatten(v)),resultFlatten(resultMap(v,resultFlatten)));
    const reduced=resultFlatten(v);
    assert.ok(["Ok","Err"].includes(reduced.tag));
  }
  const three=Ok(Ok(Ok(42)));
  equal(resultFlatten(three),Ok(Ok(42)));
  equal(resultFlatten(resultFlatten(three)),Ok(42));
  equal(resultFlatten(Ok(Err("inner"))),Err("inner"));
  equal(resultFlatten(Err("outer")),Err("outer"));
});
test("three-layer Option<Result<T,E>> map preserves all constructors and error identity",()=>{
  for(const v of [None(),Some(None()),Some(Some(Err("inner"))),Some(Some(Ok(42)))]){
    let calls=0;
    const mapped=optionMap(v,x=>optionMap(x,y=>resultMap(y,n=>{calls++;return n+1;})));
    assert.equal(calls,v.tag==="Some"&&v.value.tag==="Some"&&v.value.value.tag==="Ok"?1:0);
    if(v.tag==="Some"&&v.value.tag==="Some"&&v.value.value.tag==="Ok")
      equal(mapped,Some(Some(Ok(43))));
    else equal(mapped,v);
  }
});
test("all nominal error branches survive nested map/transposes (no accidental cross-domain conversion)",()=>{
  for(const error of errors){
    equal(optionTranspose(Some(Err(error))),Err(error));
    equal(resultTranspose(Err(error)),Some(Err(error)));
    equal(resultFlatten(Ok(Err(error))),Err(error));
    equal(resultFlatten(Err(error)),Err(error));
    equal(resultMapErr(Err(error),x=>({nominal:"E2",x})),Err({nominal:"E2",x:error}));
  }
});
test("negative mutation controls catch illegal collapse, misplaced errors and eager callbacks",()=>{
  const good3=Some(Some(Some(42)));
  const badFlatten=()=>optionFlatten(optionFlatten(good3));
  assert.notDeepEqual(badFlatten(),optionFlatten(good3),"accidental two-layer collapse accepted");
  const badTranspose=()=>Ok(None()); // incorrectly swallows Err
  assert.notDeepEqual(badTranspose(),optionTranspose(Some(Err("lost"))));
  const badResultMap=()=>Ok(5); // incorrectly calls callback on Err
  assert.notDeepEqual(badResultMap(),resultMap(Err("lost"),plus));
  const badMapErr=()=>Err("rewritten"); // changes Ok branch
  assert.notDeepEqual(badMapErr(),resultMapErr(Ok(7),x=>x+"!"));
});
