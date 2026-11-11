#!/usr/bin/env node
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, readFileSync, realpathSync, rmSync, statSync, writeFileSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";

// Runs the REAL oreslang-llvmc. A parse error is NOT a verified semantic
// negative, and a failed positive is NEVER a parity pass.
const root=resolve(dirname(fileURLToPath(import.meta.url)),"..");
const spec=JSON.parse(readFileSync(join(root,"contracts/functor-adt-v1/functor-adt-v1.json"),"utf8"));
const work=mkdtempSync(join(tmpdir(),"atto-functor-llvm-gate-"));
const gitBlob=b=>createHash("sha1").update(Buffer.from("blob "+b.length+"\0")).update(b).digest("hex");
const sha256=b=>createHash("sha256").update(b).digest("hex");
function run(program,args,timeout=60_000,env=process.env){
  const r=spawnSync(program,args,{cwd:root,encoding:"utf8",timeout,maxBuffer:8*1024*1024,env});
  if(r.error)throw Error(program+" spawn error: "+r.error.message);
  if(r.signal)throw Error(program+" crashed/signal: "+r.signal);
  if(r.status===null)throw Error(program+" missing exit status");
  return r;
}
const compiler=realpathSync(process.env.ORESLANG_LLVMC||join(root,"build/oreslang-llvmc"));
assert.equal(statSync(compiler).isFile(),true);
assert.equal(compiler.split("/").at(-1),"oreslang-llvmc");
const sourceCommit=spec.source.source_commit;
assert.equal(sourceCommit,"faa149c6bcc7510a659c86499f614efdaffcc4e8");
assert.equal(spec.source.fixture_tree,"be201b1c667057582a6e7e53025bb1d1938179c6");
assert.equal(spec.cases.length,17);
const currentHead=run("git",["rev-parse","HEAD"]).stdout.trim();
const currentTree=run("git",["rev-parse","HEAD^{tree}"]).stdout.trim();
const fixtureTree=run("git",["rev-parse","HEAD:contracts/functor-adt-v1/fixtures"]).stdout.trim();
assert.equal(fixtureTree,spec.source.fixture_tree,"source fixture subtree differs from Java exact Git tree");
assert.equal(run("git",["status","--porcelain","--untracked-files=all"]).stdout.trim(),"","dirty tree");
const results=[];
try{
  for(const c of spec.cases){
    assert.match(c.id,/^[a-z0-9-]+$/);
    assert.equal(c.backends.llvm,"PENDING");
    const input=join(root,"contracts/functor-adt-v1/fixtures",c.id+".atto");
    const bytes=readFileSync(input);
    assert.equal(gitBlob(bytes),c.blob_sha,c.id+" changed from source");
    const cr=run(compiler,["--target=host",input]);
    const diagnostic=(cr.stderr||"").trim().slice(0,1600);
    if(c.kind==="compile_reject"){
      // A syntactic, unsupported-feature or other nonzero rejection is NOT
      // enough evidence that the LLVM type/ownership checker is correct.
      if(cr.status===0)throw Error(c.id+": LLVM silently accepted invalid program; investigate type unsoundness");
      if(cr.stdout.trim())throw Error(c.id+": compiler published partial IR on rejection");
      const unsupported=/unsupported|unimplemented|not supported|not implemented|unexpected token|syntax error|parse error/i.test(diagnostic);
      results.push({id:c.id,kind:c.kind,compiler_status:cr.status,
        evidence:unsupported?"UNSUPPORTED_OR_PARSER_REJECTION":"REJECTED_DIAGNOSTIC_NOT_YET_CERTIFIED",
        diagnostic,semantics_proven:false});
      continue;
    }
    assert.equal(c.kind,"compile_and_execute");
    if(cr.status!==0){
      if(cr.stdout.trim())throw Error(c.id+": failed positive compilation leaked partial IR");
      results.push({id:c.id,kind:c.kind,compiler_status:cr.status,evidence:"NOT_EXECUTED_FRONTEND_REJECTION",
        diagnostic,semantics_proven:false});
      continue;
    }
    if(!/^define\s/m.test(cr.stdout))throw Error(c.id+": successful compile emitted no LLVM definitions");
    const executions=[];
    for(const opt of [0,2]){
      const bin=join(work,c.id+"-O"+opt);
      const compile=run(join(root,"scripts/compile-aot.sh"),[input,bin],180_000,
        {...process.env,ORESLANG_LLVMC:compiler,ORESLANG_LLVM_AOT_OPT_LEVEL:String(opt)});
      if(compile.status!==0||!existsSync(bin))throw Error(c.id+" O"+opt+": native AOT failed: "+compile.stderr.slice(0,1200));
      const magic=readFileSync(bin).subarray(0,4).toString("hex");
      assert.equal(magic,"7f454c46",c.id+" did not produce native ELF");
      const guest=run(bin,[],20_000);
      assert.equal(guest.status,0,c.id+" native guest O"+opt+" exit != 0");
      assert.equal(guest.stdout.trim(),c.expected_stdout,c.id+" native guest O"+opt+" stdout mismatch");
      executions.push({optimization:opt,binary_sha256:sha256(readFileSync(bin)),executed:true});
    }
    results.push({id:c.id,kind:c.kind,evidence:"NATIVE_GUEST_EXECUTED_O0_O2",compiler_status:0,
      semantics_proven:true,executions});
  }
  assert.equal(results.length,17);
  const nativePositive=results.filter(c=>c.evidence==="NATIVE_GUEST_EXECUTED_O0_O2").length;
  // Negative "type checking" cannot be credited without an individually
  // exact semantic diagnostic; these are presently just admission probes.
  const certifiedNegative=0;
  const receipt={
    schema:"attolang.functor-llvm-admission/1",
    compiler_sha256:sha256(readFileSync(compiler)),head_sha:currentHead,tree_sha:currentTree,
    source_fixture_commit:sourceCommit,source_fixture_git_tree:fixtureTree,
    positive_native_executed:nativePositive,
    negative_semantics_certified:certifiedNegative,
    backend_parity_certified:false,
    cases:results
  };
  assert.equal(results.filter(x=>x.kind==="compile_and_execute").length,2);
  assert.equal(results.filter(x=>x.kind==="compile_reject").length,15);
  for(const entry of results){
    const diag=String(entry.diagnostic||"").replace(/\s+/g," ").slice(0,260);
    console.log("LLVM_FUNCTOR_CASE id="+entry.id+
      " evidence="+entry.evidence+" compiler_exit="+entry.compiler_status+
      " diagnostic="+JSON.stringify(diag));
  }
  const out=process.env.FUNCTOR_LLVM_RECEIPT_PATH||join(work,"functor-llvm-admission.json");
  writeFileSync(out,JSON.stringify(receipt,null,2)+"\n");
  console.log("FUNCTOR_LLVM_ADMISSION_DONE positives_executed="+nativePositive+
    " negatives_compiler_rejected="+results.filter(x=>x.kind==="compile_reject").length+
    " negative_semantics_certified="+certifiedNegative+" parity=NOT_CERTIFIED");
  console.log("FUNCTOR_LLVM_RECEIPT_PATH="+out);
}finally{
  if(!process.env.FUNCTOR_KEEP_TMP)rmSync(work,{recursive:true,force:true});
}
